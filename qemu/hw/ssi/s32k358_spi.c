/*
 * S32K3X8 LPSPI (Low Power Serial Peripheral Interface) Emulation
 * Based on S32K3xx Reference Manual, Rev. 9, 07/2024
 * 
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/dma/s32k358_dmamux.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "s32k358_spi.h"
#include "trace.h"

#define S32K358_LPSPI_DEBUG 1

#define LPSPI_PRINT(fmt, args...) do { \
    if (S32K358_LPSPI_DEBUG >= 1) { \
        qemu_log("[QEMU/S32K358-LPSPI] %s: " fmt, __func__, ## args); \
    } \
} while (0)

static uint32_t g_s32k3x8_lpspi_instance_seed = 0U;
static S32K3X8LPSPIState *g_s32k3x8_lpspi4 = NULL;
static const hwaddr g_s32k3x8_lpspi4_base = 0x404BC000U;
static const hwaddr g_s32k3x8_lpspi4_size = 0x00001000U;

static void s32k3x8_lpspi_dispatch_dma_requests(S32K3X8LPSPIState *s)
{
    bool tx_request;
    bool rx_request;

    if (4U != s->instance) {
        return;
    }

    if (true == s->dma_dispatch_active) {
        return;
    }

    s->dma_dispatch_active = true;
    do {
        tx_request = false;
        rx_request = false;

        if ((true == s->dma_tx_pending) &&
            (0U != (s->der & LPSPI_DER_TDDE)) &&
            (0U != (s->sr & LPSPI_SR_TDF))) {
            s->dma_tx_pending = false;
            tx_request = true;
        }
        if ((true == s->dma_rx_pending) &&
            (0U != (s->der & LPSPI_DER_RDDE)) &&
            (0U != (s->sr & LPSPI_SR_RDF))) {
            s->dma_rx_pending = false;
            rx_request = true;
        }

        if ((false == tx_request) && (false == rx_request)) {
            break;
        }

        if (true == tx_request) {
            s32k358_dmamux_request(S32K358_DMAMUX_INSTANCE_1,
                                   S32K358_DMAMUX1_LPSPI4_TX_REQUEST);
        }
        if (true == rx_request) {
            s32k358_dmamux_request(S32K358_DMAMUX_INSTANCE_1,
                                   S32K358_DMAMUX1_LPSPI4_RX_REQUEST);
        }
    } while ((true == s->dma_tx_pending) || (true == s->dma_rx_pending));

    s->dma_dispatch_active = false;
}

static void s32k3x8_lpspi_raise_dma_requests(S32K3X8LPSPIState *s,
                                             bool                tdf_rising,
                                             bool                rdf_rising)
{
    if (4U != s->instance) {
        return;
    }

    if ((true == tdf_rising) && (0U != (s->der & LPSPI_DER_TDDE))) {
        LPSPI_PRINT("instance=%u TDF rising -> DMA TX request\n", s->instance);
        s->dma_tx_pending = true;
    }
    if ((true == rdf_rising) && (0U != (s->der & LPSPI_DER_RDDE))) {
        LPSPI_PRINT("instance=%u RDF rising -> DMA RX request\n", s->instance);
        s->dma_rx_pending = true;
    }

    s32k3x8_lpspi_dispatch_dma_requests(s);
}

static void s32k3x8_lpspi_kick_dma_requests(S32K3X8LPSPIState *s)
{
    bool tdf_asserted;
    bool rdf_asserted;

    if (4U != s->instance) {
        return;
    }

    tdf_asserted = (0U != (s->sr & LPSPI_SR_TDF));
    rdf_asserted = (0U != (s->sr & LPSPI_SR_RDF));

    if ((true == tdf_asserted) && (0U != (s->der & LPSPI_DER_TDDE))) {
        LPSPI_PRINT("instance=%u TDF asserted -> DMA TX kick\n", s->instance);
        s->dma_tx_pending = true;
    }
    if ((true == rdf_asserted) && (0U != (s->der & LPSPI_DER_RDDE))) {
        LPSPI_PRINT("instance=%u RDF asserted -> DMA RX kick\n", s->instance);
        s->dma_rx_pending = true;
    }

    s32k3x8_lpspi_dispatch_dma_requests(s);
}

/* FIFO operation functions */
static bool fifo_is_empty(S32K3X8LPSPIState *s, bool tx)
{
    if (tx) {
        return s->tx_fifo.level == 0;
    } else {
        return s->rx_fifo.level == 0;
    }
}

