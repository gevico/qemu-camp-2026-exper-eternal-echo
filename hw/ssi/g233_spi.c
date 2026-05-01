/*
 * QEMU RISC-V G233 SPI controller
 *
 * Register map (base 0x10018000, PLIC IRQ 5):
 *   0x00  SPI_CR1  — bit0: SPE, bit2: MSTR, bit5: ERRIE,
 *                    bit6: RXNEIE, bit7: TXEIE
 *   0x04  SPI_CR2  — bits[1:0]: CS select (0=W25X16, 1=W25X32)
 *   0x08  SPI_SR   — bit0: RXNE, bit1: TXE, bit4: OVERRUN (W1C)
 *   0x0C  SPI_DR   — write triggers transfer; read returns RX byte
 *
 * Two SPI flash devices on internal SSI bus:
 *   CS0: w25x16 (2 MB, JEDEC 0xEF3015)
 *   CS1: w25x32 (4 MB, JEDEC 0xEF3016)
 *
 * Copyright (c) 2026 G233 QEMU Camp
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_G233_SPI "g233.spi"
#define G233_SPI(obj) OBJECT_CHECK(G233SPIState, (obj), TYPE_G233_SPI)

#define G233_SPI_NUM_CS 2

/* Register offsets */
#define SPI_CR1  0x00
#define SPI_CR2  0x04
#define SPI_SR   0x08
#define SPI_DR   0x0C

/* CR1 bits */
#define SPI_CR1_SPE     (1u << 0)
#define SPI_CR1_MSTR    (1u << 2)
#define SPI_CR1_ERRIE   (1u << 5)
#define SPI_CR1_RXNEIE  (1u << 6)
#define SPI_CR1_TXEIE   (1u << 7)
#define SPI_CR1_MASK    (SPI_CR1_SPE | SPI_CR1_MSTR | \
                         SPI_CR1_ERRIE | SPI_CR1_RXNEIE | SPI_CR1_TXEIE)

/* SR bits */
#define SPI_SR_RXNE     (1u << 0)
#define SPI_SR_TXE      (1u << 1)
#define SPI_SR_OVERRUN  (1u << 4)

/* Sentinel: cr2 == G233_SPI_NO_CS means no chip is currently asserted */
#define G233_SPI_NO_CS  0xFF

typedef struct G233SPIState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    SSIBus *bus;
    qemu_irq cs_lines[G233_SPI_NUM_CS];

    uint32_t cr1;
    uint32_t cr2;   /* current CS selection, or G233_SPI_NO_CS */
    uint32_t sr;    /* RXNE | TXE | OVERRUN */
    uint32_t dr;    /* RX data buffer */
} G233SPIState;

/* ------------------------------------------------------------------ */

static void g233_spi_update_irq(G233SPIState *s)
{
    bool pending =
        ((s->cr1 & SPI_CR1_TXEIE)  && (s->sr & SPI_SR_TXE))    ||
        ((s->cr1 & SPI_CR1_RXNEIE) && (s->sr & SPI_SR_RXNE))   ||
        ((s->cr1 & SPI_CR1_ERRIE)  && (s->sr & SPI_SR_OVERRUN));
    qemu_set_irq(s->irq, pending ? 1 : 0);
}

/*
 * Assert the new CS chip, de-asserting the old one.
 * Only acts when the selection actually changes.
 */
static void g233_spi_set_cs(G233SPIState *s, uint32_t new_cs)
{
    if (new_cs == s->cr2) {
        return;  /* same chip — no CS edge needed */
    }
    /* De-assert old CS (high = inactive for SSI_CS_LOW devices) */
    if (s->cr2 < G233_SPI_NUM_CS) {
        qemu_irq_raise(s->cs_lines[s->cr2]);
    }
    s->cr2 = new_cs;
    /* Assert new CS (low = active) */
    if (s->cr2 < G233_SPI_NUM_CS) {
        qemu_irq_lower(s->cs_lines[s->cr2]);
    }
}

/* ------------------------------------------------------------------ */
/* MMIO                                                                 */

