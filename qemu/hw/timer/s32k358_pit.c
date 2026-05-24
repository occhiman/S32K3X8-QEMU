/*
 * NXP S32K358 PIT model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/qdev-properties.h"
#include "hw/timer/s32k358_pit.h"

#define PIT_CLOCK_HZ        40000000ULL

#define PIT_MCR_OFF         0x00U
#define PIT_LTMR64H_OFF     0xE0U
#define PIT_LTMR64L_OFF     0xE4U
#define PIT_RTI_LDVAL_OFF   0xF0U
#define PIT_RTI_CVAL_OFF    0xF4U
#define PIT_RTI_TCTRL_OFF   0xF8U
#define PIT_RTI_TFLG_OFF    0xFCU
#define PIT_CH_BASE         0x100U
#define PIT_CH_STRIDE       0x10U
#define PIT_LDVAL_OFF       0x00U
#define PIT_CVAL_OFF        0x04U
#define PIT_TCTRL_OFF       0x08U
#define PIT_TFLG_OFF        0x0CU

#define PIT_MCR_FRZ         (1U << 0)
#define PIT_MCR_MDIS        (1U << 1)
#define PIT_MCR_MDIS_RTI    (1U << 2)

#define PIT_TCTRL_TEN       (1U << 0)
#define PIT_TCTRL_TIE       (1U << 1)
#define PIT_TCTRL_CHN       (1U << 2)

#define PIT_TFLG_TIF        (1U << 0)

static uint64_t s32k358_pit_period_ns(uint32_t ldval)
{
    return muldiv64((uint64_t)ldval + 1ULL, NANOSECONDS_PER_SECOND,
                    PIT_CLOCK_HZ);
}

static bool s32k358_pit_module_disabled(const S32K358PITState *s)
{
    return (s->mcr & PIT_MCR_MDIS) != 0U;
}

static bool s32k358_pit_rti_disabled(const S32K358PITState *s)
{
    return !s->has_rti || (s->mcr & PIT_MCR_MDIS_RTI) != 0U;
}

static bool s32k358_pit_channel_enabled(const S32K358PITState *s, unsigned ch)
{
    return (s->tctrl[ch] & PIT_TCTRL_TEN) != 0U;
}

static bool s32k358_pit_channel_chained(const S32K358PITState *s, unsigned ch)
{
    return ch > 0 && (s->tctrl[ch] & PIT_TCTRL_CHN) != 0U;
}

static bool s32k358_pit_channel_timer_driven(const S32K358PITState *s,
                                             unsigned ch)
{
    return s32k358_pit_channel_enabled(s, ch) &&
           !s32k358_pit_module_disabled(s) &&
           !s32k358_pit_channel_chained(s, ch);
}

static uint32_t s32k358_pit_get_channel_cval(const S32K358PITState *s,
                                             unsigned ch)
{
    int64_t now_ns;
    int64_t delta_ns;
    uint64_t period_ns;
    uint64_t ticks_to_expire;

    if (!s32k358_pit_channel_timer_driven(s, ch)) {
        return s->chain_cval[ch];
    }

    if (s->deadline_ns[ch] == 0) {
        return s->chain_cval[ch];
    }

    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    delta_ns = s->deadline_ns[ch] - now_ns;
    if (delta_ns <= 0) {
        return 0U;
    }

    period_ns = s32k358_pit_period_ns(s->ldval[ch]);
    if (period_ns == 0U) {
        return 0U;
    }

    ticks_to_expire = (uint64_t)(delta_ns + (int64_t)period_ns - 1) / period_ns;
    if (ticks_to_expire == 0U) {
        return 0U;
    }
    return (uint32_t)(ticks_to_expire - 1U);
}

static uint32_t s32k358_pit_get_rti_cval(const S32K358PITState *s)
{
    int64_t now_ns;
    int64_t delta_ns;
    uint64_t period_ns;
    uint64_t ticks_to_expire;

    if (s32k358_pit_rti_disabled(s) || (s->rti_tctrl & PIT_TCTRL_TEN) == 0U) {
        return s->rti_ldval;
    }

    if (s->rti_deadline_ns == 0) {
        return s->rti_ldval;
    }

    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    delta_ns = s->rti_deadline_ns - now_ns;
    if (delta_ns <= 0) {
        return 0U;
    }

    period_ns = s32k358_pit_period_ns(s->rti_ldval);
    if (period_ns == 0U) {
        return 0U;
    }

    ticks_to_expire = (uint64_t)(delta_ns + (int64_t)period_ns - 1) / period_ns;
    if (ticks_to_expire == 0U) {
        return 0U;
    }
    return (uint32_t)(ticks_to_expire - 1U);
}

static void s32k358_pit_update_irq(S32K358PITState *s)
{
    bool level = false;

    for (unsigned ch = 0; ch < S32K358_PIT_NUM_CHANNELS; ch++) {
        if ((s->tctrl[ch] & PIT_TCTRL_TIE) != 0U &&
            (s->tflg[ch] & PIT_TFLG_TIF) != 0U) {
            level = true;
            break;
        }
    }

    if (!level && s->has_rti &&
        (s->rti_tctrl & PIT_TCTRL_TIE) != 0U &&
        (s->rti_tflg & PIT_TFLG_TIF) != 0U) {
        level = true;
    }

    qemu_set_irq(s->irq, level ? 1 : 0);
}

static void s32k358_pit_schedule_channel(S32K358PITState *s, unsigned ch)
{
    int64_t now_ns;
    uint64_t period_ns;

    if (!s32k358_pit_channel_timer_driven(s, ch)) {
        s->deadline_ns[ch] = 0;
        timer_del(s->timer[ch]);
        return;
    }

    period_ns = s32k358_pit_period_ns(s->ldval[ch]);
    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->deadline_ns[ch] = now_ns + (int64_t)period_ns;
    timer_mod_ns(s->timer[ch], s->deadline_ns[ch]);
}

static void s32k358_pit_schedule_rti(S32K358PITState *s)
{
    int64_t now_ns;
    uint64_t period_ns;

    if (s32k358_pit_rti_disabled(s) || (s->rti_tctrl & PIT_TCTRL_TEN) == 0U) {
        s->rti_deadline_ns = 0;
        timer_del(s->rti_timer);
        return;
    }

    period_ns = s32k358_pit_period_ns(s->rti_ldval);
    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->rti_deadline_ns = now_ns + (int64_t)period_ns;
    timer_mod_ns(s->rti_timer, s->rti_deadline_ns);
}

static void s32k358_pit_chain_pulse(S32K358PITState *s, unsigned ch)
{
    if (ch >= S32K358_PIT_NUM_CHANNELS) {
        return;
    }
    if (!s32k358_pit_channel_enabled(s, ch) ||
        !s32k358_pit_channel_chained(s, ch) ||
        s32k358_pit_module_disabled(s)) {
        return;
    }

    if (s->chain_cval[ch] == 0U) {
        s->tflg[ch] |= PIT_TFLG_TIF;
        s->chain_cval[ch] = s->ldval[ch];
        if (ch + 1U < S32K358_PIT_NUM_CHANNELS) {
            s32k358_pit_chain_pulse(s, ch + 1U);
        }
    } else {
        s->chain_cval[ch]--;
    }
}

static void s32k358_pit_channel_expire(S32K358PITState *s, unsigned ch)
{
    if (ch >= S32K358_PIT_NUM_CHANNELS) {
        return;
    }

    s->tflg[ch] |= PIT_TFLG_TIF;
    s->chain_cval[ch] = s->ldval[ch];

    if (ch + 1U < S32K358_PIT_NUM_CHANNELS) {
        s32k358_pit_chain_pulse(s, ch + 1U);
    }

    s32k358_pit_schedule_channel(s, ch);
    s32k358_pit_update_irq(s);
}

static void s32k358_pit_timer0_cb(void *opaque)
{
    s32k358_pit_channel_expire(S32K358_PIT(opaque), 0U);
}

static void s32k358_pit_timer1_cb(void *opaque)
{
    s32k358_pit_channel_expire(S32K358_PIT(opaque), 1U);
}

static void s32k358_pit_timer2_cb(void *opaque)
{
    s32k358_pit_channel_expire(S32K358_PIT(opaque), 2U);
}

static void s32k358_pit_timer3_cb(void *opaque)
{
    s32k358_pit_channel_expire(S32K358_PIT(opaque), 3U);
}

static void s32k358_pit_rti_expire(void *opaque)
{
    S32K358PITState *s = S32K358_PIT(opaque);

    if (!s->has_rti) {
        return;
    }

    s->rti_tflg |= PIT_TFLG_TIF;
    s32k358_pit_schedule_rti(s);
    s32k358_pit_update_irq(s);
}

static void s32k358_pit_reconfigure(S32K358PITState *s)
{
    for (unsigned ch = 0; ch < S32K358_PIT_NUM_CHANNELS; ch++) {
        if (!s32k358_pit_channel_enabled(s, ch)) {
            s->chain_cval[ch] = s->ldval[ch];
        }
        s32k358_pit_schedule_channel(s, ch);
    }
    s32k358_pit_schedule_rti(s);
    s32k358_pit_update_irq(s);
}

static uint32_t s32k358_pit_read_reg(S32K358PITState *s, hwaddr off)
{
    unsigned ch;
    hwaddr ch_off;
    uint64_t life;

    switch (off) {
    case PIT_MCR_OFF:
        return s->mcr;
    case PIT_LTMR64H_OFF:
        s->ltmr64l_latch = s32k358_pit_get_channel_cval(s, 0U);
        return s32k358_pit_get_channel_cval(s, 1U);
    case PIT_LTMR64L_OFF:
        return s->ltmr64l_latch;
    case PIT_RTI_LDVAL_OFF:
        return s->has_rti ? s->rti_ldval : 0U;
    case PIT_RTI_CVAL_OFF:
        return s->has_rti ? s32k358_pit_get_rti_cval(s) : 0U;
    case PIT_RTI_TCTRL_OFF:
        return s->has_rti ? s->rti_tctrl : 0U;
    case PIT_RTI_TFLG_OFF:
        return s->has_rti ? s->rti_tflg : 0U;
    default:
        break;
    }

    if (off >= PIT_CH_BASE &&
        off < PIT_CH_BASE + (PIT_CH_STRIDE * S32K358_PIT_NUM_CHANNELS)) {
        ch = (unsigned)((off - PIT_CH_BASE) / PIT_CH_STRIDE);
        ch_off = (off - PIT_CH_BASE) % PIT_CH_STRIDE;
        switch (ch_off) {
        case PIT_LDVAL_OFF:
            return s->ldval[ch];
        case PIT_CVAL_OFF:
            return s32k358_pit_get_channel_cval(s, ch);
        case PIT_TCTRL_OFF:
            return s->tctrl[ch];
        case PIT_TFLG_OFF:
            return s->tflg[ch];
        default:
            return 0U;
        }
    }

    /* Keep lifetimer helpers deterministic on unknown reads. */
    life = ((uint64_t)s32k358_pit_get_channel_cval(s, 1U) << 32) |
           (uint64_t)s32k358_pit_get_channel_cval(s, 0U);
    return (uint32_t)life;
}