static bool fifo_is_full(S32K3X8LPSPIState *s, bool tx)
{
    if (tx) {
        return s->tx_fifo.level >= LPSPI_FIFO_SIZE;
    } else {
        return s->rx_fifo.level >= LPSPI_FIFO_SIZE;
    }
}

static void fifo_push(S32K3X8LPSPIState *s, uint32_t data, bool tx)
{
    if (tx) {
        if (!fifo_is_full(s, true)) {
            s->tx_fifo.data[s->tx_fifo.tail] = data;
            s->tx_fifo.tail = (s->tx_fifo.tail + 1) % LPSPI_FIFO_SIZE;
            s->tx_fifo.level++;
        }
    } else {
        if (!fifo_is_full(s, false)) {
            s->rx_fifo.data[s->rx_fifo.tail] = data;
            s->rx_fifo.tail = (s->rx_fifo.tail + 1) % LPSPI_FIFO_SIZE;
            s->rx_fifo.level++;
        }
    }
}

static uint32_t fifo_pop(S32K3X8LPSPIState *s, bool tx)
{
    uint32_t data = 0;
    
    if (tx) {
        if (!fifo_is_empty(s, true)) {
            data = s->tx_fifo.data[s->tx_fifo.head];
            s->tx_fifo.head = (s->tx_fifo.head + 1) % LPSPI_FIFO_SIZE;
            s->tx_fifo.level--;
        }
    } else {
        if (!fifo_is_empty(s, false)) {
            data = s->rx_fifo.data[s->rx_fifo.head];
            s->rx_fifo.head = (s->rx_fifo.head + 1) % LPSPI_FIFO_SIZE;
            s->rx_fifo.level--;
        }
    }
    
    return data;
}

static void fifo_reset(S32K3X8LPSPIState *s, bool tx)
{
    if (tx) {
        s->tx_fifo.level = 0;
        s->tx_fifo.head = 0;
        s->tx_fifo.tail = 0;
        memset(s->tx_fifo.data, 0, sizeof(s->tx_fifo.data));
    } else {
        s->rx_fifo.level = 0;
        s->rx_fifo.head = 0;
        s->rx_fifo.tail = 0;
        memset(s->rx_fifo.data, 0, sizeof(s->rx_fifo.data));
    }
}

/* Update status register */
static void s32k3x8_lpspi_update_status(S32K3X8LPSPIState *s)
{
    bool tdf_set;
    bool rdf_set;
    bool tdf_rising;
    bool rdf_rising;

    /* Update FIFO status */
    uint32_t txwater = s->fcr & 0x3;  /* TX watermark */
    uint32_t rxwater = (s->fcr >> 16) & 0x3;  /* RX watermark */
    
    /* TDF: Transmit Data Flag */
    tdf_set = (s->tx_fifo.level <= txwater);
    if (true == tdf_set) {
        s->sr |= LPSPI_SR_TDF;
    } else {
        s->sr &= ~LPSPI_SR_TDF;
    }
    
    /* RDF: Receive Data Flag */
    rdf_set = (s->rx_fifo.level > rxwater);
    if (true == rdf_set) {
        s->sr |= LPSPI_SR_RDF;
    } else {
        s->sr &= ~LPSPI_SR_RDF;
    }
    
    /* Update FIFO Status Register */
    s->fsr = (s->rx_fifo.level << 16) | s->tx_fifo.level;
    tdf_rising = ((true == tdf_set) && (false == s->prev_tdf));
    rdf_rising = ((true == rdf_set) && (false == s->prev_rdf));
    s->prev_tdf = tdf_set;
    s->prev_rdf = rdf_set;
    s32k3x8_lpspi_raise_dma_requests(s, tdf_rising, rdf_rising);
}

/* Update interrupt status */
static void s32k3x8_lpspi_update_irq(S32K3X8LPSPIState *s)
{
    bool irq_state = false;
    
    /* Check various interrupt conditions */
    if ((s->ier & 0x1) && (s->sr & LPSPI_SR_TDF)) {        /* TDIE & TDF */
        irq_state = true;
    }
    if ((s->ier & 0x2) && (s->sr & LPSPI_SR_RDF)) {        /* RDIE & RDF */
        irq_state = true;
    }
    if ((s->ier & 0x100) && (s->sr & LPSPI_SR_WCF)) {      /* WCIE & WCF */
        irq_state = true;
    }
    if ((s->ier & 0x200) && (s->sr & LPSPI_SR_FCF)) {      /* FCIE & FCF */
        irq_state = true;
    }
    if ((s->ier & 0x400) && (s->sr & LPSPI_SR_TCF)) {      /* TCIE & TCF */
        irq_state = true;
    }
    
    qemu_set_irq(s->irq, irq_state);
}

