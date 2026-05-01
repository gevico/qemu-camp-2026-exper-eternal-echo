/*
 * QEMU RISC-V G233 GPIO Controller
 *
 * Modeled after hw/gpio/nrf51_gpio.c: all MMIO writes only update shadow
 * registers, then g233_gpio_update_state() recomputes GPIO_IN readback,
 * gpio_out[] pad drivers, GPIO_IS (edge sticky / level live), and PLIC IRQ.
 *
 * gpio_in[] levels follow nRF semantics: -1 = floating (not externally driven),
 * 0/1 = driven. in_mask tracks which pins are driven from outside.
 *
 * Copyright (c) 2026 G233 QEMU Camp
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_G233_GPIO "g233.gpio"
#define G233_GPIO(obj) OBJECT_CHECK(G233GPIOState, (obj), TYPE_G233_GPIO)

#define G233_GPIO_LINES 32

/* GPIO registers */
#define GPIO_DIR    0x00  /* Direction register */
#define GPIO_OUT    0x04  /* Output register */
#define GPIO_IN     0x08  /* Input data register */
#define GPIO_IE     0x0C  /* Interrupt enable */
#define GPIO_IS     0x10  /* Interrupt status */
#define GPIO_TRIG   0x14  /* Trigger type */
#define GPIO_POL    0x18  /* Polarity */

typedef struct G233GPIOState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq output[G233_GPIO_LINES];

    /* MMIO shadow */
    uint32_t dir;
    uint32_t out;
    uint32_t ie;
    uint32_t is;
    uint32_t trig;
    uint32_t pol;

    /*
     * GPIO_IN readback (recomputed in update_state). Bits for output pins
     * mirror OUT; for input pins, externally driven level or 0 if floating.
     */
    uint32_t in;
    /* Per-pin: 1 if gpio_in line is driven (value >= 0), else floating */
    uint32_t in_mask;

    /* Previous pad levels for edge detect */
    uint32_t old_level;
    uint32_t old_out_drv;
    uint32_t old_out_lvl;
} G233GPIOState;

static void g233_gpio_update_state(G233GPIOState *s);

static void g233_gpio_set(void *opaque, int line, int value)
{
    G233GPIOState *s = G233_GPIO(opaque);

    assert(line >= 0 && line < G233_GPIO_LINES);

    s->in_mask = deposit32(s->in_mask, line, 1, value >= 0);
    if (value >= 0) {
        s->in = deposit32(s->in, line, 1, value != 0);
    }
    g233_gpio_update_state(s);
}

/*
 * Single nRF-style update: pad readback, gpio_out drivers, IS + IRQ.
 * Edge-triggered IS is sticky (only W1C clears); level IS follows pad level.
 */
static void g233_gpio_update_state(G233GPIOState *s)
{
    uint32_t new_in = 0;
    int i;

    /* Pass 1: GPIO_IN readback + pad output drivers */
    for (i = 0; i < G233_GPIO_LINES; i++) {
        bool dir_out = extract32(s->dir, i, 1);
        uint32_t in_bit;

        if (dir_out) {
            in_bit = extract32(s->out, i, 1);
        } else if (extract32(s->in_mask, i, 1)) {
            in_bit = extract32(s->in, i, 1);
        } else {
            in_bit = 0; /* floating input reads as 0 */
        }
        new_in = deposit32(new_in, i, 1, in_bit);

        /* gpio_out: drive when output, tri-state when input */
        {
            bool drive = dir_out;
            int level = drive ? (int)extract32(s->out, i, 1) : -1;
            bool old_drive = extract32(s->old_out_drv, i, 1);
            int old_lvl = extract32(s->old_out_lvl, i, 1);

            if (old_drive != drive || (drive && old_lvl != (level & 1))) {
                qemu_set_irq(s->output[i], level);
            }
            s->old_out_drv = deposit32(s->old_out_drv, i, 1, drive ? 1u : 0u);
            s->old_out_lvl = deposit32(s->old_out_lvl, i, 1,
                                       drive ? (uint32_t)(level & 1) : 0u);
        }
    }

    s->in = new_in;

    {
        uint32_t changes = s->old_level ^ new_in;

        for (i = 0; i < G233_GPIO_LINES; i++) {
            if (!extract32(s->ie, i, 1)) {
                continue;
            }

            bool edge_mode = !extract32(s->trig, i, 1);
            bool rising_pol = extract32(s->pol, i, 1);
            bool pin_changed = extract32(changes, i, 1);
            bool pin_high = extract32(new_in, i, 1);
            bool pin_low = !pin_high;

            if (edge_mode) {
                if (pin_changed) {
                    if (rising_pol && pin_high) {
                        s->is |= (1u << i);
                    } else if (!rising_pol && pin_low) {
                        s->is |= (1u << i);
                    }
                }
            } else {
                /* Level-triggered: live IS */
                s->is &= ~(1u << i);
                if (rising_pol && pin_high) {
                    s->is |= (1u << i);
                } else if (!rising_pol && pin_low) {
                    s->is |= (1u << i);
                }
            }
        }

        s->old_level = new_in;
    }

    qemu_set_irq(s->irq, s->is != 0);
}

