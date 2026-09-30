/*
 * NXP S32K358 SIUL2 ICU model (minimal implementation).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef S32K358_SIUL2_ICU_H
#define S32K358_SIUL2_ICU_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "exec/memory.h"
#include "hw/irq.h"
#include "qom/object.h"

#define TYPE_S32K358_SIUL2_ICU "s32k358-siul2-icu"

#define S32K358_SIUL2_ICU_MMIO_SIZE  0x100U
#define S32K358_SIUL2_ICU_IRQ_LINES  4U
#define S32K358_SIUL2_EIRQ_COUNT     32U

typedef struct S32K358SIUL2ICUState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq[S32K358_SIUL2_ICU_IRQ_LINES];
    uint8_t regs[S32K358_SIUL2_ICU_MMIO_SIZE];
    uint8_t eirq_level[S32K358_SIUL2_EIRQ_COUNT];
    int64_t last_event_ns[S32K358_SIUL2_EIRQ_COUNT];
} S32K358SIUL2ICUState;

OBJECT_DECLARE_SIMPLE_TYPE(S32K358SIUL2ICUState, S32K358_SIUL2_ICU)

#endif /* S32K358_SIUL2_ICU_H */