static void s32k358_pit_write_reg(S32K358PITState *s, hwaddr off, uint32_t val)
{
    unsigned ch;
    hwaddr ch_off;
    uint32_t old_tctrl;

    if (off == PIT_MCR_OFF) {
        s->mcr = val & (PIT_MCR_FRZ | PIT_MCR_MDIS | PIT_MCR_MDIS_RTI);
        s32k358_pit_reconfigure(s);
        return;
    }

    if (s->has_rti) {
        if (off == PIT_RTI_LDVAL_OFF) {
            s->rti_ldval = val;
            if ((s->rti_tctrl & PIT_TCTRL_TEN) != 0U) {
                s32k358_pit_schedule_rti(s);
            }
            return;
        }
        if (off == PIT_RTI_TCTRL_OFF) {
            s->rti_tctrl = val & (PIT_TCTRL_TEN | PIT_TCTRL_TIE);
            s32k358_pit_schedule_rti(s);
            s32k358_pit_update_irq(s);
            return;
        }
        if (off == PIT_RTI_TFLG_OFF) {
            if ((val & PIT_TFLG_TIF) != 0U) {
                s->rti_tflg &= ~PIT_TFLG_TIF;
                s32k358_pit_update_irq(s);
            }
            return;
        }
    }

    if (off < PIT_CH_BASE ||
        off >= PIT_CH_BASE + (PIT_CH_STRIDE * S32K358_PIT_NUM_CHANNELS)) {
        return;
    }

    ch = (unsigned)((off - PIT_CH_BASE) / PIT_CH_STRIDE);
    ch_off = (off - PIT_CH_BASE) % PIT_CH_STRIDE;
    switch (ch_off) {
    case PIT_LDVAL_OFF:
        s->ldval[ch] = val;
        if ((s->tctrl[ch] & PIT_TCTRL_TEN) != 0U) {
            if (s32k358_pit_channel_chained(s, ch)) {
                s->chain_cval[ch] = s->ldval[ch];
            } else {
                s32k358_pit_schedule_channel(s, ch);
            }
        } else {
            s->chain_cval[ch] = s->ldval[ch];
        }
        break;
    case PIT_TCTRL_OFF:
        old_tctrl = s->tctrl[ch];
        s->tctrl[ch] = val & (PIT_TCTRL_TEN | PIT_TCTRL_TIE | PIT_TCTRL_CHN);
        if ((old_tctrl & PIT_TCTRL_TEN) == 0U &&
            (s->tctrl[ch] & PIT_TCTRL_TEN) != 0U) {
            s->chain_cval[ch] = s->ldval[ch];
        }
        s32k358_pit_schedule_channel(s, ch);
        s32k358_pit_update_irq(s);
        break;
    case PIT_TFLG_OFF:
        if ((val & PIT_TFLG_TIF) != 0U) {
            s->tflg[ch] &= ~PIT_TFLG_TIF;
            s32k358_pit_update_irq(s);
        }
        break;
    default:
        break;
    }
}

