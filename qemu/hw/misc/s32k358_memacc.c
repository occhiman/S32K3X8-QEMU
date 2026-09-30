/*
 * NXP S32K358 MemAcc-facing FLASH control model.
 *
 * This is a register-backed compatibility model for the FLASH/PFLASH
 * control windows touched by MemAcc -> Mem_43_INFLS -> C40_Ip.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/s32k358_memacc.h"

/*
 * FLASH register subset (S32K3 C40 main interface compatible view).
 * This model only implements the bits currently exercised by MCAL C40_Ip flows.
 */
#define FLASH_MCR_OFF           0x000U
#define FLASH_MCRS_OFF          0x004U

#define FLASH_MCR_EHV_MASK      0x00000001U
#define FLASH_MCR_ERS_MASK      0x00000010U
#define FLASH_MCR_ESS_MASK      0x00000020U
#define FLASH_MCR_PGM_MASK      0x00000100U

#define FLASH_MCRS_PEG_MASK     0x00004000U
#define FLASH_MCRS_DONE_MASK    0x00008000U
#define FLASH_MCRS_PES_MASK     0x00010000U
#define FLASH_MCRS_PEP_MASK     0x00020000U
#define FLASH_MCRS_RWE_MASK     0x00100000U
#define FLASH_MCRS_RRE_MASK     0x01000000U
#define FLASH_MCRS_RVE_MASK     0x02000000U
#define FLASH_MCRS_EEE_MASK     0x10000000U
#define FLASH_MCRS_AEE_MASK     0x20000000U
#define FLASH_MCRS_SBC_MASK     0x40000000U
#define FLASH_MCRS_EER_MASK     0x80000000U

#define FLASH_MCRS_W1C_MASK     (FLASH_MCRS_PES_MASK | FLASH_MCRS_PEP_MASK | \
                                 FLASH_MCRS_RWE_MASK | FLASH_MCRS_RRE_MASK | \
                                 FLASH_MCRS_RVE_MASK | FLASH_MCRS_EEE_MASK | \
                                 FLASH_MCRS_AEE_MASK | FLASH_MCRS_SBC_MASK | \
                                 FLASH_MCRS_EER_MASK)
#define FLASH_MCRS_ERR_MASK     FLASH_MCRS_W1C_MASK

static uint32_t s32k358_memacc_get_reg32(S32K358MemAccState *s, hwaddr off)
{
    return (uint32_t)s->regs[off] |
           ((uint32_t)s->regs[off + 1U] << 8U) |
           ((uint32_t)s->regs[off + 2U] << 16U) |
           ((uint32_t)s->regs[off + 3U] << 24U);
}

static void s32k358_memacc_set_reg32(S32K358MemAccState *s, hwaddr off, uint32_t v)
{
    s->regs[off] = (uint8_t)(v & 0xFFU);
    s->regs[off + 1U] = (uint8_t)((v >> 8U) & 0xFFU);
    s->regs[off + 2U] = (uint8_t)((v >> 16U) & 0xFFU);
    s->regs[off + 3U] = (uint8_t)((v >> 24U) & 0xFFU);
}

static bool s32k358_memacc_overlap_mask(hwaddr addr, unsigned size, hwaddr reg_off,
                                        uint32_t *mask)
{
    hwaddr start = MAX(addr, reg_off);
    hwaddr end = MIN(addr + (hwaddr)size, reg_off + 4U);
    uint32_t m = 0U;

    if (start >= end) {
        *mask = 0U;
        return false;
    }

    for (hwaddr b = start; b < end; b++) {
        m |= (0xFFU << ((b - reg_off) * 8U));
    }

    *mask = m;
    return true;
}

static void s32k358_memacc_apply_mcr_side_effects(S32K358MemAccState *s,
                                                  uint32_t old_mcr,
                                                  uint32_t new_mcr)
{
    uint32_t mcrs = s32k358_memacc_get_reg32(s, FLASH_MCRS_OFF);
    bool old_ehv = (old_mcr & FLASH_MCR_EHV_MASK) != 0U;
    bool new_ehv = (new_mcr & FLASH_MCR_EHV_MASK) != 0U;

    /*
     * Keep a compatibility-friendly completion model:
     * - DONE/PEG are asserted in idle state.
     * - clearing EHV finalizes/aborts and guarantees DONE=1.
     */
    if (old_ehv && !new_ehv) {
        mcrs |= FLASH_MCRS_DONE_MASK;
        if ((mcrs & (FLASH_MCRS_PES_MASK | FLASH_MCRS_PEP_MASK)) == 0U) {
            mcrs |= FLASH_MCRS_PEG_MASK;
        } else {
            mcrs &= ~FLASH_MCRS_PEG_MASK;
        }
    } else if (!old_ehv && !new_ehv) {
        mcrs |= FLASH_MCRS_DONE_MASK | FLASH_MCRS_PEG_MASK;
    }

    s32k358_memacc_set_reg32(s, FLASH_MCRS_OFF, mcrs);
}

