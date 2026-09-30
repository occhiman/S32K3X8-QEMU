/*
 * S32K358 UART
 *
 * Copyright (c) 2025 Emiliano Salvetto <em1l14n0s@gmail.com>
 * SPDX-License-Identifier: CC-BY-NC-4.0
 *
 * This work is licensed under the terms of the Creative Commons Attribution Non Commercial 4.0 International
 * See the COPYING file in the top-level directory.
 */
#include "hw/dma/s32k358_dma.h"
#include "hw/ssi/s32k358_spi.h"
#include "exec/memattrs.h"
#include "exec/address-spaces.h"
#define DEBUG_DMA_TCD
#define S32K358_DMA_DEBUG 1
/* Those callbacks are made to set the registers of the eDMA engine */

#define DB_PRINT_L(lvl, fmt, args...) do { \
    if (S32K358_DMA_DEBUG >= lvl) { \
        qemu_log("[QEMU/S32K358-DMA] %s: " fmt, __func__, ## args); \
    } \
} while (0)

#define DB_PRINT(fmt, args...) DB_PRINT_L(1, fmt, ## args)

static S32K358DMAState *g_s32k358_dma = NULL;
static const hwaddr g_s32k358_lpspi4_tdr_addr = 0x404BC064U;
static const hwaddr g_s32k358_lpspi4_rdr_addr = 0x404BC074U;

MemTxResult s32k3x8_lpspi_dma_read(hwaddr addr, uint8_t *buf, uint32_t size);
MemTxResult s32k3x8_lpspi_dma_write(hwaddr addr, const uint8_t *buf, uint32_t size);
static const char *dma_handle_rw_error(MemTxResult result);

static inline int s32k358_dma_channel_index(const S32K358DMAState *s,
                                            const S32K358DMAChannel *ch)
{
    return (int)(ch - &s->channels[0]);
}

static MemTxResult s32k358_dma_read_source(S32K358DMAState *s,
                                           hwaddr           addr,
                                           uint8_t         *buf,
                                           uint32_t         size)
{
    MemTxResult res;
    uint32_t value = 0U;

    if ((g_s32k358_lpspi4_rdr_addr == addr) && (size <= 4U)) {
        res = s32k3x8_lpspi_dma_read(addr, buf, size);
        DB_PRINT("Direct LPSPI4 RDR DMA read addr=0x%08X size=%u -> %s\n",
                 (uint32_t)addr,
                 size,
                 dma_handle_rw_error(res));
        return res;
    }

    if ((g_s32k358_lpspi4_rdr_addr == addr) && (size < 4U)) {
        res = address_space_read(&s->system_as, addr, MEMTXATTRS_UNSPECIFIED, &value, 4U);
        if (MEMTX_OK == res) {
            memcpy(buf, &value, size);
        }
    } else {
        res = address_space_read(&s->system_as, addr, MEMTXATTRS_UNSPECIFIED, buf, size);
    }

    return res;
}

static MemTxResult s32k358_dma_write_target(S32K358DMAState *s,
                                            hwaddr           addr,
                                            const uint8_t   *buf,
                                            uint32_t         size)
{
    MemTxResult res;
    uint32_t value32 = 0U;
    bool lpspi_tdr_byte_write;

    if ((g_s32k358_lpspi4_tdr_addr == addr) && (size <= 4U)) {
        res = s32k3x8_lpspi_dma_write(addr, buf, size);
        DB_PRINT("Direct LPSPI4 TDR DMA write addr=0x%08X size=%u -> %s\n",
                 (uint32_t)addr,
                 size,
                 dma_handle_rw_error(res));
        return res;
    }

    lpspi_tdr_byte_write = ((g_s32k358_lpspi4_tdr_addr == addr) && (size < 4U));
    if (true == lpspi_tdr_byte_write) {
        memcpy(&value32, buf, size);
        buf = (const uint8_t *)&value32;
        size = 4U;
    }

    res = address_space_write(&s->system_as, addr, MEMTXATTRS_UNSPECIFIED, buf, size);
    if ((MEMTX_ACCESS_ERROR == res) && (size <= 4U)) {
        MemoryRegion *mr;
        hwaddr local_addr;

        mr = address_space_translate(&s->system_as,
                                     addr,
                                     &local_addr,
                                     NULL,
                                     true,
                                     MEMTXATTRS_UNSPECIFIED);
        if ((NULL != mr) && memory_region_is_ram(mr) == false) {
            uint64_t value = 0U;
            memcpy(&value, buf, size);
            res = memory_region_dispatch_write(mr,
                                               local_addr,
                                               value,
                                               MO_LE | size_memop(size),
                                               MEMTXATTRS_UNSPECIFIED);
        }
    }

    return res;
}

