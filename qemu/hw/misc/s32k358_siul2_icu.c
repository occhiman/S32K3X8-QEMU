/*
 * NXP S32K358 SIUL2 ICU model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/misc/s32k358_siul2_icu.h"

#define SIUL2_DISR0_OFF   0x10U
#define SIUL2_DIRER0_OFF  0x18U
#define SIUL2_DIRSR0_OFF  0x20U
#define SIUL2_IREER0_OFF  0x28U
#define SIUL2_IFEER0_OFF  0x30U
#define SIUL2_IFER0_OFF   0x38U
#define SIUL2_IFMCR_BASE  0x40U
#define SIUL2_IFCPR_OFF   0xC0U

#define SIUL2_IRQ_GROUP_WIDTH 8U
#define SIUL2_FILTER_TICK_NS  1000ULL

static uint32_t s32k358_siul2_icu_load_u32(const uint8_t *buf, hwaddr off)
{
    return (uint32_t)buf[off + 0] |
           ((uint32_t)buf[off + 1] << 8U) |
           ((uint32_t)buf[off + 2] << 16U) |
           ((uint32_t)buf[off + 3] << 24U);
}

static void s32k358_siul2_icu_store_u32(uint8_t *buf, hwaddr off, uint32_t val)
{
    buf[off + 0] = (uint8_t)(val & 0xffU);
    buf[off + 1] = (uint8_t)((val >> 8U) & 0xffU);
    buf[off + 2] = (uint8_t)((val >> 16U) & 0xffU);
    buf[off + 3] = (uint8_t)((val >> 24U) & 0xffU);
}

static bool s32k358_siul2_icu_is_ifmcr_byte(hwaddr off)
{
    return off >= SIUL2_IFMCR_BASE && off < (SIUL2_IFCPR_OFF);
}

static bool s32k358_siul2_icu_is_valid_byte(hwaddr off)
{
    return (off >= SIUL2_DISR0_OFF && off < (SIUL2_DISR0_OFF + 4U)) ||
           (off >= SIUL2_DIRER0_OFF && off < (SIUL2_DIRER0_OFF + 4U)) ||
           (off >= SIUL2_DIRSR0_OFF && off < (SIUL2_DIRSR0_OFF + 4U)) ||
           (off >= SIUL2_IREER0_OFF && off < (SIUL2_IREER0_OFF + 4U)) ||
           (off >= SIUL2_IFEER0_OFF && off < (SIUL2_IFEER0_OFF + 4U)) ||
           (off >= SIUL2_IFER0_OFF && off < (SIUL2_IFER0_OFF + 4U)) ||
           s32k358_siul2_icu_is_ifmcr_byte(off) ||
           (off >= SIUL2_IFCPR_OFF && off < (SIUL2_IFCPR_OFF + 4U));
}

static void s32k358_siul2_icu_update_irqs(S32K358SIUL2ICUState *s)
{
    uint32_t disr = s32k358_siul2_icu_load_u32(s->regs, SIUL2_DISR0_OFF);
    uint32_t direr = s32k358_siul2_icu_load_u32(s->regs, SIUL2_DIRER0_OFF);
    uint32_t active = disr & direr;

    for (unsigned i = 0; i < S32K358_SIUL2_ICU_IRQ_LINES; i++) {
        uint32_t group_mask = (uint32_t)0xffU << (i * SIUL2_IRQ_GROUP_WIDTH);
        qemu_set_irq(s->irq[i], (active & group_mask) != 0U);
    }
}

static bool s32k358_siul2_icu_filter_allows_event(S32K358SIUL2ICUState *s,
                                                   unsigned index)
{
    uint32_t ifer = s32k358_siul2_icu_load_u32(s->regs, SIUL2_IFER0_OFF);
    hwaddr ifmcr_off = SIUL2_IFMCR_BASE + (4U * index);
    uint32_t ifmcr = s32k358_siul2_icu_load_u32(s->regs, ifmcr_off);
    uint32_t ifcpr = s32k358_siul2_icu_load_u32(s->regs, SIUL2_IFCPR_OFF);
    uint64_t bit = 1ULL << index;

    if ((ifer & bit) == 0U || ifmcr == 0U) {
        return true;
    }

    /*
     * The real filter clocks from SIUL2 clock domains. We approximate filter
     * timing with a virtual nanosecond threshold using IFMCR and IFCPR.
     */
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t min_delta_ns = ((uint64_t)ifmcr + 1U) * ((uint64_t)ifcpr + 1U) *
                            SIUL2_FILTER_TICK_NS;

    if (s->last_event_ns[index] != 0 &&
        (uint64_t)(now_ns - s->last_event_ns[index]) < min_delta_ns) {
        return false;
    }

    s->last_event_ns[index] = now_ns;
    return true;
}