static uint64_t g233_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    G233SPIState *s = G233_SPI(opaque);
    uint32_t value = 0;

    switch (offset) {
    case SPI_CR1:
        value = s->cr1;
        break;
    case SPI_CR2:
        value = (s->cr2 < G233_SPI_NUM_CS) ? s->cr2 : 0;
        break;
    case SPI_SR:
        value = s->sr;
        break;
    case SPI_DR:
        value = s->dr;
        s->sr &= ~SPI_SR_RXNE;
        g233_spi_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }

    trace_g233_spi_read(offset, value);
    return value;
}

static void g233_spi_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    G233SPIState *s = G233_SPI(opaque);

    trace_g233_spi_write(offset, value);

    switch (offset) {
    case SPI_CR1:
        s->cr1 = (uint32_t)value & SPI_CR1_MASK;
        g233_spi_update_irq(s);
        break;

    case SPI_CR2:
        g233_spi_set_cs(s, (uint32_t)value & 0x3);
        break;

    case SPI_SR:
        /* W1C for OVERRUN; RXNE and TXE are hardware-controlled */
        s->sr &= ~((uint32_t)value & SPI_SR_OVERRUN);
        g233_spi_update_irq(s);
        break;

    case SPI_DR: {
        bool was_rxne = !!(s->sr & SPI_SR_RXNE);
        uint8_t rx = (uint8_t)ssi_transfer(s->bus, (uint8_t)value);
        s->sr |= SPI_SR_TXE;   /* Transfer complete: TX empty again */
        if (was_rxne) {
            /* RX buffer still full — overrun: new byte is discarded */
            s->sr |= SPI_SR_OVERRUN;
        } else {
            s->dr = rx;
            s->sr |= SPI_SR_RXNE;
        }
        g233_spi_update_irq(s);
        break;
    }

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }
}

static const MemoryRegionOps g233_spi_ops = {
    .read = g233_spi_read,
    .write = g233_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */

static void g233_spi_reset(DeviceState *dev)
{
    G233SPIState *s = G233_SPI(dev);

    /* De-assert all CS lines */
    for (int i = 0; i < G233_SPI_NUM_CS; i++) {
        qemu_irq_raise(s->cs_lines[i]);
    }

    s->cr1 = 0;
    s->cr2 = G233_SPI_NO_CS;  /* No chip selected */
    s->sr  = SPI_SR_TXE;      /* TX buffer starts empty */
    s->dr  = 0;

    qemu_set_irq(s->irq, 0);
}

static void g233_spi_realize(DeviceState *dev, Error **errp)
{
    G233SPIState *s = G233_SPI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    DeviceState *flash;

    memory_region_init_io(&s->iomem, OBJECT(s), &g233_spi_ops, s,
                          TYPE_G233_SPI, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->bus = ssi_create_bus(dev, "spi");

    /* CS0: W25X16 (2 MB, JEDEC 0xEF3015) — default cs_index=0 */
    flash = ssi_create_peripheral(s->bus, "w25x16");
    s->cs_lines[0] = qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0);

    /* CS1: W25X32 (4 MB, JEDEC 0xEF3016) — cs_index must be 1 */
    flash = qdev_new("w25x32");
    qdev_prop_set_uint8(flash, "cs", 1);
    qdev_realize_and_unref(flash, BUS(s->bus), &error_fatal);
    s->cs_lines[1] = qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0);

    /* De-assert both CS lines initially */
    qemu_irq_raise(s->cs_lines[0]);
    qemu_irq_raise(s->cs_lines[1]);
}

static const VMStateDescription vmstate_g233_spi = {
    .name = TYPE_G233_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(cr1, G233SPIState),
        VMSTATE_UINT32(cr2, G233SPIState),
        VMSTATE_UINT32(sr,  G233SPIState),
        VMSTATE_UINT32(dr,  G233SPIState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = g233_spi_realize;
    device_class_set_legacy_reset(dc, g233_spi_reset);
    dc->vmsd = &vmstate_g233_spi;
}

static const TypeInfo g233_spi_info = {
    .name          = TYPE_G233_SPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233SPIState),
    .class_init    = g233_spi_class_init,
};

static void g233_spi_register_types(void)
{
    type_register_static(&g233_spi_info);
}

type_init(g233_spi_register_types)