/* SPI transfer processing */
static void s32k3x8_lpspi_do_transfer(S32K3X8LPSPIState *s)
{
    if (!s->enabled || !(s->cr & LPSPI_CR_MEN)) {
        return;
    }
    
    /* Simple loopback test implementation */
    if (!fifo_is_empty(s, true) && !fifo_is_full(s, false)) {
        uint32_t tx_data = fifo_pop(s, true);
        
        /* In loopback mode, transmitted data is directly used as received data */
        fifo_push(s, tx_data, false);
        
        /* Set transfer complete flags */
        s->sr |= LPSPI_SR_WCF;  /* Word Complete */
        s->sr |= LPSPI_SR_FCF;  /* Frame Complete */
        
        if (fifo_is_empty(s, true)) {
            s->sr |= LPSPI_SR_TCF;  /* Transfer Complete */
        }
    }
    
    s32k3x8_lpspi_update_status(s);
    s32k3x8_lpspi_kick_dma_requests(s);
    s32k3x8_lpspi_update_irq(s);
}

/* Register read operation */
static uint64_t s32k3x8_lpspi_read(void *opaque, hwaddr addr, unsigned size)
{
    S32K3X8LPSPIState *s = S32K3X8_LPSPI(opaque);
    uint64_t ret = 0;
    
    switch (addr) {
    case LPSPI_VERID_OFFSET:
        ret = s->verid;
        break;
    case LPSPI_PARAM_OFFSET:
        ret = s->param;
        break;
    case LPSPI_CR_OFFSET:
        ret = s->cr;
        break;
    case LPSPI_SR_OFFSET:
        s32k3x8_lpspi_update_status(s);
        ret = s->sr;
        break;
    case LPSPI_IER_OFFSET:
        ret = s->ier;
        break;
    case LPSPI_DER_OFFSET:
        ret = s->der;
        break;
    case LPSPI_CFGR0_OFFSET:
        ret = s->cfgr0;
        break;
    case LPSPI_CFGR1_OFFSET:
        ret = s->cfgr1;
        break;
    case LPSPI_DMR0_OFFSET:
        ret = s->dmr0;
        break;
    case LPSPI_DMR1_OFFSET:
        ret = s->dmr1;
        break;
    case LPSPI_CCR_OFFSET:
        ret = s->ccr;
        break;
    case LPSPI_CCR1_OFFSET:
        ret = s->ccr1;
        break;
    case LPSPI_FCR_OFFSET:
        ret = s->fcr;
        break;
    case LPSPI_FSR_OFFSET:
        s32k3x8_lpspi_update_status(s);
        ret = s->fsr;
        break;
    case LPSPI_TCR_OFFSET:
        ret = s->tcr;
        break;
    case LPSPI_TDR_OFFSET:
        /* Write-only register, read returns 0 */
        ret = 0;
        break;
    case LPSPI_RSR_OFFSET:
        /* Receive status register */
        ret = s->rsr;
        if (fifo_is_empty(s, false)) {
            ret |= 0x2;  /* RXEMPTY */
        }
        break;
    case LPSPI_RDR_OFFSET:
        if (!fifo_is_empty(s, false)) {
            ret = fifo_pop(s, false);
            s32k3x8_lpspi_update_status(s);
            s32k3x8_lpspi_update_irq(s);
        }
        break;
    case LPSPI_RDROR_OFFSET:
        /* Read-only, does not remove data from FIFO */
        if (!fifo_is_empty(s, false)) {
            ret = s->rx_fifo.data[s->rx_fifo.head];
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k3x8_lpspi: Read from invalid offset 0x%"HWADDR_PRIx"\n",
                      addr);
        break;
    }
    
    return ret;
}