static void s32k358_memacc_apply_mcrs_write(S32K358MemAccState *s,
                                            uint32_t old_mcrs,
                                            uint32_t write_mask,
                                            uint32_t write_value)
{
    uint32_t clear_mask = write_value & write_mask & FLASH_MCRS_W1C_MASK;
    uint32_t new_mcrs = old_mcrs & ~clear_mask;

    /*
     * DONE is a status bit and should remain set in the compatibility model.
     * PEG reflects success when no program/erase error flags are set.
     */
    new_mcrs |= FLASH_MCRS_DONE_MASK;
    if ((new_mcrs & FLASH_MCRS_ERR_MASK) == 0U) {
        new_mcrs |= FLASH_MCRS_PEG_MASK;
    } else {
        new_mcrs &= ~FLASH_MCRS_PEG_MASK;
    }

    s32k358_memacc_set_reg32(s, FLASH_MCRS_OFF, new_mcrs);
}

static uint64_t s32k358_memacc_read(void *opaque, hwaddr addr, unsigned size)
{
    S32K358MemAccState *s = S32K358_MEMACC(opaque);
    uint64_t value = 0;

    if (addr + size > S32K358_MEMACC_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-memacc: invalid read addr=0x%" HWADDR_PRIx
                      " size=%u\n", addr, size);
        return 0;
    }

    for (unsigned i = 0; i < size; i++) {
        value |= ((uint64_t)s->regs[addr + i] << (8U * i));
    }

    return value;
}

static void s32k358_memacc_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    S32K358MemAccState *s = S32K358_MEMACC(opaque);
    uint32_t old_mcr;
    uint32_t old_mcrs;
    uint32_t mcr_write_mask = 0U;
    uint32_t mcrs_write_mask = 0U;
    bool wrote_mcr;
    bool wrote_mcrs;

    if (addr + size > S32K358_MEMACC_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-memacc: invalid write addr=0x%" HWADDR_PRIx
                      " size=%u val=0x%" PRIx64 "\n",
                      addr, size, value);
        return;
    }

    old_mcr = s32k358_memacc_get_reg32(s, FLASH_MCR_OFF);
    old_mcrs = s32k358_memacc_get_reg32(s, FLASH_MCRS_OFF);

    wrote_mcr = s32k358_memacc_overlap_mask(addr, size, FLASH_MCR_OFF,
                                            &mcr_write_mask);
    wrote_mcrs = s32k358_memacc_overlap_mask(addr, size, FLASH_MCRS_OFF,
                                             &mcrs_write_mask);

    for (unsigned i = 0U; i < size; i++) {
        s->regs[addr + i] = (uint8_t)(value >> (8U * i));
    }

    if (wrote_mcr) {
        uint32_t new_mcr = s32k358_memacc_get_reg32(s, FLASH_MCR_OFF);
        s32k358_memacc_apply_mcr_side_effects(s, old_mcr, new_mcr);
    }

    if (wrote_mcrs) {
        uint32_t write_value = s32k358_memacc_get_reg32(s, FLASH_MCRS_OFF);
        s32k358_memacc_apply_mcrs_write(s, old_mcrs,
                                        mcrs_write_mask, write_value);
    }
}

static const MemoryRegionOps s32k358_memacc_ops = {
    .read = s32k358_memacc_read,
    .write = s32k358_memacc_write,
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

static void s32k358_memacc_reset(DeviceState *dev)
{
    S32K358MemAccState *s = S32K358_MEMACC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s32k358_memacc_set_reg32(s, FLASH_MCRS_OFF,
                             FLASH_MCRS_DONE_MASK | FLASH_MCRS_PEG_MASK);
}

static void s32k358_memacc_init(Object *obj)
{
    S32K358MemAccState *s = S32K358_MEMACC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &s32k358_memacc_ops, s,
                          TYPE_S32K358_MEMACC, S32K358_MEMACC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
}

static void s32k358_memacc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S32K358 MemAcc/FLASH Control";
    device_class_set_legacy_reset(dc, s32k358_memacc_reset);
}

static const TypeInfo s32k358_memacc_info = {
    .name = TYPE_S32K358_MEMACC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S32K358MemAccState),
    .instance_init = s32k358_memacc_init,
    .class_init = s32k358_memacc_class_init,
};

static void s32k358_memacc_register_types(void)
{
    type_register_static(&s32k358_memacc_info);
}

type_init(s32k358_memacc_register_types)