static void s32k358_dma_update_channel_irq(S32K358DMAState *s, int ch_num)
{
    uint32_t pending = s->channels[ch_num].INT & 0x1U;

    if (pending) {
        s->reg_int |= (1U << ch_num);
    } else {
        s->reg_int &= ~(1U << ch_num);
    }

    DB_PRINT("IRQ update ch=%d pending=%u reg_int=0x%08X\n",
             ch_num,
             pending ? 1U : 0U,
             s->reg_int);
    qemu_set_irq(s->irq[ch_num], pending ? 1 : 0);
}

void s32k358_dma_hw_request(uint32_t ch_num)
{
    S32K358DMAChannel *ch;

    if ((NULL == g_s32k358_dma) || (S32K358_NUM_DMA_CH <= ch_num)) {
        return;
    }

    ch = &g_s32k358_dma->channels[ch_num];
    DB_PRINT("HW request ch=%d CSR=0x%08X TCD_CSR=0x%04X SADDR=0x%08X DADDR=0x%08X NBYTES=%u CITER=0x%04X BITER=0x%04X\n",
             (int)ch_num,
             ch->CSR,
             ch->tcd.CSR,
             ch->tcd.SADDR,
             ch->tcd.DADDR,
             ch->tcd.NBYTES,
             ch->tcd.CITER,
             ch->tcd.BITER);
    if (0U != (ch->CSR & (1U << 30))) {
        DB_PRINT("HW request ignored for done channel:%d CSR=0x%08X\n",
                 (int)ch_num,
                 ch->CSR);
        return;
    }
    if ((0U != (ch->CSR & 0x1U)) && (0U == (ch->tcd.CSR & 0x1U))) {
        g_s32k358_dma->reg_hrs |= (1U << ch_num);
        DB_PRINT("Hardware request start for the channel:%d\n", (int)ch_num);
        s32k358_dma_preemption(g_s32k358_dma, ch);
        g_s32k358_dma->reg_hrs &= ~(1U << ch_num);
    }
}

static void s32k358_dma_write (void *opaque, hwaddr addr, uint64_t val, unsigned size){
    S32K358DMAState* s = S32K358_DMA(opaque);
    switch(addr){
        case CSR_OFF:
            s->reg_csr=val;
            break;
        case ES_OFF:
            s->reg_es=val;
            break;
        case INT_OFF:
            s->reg_int=val;
            break;
        case HRS_OFF:
            s->reg_hrs=val;
            break;
        case EDMA_REGPROT_GCR_OFF:
            s->regprot_gcr = (uint32_t)val;
            break;
        default:
            long chNum = (addr - CH_GRPRI_OFF) / 4;
            if( chNum >= 0 && chNum < S32K358_NUM_DMA_CH ){
                s->reg_ch_gpri[chNum]=val;
            }
            break;
    }
}

static uint64_t s32k358_dma_read (void *opaque, hwaddr addr, unsigned size){
    S32K358DMAState* s = S32K358_DMA(opaque);
    switch(addr){
        case CSR_OFF:
            return s->reg_csr;
        case ES_OFF:
            return s->reg_es;
        case INT_OFF:
            return s->reg_int;
        case HRS_OFF:
            return s->reg_hrs;
        case EDMA_REGPROT_GCR_OFF:
            return s->regprot_gcr;
        default:
            long chNum = (addr - CH_GRPRI_OFF) / 4;
            if( chNum >= 0 && chNum < S32K358_NUM_DMA_CH ){
                return s->reg_ch_gpri[chNum];
            }
            break;
    }
    return 0;
}

