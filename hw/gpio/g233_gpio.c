/*
 * QEMU RISC-V G233 GPIO Controller
 *
 * Copyright (c) 2026 G233 QEMU Camp
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_G233_GPIO "g233.gpio"
#define G233_GPIO(obj) OBJECT_CHECK(G233GPIOState, (obj), TYPE_G233_GPIO)

/* GPIO registers */
#define GPIO_DIR    0x00  /* Direction register */
#define GPIO_OUT    0x04  /* Output data register */
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

    uint32_t dir;      /* Direction register */
    uint32_t out;      /* Output register */
    uint32_t last_level; /* Previous output level for edge detection */
    uint32_t ie;       /* Interrupt enable */
    uint32_t is;       /* Interrupt status */
    uint32_t trig;     /* Trigger type */
    uint32_t pol;      /* Polarity */
    bool irq_cleared;   /* Flag to prevent re-triggering after IS clear */
} G233GPIOState;

static void gpio_update_interrupt(G233GPIOState *s);

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
        /* In output mode, read back the output latched value */
        value = s->out;
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
        s->dir = value;
        gpio_update_interrupt(s);
        break;
    case GPIO_OUT:
        s->out = value;
        gpio_update_interrupt(s);
        break;
    case GPIO_IN:
        /* Read-only register */
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only register 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        break;
    case GPIO_IE:
        s->ie = value;
        gpio_update_interrupt(s);
        break;
    case GPIO_IS:
        /* Write-1-to-clear */
        s->is &= ~value;
        /* If all interrupts are cleared, lower IRQ line and prevent re-trigger */
        if (s->is == 0) {
            qemu_set_irq(s->irq, 0);
            s->irq_cleared = true;
        }
        break;
    case GPIO_TRIG:
        s->trig = value;
        gpio_update_interrupt(s);
        break;
    case GPIO_POL:
        s->pol = value;
        gpio_update_interrupt(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        break;
    }
}

static void gpio_update_interrupt(G233GPIOState *s)
{
    uint32_t old_level = s->last_level;
    uint32_t new_level = s->out;
    uint32_t changes = old_level ^ new_level;

    /* If we just cleared interrupts, don't re-trigger immediately.
     * Update last_level to current level to prevent edge re-trigger. */
    if (s->irq_cleared) {
        s->irq_cleared = false;
        s->last_level = new_level;  /* Sync to current level */
        qemu_set_irq(s->irq, 0);    /* Ensure IRQ stays low */
        return;
    }

    for (int i = 0; i < 32; i++) {
        /* Skip if interrupt not enabled for this pin */
        if (!((s->ie >> i) & 1)) {
            continue;
        }

        /* Skip if pin is not configured as output */
        if (!((s->dir >> i) & 1)) {
            continue;
        }

        bool pin_changed = (changes >> i) & 1;
        bool pin_high = (new_level >> i) & 1;
        bool pin_low = !pin_high;
        bool edge_triggered = !((s->trig >> i) & 1);
        bool level_triggered = (s->trig >> i) & 1;
        bool rising_polarity = (s->pol >> i) & 1;
        bool falling_polarity = !rising_polarity;

        /* Clear the interrupt status for this pin */
        s->is &= ~(1 << i);

        /* Edge-triggered mode */
        if (edge_triggered && pin_changed) {
            if (rising_polarity && pin_high) {
                s->is |= (1 << i);  /* Rising edge detected */
            } else if (falling_polarity && pin_low) {
                s->is |= (1 << i);  /* Falling edge detected */
            }
        }

        /* Level-triggered mode */
        if (level_triggered) {
            if (rising_polarity && pin_high) {
                s->is |= (1 << i);  /* High level */
            } else if (falling_polarity && pin_low) {
                s->is |= (1 << i);  /* Low level */
            }
        }
    }

    /* Update last level */
    s->last_level = new_level;

    /* Trigger PLIC interrupt if any interrupt status is set */
    qemu_set_irq(s->irq, (s->is != 0) ? 1 : 0);
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
    s->last_level = 0;
    s->ie = 0;
    s->is = 0;
    s->trig = 0;
    s->pol = 0;
    s->irq_cleared = false;
}

static void g233_gpio_realize(DeviceState *dev, Error **errp)
{
    G233GPIOState *s = G233_GPIO(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &g233_gpio_ops, s,
                          TYPE_G233_GPIO, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_g233_gpio = {
    .name = TYPE_G233_GPIO,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(dir, G233GPIOState),
        VMSTATE_UINT32(out, G233GPIOState),
        VMSTATE_UINT32(last_level, G233GPIOState),
        VMSTATE_UINT32(ie, G233GPIOState),
        VMSTATE_UINT32(is, G233GPIOState),
        VMSTATE_UINT32(trig, G233GPIOState),
        VMSTATE_UINT32(pol, G233GPIOState),
        VMSTATE_BOOL(irq_cleared, G233GPIOState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = g233_gpio_realize;
    device_class_set_legacy_reset(dc, g233_gpio_reset);
    dc->vmsd = &vmstate_g233_gpio;
}

static const TypeInfo g233_gpio_info = {
    .name          = TYPE_G233_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233GPIOState),
    .class_init    = g233_gpio_class_init,
};

static void g233_gpio_register_types(void)
{
    type_register_static(&g233_gpio_info);
}

type_init(g233_gpio_register_types)