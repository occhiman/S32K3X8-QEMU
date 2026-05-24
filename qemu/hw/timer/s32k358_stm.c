/*
 * NXP S32K358 STM model.
 *
 * Provides a register-compatible STM block for S32K3x8 firmware:
 * - CR/CNT + 4 channels (CCR/CIR/CMP) at RM offsets.
 * - Counter progression while CR.TEN=1 with CR.CPS prescaling.
 * - Compare-channel interrupt flagging and single IRQ line output.
 * - Register-protection window accesses in the same MMIO aperture
 *   (for SET_USER_ACCESS_ALLOWED(base + prot_mem * 0x900)).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/qdev-properties.h"
#include "hw/timer/s32k358_stm.h"

#define STM_CLOCK_HZ         80000000ULL

#define STM_CR_OFF           0x00U
#define STM_CNT_OFF          0x04U
#define STM_CH_BASE          0x10U
#define STM_CH_STRIDE        0x10U
#define STM_CCR_OFF          0x00U
#define STM_CIR_OFF          0x04U
#define STM_CMP_OFF          0x08U

#define STM_CR_TEN           (1U << 0)
#define STM_CR_FRZ           (1U << 1)
#define STM_CR_CPS_MASK      (0xFFU << 8)

#define STM_CCR_CEN          (1U << 0)
#define STM_CIR_CIF          (1U << 0)

static uint32_t s32k358_stm_read_raw32(const S32K358STMState *s, hwaddr off)
{
    return ((uint32_t)s->regs[off]) |
           ((uint32_t)s->regs[off + 1U] << 8) |
           ((uint32_t)s->regs[off + 2U] << 16) |
           ((uint32_t)s->regs[off + 3U] << 24);
}

static void s32k358_stm_write_raw32(S32K358STMState *s, hwaddr off,
                                    uint32_t value)
{
    s->regs[off] = (uint8_t)(value & 0xffU);
    s->regs[off + 1U] = (uint8_t)((value >> 8) & 0xffU);
    s->regs[off + 2U] = (uint8_t)((value >> 16) & 0xffU);
    s->regs[off + 3U] = (uint8_t)((value >> 24) & 0xffU);
}

static uint32_t s32k358_stm_divider(const S32K358STMState *s)
{
    return ((s->cr & STM_CR_CPS_MASK) >> 8) + 1U;
}

static bool s32k358_stm_counter_enabled(const S32K358STMState *s)
{
    return (s->cr & STM_CR_TEN) != 0U;
}

static uint32_t s32k358_stm_get_cnt(S32K358STMState *s)
{
    int64_t now_ns;
    int64_t delta_ns;
    uint64_t ticks;
    uint32_t div;

    if (!s32k358_stm_counter_enabled(s)) {
        return s->cnt_base;
    }

    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    delta_ns = now_ns - s->cnt_anchor_ns;
    if (delta_ns <= 0) {
        return s->cnt_base;
    }

    div = s32k358_stm_divider(s);
    ticks = muldiv64((uint64_t)delta_ns, (uint32_t)STM_CLOCK_HZ,
                     (uint32_t)NANOSECONDS_PER_SECOND);
    ticks /= div;

    return s->cnt_base + (uint32_t)ticks;
}

static void s32k358_stm_latch_counter(S32K358STMState *s)
{
    s->cnt_base = s32k358_stm_get_cnt(s);
    s->cnt_anchor_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void s32k358_stm_update_irq(S32K358STMState *s)
{
    bool level = false;

    for (unsigned ch = 0; ch < S32K358_STM_NUM_CHANNELS; ch++) {
        if ((s->ccr[ch] & STM_CCR_CEN) != 0U &&
            (s->cir[ch] & STM_CIR_CIF) != 0U) {
            level = true;
            break;
        }
    }

    qemu_set_irq(s->irq, level ? 1 : 0);
}

static void s32k358_stm_schedule_channel(S32K358STMState *s, unsigned ch)
{
    uint32_t cur_cnt;
    uint32_t diff_ticks;
    uint32_t div;
    uint64_t ns_delta;
    int64_t expire_ns;

    if (ch >= S32K358_STM_NUM_CHANNELS) {
        return;
    }

    timer_del(s->timer[ch]);

    if (!s32k358_stm_counter_enabled(s)) {
        return;
    }
    if ((s->ccr[ch] & STM_CCR_CEN) == 0U) {
        return;
    }
    if ((s->cir[ch] & STM_CIR_CIF) != 0U) {
        return;
    }

    cur_cnt = s32k358_stm_get_cnt(s);
    diff_ticks = s->cmp[ch] - cur_cnt;
    if (diff_ticks == 0U) {
        diff_ticks = 1U;
    }

    div = s32k358_stm_divider(s);
    ns_delta = muldiv64((uint64_t)diff_ticks * (uint64_t)div,
                        (uint32_t)NANOSECONDS_PER_SECOND,
                        (uint32_t)STM_CLOCK_HZ);
    if (ns_delta == 0U) {
        ns_delta = 1U;
    }

    expire_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (int64_t)ns_delta;
    timer_mod_ns(s->timer[ch], expire_ns);
}

static void s32k358_stm_schedule_all(S32K358STMState *s)
{
    for (unsigned ch = 0; ch < S32K358_STM_NUM_CHANNELS; ch++) {
        s32k358_stm_schedule_channel(s, ch);
    }
}

static void s32k358_stm_channel_expire(S32K358STMState *s, unsigned ch)
{
    if (ch >= S32K358_STM_NUM_CHANNELS) {
        return;
    }
    if (!s32k358_stm_counter_enabled(s) || (s->ccr[ch] & STM_CCR_CEN) == 0U) {
        return;
    }

    s->cir[ch] |= STM_CIR_CIF;
    s32k358_stm_write_raw32(s, STM_CH_BASE + (ch * STM_CH_STRIDE) + STM_CIR_OFF,
                            s->cir[ch]);
    s32k358_stm_update_irq(s);
}

static void s32k358_stm_timer0_cb(void *opaque)
{
    s32k358_stm_channel_expire(S32K358_STM(opaque), 0U);
}

static void s32k358_stm_timer1_cb(void *opaque)
{
    s32k358_stm_channel_expire(S32K358_STM(opaque), 1U);
}

static void s32k358_stm_timer2_cb(void *opaque)
{
    s32k358_stm_channel_expire(S32K358_STM(opaque), 2U);
}

static void s32k358_stm_timer3_cb(void *opaque)
{
    s32k358_stm_channel_expire(S32K358_STM(opaque), 3U);
}

static uint32_t s32k358_stm_read_reg(S32K358STMState *s, hwaddr off)
{
    unsigned ch;
    hwaddr ch_off;

    if (off == STM_CR_OFF) {
        return s->cr;
    }
    if (off == STM_CNT_OFF) {
        return s32k358_stm_get_cnt(s);
    }

    if (off >= STM_CH_BASE &&
        off < STM_CH_BASE + (STM_CH_STRIDE * S32K358_STM_NUM_CHANNELS)) {
        ch = (unsigned)((off - STM_CH_BASE) / STM_CH_STRIDE);
        ch_off = (off - STM_CH_BASE) % STM_CH_STRIDE;

        switch (ch_off) {
        case STM_CCR_OFF:
            return s->ccr[ch];
        case STM_CIR_OFF:
            return s->cir[ch];
        case STM_CMP_OFF:
            return s->cmp[ch];
        default:
            break;
        }
    }

    return s32k358_stm_read_raw32(s, off);
}

static void s32k358_stm_write_reg(S32K358STMState *s, hwaddr off, uint32_t val)
{
    unsigned ch;
    hwaddr ch_off;
    uint32_t new_cr;

    if (off == STM_CR_OFF) {
        s32k358_stm_latch_counter(s);
        new_cr = val & (STM_CR_TEN | STM_CR_FRZ | STM_CR_CPS_MASK);
        s->cr = new_cr;
        s->cnt_anchor_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s32k358_stm_write_raw32(s, off, s->cr);
        s32k358_stm_schedule_all(s);
        s32k358_stm_update_irq(s);
        return;
    }

    if (off == STM_CNT_OFF) {
        s->cnt_base = val;
        s->cnt_anchor_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s32k358_stm_write_raw32(s, off, s->cnt_base);
        s32k358_stm_schedule_all(s);
        return;
    }

    if (off >= STM_CH_BASE &&
        off < STM_CH_BASE + (STM_CH_STRIDE * S32K358_STM_NUM_CHANNELS)) {
        ch = (unsigned)((off - STM_CH_BASE) / STM_CH_STRIDE);
        ch_off = (off - STM_CH_BASE) % STM_CH_STRIDE;
        switch (ch_off) {
        case STM_CCR_OFF:
            s->ccr[ch] = val & STM_CCR_CEN;
            s32k358_stm_write_raw32(s, off, s->ccr[ch]);
            s32k358_stm_schedule_channel(s, ch);
            s32k358_stm_update_irq(s);
            return;
        case STM_CIR_OFF:
            if ((val & STM_CIR_CIF) != 0U) {
                s->cir[ch] &= ~STM_CIR_CIF;
            }
            s32k358_stm_write_raw32(s, off, s->cir[ch]);
            s32k358_stm_schedule_channel(s, ch);
            s32k358_stm_update_irq(s);
            return;
        case STM_CMP_OFF:
            s->cmp[ch] = val;
            s32k358_stm_write_raw32(s, off, s->cmp[ch]);
            s32k358_stm_schedule_channel(s, ch);
            return;
        default:
            break;
        }
    }

    s32k358_stm_write_raw32(s, off, val);
}

static uint64_t s32k358_stm_read(void *opaque, hwaddr addr, unsigned size)
{
    S32K358STMState *s = S32K358_STM(opaque);
    uint64_t val = 0;
    unsigned i;

    if (addr + size > S32K358_STM_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-stm: invalid read addr=0x%" HWADDR_PRIx
                      " size=%u\n", addr, size);
        return 0;
    }

    for (i = 0; i < size; i++) {
        hwaddr byte_off = addr + i;
        uint32_t word = s32k358_stm_read_reg(s, byte_off & ~0x3U);
        uint8_t byte = (uint8_t)(word >> (8U * (byte_off & 0x3U)));
        val |= ((uint64_t)byte << (8U * i));
    }

    return val;
}

static void s32k358_stm_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    S32K358STMState *s = S32K358_STM(opaque);
    unsigned i;

    if (addr + size > S32K358_STM_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-stm: invalid write addr=0x%" HWADDR_PRIx
                      " size=%u val=0x%" PRIx64 "\n",
                      addr, size, value);
        return;
    }

    for (i = 0; i < size; i++) {
        hwaddr byte_off = addr + i;
        hwaddr word_off = byte_off & ~0x3U;
        unsigned shift = 8U * (byte_off & 0x3U);
        uint32_t cur = s32k358_stm_read_reg(s, word_off);
        uint32_t next = (cur & ~(0xffU << shift)) |
                        (((uint32_t)(value >> (8U * i)) & 0xffU) << shift);

        s32k358_stm_write_reg(s, word_off, next);
    }
}

static const MemoryRegionOps s32k358_stm_ops = {
    .read = s32k358_stm_read,
    .write = s32k358_stm_write,
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

static void s32k358_stm_reset(DeviceState *dev)
{
    S32K358STMState *s = S32K358_STM(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->cr = 0U;
    s->cnt_base = 0U;
    s->cnt_anchor_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    memset(s->ccr, 0, sizeof(s->ccr));
    memset(s->cir, 0, sizeof(s->cir));
    memset(s->cmp, 0, sizeof(s->cmp));

    s32k358_stm_write_raw32(s, STM_CR_OFF, s->cr);
    s32k358_stm_write_raw32(s, STM_CNT_OFF, s->cnt_base);
    for (unsigned ch = 0; ch < S32K358_STM_NUM_CHANNELS; ch++) {
        s32k358_stm_write_raw32(s, STM_CH_BASE + (ch * STM_CH_STRIDE) +
                                STM_CCR_OFF, s->ccr[ch]);
        s32k358_stm_write_raw32(s, STM_CH_BASE + (ch * STM_CH_STRIDE) +
                                STM_CIR_OFF, s->cir[ch]);
        s32k358_stm_write_raw32(s, STM_CH_BASE + (ch * STM_CH_STRIDE) +
                                STM_CMP_OFF, s->cmp[ch]);
        timer_del(s->timer[ch]);
    }

    s32k358_stm_update_irq(s);
}

static void s32k358_stm_init(Object *obj)
{
    S32K358STMState *s = S32K358_STM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &s32k358_stm_ops, s,
                          TYPE_S32K358_STM, S32K358_STM_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);

    s->timer[0] = timer_new_ns(QEMU_CLOCK_VIRTUAL, s32k358_stm_timer0_cb, s);
    s->timer[1] = timer_new_ns(QEMU_CLOCK_VIRTUAL, s32k358_stm_timer1_cb, s);
    s->timer[2] = timer_new_ns(QEMU_CLOCK_VIRTUAL, s32k358_stm_timer2_cb, s);
    s->timer[3] = timer_new_ns(QEMU_CLOCK_VIRTUAL, s32k358_stm_timer3_cb, s);
}

static Property s32k358_stm_properties[] = {
    DEFINE_PROP_UINT32("stm-index", S32K358STMState, stm_index, 0U),
    DEFINE_PROP_END_OF_LIST(),
};

static void s32k358_stm_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S32K358 STM";
    device_class_set_legacy_reset(dc, s32k358_stm_reset);
    device_class_set_props(dc, s32k358_stm_properties);
}

static const TypeInfo s32k358_stm_info = {
    .name = TYPE_S32K358_STM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S32K358STMState),
    .instance_init = s32k358_stm_init,
    .class_init = s32k358_stm_class_init,
};

static void s32k358_stm_register_types(void)
{
    type_register_static(&s32k358_stm_info);
}

type_init(s32k358_stm_register_types)
