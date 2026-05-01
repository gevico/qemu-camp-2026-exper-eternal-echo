/*
 * QEMU RISC-V G233 Watchdog Timer
 *
 * Register map (base 0x10010000, PLIC IRQ 4):
 *   0x00  WDT_CTRL  — bit0: EN, bit1: INTEN
 *   0x04  WDT_LOAD  — reload value (written to VAL on feed or enable)
 *   0x08  WDT_VAL   — current counter (read-only, countdown)
 *   0x0C  WDT_KEY   — 0x5A5A5A5A = feed (reload), 0x1ACCE551 = lock
 *   0x10  WDT_SR    — bit0: TIMEOUT (write-1-to-clear)
 *
 * Copyright (c) 2026 G233 QEMU Camp
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_G233_WDT "g233.wdt"
#define G233_WDT(obj) OBJECT_CHECK(G233WDTState, (obj), TYPE_G233_WDT)

/* Register offsets */
#define WDT_CTRL  0x00
#define WDT_LOAD  0x04
#define WDT_VAL   0x08
#define WDT_KEY   0x0C
#define WDT_SR    0x10

/* CTRL bits */
#define WDT_CTRL_EN     (1u << 0)
#define WDT_CTRL_INTEN  (1u << 1)
#define WDT_CTRL_MASK   (WDT_CTRL_EN | WDT_CTRL_INTEN)

/* SR bits */
#define WDT_SR_TIMEOUT  (1u << 0)

/* KEY magic values */
#define WDT_KEY_FEED  0x5A5A5A5Au
#define WDT_KEY_LOCK  0x1ACCE551u

/* 1 tick == 1 ns in this model */

typedef struct G233WDTState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer timer;

    int64_t  last_ns;    /* virtual-clock time of last sync */
    uint32_t ctrl;
    uint32_t load;
    uint32_t val;       /* current countdown value */
    uint32_t sr;        /* status: bit0 = TIMEOUT */
    bool     locked;    /* KEY_LOCK was written */
} G233WDTState;

/* ------------------------------------------------------------------ */
/* Timer helpers                                                        */

static void g233_wdt_update_irq(G233WDTState *s)
{
    bool pending = (s->ctrl & WDT_CTRL_INTEN) && (s->sr & WDT_SR_TIMEOUT);
    qemu_set_irq(s->irq, pending ? 1 : 0);
}

/*
 * Advance the countdown by the elapsed nanoseconds since last_ns.
 * Sets SR.TIMEOUT and IRQ when val reaches 0.
 */
static void g233_wdt_process_time(G233WDTState *s, int64_t now)
{
    int64_t delta = now - s->last_ns;

    if (delta <= 0) {
        return;
    }
    s->last_ns = now;

    if (!(s->ctrl & WDT_CTRL_EN)) {
        return;
    }

    if (s->sr & WDT_SR_TIMEOUT) {
        /* already timed out — counter stays at 0 */
        s->val = 0;
        return;
    }

    if ((uint64_t)delta >= (uint64_t)s->val) {
        s->val = 0;
        s->sr |= WDT_SR_TIMEOUT;
    } else {
        s->val -= (uint32_t)delta;
    }

    g233_wdt_update_irq(s);
}

static void g233_wdt_sync(G233WDTState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    g233_wdt_process_time(s, now);
}

/* Schedule timer to fire exactly when val reaches 0. */
static void g233_wdt_reschedule(G233WDTState *s)
{
    if (!(s->ctrl & WDT_CTRL_EN) || (s->sr & WDT_SR_TIMEOUT) || s->val == 0) {
        timer_del(&s->timer);
        return;
    }
    timer_mod_ns(&s->timer, s->last_ns + s->val);
}

static void g233_wdt_timer_cb(void *opaque)
{
    G233WDTState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    g233_wdt_process_time(s, now);
    g233_wdt_reschedule(s);
}

