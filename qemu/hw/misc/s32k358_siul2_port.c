/*
 * NXP S32K358 SIUL2 Port model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/s32k358_siul2_port.h"

#define SIUL2_PGPDO_BASE         0x1700U
#define SIUL2_PGPDI_BASE         0x1740U
#define SIUL2_MPGPDO_BASE        0x1780U
#define SIUL2_GPIO_BANK_COUNT    32U

static uint16_t s32k358_siul2_port_load_u16(const uint8_t *buf, hwaddr off)
{
    return (uint16_t)buf[off + 0] | ((uint16_t)buf[off + 1] << 8U);
}

static void s32k358_siul2_port_store_u16(uint8_t *buf, hwaddr off, uint16_t val)
{
    buf[off + 0] = (uint8_t)(val & 0xffU);
    buf[off + 1] = (uint8_t)((val >> 8U) & 0xffU);
}

static void s32k358_siul2_port_sync_gpio_bank(S32K358SIUL2PortState *s,
                                               unsigned bank)
{
    hwaddr pgpdo_off = SIUL2_PGPDO_BASE + (2U * bank);
    hwaddr pgpdi_off = SIUL2_PGPDI_BASE + (2U * bank);

    s32k358_siul2_port_store_u16(s->regs, pgpdo_off, s->pgpdo[bank]);
    /*
     * Without external pad wiring, feed input view from output latch so DIO
     * readback remains deterministic.
     */
    s32k358_siul2_port_store_u16(s->regs, pgpdi_off, s->pgpdo[bank]);
}

static bool s32k358_siul2_port_is_pgpdi(hwaddr off)
{
    return off >= SIUL2_PGPDI_BASE &&
           off < (SIUL2_PGPDI_BASE + (2U * SIUL2_GPIO_BANK_COUNT));
}

static bool s32k358_siul2_port_is_mpgpdo_word(hwaddr off)
{
    return off >= SIUL2_MPGPDO_BASE &&
           off < (SIUL2_MPGPDO_BASE + (4U * SIUL2_GPIO_BANK_COUNT)) &&
           ((off & 0x3U) == 0U);
}

static void s32k358_siul2_port_update_pgpdo_from_bytes(S32K358SIUL2PortState *s,
                                                       hwaddr addr, unsigned size)
{
    hwaddr end = addr + size - 1U;

    for (unsigned bank = 0; bank < SIUL2_GPIO_BANK_COUNT; bank++) {
        hwaddr reg_start = SIUL2_PGPDO_BASE + (2U * bank);
        hwaddr reg_end = reg_start + 1U;

        if (reg_start > end || reg_end < addr) {
            continue;
        }

        s->pgpdo[bank] = s32k358_siul2_port_load_u16(s->regs, reg_start);
        s32k358_siul2_port_sync_gpio_bank(s, bank);
    }
}

static void s32k358_siul2_port_apply_mpgpdo(S32K358SIUL2PortState *s,
                                            hwaddr off, uint32_t val)
{
    unsigned bank = (unsigned)((off - SIUL2_MPGPDO_BASE) / 4U);
    uint16_t data = (uint16_t)(val & 0xffffU);
    uint16_t mask = (uint16_t)((val >> 16U) & 0xffffU);
    uint16_t next = s->pgpdo[bank];

    next = (uint16_t)((next & (uint16_t)~mask) | (data & mask));
    s->pgpdo[bank] = next;
    s32k358_siul2_port_sync_gpio_bank(s, bank);
}

static uint64_t s32k358_siul2_port_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    S32K358SIUL2PortState *s = S32K358_SIUL2_PORT(opaque);
    uint64_t value = 0;

    if (addr + size > S32K358_SIUL2_PORT_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-siul2-port: invalid read addr=0x%" HWADDR_PRIx
                      " size=%u\n", addr, size);
        return 0;
    }

    /* Keep PGPDI coherent with current output latch state. */
    for (unsigned bank = 0; bank < SIUL2_GPIO_BANK_COUNT; bank++) {
        s32k358_siul2_port_sync_gpio_bank(s, bank);
    }

    for (unsigned i = 0; i < size; i++) {
        value |= ((uint64_t)s->regs[addr + i] << (8U * i));
    }

    return value;
}

static void s32k358_siul2_port_write(void *opaque, hwaddr addr, uint64_t value,
                                     unsigned size)
{
    S32K358SIUL2PortState *s = S32K358_SIUL2_PORT(opaque);

    if (addr + size > S32K358_SIUL2_PORT_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-siul2-port: invalid write addr=0x%" HWADDR_PRIx
                      " size=%u val=0x%" PRIx64 "\n",
                      addr, size, value);
        return;
    }

    /*
     * MPGPDO is mask+data GPIO update in one 32-bit access.
     * We still retain the last written value in the backing byte array.
     */
    if (size == 4U && s32k358_siul2_port_is_mpgpdo_word(addr)) {
        uint32_t val = (uint32_t)value;
        unsigned bank = (unsigned)((addr - SIUL2_MPGPDO_BASE) / 4U);

        s->regs[addr + 0] = (uint8_t)(val & 0xffU);
        s->regs[addr + 1] = (uint8_t)((val >> 8U) & 0xffU);
        s->regs[addr + 2] = (uint8_t)((val >> 16U) & 0xffU);
        s->regs[addr + 3] = (uint8_t)((val >> 24U) & 0xffU);
        if (bank < SIUL2_GPIO_BANK_COUNT) {
            s32k358_siul2_port_apply_mpgpdo(s, addr, val);
        }
        return;
    }

    for (unsigned i = 0; i < size; i++) {
        hwaddr off = addr + i;

        if (s32k358_siul2_port_is_pgpdi(off)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "s32k358-siul2-port: write ignored on read-only "
                          "PGPDI offset 0x%" HWADDR_PRIx "\n", off);
            continue;
        }
        s->regs[off] = (uint8_t)(value >> (8U * i));
    }

    s32k358_siul2_port_update_pgpdo_from_bytes(s, addr, size);
}

static const MemoryRegionOps s32k358_siul2_port_ops = {
    .read = s32k358_siul2_port_read,
    .write = s32k358_siul2_port_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void s32k358_siul2_port_reset(DeviceState *dev)
{
    S32K358SIUL2PortState *s = S32K358_SIUL2_PORT(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->pgpdo, 0, sizeof(s->pgpdo));
    for (unsigned bank = 0; bank < SIUL2_GPIO_BANK_COUNT; bank++) {
        s32k358_siul2_port_sync_gpio_bank(s, bank);
    }
}

static void s32k358_siul2_port_init(Object *obj)
{
    S32K358SIUL2PortState *s = S32K358_SIUL2_PORT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &s32k358_siul2_port_ops, s,
                          TYPE_S32K358_SIUL2_PORT,
                          S32K358_SIUL2_PORT_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
}

static void s32k358_siul2_port_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S32K358 SIUL2 Port";
    device_class_set_legacy_reset(dc, s32k358_siul2_port_reset);
}

static const TypeInfo s32k358_siul2_port_info = {
    .name = TYPE_S32K358_SIUL2_PORT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S32K358SIUL2PortState),
    .instance_init = s32k358_siul2_port_init,
    .class_init = s32k358_siul2_port_class_init,
};

static void s32k358_siul2_port_register_types(void)
{
    type_register_static(&s32k358_siul2_port_info);
}

type_init(s32k358_siul2_port_register_types)