/* Register write operation */
static void s32k3x8_lpspi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S32K3X8LPSPIState *s = S32K3X8_LPSPI(opaque);
    
    switch (addr) {
    case LPSPI_CR_OFFSET:
        s->cr = val;
        
        /* Handle module enable */
        if (val & LPSPI_CR_MEN) {
            s->enabled = true;
        } else {
            s->enabled = false;
        }
        
        /* Handle software reset */
        if (val & LPSPI_CR_RST) {
            /* Reset all registers except control register */
            s->sr = 0x1;  /* Default TDF=1 */
            fifo_reset(s, true);
            fifo_reset(s, false);
        }
        
        /* Handle FIFO reset */
        if (val & LPSPI_CR_RTF) {
            fifo_reset(s, true);
        }
        if (val & LPSPI_CR_RRF) {
            fifo_reset(s, false);
        }
        
        s32k3x8_lpspi_update_status(s);
        s32k3x8_lpspi_update_irq(s);
        break;
        
    case LPSPI_SR_OFFSET:
        /* Write 1 to clear status bits */
        s->sr &= ~(val & 0x3F00);  /* Clear clearable status bits */
        s32k3x8_lpspi_update_irq(s);
        break;
        
    case LPSPI_IER_OFFSET:
        s->ier = val;
        s32k3x8_lpspi_update_irq(s);
        break;
        
    case LPSPI_DER_OFFSET:
        s->der = val;
        LPSPI_PRINT("instance=%u DER=0x%08X SR=0x%08X FSR=0x%08X\n",
                    s->instance,
                    s->der,
                    s->sr,
                    s->fsr);
        s32k3x8_lpspi_update_status(s);
        s32k3x8_lpspi_kick_dma_requests(s);
        break;
        
    case LPSPI_CFGR0_OFFSET:
        s->cfgr0 = val;
        break;
        
    case LPSPI_CFGR1_OFFSET:
        s->cfgr1 = val;
        s->master_mode = !!(val & LPSPI_CFGR1_MASTER);
        break;
        
    case LPSPI_DMR0_OFFSET:
        s->dmr0 = val;
        break;
        
    case LPSPI_DMR1_OFFSET:
        s->dmr1 = val;
        break;
        
    case LPSPI_CCR_OFFSET:
        s->ccr = val;
        break;
        
    case LPSPI_CCR1_OFFSET:
        s->ccr1 = val;
        break;
        
    case LPSPI_FCR_OFFSET:
        s->fcr = val;
        s32k3x8_lpspi_update_status(s);
        s32k3x8_lpspi_update_irq(s);
        break;
        
    case LPSPI_TCR_OFFSET:
        s->tcr = val;
        /* Extract frame size */
        s->transfer_size = (val & LPSPI_TCR_FRAMESZ_MASK) + 1;
        break;
        
    case LPSPI_TDR_OFFSET:
        if (!fifo_is_full(s, true)) {
            LPSPI_PRINT("instance=%u TDR write val=0x%08X tx_level=%u rx_level=%u\n",
                        s->instance,
                        (uint32_t)val,
                        s->tx_fifo.level,
                        s->rx_fifo.level);
            fifo_push(s, val, true);
            s32k3x8_lpspi_do_transfer(s);
        }
        break;
        
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k3x8_lpspi: Write to invalid offset 0x%"HWADDR_PRIx"\n",
                      addr);
        break;
    }
}

static S32K3X8LPSPIState *s32k3x8_lpspi_dma_lookup(hwaddr addr, hwaddr *offset)
{
    S32K3X8LPSPIState *s = NULL;
    bool addr_in_lpspi4;

    addr_in_lpspi4 = ((addr >= g_s32k3x8_lpspi4_base) &&
                      (addr < (g_s32k3x8_lpspi4_base + g_s32k3x8_lpspi4_size)));
    if (true == addr_in_lpspi4) {
        s = g_s32k3x8_lpspi4;
        *offset = addr - g_s32k3x8_lpspi4_base;
    }

    return s;
}

MemTxResult s32k3x8_lpspi_dma_read(hwaddr addr, uint8_t *buf, uint32_t size)
{
    S32K3X8LPSPIState *s;
    hwaddr offset = 0U;
    uint64_t value;

    if ((NULL == buf) || (0U == size) || (size > 4U)) {
        return MEMTX_ACCESS_ERROR;
    }

    s = s32k3x8_lpspi_dma_lookup(addr, &offset);
    if (NULL == s) {
        return MEMTX_ACCESS_ERROR;
    }

    value = s32k3x8_lpspi_read(s, offset, size);
    memcpy(buf, &value, size);
    LPSPI_PRINT("DMA direct read addr=0x%08" HWADDR_PRIx " off=0x%02" HWADDR_PRIx " size=%u val=0x%08X\n",
                addr,
                offset,
                size,
                (uint32_t)value);

    return MEMTX_OK;
}

