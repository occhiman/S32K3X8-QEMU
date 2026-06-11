/*
 * NXP S32K358 DMAMUX model (minimal CHCFG implementation).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef S32K358_DMAMUX_H
#define S32K358_DMAMUX_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "exec/memory.h"
#include "qom/object.h"

#define TYPE_S32K358_DMAMUX "s32k358-dmamux"

#define S32K358_DMAMUX_NUM_CHANNELS 16U
#define S32K358_DMAMUX_MMIO_SIZE    0x1000U
#define S32K358_DMAMUX_CHCFG_SOURCE_MASK 0x3FU
#define S32K358_DMAMUX_CHCFG_ENBL_MASK   0x80U

typedef enum {
    S32K358_DMAMUX_INSTANCE_0 = 0U,
    S32K358_DMAMUX_INSTANCE_1 = 1U,
    S32K358_DMAMUX_INSTANCE_COUNT = 2U
} S32K358DMAMUXInstance;

typedef enum {
    S32K358_DMAMUX1_LPSPI4_TX_REQUEST = 52U,
    S32K358_DMAMUX1_LPSPI4_RX_REQUEST = 53U
} S32K358DMAMUXRequestSource;

typedef struct S32K358DMAMUXState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint8_t chcfg[S32K358_DMAMUX_NUM_CHANNELS];
    uint8_t instance;
} S32K358DMAMUXState;

void s32k358_dmamux_request(uint32_t instance, uint32_t source);

OBJECT_DECLARE_SIMPLE_TYPE(S32K358DMAMUXState, S32K358_DMAMUX)

#endif /* S32K358_DMAMUX_H */
