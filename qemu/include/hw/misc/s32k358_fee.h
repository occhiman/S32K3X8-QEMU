/*
 * NXP S32K358 Fee model (minimal register-backed implementation).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef S32K358_FEE_H
#define S32K358_FEE_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "exec/memory.h"
#include "qom/object.h"

#define TYPE_S32K358_FEE "s32k358-fee"

#define S32K358_FEE_MMIO_SIZE 0x4000U

typedef struct S32K358FeeState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint8_t regs[S32K358_FEE_MMIO_SIZE];
} S32K358FeeState;

OBJECT_DECLARE_SIMPLE_TYPE(S32K358FeeState, S32K358_FEE)

#endif /* S32K358_FEE_H */
