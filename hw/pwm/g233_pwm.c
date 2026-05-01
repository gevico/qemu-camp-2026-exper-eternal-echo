/*
 * QEMU RISC-V G233 PWM controller
 *
 * Register layout matches tests/gevico/qtest/test-pwm-basic.c
 * (base 0x10015000; GLB at +0x00 — differs from g233-datasheet +0x40).
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

#define TYPE_G233_PWM "g233.pwm"
#define G233_PWM(obj) OBJECT_CHECK(G233PWMState, (obj), TYPE_G233_PWM)

#define G233_PWM_NUM_CH 4

/* Layout per test-pwm-basic.c */
#define PWM_OFF_GLB       0x00
#define PWM_OFF_CH_BASE   0x10
#define PWM_CH_STRIDE     0x10
#define PWM_OFF_CTRL      0x00
#define PWM_OFF_PERIOD    0x04
#define PWM_OFF_DUTY      0x08
#define PWM_OFF_CNT       0x0C

#define PWM_CTRL_EN    (1u << 0)
#define PWM_CTRL_POL   (1u << 1)
#define PWM_CTRL_INTIE (1u << 2)
#define PWM_CTRL_MASK  (PWM_CTRL_EN | PWM_CTRL_POL | PWM_CTRL_INTIE)

typedef struct G233PWMState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer timer;
    int64_t last_ns;

    uint32_t ctrl[G233_PWM_NUM_CH];
    uint32_t period[G233_PWM_NUM_CH];
    uint32_t duty[G233_PWM_NUM_CH];
    uint32_t cnt[G233_PWM_NUM_CH];
    uint32_t done_mask; /* bits 0..3 = CH0..CH3 DONE */
} G233PWMState;

static bool g233_pwm_channel_active(const G233PWMState *s, int ch)
{
    return (s->ctrl[ch] & PWM_CTRL_EN) && s->period[ch] != 0;
}

static void g233_pwm_update_irq(G233PWMState *s)
{
    bool pending = false;

    for (int ch = 0; ch < G233_PWM_NUM_CH; ch++) {
        if ((s->ctrl[ch] & PWM_CTRL_INTIE) &&
            (s->done_mask & (1u << ch))) {
            pending = true;
            break;
        }
    }
    qemu_set_irq(s->irq, pending ? 1 : 0);
}

static void g233_pwm_process_time(G233PWMState *s, int64_t now)
{
    int64_t delta = now - s->last_ns;

    if (delta <= 0) {
        return;
    }
    s->last_ns = now;

    for (int ch = 0; ch < G233_PWM_NUM_CH; ch++) {
        if (!g233_pwm_channel_active(s, ch)) {
            continue;
        }

        uint64_t mod = (uint64_t)s->period[ch] + 1;
        uint64_t c = s->cnt[ch];
        uint64_t new_c = c + (uint64_t)delta;
        uint64_t wraps = new_c / mod;

        s->cnt[ch] = (uint32_t)(new_c % mod);
        if (wraps) {
            s->done_mask |= (1u << ch);
        }
    }

    g233_pwm_update_irq(s);
}

static void g233_pwm_sync(G233PWMState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    g233_pwm_process_time(s, now);
}

static void g233_pwm_reschedule(G233PWMState *s)
{
    int64_t earliest = INT64_MAX;

    for (int ch = 0; ch < G233_PWM_NUM_CH; ch++) {
        if (!g233_pwm_channel_active(s, ch)) {
            continue;
        }
        /* Skip channels that already have DONE set — no need to fire again */
        if (s->done_mask & (1u << ch)) {
            continue;
        }
        /* 1 tick == 1 ns in this model; remaining = ticks until next period wrap */
        uint64_t mod = (uint64_t)s->period[ch] + 1;
        uint64_t remaining = mod - s->cnt[ch];
        int64_t fire_at = s->last_ns + (int64_t)remaining;
        if (fire_at < earliest) {
            earliest = fire_at;
        }
    }

    if (earliest == INT64_MAX) {
        timer_del(&s->timer);
    } else {
        timer_mod_ns(&s->timer, earliest);
    }
}

static void g233_pwm_timer_cb(void *opaque)
{
    G233PWMState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    g233_pwm_process_time(s, now);
    g233_pwm_reschedule(s);
}

static uint32_t g233_pwm_read_glb(const G233PWMState *s)
{
    uint32_t en_mirror = 0;

    for (int ch = 0; ch < G233_PWM_NUM_CH; ch++) {
        if (s->ctrl[ch] & PWM_CTRL_EN) {
            en_mirror |= (1u << ch);
        }
    }
    return en_mirror | (s->done_mask << 4);
}