/* ------------------------------------------------------------------ */
/* MMIO                                                                 */

static uint64_t g233_wdt_read(void *opaque, hwaddr offset, unsigned size)
{
    G233WDTState *s = G233_WDT(opaque);
    uint32_t value = 0;

    g233_wdt_sync(s);

    switch (offset) {
    case WDT_CTRL:
        value = s->ctrl;
        break;
    case WDT_LOAD:
        value = s->load;
        break;
    case WDT_VAL:
        value = s->val;
        break;
    case WDT_KEY:
        /* write-only, reads 0 */
        break;
    case WDT_SR:
        value = s->sr;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }

    trace_g233_wdt_read(offset, value);
    return value;
}

static void g233_wdt_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    G233WDTState *s = G233_WDT(opaque);

    trace_g233_wdt_write(offset, value);

    g233_wdt_sync(s);

    switch (offset) {
    case WDT_CTRL:
        if (s->locked) {
            /* writes to CTRL are silently ignored when locked */
            break;
        }
        s->ctrl = (uint32_t)value & WDT_CTRL_MASK;
        if (s->ctrl & WDT_CTRL_EN) {
            /* (Re-)enable: reload counter if val is 0 */
            if (s->val == 0) {
                s->val = s->load;
            }
        }
        g233_wdt_update_irq(s);
        g233_wdt_reschedule(s);
        break;

    case WDT_LOAD:
        if (!s->locked) {
            s->load = (uint32_t)value;
        }
        break;

    case WDT_VAL:
        /* read-only */
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only VAL\n",
                      __func__);
        break;

    case WDT_KEY:
        if ((uint32_t)value == WDT_KEY_FEED) {
            /* Feed: reload val from load, clear TIMEOUT */
            s->val = s->load;
            s->sr &= ~WDT_SR_TIMEOUT;
            g233_wdt_update_irq(s);
            g233_wdt_reschedule(s);
        } else if ((uint32_t)value == WDT_KEY_LOCK) {
            s->locked = true;
        }
        break;

    case WDT_SR:
        /* W1C */
        s->sr &= ~((uint32_t)value & WDT_SR_TIMEOUT);
        g233_wdt_update_irq(s);
        g233_wdt_reschedule(s);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }
}

static const MemoryRegionOps g233_wdt_ops = {
    .read = g233_wdt_read,
    .write = g233_wdt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */

static void g233_wdt_reset(DeviceState *dev)
{
    G233WDTState *s = G233_WDT(dev);

    timer_del(&s->timer);
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->ctrl = 0;
    s->load = 0;
    s->val  = 0;
    s->sr   = 0;
    s->locked = false;
    qemu_set_irq(s->irq, 0);
}

static void g233_wdt_realize(DeviceState *dev, Error **errp)
{
    G233WDTState *s = G233_WDT(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &g233_wdt_ops, s,
                          TYPE_G233_WDT, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, g233_wdt_timer_cb, s);
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static const VMStateDescription vmstate_g233_wdt = {
    .name = TYPE_G233_WDT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_INT64(last_ns,  G233WDTState),
        VMSTATE_UINT32(ctrl,    G233WDTState),
        VMSTATE_UINT32(load,    G233WDTState),
        VMSTATE_UINT32(val,     G233WDTState),
        VMSTATE_UINT32(sr,      G233WDTState),
        VMSTATE_BOOL(locked,    G233WDTState),
        VMSTATE_TIMER(timer,    G233WDTState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = g233_wdt_realize;
    device_class_set_legacy_reset(dc, g233_wdt_reset);
    dc->vmsd = &vmstate_g233_wdt;
}

static const TypeInfo g233_wdt_info = {
    .name          = TYPE_G233_WDT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233WDTState),
    .class_init    = g233_wdt_class_init,
};

static void g233_wdt_register_types(void)
{
    type_register_static(&g233_wdt_info);
}

type_init(g233_wdt_register_types)