static uint64_t s32k358_pit_read(void *opaque, hwaddr addr, unsigned size)
{
    S32K358PITState *s = S32K358_PIT(opaque);
    uint64_t val = 0;
    unsigned i;

    if (addr + size > S32K358_PIT_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-pit: invalid read addr=0x%" HWADDR_PRIx
                      " size=%u\n", addr, size);
        return 0;
    }

    for (i = 0; i < size; i++) {
        hwaddr byte_off = addr + i;
        uint32_t word = s32k358_pit_read_reg(s, byte_off & ~0x3U);
        uint8_t byte = (uint8_t)(word >> (8U * (byte_off & 0x3U)));
        val |= ((uint64_t)byte << (8U * i));
    }

    return val;
}

static void s32k358_pit_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    S32K358PITState *s = S32K358_PIT(opaque);
    unsigned i;

    if (addr + size > S32K358_PIT_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-pit: invalid write addr=0x%" HWADDR_PRIx
                      " size=%u val=0x%" PRIx64 "\n",
                      addr, size, value);
        return;
    }

    for (i = 0; i < size; i++) {
        hwaddr byte_off = addr + i;
        hwaddr word_off = byte_off & ~0x3U;
        unsigned shift = 8U * (byte_off & 0x3U);
        uint32_t cur = s32k358_pit_read_reg(s, word_off);
        uint32_t next = (cur & ~(0xffU << shift)) |
                        (((uint32_t)(value >> (8U * i)) & 0xffU) << shift);

        s32k358_pit_write_reg(s, word_off, next);
    }
}

