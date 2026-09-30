/*
 * NXP S32K358 STM model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef S32K358_STM_H
#define S32K358_STM_H

#include "qemu/osdep.h"
#include "exec/memory.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_S32K358_STM "s32k358-stm"

#define S32K358_STM_MMIO_SIZE      0x4000U
#define S32K358_STM_NUM_CHANNELS   4U

typedef struct S32K358STMState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    QEMUTimer *timer[S32K358_STM_NUM_CHANNELS];
    uint32_t stm_index;

    /*
     * Backing storage for unknown/reserved offsets and register-protection
     * windows (for example base + 0x2400 used by SET_USER_ACCESS_ALLOWED()).
     */
    uint8_t regs[S32K358_STM_MMIO_SIZE];

    uint32_t cr;
    uint32_t cnt_base;
    int64_t cnt_anchor_ns;
    uint32_t ccr[S32K358_STM_NUM_CHANNELS];
    uint32_t cir[S32K358_STM_NUM_CHANNELS];
    uint32_t cmp[S32K358_STM_NUM_CHANNELS];
} S32K358STMState;

OBJECT_DECLARE_SIMPLE_TYPE(S32K358STMState, S32K358_STM)

#endif /* S32K358_STM_H */