static const MemoryRegionOps s32k358_dma_ops = {
    .read = s32k358_dma_read,
    .write = s32k358_dma_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const char* dma_handle_rw_error(MemTxResult result){
    switch(result){
        case MEMTX_OK:
            return "DMA R/W OK";
        case MEMTX_ERROR:
            return "Device returned error";
        case MEMTX_DECODE_ERROR:
            return "Nothing at that address error";
        case MEMTX_ACCESS_ERROR:
            return "Access denied error";
        default:
            break;
    }
    return "Undefinied error";
}
void s32k358_dma_preemption(S32K358DMAState* s, S32K358DMAChannel* ch){
    /* If the eDMA is idle there is no preemption and the transfer can start immediately */
    DB_PRINT("DMA CSR:%d\n",s->reg_csr);
    if( ( DMA_IS_ACTIVE(s->reg_csr) ) == 0 ){
        s32k358_dma_transfer(s, ch);
    }
    else{
        DB_PRINT("DMA is already active!\n");
    }
    return;
}
void s32k358_dma_transfer(S32K358DMAState* s, S32K358DMAChannel* ch){
    bool software_start;
    bool transfer_complete;
    int16_t linkCh = -1;
    int ch_num = s32k358_dma_channel_index(s, ch);
    uint32_t minor_bytes;
    uint32_t transfer_bytes;
    uint32_t request_iterations;
    uint32_t src_step;
    uint32_t dst_step;
    DB_PRINT("---[QEMU] TCD Debug:\n\nCH:%d\nSADDR:%d\nDADDR:%d\nCITER:%d\nBITER:%d\nNBYTES:%d\nSOFF:%d\nDOFF:%d\nSLAST:%d\nDLAST:%d\n---\n", ch_num, ch->tcd.SADDR, ch->tcd.DADDR, ch->tcd.CITER, ch->tcd.BITER, ch->tcd.NBYTES, ch->tcd.SOFF, ch->tcd.DOFF, ch->tcd.SLAST_SDA, ch->tcd.DLAST_SGA);
    software_start = ((ch->tcd.CSR & 0x1U) != 0U);
    transfer_complete = false;
    if ( CH_TCD_GET_ELINK(ch->tcd.CITER) != CH_TCD_GET_ELINK(ch->tcd.BITER) ){
        DB_PRINT("DMA CITER/BITER Configuration error, Aborting...\n");
        return;
    }
    ch->CSR = CH_CSR_SET_DONE(ch->CSR, 0);
    if (true == software_start) {
        /* Clear software START only for software-triggered transfers. */
        ch->tcd.CSR &= (uint16_t)~0x1U;
    }
    ch->CSR = CH_CSR_SET_ACTIVE(ch->CSR, 1);
    /* If the ELINK flag is ENABLED */
    if( CH_TCD_GET_ELINK(ch->tcd.CITER) ){
       /* 9-13 LINKCH */
        linkCh = CH_TCD_GET_LINKCH(ch->tcd.CITER);
        ch->tcd.CITER = CH_TCD_GET_CITER(ch->tcd.CITER);
    }
    else
        ch->tcd.CITER = CH_TCD_GET_CITER(ch->tcd.CITER);
    MemTxResult res;
    minor_bytes = ch->tcd.NBYTES;
    transfer_bytes = minor_bytes;
    src_step = (0U != ch->tcd.SOFF) ? (uint32_t)ch->tcd.SOFF : minor_bytes;
    dst_step = (0U != ch->tcd.DOFF) ? (uint32_t)ch->tcd.DOFF : minor_bytes;
    if (0U == transfer_bytes) {
        transfer_bytes = (src_step > dst_step) ? src_step : dst_step;
    }
    if (0U == transfer_bytes) {
        DB_PRINT("DMA transfer size is zero, Aborting...\n");
        ch->CSR = CH_CSR_SET_ACTIVE(ch->CSR,0);
        return;
    }
    request_iterations = (true == software_start) ? (uint32_t)ch->tcd.CITER : 1U;
    char* buf = g_new0(char, transfer_bytes);
    for (; (request_iterations > 0U) && (ch->tcd.CITER > 0U); request_iterations--) {
        DB_PRINT("minor start ch=%d SADDR=0x%08X DADDR=0x%08X bytes=%u\n",
                 ch_num,
                 ch->tcd.SADDR,
                 ch->tcd.DADDR,
                 transfer_bytes);
        res = s32k358_dma_read_source(s, ch->tcd.SADDR, (uint8_t *)buf, transfer_bytes);
        DB_PRINT("Address space read:%s\n", dma_handle_rw_error(res));
        res = s32k358_dma_write_target(s, ch->tcd.DADDR, (const uint8_t *)buf, transfer_bytes);
        DB_PRINT("Address space write:%s\n", dma_handle_rw_error(res));
        ch->tcd.SADDR += ch->tcd.SOFF;
        ch->tcd.DADDR += ch->tcd.DOFF;
        ch->tcd.CITER--;

        if(ch->tcd.BITER > 1 && ch->tcd.CITER == (ch->tcd.BITER / 2) ){
            DB_PRINT("INTHALF Triggered\n");
        }
        if( linkCh >= 0 && (ch->tcd.CITER == 0U) ){
            DB_PRINT("Interrupt request to another channel\n");
        }
    }
    transfer_complete = (0U == ch->tcd.CITER);

    if (false == transfer_complete) {
        ch->CSR = CH_CSR_SET_ACTIVE(ch->CSR,0);
        DB_PRINT("request serviced ch=%d remaining CITER=%u\n",
                 ch_num,
                 ch->tcd.CITER);
        g_free(buf);
        return;
    }

    /* Linking to another channel defined in MAJORLINKCH via an internal mechanism
     * started by setting up TCDn_CSR[START] to 1 of desired channel */

    if( CH_TCD_GET_MAJORELINK( ch->tcd.CSR ) ){

        uint8_t majorlinkCh = CH_TCD_GET_MAJORLINKCH(ch->tcd.CSR);

        DB_PRINT("MAJORELINK: Linking to DMA Channel:%d\n",majorlinkCh);
        S32K358DMAChannel *newCh = &s->channels[majorlinkCh];
        newCh->tcd.CSR |= 0x1;
        s32k358_dma_transfer(s,newCh);

    }

    ch->tcd.SADDR += ch->tcd.SLAST_SDA;
    ch->tcd.DADDR += ch->tcd.DLAST_SGA;
    ch->tcd.CITER = ch->tcd.BITER;
    ch->CSR = CH_CSR_SET_DONE(ch->CSR,1);
    ch->CSR = CH_CSR_SET_ACTIVE(ch->CSR,0);
    ch->INT = CH_INT_SET_INT(ch->INT,1);
    DB_PRINT("transfer complete ch=%d INT=0x%08X CSR=0x%08X\n",
             ch_num,
             ch->INT,
             ch->CSR);
    s32k358_dma_update_channel_irq(s, ch_num);

    if(CH_TCD_GET_MAJORINT(ch->tcd.CSR)){
        DB_PRINT("Interrupt request to another channel\n");
    }
    DB_PRINT("s32k358_dma_transfer END\n");
    g_free(buf);
    return;
}
#define EDMA_TCD_CHANNEL_ADDR_SPACE 0x4000U

static uint32_t s32k358_tcd_read_word(const S32K358DMAChannel *ch, hwaddr reg_word)
{
    switch (reg_word) {
    case CH_CSR_OFF:
        return ch->CSR;
    case CH_ES_OFF:
        return ch->ES;
    case CH_INT_OFF:
        return ch->INT;
    case CH_SBR_OFF:
        return ch->SBR;
    case CH_PRI_OFF:
        return ch->PRI;
    case CH_TCD_SADDR_OFF:
        return ch->tcd.SADDR;
    case CH_TCD_SOFF_OFF:
        return ((uint32_t)ch->tcd.ATTR << 16) | ch->tcd.SOFF;
    case CH_TCD_NBYTES_MLOFF_OFF:
        return ch->tcd.NBYTES;
    case CH_TCD_SLAST_SDA_OFF:
        return (uint32_t)ch->tcd.SLAST_SDA;
    case CH_TCD_DADDR_OFF:
        return ch->tcd.DADDR;
    case CH_TCD_DOFF_OFF:
        return ((uint32_t)ch->tcd.CITER << 16) | ch->tcd.DOFF;
    case CH_TCD_DLAST_SGA_OFF:
        return (uint32_t)ch->tcd.DLAST_SGA;
    case CH_TCD_CSR_OFF:
        return ((uint32_t)ch->tcd.BITER << 16) | ch->tcd.CSR;
    default:
        /* Reserved / unimplemented offsets read as zero. */
        return 0;
    }
}

static void s32k358_tcd_apply_word_write(S32K358DMAState *s,
                                         int ch_num,
                                         hwaddr reg_word,
                                         uint32_t val)
{
    S32K358DMAChannel *ch = &s->channels[ch_num];

    switch (reg_word) {
    case CH_CSR_OFF:
    {
        uint32_t old = ch->CSR;

        /* Keep writable control bits and implement W1C semantics. */
        old = (old & ~0xFU) | (val & 0xFU);      /* ERQ/EARQ/EEI/EBW */
        if (val & (1U << 30)) {                  /* DONE is W1C */
            old &= ~(1U << 30);
        }
        ch->CSR = old;
        break;
    }
    case CH_ES_OFF:
        /* CH_ES is W1C. */
        ch->ES &= ~val;
        break;
    case CH_INT_OFF:
        /* CH_INT is W1C. */
        ch->INT &= ~(val & 0x1U);
        s32k358_dma_update_channel_irq(s, ch_num);
        break;
    case CH_SBR_OFF:
        ch->SBR = val;
        break;
    case CH_PRI_OFF:
        ch->PRI = val;
        break;
    case CH_TCD_SADDR_OFF:
        ch->tcd.SADDR = val;
        break;
    case CH_TCD_SOFF_OFF:
        ch->tcd.SOFF = (uint16_t)(val & 0xFFFFU);
        ch->tcd.ATTR = (uint16_t)((val >> 16) & 0xFFFFU);
        break;
    case CH_TCD_NBYTES_MLOFF_OFF:
        ch->tcd.NBYTES = val;
        break;
    case CH_TCD_SLAST_SDA_OFF:
        ch->tcd.SLAST_SDA = (int32_t)val;
        break;
    case CH_TCD_DADDR_OFF:
        ch->tcd.DADDR = val;
        break;
    case CH_TCD_DOFF_OFF:
        ch->tcd.DOFF = (uint16_t)(val & 0xFFFFU);
        ch->tcd.CITER = (uint16_t)((val >> 16) & 0xFFFFU);
        break;
    case CH_TCD_DLAST_SGA_OFF:
        ch->tcd.DLAST_SGA = (int32_t)val;
        break;
    case CH_TCD_CSR_OFF:
    {
        uint16_t old_csr = ch->tcd.CSR;

        ch->tcd.CSR = (uint16_t)(val & 0xFFFFU);
        ch->tcd.BITER = (uint16_t)((val >> 16) & 0xFFFFU);

        /* START is software trigger, act on 0->1 transitions. */
        if (((old_csr & 1U) == 0U) && ((ch->tcd.CSR & 1U) != 0U)) {
            DB_PRINT("Requested start for the channel:%d\n", ch_num);
            s32k358_dma_preemption(s, ch);
        }
        break;
    }
    default:
        /* Reserved / unimplemented offsets ignore writes. */
        break;
    }
}

#define EDMA_TCD0_CHANNEL_BASE 0U
#define EDMA_TCD0_CHANNEL_COUNT 12U
#define EDMA_TCD1_CHANNEL_BASE 12U
#define EDMA_TCD1_CHANNEL_COUNT 20U

static bool s32k358_tcd_decode_addr(hwaddr addr,
                                    unsigned channel_base,
                                    unsigned channel_count,
                                    int *ch_num,
                                    hwaddr *reg)
{
    unsigned local_channel = (unsigned)CH_GET_NUM(addr);
    unsigned global_channel;

    if (local_channel >= channel_count) {
        return false;
    }

    global_channel = channel_base + local_channel;
    if (global_channel >= S32K358_NUM_DMA_CH) {
        return false;
    }

    *ch_num = (int)global_channel;
    *reg = CH_GET_REG(addr);
    return true;
}

/* Those callbacks are made to set the channels registers (all mapped in TCD memory region) */
static void s32k358_tcd_write_common(void *opaque,
                                     hwaddr addr,
                                     uint64_t val,
                                     unsigned size,
                                     unsigned channel_base,
                                     unsigned channel_count)
{
    S32K358DMAState* s = S32K358_DMA(opaque);
    int ch_num;
    hwaddr reg;

    if (!s32k358_tcd_decode_addr(addr, channel_base, channel_count, &ch_num, &reg)) {
        return;
    }

    if ((size == 0) || (size > sizeof(uint32_t)) || (reg + size > EDMA_TCD_CHANNEL_ADDR_SPACE)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-dma: invalid TCD write reg=0x%" HWADDR_PRIx " size=%u (ch=%d)\n",
                      reg, size, ch_num);
        return;
    }

    for (unsigned i = 0; i < size; i++) {
        hwaddr byte_reg = reg + i;
        hwaddr reg_word = byte_reg & ~((hwaddr)0x3U);
        uint32_t word = s32k358_tcd_read_word(&s->channels[ch_num], reg_word);
        uint32_t shift = (uint32_t)(byte_reg & 0x3U) * 8U;
        uint32_t byte_val = (uint32_t)((val >> (8U * i)) & 0xFFU);

        word &= ~(0xFFU << shift);
        word |= (byte_val << shift);

        s32k358_tcd_apply_word_write(s, ch_num, reg_word, word);
    }
}