static const MemoryRegionOps s32k358_pit_ops = {
    .read = s32k358_pit_read,
    .write = s32k358_pit_write,
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

static void s32k358_pit_reset(DeviceState *dev)
{
    S32K358PITState *s = S32K358_PIT(dev);

    memset(s->ldval, 0, sizeof(s->ldval));
    memset(s->tctrl, 0, sizeof(s->tctrl));
    memset(s->tflg, 0, sizeof(s->tflg));
    memset(s->deadline_ns, 0, sizeof(s->deadline_ns));
    memset(s->chain_cval, 0, sizeof(s->chain_cval));
    s->ltmr64l_latch = 0U;

    s->rti_ldval = 0U;
    s->rti_tctrl = 0U;
    s->rti_tflg = 0U;
    s->rti_deadline_ns = 0;

    s->has_rti = (s->pit_index == 0U);
    s->mcr = s->has_rti ? (PIT_MCR_MDIS | PIT_MCR_MDIS_RTI) : PIT_MCR_MDIS;

    for (unsigned ch = 0; ch < S32K358_PIT_NUM_CHANNELS; ch++) {
        timer_del(s->timer[ch]);
    }
    timer_del(s->rti_timer);
    s32k358_pit_update_irq(s);
}

static void s32k358_pit_init(Object *obj)
{
    S32K358PITState *s = S32K358_PIT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &s32k358_pit_ops, s,
                          TYPE_S32K358_PIT, S32K358_PIT_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);

    s->timer[0] = timer_new_ns(QEMU_CLOCK_VIRTUAL, s32k358_pit_timer0_cb, s);
    s->timer[1] = timer_new_ns(QEMU_CLOCK_VIRTUAL, s32k358_pit_timer1_cb, s);
    s->timer[2] = timer_new_ns(QEMU_CLOCK_VIRTUAL, s32k358_pit_timer2_cb, s);
    s->timer[3] = timer_new_ns(QEMU_CLOCK_VIRTUAL, s32k358_pit_timer3_cb, s);
    s->rti_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, s32k358_pit_rti_expire, s);
}

static Property s32k358_pit_properties[] = {
    DEFINE_PROP_UINT32("pit-index", S32K358PITState, pit_index, 0U),
    DEFINE_PROP_END_OF_LIST(),
};

static void s32k358_pit_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S32K358 PIT";
    device_class_set_legacy_reset(dc, s32k358_pit_reset);
    device_class_set_props(dc, s32k358_pit_properties);
}

static const TypeInfo s32k358_pit_info = {
    .name = TYPE_S32K358_PIT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S32K358PITState),
    .instance_init = s32k358_pit_init,
    .class_init = s32k358_pit_class_init,
};

static void s32k358_pit_register_types(void)
{
    type_register_static(&s32k358_pit_info);
}

type_init(s32k358_pit_register_types)