static uint64_t g233_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    G233GPIOState *s = G233_GPIO(opaque);
    uint64_t value = 0;

    switch (offset) {
    case GPIO_DIR:
        value = s->dir;
        break;
    case GPIO_OUT:
        value = s->out;
        break;
    case GPIO_IN:
        value = s->in;
        break;
    case GPIO_IE:
        value = s->ie;
        break;
    case GPIO_IS:
        value = s->is;
        break;
    case GPIO_TRIG:
        value = s->trig;
        break;
    case GPIO_POL:
        value = s->pol;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }

    trace_g233_gpio_read(offset, value);
    return value;
}

static void g233_gpio_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    G233GPIOState *s = G233_GPIO(opaque);

    trace_g233_gpio_write(offset, value);

    switch (offset) {
    case GPIO_DIR:
        s->dir = (uint32_t)value;
        break;
    case GPIO_OUT:
        s->out = (uint32_t)value;
        break;
    case GPIO_IN:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only register 0x%"
                      HWADDR_PRIx "\n", __func__, offset);
        return;
    case GPIO_IE:
        s->ie = (uint32_t)value;
        break;
    case GPIO_IS:
        /* Write-1-to-clear */
        s->is &= ~(uint32_t)value;
        break;
    case GPIO_TRIG:
        s->trig = (uint32_t)value;
        break;
    case GPIO_POL:
        s->pol = (uint32_t)value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }

    g233_gpio_update_state(s);
}

static const MemoryRegionOps g233_gpio_ops = {
    .read = g233_gpio_read,
    .write = g233_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void g233_gpio_reset(DeviceState *dev)
{
    G233GPIOState *s = G233_GPIO(dev);

    s->dir = 0;
    s->out = 0;
    s->ie = 0;
    s->is = 0;
    s->trig = 0;
    s->pol = 0;
    s->in = 0;
    s->in_mask = 0;
    s->old_level = 0;
    s->old_out_drv = 0;
    s->old_out_lvl = 0;

    g233_gpio_update_state(s);
}

static void g233_gpio_init(Object *obj)
{
    G233GPIOState *s = G233_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &g233_gpio_ops, s,
                          TYPE_G233_GPIO, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    qdev_init_gpio_in(DEVICE(s), g233_gpio_set, G233_GPIO_LINES);
    qdev_init_gpio_out(DEVICE(s), s->output, G233_GPIO_LINES);
}

static const VMStateDescription vmstate_g233_gpio = {
    .name = TYPE_G233_GPIO,
    .version_id = 6,
    .minimum_version_id = 6,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(dir, G233GPIOState),
        VMSTATE_UINT32(out, G233GPIOState),
        VMSTATE_UINT32(in, G233GPIOState),
        VMSTATE_UINT32(in_mask, G233GPIOState),
        VMSTATE_UINT32(ie, G233GPIOState),
        VMSTATE_UINT32(is, G233GPIOState),
        VMSTATE_UINT32(trig, G233GPIOState),
        VMSTATE_UINT32(pol, G233GPIOState),
        VMSTATE_UINT32(old_level, G233GPIOState),
        VMSTATE_UINT32(old_out_drv, G233GPIOState),
        VMSTATE_UINT32(old_out_lvl, G233GPIOState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, g233_gpio_reset);
    dc->vmsd = &vmstate_g233_gpio;
}

static const TypeInfo g233_gpio_info = {
    .name          = TYPE_G233_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233GPIOState),
    .instance_init = g233_gpio_init,
    .class_init    = g233_gpio_class_init,
};

static void g233_gpio_register_types(void)
{
    type_register_static(&g233_gpio_info);
}

type_init(g233_gpio_register_types)