static uint64_t s32k358_tcd_read_common(void *opaque,
                                        hwaddr addr,
                                        unsigned size,
                                        unsigned channel_base,
                                        unsigned channel_count)
{
    S32K358DMAState* s = S32K358_DMA(opaque);
    int ch_num;
    hwaddr reg;
    uint64_t ret = 0;

    if (!s32k358_tcd_decode_addr(addr, channel_base, channel_count, &ch_num, &reg)) {
        return 0;
    }

    if ((size == 0) || (size > sizeof(uint32_t)) || (reg + size > EDMA_TCD_CHANNEL_ADDR_SPACE)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-dma: invalid TCD read reg=0x%" HWADDR_PRIx " size=%u (ch=%d)\n",
                      reg, size, ch_num);
        return 0;
    }

    for (unsigned i = 0; i < size; i++) {
        hwaddr byte_reg = reg + i;
        hwaddr reg_word = byte_reg & ~((hwaddr)0x3U);
        uint32_t word = s32k358_tcd_read_word(&s->channels[ch_num], reg_word);
        uint32_t shift = (uint32_t)(byte_reg & 0x3U) * 8U;
        uint64_t byte_val = (uint64_t)((word >> shift) & 0xFFU);

        ret |= (byte_val << (8U * i));
    }

    return ret;
}