static uint64_t g233_pwm_read(void *opaque, hwaddr offset, unsigned size)
{
    G233PWMState *s = G233_PWM(opaque);
    uint32_t value = 0;

    g233_pwm_sync(s);

    if (offset == PWM_OFF_GLB) {
        value = g233_pwm_read_glb(s);
    } else if (offset >= PWM_OFF_CH_BASE &&
               offset < PWM_OFF_CH_BASE + G233_PWM_NUM_CH * PWM_CH_STRIDE) {
        unsigned ch = (offset - PWM_OFF_CH_BASE) / PWM_CH_STRIDE;
        unsigned reg = offset % PWM_CH_STRIDE;

        if (ch >= G233_PWM_NUM_CH) {
            goto bad;
        }
        switch (reg) {
        case PWM_OFF_CTRL:
            value = s->ctrl[ch];
            break;
        case PWM_OFF_PERIOD:
            value = s->period[ch];
            break;
        case PWM_OFF_DUTY:
            value = s->duty[ch];
            break;
        case PWM_OFF_CNT:
            value = s->cnt[ch];
            break;
        default:
            goto bad;
        }
    } else {
        goto bad;
    }

    trace_g233_pwm_read(offset, value);
    return value;

bad:
    qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                  __func__, offset);
    return 0;
}

static void g233_pwm_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    G233PWMState *s = G233_PWM(opaque);

    trace_g233_pwm_write(offset, value);

    g233_pwm_sync(s);

    if (offset == PWM_OFF_GLB) {
        /* W1C on CHn_DONE in bits 7:4 */
        uint32_t clr = (value >> 4) & 0xfu;

        s->done_mask &= ~clr;
        g233_pwm_update_irq(s);
        g233_pwm_reschedule(s);
        return;
    }

    if (offset >= PWM_OFF_CH_BASE &&
        offset < PWM_OFF_CH_BASE + G233_PWM_NUM_CH * PWM_CH_STRIDE) {
        unsigned ch = (offset - PWM_OFF_CH_BASE) / PWM_CH_STRIDE;
        unsigned reg = offset % PWM_CH_STRIDE;

        if (ch >= G233_PWM_NUM_CH) {
            goto bad;
        }

        switch (reg) {
        case PWM_OFF_CTRL:
            s->ctrl[ch] = (uint32_t)value & PWM_CTRL_MASK;
            break;
        case PWM_OFF_PERIOD:
            s->period[ch] = (uint32_t)value;
            break;
        case PWM_OFF_DUTY:
            s->duty[ch] = (uint32_t)value;
            break;
        case PWM_OFF_CNT:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: write to read-only CNT ch%u\n", __func__, ch);
            return;
        default:
            goto bad;
        }

        g233_pwm_reschedule(s);
        return;
    }

bad:
    qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                  __func__, offset);
}

static const MemoryRegionOps g233_pwm_ops = {
    .read = g233_pwm_read,
    .write = g233_pwm_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void g233_pwm_reset(DeviceState *dev)
{
    G233PWMState *s = G233_PWM(dev);

    timer_del(&s->timer);
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (int ch = 0; ch < G233_PWM_NUM_CH; ch++) {
        s->ctrl[ch] = 0;
        s->period[ch] = 0;
        s->duty[ch] = 0;
        s->cnt[ch] = 0;
    }
    s->done_mask = 0;
    qemu_set_irq(s->irq, 0);
}

static void g233_pwm_realize(DeviceState *dev, Error **errp)
{
    G233PWMState *s = G233_PWM(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &g233_pwm_ops, s,
                          TYPE_G233_PWM, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, g233_pwm_timer_cb, s);
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static const VMStateDescription vmstate_g233_pwm = {
    .name = TYPE_G233_PWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_INT64(last_ns, G233PWMState),
        VMSTATE_UINT32_ARRAY(ctrl, G233PWMState, G233_PWM_NUM_CH),
        VMSTATE_UINT32_ARRAY(period, G233PWMState, G233_PWM_NUM_CH),
        VMSTATE_UINT32_ARRAY(duty, G233PWMState, G233_PWM_NUM_CH),
        VMSTATE_UINT32_ARRAY(cnt, G233PWMState, G233_PWM_NUM_CH),
        VMSTATE_UINT32(done_mask, G233PWMState),
        VMSTATE_TIMER(timer, G233PWMState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = g233_pwm_realize;
    device_class_set_legacy_reset(dc, g233_pwm_reset);
    dc->vmsd = &vmstate_g233_pwm;
}

static const TypeInfo g233_pwm_info = {
    .name          = TYPE_G233_PWM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233PWMState),
    .class_init    = g233_pwm_class_init,
};

static void g233_pwm_register_types(void)
{
    type_register_static(&g233_pwm_info);
}

type_init(g233_pwm_register_types)
