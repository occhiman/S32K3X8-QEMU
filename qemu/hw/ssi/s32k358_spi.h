#ifndef S32K3X8_LPSPI_H
#define S32K3X8_LPSPI_H

#include "hw/sysbus.h"
#include "hw/qdev-core.h"
#include "qom/object.h"
#include "hw/irq.h"

/* LPSPI register offset addresses - based on S32K3XX Reference Manual */
#define LPSPI_VERID_OFFSET      0x00    /* Version ID Register */
#define LPSPI_PARAM_OFFSET      0x04    /* Parameter Register */
#define LPSPI_CR_OFFSET         0x10    /* Control Register */
#define LPSPI_SR_OFFSET         0x14    /* Status Register */
#define LPSPI_IER_OFFSET        0x18    /* Interrupt Enable Register */
#define LPSPI_DER_OFFSET        0x1C    /* DMA Enable Register */
#define LPSPI_CFGR0_OFFSET      0x20    /* Configuration 0 Register */
#define LPSPI_CFGR1_OFFSET      0x24    /* Configuration 1 Register */
#define LPSPI_DMR0_OFFSET       0x30    /* Data Match 0 Register */
#define LPSPI_DMR1_OFFSET       0x34    /* Data Match 1 Register */
#define LPSPI_CCR_OFFSET        0x40    /* Clock Configuration Register */
#define LPSPI_CCR1_OFFSET       0x44    /* Clock Configuration 1 Register */
#define LPSPI_FCR_OFFSET        0x58    /* FIFO Control Register */
#define LPSPI_FSR_OFFSET        0x5C    /* FIFO Status Register */
#define LPSPI_TCR_OFFSET        0x60    /* Transmit Command Register */
#define LPSPI_TDR_OFFSET        0x64    /* Transmit Data Register */
#define LPSPI_RSR_OFFSET        0x70    /* Receive Status Register */
#define LPSPI_RDR_OFFSET        0x74    /* Receive Data Register */
#define LPSPI_RDROR_OFFSET      0x78    /* Receive Data Read Only Register */

/* LPSPI register bit definitions */
/* Control Register (CR) */
#define LPSPI_CR_MEN            (1 << 0)    /* Module Enable */
#define LPSPI_CR_RST            (1 << 1)    /* Software Reset */
#define LPSPI_CR_DBGEN          (1 << 3)    /* Debug Enable */
#define LPSPI_CR_RTF            (1 << 8)    /* Reset Transmit FIFO */
#define LPSPI_CR_RRF            (1 << 9)    /* Reset Receive FIFO */

/* Status Register (SR) */
#define LPSPI_SR_TDF            (1 << 0)    /* Transmit Data Flag */
#define LPSPI_SR_RDF            (1 << 1)    /* Receive Data Flag */
#define LPSPI_SR_WCF            (1 << 8)    /* Word Complete Flag */
#define LPSPI_SR_FCF            (1 << 9)    /* Frame Complete Flag */
#define LPSPI_SR_TCF            (1 << 10)   /* Transfer Complete Flag */
#define LPSPI_SR_TEF            (1 << 11)   /* Transmit Error Flag */
#define LPSPI_SR_REF            (1 << 12)   /* Receive Error Flag */
#define LPSPI_SR_DMF            (1 << 13)   /* Data Match Flag */
#define LPSPI_SR_MBF            (1 << 24)   /* Module Busy Flag */

/* Configuration 1 Register (CFGR1) */
#define LPSPI_CFGR1_MASTER      (1 << 0)    /* Controller Mode */
#define LPSPI_CFGR1_SAMPLE      (1 << 1)    /* Sample Point */
#define LPSPI_CFGR1_AUTOPCS     (1 << 2)    /* Automatic PCS */
#define LPSPI_CFGR1_NOSTALL     (1 << 3)    /* No Stall */
#define LPSPI_CFGR1_PARTIAL     (1 << 4)    /* Partial Enable */