MemTxResult s32k3x8_lpspi_dma_write(hwaddr addr, const uint8_t *buf, uint32_t size)
{
    S32K3X8LPSPIState *s;
    hwaddr offset = 0U;
    uint32_t value = 0U;

    if ((NULL == buf) || (0U == size) || (size > 4U)) {
        return MEMTX_ACCESS_ERROR;
    }

    s = s32k3x8_lpspi_dma_lookup(addr, &offset);
    if (NULL == s) {
        return MEMTX_ACCESS_ERROR;
    }

    memcpy(&value, buf, size);
    LPSPI_PRINT("DMA direct write addr=0x%08" HWADDR_PRIx " off=0x%02" HWADDR_PRIx " size=%u val=0x%08X\n",
                addr,
                offset,
                size,
                value);
    s32k3x8_lpspi_write(s, offset, value, size);

    return MEMTX_OK;
}

static const MemoryRegionOps s32k3x8_lpspi_ops = {
    .read = s32k3x8_lpspi_read,
    .write = s32k3x8_lpspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    }
};

/* Device reset */
static void s32k3x8_lpspi_reset(DeviceState *dev)
{
    S32K3X8LPSPIState *s = S32K3X8_LPSPI(dev);
    
    /* Set default values according to reference manual */
    s->verid = 0x02000004;      /* Version ID */
    s->param = 0x00040202;      /* Parameters: 4 PCS, 4-word FIFO */
    s->cr = 0x00000000;
    s->sr = 0x00000001;         /* TDF=1 */
    s->ier = 0x00000000;
    s->der = 0x00000000;
    s->cfgr0 = 0x00000000;
    s->cfgr1 = 0x00000000;
    s->dmr0 = 0x00000000;
    s->dmr1 = 0x00000000;
    s->ccr = 0x00000000;
    s->ccr1 = 0x00000000;
    s->fcr = 0x00000000;
    s->fsr = 0x00000000;
    s->tcr = 0x0000001F;        /* Default 32-bit frame */
    s->tdr = 0x00000000;
    s->rsr = 0x00000002;        /* RXEMPTY=1 */
    s->rdr = 0x00000000;
    s->rdror = 0x00000000;
    
    /* Reset internal state */
    s->enabled = false;
    s->master_mode = false;
    s->transfer_size = 32;
    s->current_cs = 0;
    s->prev_tdf = true;
    s->prev_rdf = false;
    s->dma_dispatch_active = false;
    s->dma_tx_pending = false;
    s->dma_rx_pending = false;
    
    /* Reset FIFO */
    fifo_reset(s, true);
    fifo_reset(s, false);
}

/* Device instantiation */
static void s32k3x8_lpspi_instance_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    S32K3X8LPSPIState *s = S32K3X8_LPSPI(obj);
    
    memory_region_init_io(&s->iomem, obj, &s32k3x8_lpspi_ops, s,
                          TYPE_S32K3X8_LPSPI, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    
    sysbus_init_irq(sbd, &s->irq);
    
    /* Initialize chip select signals */
    for (int i = 0; i < 8; i++) {
        sysbus_init_irq(sbd, &s->cs_lines[i]);
    }

    s->instance = g_s32k3x8_lpspi_instance_seed;
    g_s32k3x8_lpspi_instance_seed++;
    if (4U == s->instance) {
        g_s32k3x8_lpspi4 = s;
    }
}
static void s32k3x8_lpspi_realize(DeviceState *dev, Error **errp)
{
	s32k3x8_lpspi_reset(dev);

}
/* Device class initialization */
static void s32k3x8_lpspi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    
    dc->realize = s32k3x8_lpspi_realize;
    dc->desc = "S32K3X8 LPSPI Controller";
}

static const TypeInfo s32k3x8_lpspi_info = {
    .name = TYPE_S32K3X8_LPSPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S32K3X8LPSPIState),
    .instance_init = s32k3x8_lpspi_instance_init,
    .class_init = s32k3x8_lpspi_class_init,
};

void s32k3x8_lpspi_register_types(void)
{
	
    type_register_static(&s32k3x8_lpspi_info);
}

type_init(s32k3x8_lpspi_register_types)
