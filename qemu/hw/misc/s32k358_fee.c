/*
 * NXP S32K358 Fee model (minimal register-backed implementation).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/s32k358_fee.h"

static uint64_t s32k358_fee_read(void *opaque, hwaddr addr, unsigned size)
{
    S32K358FeeState *s = S32K358_FEE(opaque);
    uint64_t value = 0;

    if (addr + size > S32K358_FEE_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-fee: invalid read addr=0x%" HWADDR_PRIx
                      " size=%u\n", addr, size);
        return 0;
    }

    for (unsigned i = 0; i < size; i++) {
        value |= ((uint64_t)s->regs[addr + i] << (8U * i));
    }

    return value;
}

static void s32k358_fee_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    S32K358FeeState *s = S32K358_FEE(opaque);

    if (addr + size > S32K358_FEE_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-fee: invalid write addr=0x%" HWADDR_PRIx
                      " size=%u val=0x%" PRIx64 "\n",
                      addr, size, value);
        return;
    }

    for (unsigned i = 0; i < size; i++) {
        s->regs[addr + i] = (uint8_t)(value >> (8U * i));
    }
}

static const MemoryRegionOps s32k358_fee_ops = {
    .read = s32k358_fee_read,
    .write = s32k358_fee_write,
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

static void s32k358_fee_reset(DeviceState *dev)
{
    S32K358FeeState *s = S32K358_FEE(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void s32k358_fee_init(Object *obj)
{
    S32K358FeeState *s = S32K358_FEE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &s32k358_fee_ops, s,
                          TYPE_S32K358_FEE, S32K358_FEE_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
}

static void s32k358_fee_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S32K358 Fee";
    device_class_set_legacy_reset(dc, s32k358_fee_reset);
}

static const TypeInfo s32k358_fee_info = {
    .name = TYPE_S32K358_FEE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S32K358FeeState),
    .instance_init = s32k358_fee_init,
    .class_init = s32k358_fee_class_init,
};

static void s32k358_fee_register_types(void)
{
    type_register_static(&s32k358_fee_info);
}

type_init(s32k358_fee_register_types)
