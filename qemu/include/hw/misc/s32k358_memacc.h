/*
 * NXP S32K358 MemAcc-facing FLASH control model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef S32K358_MEMACC_H
#define S32K358_MEMACC_H

#include "qemu/osdep.h"
#include "exec/memory.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_S32K358_MEMACC "s32k358-memacc"

#define S32K358_MEMACC_MMIO_SIZE 0x4000U

typedef struct S32K358MemAccState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint8_t regs[S32K358_MEMACC_MMIO_SIZE];
} S32K358MemAccState;

OBJECT_DECLARE_SIMPLE_TYPE(S32K358MemAccState, S32K358_MEMACC)

#endif /* S32K358_MEMACC_H */
