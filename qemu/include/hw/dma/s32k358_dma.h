/*
 * S32K358 UART
 *
 * Copyright (c) 2025 Emiliano Salvetto <em1l14n0s@gmail.com>
 * SPDX-License-Identifier: CC-BY-NC-4.0
 *
 * This work is licensed under the terms of the Creative Commons Attribution Non Commercial 4.0 International
 * See the COPYING file in the top-level directory.
 */
#ifndef S32K358_DMA_H
#define S32K358_DMA_H
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/register.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include <stdint.h>
#include "exec/memory.h"
#include "migration/vmstate.h"
#include "sysemu/dma.h"
#include "qom/object.h"

/* eDMA Engine Registers offsets */
#define CSR_OFF 0x0
#define ES_OFF 0x4
#define INT_OFF 0x8
#define HRS_OFF 0xC
#define CH_GRPRI_OFF 0x100


#define CH_GET_NUM(addr) ((addr) / 0x4000U)
#define CH_GET_REG(addr) ((addr) % 0x4000U)
/* The 31th bit of CSR specifiy if the DMA is in idle or active */
#define DMA_IS_ACTIVE(csr) ((csr) & 0x80000000U)
/* The bits 28-24 specifiy which channel is active */
#define DMA_GET_ACTIVE_CHANNEL(csr) ((csr) & 0x0F800000U) //TODO Add a shift
/* The 2nd bit of CSR specify if round robin is active */
/* If it is not active the DMA will use a fixed priority arbitration*/
#define DMA_IS_RR(csr) ((csr) & 0x2U)
//Priority CHn_GPRI:CHn_PRI:Ch_Num
#define CH_CSR_OFF 0x0
#define CH_ES_OFF 0x4
#define CH_INT_OFF 0x8
#define CH_SBR_OFF 0xC
#define CH_PRI_OFF 0x10
#define CH_TCD_SADDR_OFF 0x20
#define CH_TCD_SOFF_OFF 0x24
#define CH_TCD_ATTR_OFF 0x26
#define CH_TCD_NBYTES_MLOFF_OFF 0x28 // (Yes and No both)
#define CH_TCD_SLAST_SDA_OFF 0x2C
#define CH_TCD_DADDR_OFF 0x30
#define CH_TCD_DOFF_OFF 0x34
#define CH_TCD_CITER_ELINK_OFF 0x36 // (Yes and no both)
#define CH_TCD_DLAST_SGA_OFF 0x38
#define CH_TCD_CSR_OFF 0x3C
#define CH_TCD_BITER_ELINK 0x3E // (Yes and no both)

#define CH_TCD_LINKCH_MASK  0x3e00
#define CH_TCD_GET_LINKCH(reg) ( (reg & CH_TCD_LINKCH_MASK) >> 9 )

#define CH_TCD_CITER_MASK 0x1ff
#define CH_TCD_GET_CITER(reg) (reg & CH_TCD_CITER_MASK)

#define CH_TCD_GET_ELINK(reg) ( (reg >> 15) & 1)
#define CH_TCD_GET_MAJORINT(reg) ( reg & ( 1 << 1 ) )
#define CH_TCD_GET_MAJORELINK(reg) ( reg & ( 1 << 5 ) )

#define CH_TCD_MAJORLINKCH_MASK 0x1f00
#define CH_TCD_GET_MAJORLINKCH(reg) ( (reg & CH_TCD_MAJORLINKCH_MASK) >> 8)

#define CH_CSR_DONE_MASK 0xbfffffff
#define CH_CSR_SET_DONE(reg,val) ( ( reg & CH_CSR_DONE_MASK ) | ( val << 30 ) )

#define CH_CSR_ACTIVE_MASK 0x7fffffff
#define CH_CSR_SET_ACTIVE(reg,val) ( ( reg & CH_CSR_ACTIVE_MASK ) | ( val << 31 ) )

#define CH_INT_SET_INT(reg,val) ( ( reg & 0 ) | val )

#define S32K358_NUM_DMA_CH 32        // real -> 32 channels; these are 64kB each

#define EDMA_REGS_BASE_ADDR 0x4020C000U
/* Covers management registers plus REG_PROT GCR mirror window up to TCD base. */
#define EDMA_REGS_SIZE 0x00004000U
/* REG_PROT GCR slot used by MCAL: DMA_PROT_MEM(4) * GCR_OFFSET(0x900) = 0x2400. */
#define EDMA_REGPROT_GCR_OFF 0x00002400U

#define EDMA_TCD1_BASE_ADDR 0x40210000U
#define EDMA_TCD1_SIZE 0x0003C04FU // (0x4024C04E - 0x40210000 + 1)
#define EDMA_TCD2_BASE_ADDR (EDMA_TCD1_BASE_ADDR + 0x00200000U)
#define EDMA_TCD2_SIZE 0x0004C03FU // (0x4045C03E - 0x40410000 + 1)

#define S32K358_EDMA_NAME "S32K358_eDMA" //NOTE This name can't have spaces
#define TYPE_S32K358_DMA "s32k358-dma"
/* 32 bytes structure to handle byte transfers in eDMA */
typedef struct TCD{
    uint32_t SADDR;
    uint16_t SOFF;
    uint16_t ATTR;
    uint32_t NBYTES;
    int32_t SLAST_SDA;
    uint32_t DADDR;
    uint16_t DOFF;
    uint16_t CITER;
    int32_t DLAST_SGA;
    uint16_t CSR;
    uint16_t BITER;
} TCD;
typedef struct S32K358DMAChannel{
    uint32_t CSR;
    /* Channel Error Status Register */
    uint32_t ES;
    /* Channel Interrupt Status Register */
    uint32_t INT;
    /* Channel System Bus Register*/
    uint32_t SBR;
    /* Channel Priority */
    uint32_t PRI;

    TCD tcd;

} S32K358DMAChannel;

typedef struct S32K358DMAState {
    SysBusDevice parent_obj;
    /* The TCDs are stored in an internal memory region of eDMA */
    MemoryRegion tcd_region[2];
    S32K358DMAChannel channels[S32K358_NUM_DMA_CH];

    /* Define eDMA configuration */
    MemoryRegion registers;

    uint32_t reg_csr; // +0h
    uint32_t reg_es; // +4h
    uint32_t reg_int; // +8h
    uint32_t reg_hrs; // +Ch
    uint32_t regprot_gcr; // +0x2400h (REG_PROT GCR mirror)
    uint32_t reg_ch_gpri[S32K358_NUM_DMA_CH];


    qemu_irq irq[S32K358_NUM_DMA_CH];
    qemu_irq error_irq;

    AddressSpace system_as;
    MemoryRegion *system_memory;
} S32K358DMAState;


void s32k358_dma_preemption(S32K358DMAState* s, S32K358DMAChannel* ch);
void s32k358_dma_transfer(S32K358DMAState* s, S32K358DMAChannel* ch);
void s32k358_dma_hw_request(uint32_t ch_num);

OBJECT_DECLARE_SIMPLE_TYPE(S32K358DMAState, S32K358_DMA)
#endif