static void s32k358_siul2_icu_handle_eirq(void *opaque, int n, int level)
{
    S32K358SIUL2ICUState *s = S32K358_SIUL2_ICU(opaque);
    uint8_t prev_level;
    uint32_t iree, ifee, disr;
    uint32_t bit;

    if (n < 0 || n >= S32K358_SIUL2_EIRQ_COUNT) {
        return;
    }

    prev_level = s->eirq_level[n];
    level = !!level;
    if (prev_level == level) {
        return;
    }

    s->eirq_level[n] = (uint8_t)level;
    if (!s32k358_siul2_icu_filter_allows_event(s, (unsigned)n)) {
        return;
    }

    iree = s32k358_siul2_icu_load_u32(s->regs, SIUL2_IREER0_OFF);
    ifee = s32k358_siul2_icu_load_u32(s->regs, SIUL2_IFEER0_OFF);
    disr = s32k358_siul2_icu_load_u32(s->regs, SIUL2_DISR0_OFF);
    bit = 1U << n;

    if ((!prev_level && level && (iree & bit) != 0U) ||
        (prev_level && !level && (ifee & bit) != 0U)) {
        disr |= bit;
        s32k358_siul2_icu_store_u32(s->regs, SIUL2_DISR0_OFF, disr);
        s32k358_siul2_icu_update_irqs(s);
    }
}

static uint64_t s32k358_siul2_icu_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    S32K358SIUL2ICUState *s = S32K358_SIUL2_ICU(opaque);
    uint64_t value = 0;

    if (addr + size > S32K358_SIUL2_ICU_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-siul2-icu: invalid read addr=0x%" HWADDR_PRIx
                      " size=%u\n", addr, size);
        return 0;
    }

    for (unsigned i = 0; i < size; i++) {
        hwaddr off = addr + i;
        if (!s32k358_siul2_icu_is_valid_byte(off)) {
            continue;
        }
        value |= ((uint64_t)s->regs[off] << (8U * i));
    }

    return value;
}

static void s32k358_siul2_icu_write(void *opaque, hwaddr addr, uint64_t value,
                                    unsigned size)
{
    S32K358SIUL2ICUState *s = S32K358_SIUL2_ICU(opaque);

    if (addr + size > S32K358_SIUL2_ICU_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-siul2-icu: invalid write addr=0x%" HWADDR_PRIx
                      " size=%u val=0x%" PRIx64 "\n",
                      addr, size, value);
        return;
    }

    for (unsigned i = 0; i < size; i++) {
        hwaddr off = addr + i;
        uint8_t byte = (uint8_t)(value >> (8U * i));

        if (!s32k358_siul2_icu_is_valid_byte(off)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "s32k358-siul2-icu: write to reserved offset "
                          "0x%" HWADDR_PRIx "\n", off);
            continue;
        }

        if (off >= SIUL2_DISR0_OFF && off < (SIUL2_DISR0_OFF + 4U)) {
            /* DISR0 flags are W1C. */
            s->regs[off] &= (uint8_t)~byte;
        } else {
            s->regs[off] = byte;
        }
    }

    s32k358_siul2_icu_update_irqs(s);
}

static const MemoryRegionOps s32k358_siul2_icu_ops = {
    .read = s32k358_siul2_icu_read,
    .write = s32k358_siul2_icu_write,
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

static void s32k358_siul2_icu_reset(DeviceState *dev)
{
    S32K358SIUL2ICUState *s = S32K358_SIUL2_ICU(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->eirq_level, 0, sizeof(s->eirq_level));
    memset(s->last_event_ns, 0, sizeof(s->last_event_ns));
    s32k358_siul2_icu_update_irqs(s);
}

static void s32k358_siul2_icu_init(Object *obj)
{
    S32K358SIUL2ICUState *s = S32K358_SIUL2_ICU(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &s32k358_siul2_icu_ops, s,
                          TYPE_S32K358_SIUL2_ICU, S32K358_SIUL2_ICU_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);

    for (unsigned i = 0; i < S32K358_SIUL2_ICU_IRQ_LINES; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }

    qdev_init_gpio_in(DEVICE(obj), s32k358_siul2_icu_handle_eirq,
                      S32K358_SIUL2_EIRQ_COUNT);
}

static void s32k358_siul2_icu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S32K358 SIUL2 ICU";
    device_class_set_legacy_reset(dc, s32k358_siul2_icu_reset);
}

static const TypeInfo s32k358_siul2_icu_info = {
    .name = TYPE_S32K358_SIUL2_ICU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S32K358SIUL2ICUState),
    .instance_init = s32k358_siul2_icu_init,
    .class_init = s32k358_siul2_icu_class_init,
};

static void s32k358_siul2_icu_register_types(void)
{
    type_register_static(&s32k358_siul2_icu_info);
}

type_init(s32k358_siul2_icu_register_types)