static void s32k358_tcd0_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    s32k358_tcd_write_common(opaque, addr, val, size,
                             EDMA_TCD0_CHANNEL_BASE,
                             EDMA_TCD0_CHANNEL_COUNT);
}

static uint64_t s32k358_tcd0_read(void *opaque, hwaddr addr, unsigned size)
{
    return s32k358_tcd_read_common(opaque, addr, size,
                                   EDMA_TCD0_CHANNEL_BASE,
                                   EDMA_TCD0_CHANNEL_COUNT);
}

static void s32k358_tcd1_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    s32k358_tcd_write_common(opaque, addr, val, size,
                             EDMA_TCD1_CHANNEL_BASE,
                             EDMA_TCD1_CHANNEL_COUNT);
}

static uint64_t s32k358_tcd1_read(void *opaque, hwaddr addr, unsigned size)
{
    return s32k358_tcd_read_common(opaque, addr, size,
                                   EDMA_TCD1_CHANNEL_BASE,
                                   EDMA_TCD1_CHANNEL_COUNT);
}

static const MemoryRegionOps s32k358_tcd0_ops = {
    .read = s32k358_tcd0_read,
    .write = s32k358_tcd0_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps s32k358_tcd1_ops = {
    .read = s32k358_tcd1_read,
    .write = s32k358_tcd1_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};
static void s32k358_dma_init(Object* obj){
    S32K358DMAState* dma = S32K358_DMA(obj);
    SysBusDevice *d = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&dma->registers, obj, &s32k358_dma_ops, dma, "eDMA engine registers", EDMA_REGS_SIZE);
    sysbus_init_mmio(d, &dma->registers);

    memory_region_init_io(&dma->tcd_region[0], obj, &s32k358_tcd0_ops, dma, "eDMA TCDs local memory (first part)", EDMA_TCD1_SIZE);
    sysbus_init_mmio(d, &dma->tcd_region[0]);

    memory_region_init_io(&dma->tcd_region[1], obj, &s32k358_tcd1_ops, dma, "eDMA TCDs local memory (second part)", EDMA_TCD2_SIZE);
    sysbus_init_mmio(d, &dma->tcd_region[1]);

    for (int i = 0; i < S32K358_NUM_DMA_CH; i++) {
        sysbus_init_irq(d, &dma->irq[i]);
    }
    sysbus_init_irq(d, &dma->error_irq);
}

