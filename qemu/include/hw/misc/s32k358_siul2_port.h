/*
 * NXP S32K358 SIUL2 Port model (minimal register-backed implementation).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef S32K358_SIUL2_PORT_H
#define S32K358_SIUL2_PORT_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "exec/memory.h"
#include "qom/object.h"

#define TYPE_S32K358_SIUL2_PORT "s32k358-siul2-port"

/*
 * SIUL2 functional registers are in low region, but RTD Port init also uses
 * REG_PROT mirror/GCR windows (for user access enable) up to 0x48FF.
 */
#define S32K358_SIUL2_PORT_MMIO_SIZE 0x5000U

typedef struct S32K358SIUL2PortState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint8_t regs[S32K358_SIUL2_PORT_MMIO_SIZE];
    uint16_t pgpdo[32];
} S32K358SIUL2PortState;

OBJECT_DECLARE_SIMPLE_TYPE(S32K358SIUL2PortState, S32K358_SIUL2_PORT)

#endif /* S32K358_SIUL2_PORT_H */
