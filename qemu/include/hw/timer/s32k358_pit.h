/*
 * NXP S32K358 PIT model (minimal implementation).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef S32K358_PIT_H
#define S32K358_PIT_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "exec/memory.h"
#include "hw/irq.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_S32K358_PIT "s32k358-pit"

#define S32K358_PIT_MMIO_SIZE      0x4000U
#define S32K358_PIT_NUM_CHANNELS   4U

typedef struct S32K358PITState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    QEMUTimer *timer[S32K358_PIT_NUM_CHANNELS];
    QEMUTimer *rti_timer;
    uint32_t pit_index;
    bool has_rti;

    uint32_t mcr;
    uint32_t ldval[S32K358_PIT_NUM_CHANNELS];
    uint32_t tctrl[S32K358_PIT_NUM_CHANNELS];
    uint32_t tflg[S32K358_PIT_NUM_CHANNELS];
    uint32_t chain_cval[S32K358_PIT_NUM_CHANNELS];

    int64_t deadline_ns[S32K358_PIT_NUM_CHANNELS];

    uint32_t rti_ldval;
    uint32_t rti_tctrl;
    uint32_t rti_tflg;
    int64_t rti_deadline_ns;
    uint32_t ltmr64l_latch;
} S32K358PITState;

OBJECT_DECLARE_SIMPLE_TYPE(S32K358PITState, S32K358_PIT)

#endif /* S32K358_PIT_H */