static Property s32k358_dma_properties[] = {
    DEFINE_PROP_LINK("memory", S32K358DMAState, system_memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_END_OF_LIST(),
};

static void s32k358_dma_realize(DeviceState *dev, Error **errp){
    S32K358DMAState *s = S32K358_DMA(dev);
    if (!s->system_memory) {
        error_setg(errp, "s32k358-dma: memory property not set");
        return;
    }

    s->regprot_gcr = 0x0;
    g_s32k358_dma = s;

    // Create address space from memory region
    address_space_init(&s->system_as, s->system_memory, "eDMA-AddressSpace");

    // Setup registers...
    /* Reset values from documentation */
    for(int i=0; i < S32K358_NUM_DMA_CH; i++){
        s->channels[i].CSR = 0x0;
        s->channels[i].ES = 0x0;
        s->channels[i].INT = 0x0;
        s->channels[i].SBR = 0x00008002;
        s->channels[i].PRI = 0x0;
        s->channels[i].tcd.SADDR = 0x0;
        s->channels[i].tcd.SOFF = 0x0;
        s->channels[i].tcd.ATTR = 0x0;
        s->channels[i].tcd.NBYTES = 0x0;
        s->channels[i].tcd.CSR = 0x0;
        s->channels[i].tcd.SLAST_SDA = 0x0;
        s->channels[i].tcd.DADDR = 0x0;
        s->channels[i].tcd.DOFF = 0x0;
        s->channels[i].tcd.CITER = 0x0;
        s->channels[i].tcd.DLAST_SGA = 0x0;
        s->channels[i].tcd.CSR = 0x0;
        s->channels[i].tcd.BITER = 0x0;
    }
}

static void s32k358_dma_class_init(ObjectClass *klass, void *data){
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->desc = S32K358_EDMA_NAME;
    dc->realize = s32k358_dma_realize;
    device_class_set_props(dc, s32k358_dma_properties);
}

static const TypeInfo s32k358_dma_info = {
    .name = TYPE_S32K358_DMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S32K358DMAState),
    .instance_init = s32k358_dma_init,
    .class_init = s32k358_dma_class_init,
};

static void s32k358_dma_register_types(void)
{
    type_register_static(&s32k358_dma_info);
}

type_init(s32k358_dma_register_types)