/* Transmit Command Register (TCR) */
#define LPSPI_TCR_FRAMESZ_MASK  0x00000FFF  /* Frame Size Mask */
#define LPSPI_TCR_WIDTH_SHIFT   16          /* Transfer Width Shift */
#define LPSPI_TCR_WIDTH_MASK    0x00030000  /* Transfer Width Mask */
#define LPSPI_TCR_TXMSK         (1 << 18)   /* Transmit Data Mask */
#define LPSPI_TCR_RXMSK         (1 << 19)   /* Receive Data Mask */
#define LPSPI_TCR_CONTC         (1 << 20)   /* Continuing Command */
#define LPSPI_TCR_CONT          (1 << 21)   /* Continuous Transfer */
#define LPSPI_TCR_BYSW          (1 << 22)   /* Byte Swap */
#define LPSPI_TCR_LSBF          (1 << 23)   /* LSB First */
#define LPSPI_TCR_PCS_SHIFT     24          /* Peripheral Chip Select Shift */
#define LPSPI_TCR_PCS_MASK      0x07000000  /* Peripheral Chip Select Mask */
#define LPSPI_TCR_PRESCALE_SHIFT 27         /* Prescaler Value Shift */
#define LPSPI_TCR_PRESCALE_MASK 0x38000000  /* Prescaler Value Mask */
#define LPSPI_TCR_CPHA          (1 << 30)   /* Clock Phase */
#define LPSPI_TCR_CPOL          (1 << 31)   /* Clock Polarity */

/* FIFO size definitions */
#define LPSPI_FIFO_SIZE         4           /* 4-word FIFO */

#define LPSPI_DER_TDDE          (1U << 0)
#define LPSPI_DER_RDDE          (1U << 1)

/* Device type definition */
#define TYPE_S32K3X8_LPSPI      "s32k3x8.lpspi"

typedef struct S32K3X8LPSPIState {
    SysBusDevice parent_obj;
    
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq cs_lines[8];  /* Up to 8 chip select signals */
    
    /* Registers */
    uint32_t verid;        /* Version ID Register */
    uint32_t param;        /* Parameter Register */
    uint32_t cr;           /* Control Register */
    uint32_t sr;           /* Status Register */
    uint32_t ier;          /* Interrupt Enable Register */
    uint32_t der;          /* DMA Enable Register */
    uint32_t cfgr0;        /* Configuration 0 Register */
    uint32_t cfgr1;        /* Configuration 1 Register */
    uint32_t dmr0;         /* Data Match 0 Register */
    uint32_t dmr1;         /* Data Match 1 Register */
    uint32_t ccr;          /* Clock Configuration Register */
    uint32_t ccr1;         /* Clock Configuration 1 Register */
    uint32_t fcr;          /* FIFO Control Register */
    uint32_t fsr;          /* FIFO Status Register */
    uint32_t tcr;          /* Transmit Command Register */
    uint32_t tdr;          /* Transmit Data Register */
    uint32_t rsr;          /* Receive Status Register */
    uint32_t rdr;          /* Receive Data Register */
    uint32_t rdror;        /* Receive Data Read Only Register */
    
    /* FIFO implementation */
    struct {
        uint32_t data[LPSPI_FIFO_SIZE];
        uint32_t level;
        uint32_t head;
        uint32_t tail;
    } tx_fifo, rx_fifo;
    
    /* Internal state */
    bool enabled;
    bool master_mode;
    uint32_t transfer_size;
    uint32_t current_cs;
    uint32_t instance;
    bool prev_tdf;
    bool prev_rdf;
    bool dma_dispatch_active;
    bool dma_tx_pending;
    bool dma_rx_pending;
    
} S32K3X8LPSPIState;

#define S32K3X8_LPSPI(obj) \
    OBJECT_CHECK(S32K3X8LPSPIState, (obj), TYPE_S32K3X8_LPSPI)

/* Function declarations */
void s32k3x8_lpspi_register_types(void);

#endif /* S32K3X8_LPSPI_H */
