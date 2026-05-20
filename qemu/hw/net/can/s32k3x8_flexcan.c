/*
 * NXP S32K3X8 FlexCAN model (minimal MCAL-oriented implementation).
 *
 * This model focuses on the register and mailbox behavior needed for
 * AUTOSAR MCAL FlexCAN bring-up on S32K3X8 QEMU.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/net/s32k3x8_flexcan.h"

/* Core register offsets (S32K3xx FlexCAN layout). */
#define FLEXCAN_MCR_OFFSET             0x000
#define FLEXCAN_CTRL1_OFFSET           0x004
#define FLEXCAN_TIMER_OFFSET           0x008
#define FLEXCAN_RXMGMASK_OFFSET        0x010
#define FLEXCAN_RX14MASK_OFFSET        0x014
#define FLEXCAN_RX15MASK_OFFSET        0x018
#define FLEXCAN_ECR_OFFSET             0x01c
#define FLEXCAN_ESR1_OFFSET            0x020
#define FLEXCAN_IMASK2_OFFSET          0x024
#define FLEXCAN_IMASK1_OFFSET          0x028
#define FLEXCAN_IFLAG2_OFFSET          0x02c
#define FLEXCAN_IFLAG1_OFFSET          0x030
#define FLEXCAN_CTRL2_OFFSET           0x034
#define FLEXCAN_ESR2_OFFSET            0x038
#define FLEXCAN_RXFGMASK_OFFSET        0x048
#define FLEXCAN_RXFIR_OFFSET           0x04c
#define FLEXCAN_IMASK3_OFFSET          0x06c
#define FLEXCAN_IFLAG3_OFFSET          0x074
#define FLEXCAN_RXIMR_OFFSET           0x880
#define FLEXCAN_FDCTRL_OFFSET          0xc00
#define FLEXCAN_ERFCR_OFFSET           0xc0c
#define FLEXCAN_ERFIER_OFFSET          0xc10
#define FLEXCAN_ERFSR_OFFSET           0xc14
#define FLEXCAN_ERFFEL_OFFSET          0x3000

/*
 * Message buffer RAM layout:
 * - 3 RAM blocks of 512 bytes each starting at FLEXCAN_MB_OFFSET.
 * - MB payload size per block is selected by FDCTRL.MBDSR{0,1,2}.
 * - Each MB uses 8 bytes arbitration/control + payload bytes.
 */
#define FLEXCAN_MB_OFFSET              0x080
#define FLEXCAN_MB_WORDS               4
#define FLEXCAN_MB_COUNT               32
#define FLEXCAN_MB_RAMBLOCK_BYTES      512U
#define FLEXCAN_MB_ARBITRATION_BYTES   8U
#define FLEXCAN_MBDSR_BLOCK_COUNT      3U
#define FLEXCAN_LEGACY_FILTER_OFFSET   0x0e0
#define FLEXCAN_ENHANCED_FIFO_OFFSET   0x2000

/* MCR bits used by MCAL startup/stop sequence. */
#define FLEXCAN_MCR_MAXMB_MASK         0x0000007fU
#define FLEXCAN_MCR_IDAM_MASK          0x00000300U
#define FLEXCAN_MCR_IDAM_SHIFT         8
#define FLEXCAN_MCR_IRMQ_MASK          0x00010000U
#define FLEXCAN_MCR_SRXDIS_MASK        0x00020000U
#define FLEXCAN_MCR_LPMACK_MASK        0x00100000U
#define FLEXCAN_MCR_FRZACK_MASK        0x01000000U
#define FLEXCAN_MCR_SOFTRST_MASK       0x02000000U
#define FLEXCAN_MCR_NOTRDY_MASK        0x08000000U
#define FLEXCAN_MCR_HALT_MASK          0x10000000U
#define FLEXCAN_MCR_RFEN_MASK          0x20000000U
#define FLEXCAN_MCR_FRZ_MASK           0x40000000U
#define FLEXCAN_MCR_MDIS_MASK          0x80000000U
#define FLEXCAN_CTRL1_LPB_MASK         0x00001000U
#define FLEXCAN_CTRL2_RFFN_MASK        0x0f000000U
#define FLEXCAN_CTRL2_RFFN_SHIFT       24

/* MB CS field bits. */
#define FLEXCAN_CS_TIMESTAMP_MASK      0x0000ffffU
#define FLEXCAN_CS_DLC_MASK            0x000f0000U
#define FLEXCAN_CS_DLC_SHIFT           16
#define FLEXCAN_CS_RTR_MASK            0x00100000U
#define FLEXCAN_CS_IDE_MASK            0x00200000U
#define FLEXCAN_CS_CODE_MASK           0x0f000000U
#define FLEXCAN_CS_CODE_SHIFT          24
#define FLEXCAN_CS_ESI_MASK            0x20000000U
#define FLEXCAN_CS_BRS_MASK            0x40000000U
#define FLEXCAN_CS_EDL_MASK            0x80000000U

#define FLEXCAN_FDCTRL_MBDSR0_MASK     0x00030000U
#define FLEXCAN_FDCTRL_MBDSR0_SHIFT    16U
#define FLEXCAN_FDCTRL_MBDSR1_MASK     0x00180000U
#define FLEXCAN_FDCTRL_MBDSR1_SHIFT    19U
#define FLEXCAN_FDCTRL_MBDSR2_MASK     0x00c00000U
#define FLEXCAN_FDCTRL_MBDSR2_SHIFT    22U

#define FLEXCAN_STD_ID_SHIFT           18

/*
 * RX FIFO format A table encodes RTR/IDE in bits 31/30 (not MB CS layout).
 * See FlexCAN_Ip_HwAccess.c: FlexCAN_SetRxFifoFilterFormatA().
 */
#define FLEXCAN_FIFO_FILTER_RTR_MASK   (1U << 31)
#define FLEXCAN_FIFO_FILTER_IDE_MASK   (1U << 30)

/* MB code values used by MCAL. */
#define FLEXCAN_RX_FULL                0x2U
#define FLEXCAN_RX_EMPTY               0x4U
#define FLEXCAN_TX_INACTIVE            0x8U
#define FLEXCAN_TX_ABORT               0x9U
#define FLEXCAN_TX_DATA_CODE           0xcU

#define FLEXCAN_IFLAG1_BUF5I_MASK      0x00000020U
#define FLEXCAN_IFLAG1_BUF6I_MASK      0x00000040U
#define FLEXCAN_IFLAG1_BUF7I_MASK      0x00000080U
#define FLEXCAN_IFLAG1_FIFO_MASK       (FLEXCAN_IFLAG1_BUF5I_MASK | \
                                        FLEXCAN_IFLAG1_BUF6I_MASK | \
                                        FLEXCAN_IFLAG1_BUF7I_MASK)
#define FLEXCAN_TRACE_CAN_INSTANCE     3U
#define FLEXCAN_TRACE_RX_MB_A          24U
#define FLEXCAN_TRACE_RX_MB_B          25U

#define FLEXCAN_ERFCR_ERFWM_MASK       0x0000001fU
#define FLEXCAN_ERFCR_NFE_MASK         0x00003f00U
#define FLEXCAN_ERFCR_NFE_SHIFT        8
#define FLEXCAN_ERFCR_NEXIF_MASK       0x007f0000U
#define FLEXCAN_ERFCR_NEXIF_SHIFT      16
#define FLEXCAN_ERFCR_ERFEN_MASK       0x80000000U

#define FLEXCAN_ERFSR_ERFEL_MASK       0x0000003fU
#define FLEXCAN_ERFSR_ERFF_MASK        0x00010000U
#define FLEXCAN_ERFSR_ERFE_MASK        0x00020000U
#define FLEXCAN_ERFSR_ERFCLR_MASK      0x08000000U
#define FLEXCAN_ERFSR_ERFDA_MASK       0x10000000U
#define FLEXCAN_ERFSR_ERFWMI_MASK      0x20000000U
#define FLEXCAN_ERFSR_ERFOVF_MASK      0x40000000U
#define FLEXCAN_ERFSR_ERFUFW_MASK      0x80000000U
#define FLEXCAN_ERFSR_INT_MASK         (FLEXCAN_ERFSR_ERFDA_MASK | \
                                        FLEXCAN_ERFSR_ERFWMI_MASK | \
                                        FLEXCAN_ERFSR_ERFOVF_MASK | \
                                        FLEXCAN_ERFSR_ERFUFW_MASK)

#define FLEXCAN_LEGACY_IDHIT_MASK      0x000001ffU

/* ESR1 is mostly W1C from software perspective. */
#define FLEXCAN_ESR1_W1C_MASK          0xffffffffU

static ssize_t s32k3x8_flexcan_receive(CanBusClientState *client,
                                       const qemu_can_frame *frames,
                                       size_t frames_cnt);

static inline uint32_t flexcan_reg_index(hwaddr addr)
{
    return (uint32_t)(addr >> 2);
}

static inline uint8_t s32k3x8_flexcan_block_payload_size(
    const S32K3X8FlexCANState *s,
    unsigned block_idx)
{
    uint32_t fdctrl = s->regs[flexcan_reg_index(FLEXCAN_FDCTRL_OFFSET)];
    uint32_t mbdsr = 0U;

    switch (block_idx) {
    case 0:
        mbdsr = (fdctrl & FLEXCAN_FDCTRL_MBDSR0_MASK) >>
                FLEXCAN_FDCTRL_MBDSR0_SHIFT;
        break;
    case 1:
        mbdsr = (fdctrl & FLEXCAN_FDCTRL_MBDSR1_MASK) >>
                FLEXCAN_FDCTRL_MBDSR1_SHIFT;
        break;
    case 2:
        mbdsr = (fdctrl & FLEXCAN_FDCTRL_MBDSR2_MASK) >>
                FLEXCAN_FDCTRL_MBDSR2_SHIFT;
        break;
    default:
        break;
    }

    if (mbdsr > 3U) {
        mbdsr = 0U;
    }

    return (uint8_t)(8U << mbdsr);
}

static bool s32k3x8_flexcan_mb_offset(const S32K3X8FlexCANState *s,
                                      unsigned mb_idx,
                                      uint32_t *offset)
{
    unsigned msg_idx = mb_idx;
    uint32_t block_offset = 0U;

    if (mb_idx >= FLEXCAN_MB_COUNT) {
        return false;
    }

    for (unsigned block = 0; block < FLEXCAN_MBDSR_BLOCK_COUNT; block++) {
        uint8_t payload_size = s32k3x8_flexcan_block_payload_size(s, block);
        uint8_t mb_size = payload_size + FLEXCAN_MB_ARBITRATION_BYTES;
        uint8_t max_mb = (uint8_t)(FLEXCAN_MB_RAMBLOCK_BYTES / mb_size);

        if (max_mb == 0U) {
            return false;
        }

        if (msg_idx < max_mb) {
            *offset = FLEXCAN_MB_OFFSET + block_offset +
                      ((uint32_t)msg_idx * (uint32_t)mb_size);
            return true;
        }

        msg_idx -= max_mb;
        block_offset += FLEXCAN_MB_RAMBLOCK_BYTES;
    }

    return false;
}

static inline uint8_t s32k3x8_flexcan_mb_payload_size(
    const S32K3X8FlexCANState *s,
    unsigned mb_idx)
{
    unsigned msg_idx = mb_idx;

    for (unsigned block = 0; block < FLEXCAN_MBDSR_BLOCK_COUNT; block++) {
        uint8_t payload_size = s32k3x8_flexcan_block_payload_size(s, block);
        uint8_t mb_size = payload_size + FLEXCAN_MB_ARBITRATION_BYTES;
        uint8_t max_mb = (uint8_t)(FLEXCAN_MB_RAMBLOCK_BYTES / mb_size);

        if (max_mb == 0U) {
            return 8U;
        }

        if (msg_idx < max_mb) {
            return payload_size;
        }

        msg_idx -= max_mb;
    }

    return 8U;
}

static inline uint32_t *flexcan_mb_addr(S32K3X8FlexCANState *s, unsigned mb_idx)
{
    uint32_t offset;

    if (!s32k3x8_flexcan_mb_offset(s, mb_idx, &offset)) {
        return NULL;
    }

    return &s->regs[flexcan_reg_index(offset)];
}

static inline const uint32_t *flexcan_mb_addr_const(
    const S32K3X8FlexCANState *s,
    unsigned mb_idx)
{
    uint32_t offset;

    if (!s32k3x8_flexcan_mb_offset(s, mb_idx, &offset)) {
        return NULL;
    }

    return &s->regs[flexcan_reg_index(offset)];
}

static inline uint32_t flexcan_raw_read_le(const S32K3X8FlexCANState *s,
                                           hwaddr addr,
                                           unsigned size)
{
    const uint8_t *raw = (const uint8_t *)s->regs;
    uint32_t value = 0U;

    for (unsigned i = 0; i < size; i++) {
        value |= ((uint32_t)raw[addr + i]) << (8U * i);
    }

    return value;
}

static inline void flexcan_raw_write_le(S32K3X8FlexCANState *s,
                                        hwaddr addr,
                                        uint32_t value,
                                        unsigned size)
{
    uint8_t *raw = (uint8_t *)s->regs;

    for (unsigned i = 0; i < size; i++) {
        raw[addr + i] = (uint8_t)(value >> (8U * i));
    }
}

static inline bool flexcan_is_mb_cs_addr(const S32K3X8FlexCANState *s,
                                         hwaddr addr,
                                         unsigned *mb_idx)
{
    uint32_t mb_off;

    if ((addr & 0x3U) != 0U) {
        return false;
    }

    for (unsigned idx = 0; idx < FLEXCAN_MB_COUNT; idx++) {
        if (!s32k3x8_flexcan_mb_offset(s, idx, &mb_off)) {
            break;
        }
        if (addr == mb_off) {
            *mb_idx = idx;
            return true;
        }
    }

    return false;
}

static inline uint32_t flexcan_cs_code(uint32_t cs)
{
    return (cs & FLEXCAN_CS_CODE_MASK) >> FLEXCAN_CS_CODE_SHIFT;
}

static inline uint8_t flexcan_word_swapped_index(uint8_t index)
{
    return (index & (uint8_t)~3U) + (3U - (index & 3U));
}

static inline bool s32k3x8_flexcan_legacy_fifo_enabled(
    const S32K3X8FlexCANState *s)
{
    return (s->regs[flexcan_reg_index(FLEXCAN_MCR_OFFSET)] &
            FLEXCAN_MCR_RFEN_MASK) != 0U;
}

static inline bool s32k3x8_flexcan_enhanced_fifo_enabled(
    const S32K3X8FlexCANState *s)
{
    return (s->regs[flexcan_reg_index(FLEXCAN_ERFCR_OFFSET)] &
            FLEXCAN_ERFCR_ERFEN_MASK) != 0U;
}

static inline uint32_t s32k3x8_flexcan_legacy_filter_count(
    const S32K3X8FlexCANState *s)
{
    uint32_t rffn = (s->regs[flexcan_reg_index(FLEXCAN_CTRL2_OFFSET)] &
                     FLEXCAN_CTRL2_RFFN_MASK) >> FLEXCAN_CTRL2_RFFN_SHIFT;

    return (rffn + 1U) * 8U;
}

static inline uint32_t s32k3x8_flexcan_legacy_filter_mask(
    const S32K3X8FlexCANState *s,
    uint32_t filter_idx)
{
    uint32_t mcr = s->regs[flexcan_reg_index(FLEXCAN_MCR_OFFSET)];

    if ((mcr & FLEXCAN_MCR_IRMQ_MASK) && (filter_idx < 96U)) {
        return s->regs[flexcan_reg_index(FLEXCAN_RXIMR_OFFSET) + filter_idx];
    }

    return s->regs[flexcan_reg_index(FLEXCAN_RXFGMASK_OFFSET)];
}

static void s32k3x8_flexcan_encode_rx_words(S32K3X8FlexCANState *s,
                                            uint32_t *target,
                                            const qemu_can_frame *frame,
                                            uint8_t payload_capacity)
{
    uint32_t frame_id = frame->can_id;
    bool eff = (frame_id & QEMU_CAN_EFF_FLAG) != 0U;
    bool rtr = (frame_id & QEMU_CAN_RTR_FLAG) != 0U;
    uint8_t len = frame->can_dlc;
    bool is_fd = (frame->flags & QEMU_CAN_FRMF_TYPE_FD) != 0U;
    uint8_t *target_bytes = (uint8_t *)&target[2];
    uint8_t copy_len;
    uint32_t cs = 0;

    if (len > 64U) {
        len = 64U;
    }
    if (!is_fd && (len > 8U)) {
        is_fd = true;
    }
    copy_len = MIN(len, payload_capacity);

    memset(&target[2], 0, payload_capacity);
    for (uint8_t i = 0; i < copy_len; i++) {
        target_bytes[flexcan_word_swapped_index(i)] = frame->data[i];
    }

    if (eff) {
        target[1] = frame_id & QEMU_CAN_EFF_MASK;
        cs |= FLEXCAN_CS_IDE_MASK;
    } else {
        target[1] = (frame_id & QEMU_CAN_SFF_MASK) << FLEXCAN_STD_ID_SHIFT;
    }

    if (rtr) {
        cs |= FLEXCAN_CS_RTR_MASK;
    }
    if (is_fd) {
        cs |= FLEXCAN_CS_EDL_MASK;
        if (frame->flags & QEMU_CAN_FRMF_BRS) {
            cs |= FLEXCAN_CS_BRS_MASK;
        }
        if (frame->flags & QEMU_CAN_FRMF_ESI) {
            cs |= FLEXCAN_CS_ESI_MASK;
        }
    }

    cs |= ((uint32_t)can_len2dlc(len) << FLEXCAN_CS_DLC_SHIFT) &
          FLEXCAN_CS_DLC_MASK;
    cs |= ((uint32_t)FLEXCAN_RX_FULL << FLEXCAN_CS_CODE_SHIFT) &
          FLEXCAN_CS_CODE_MASK;
    cs |= (uint32_t)s->timestamp++ & FLEXCAN_CS_TIMESTAMP_MASK;
    target[0] = cs;
}

static void s32k3x8_flexcan_reset_legacy_fifo(S32K3X8FlexCANState *s)
{
    s->legacy_fifo_head = 0;
    s->legacy_fifo_tail = 0;
    s->legacy_fifo_count = 0;
    s->regs[flexcan_reg_index(FLEXCAN_RXFIR_OFFSET)] = 0;
    s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)] &= ~FLEXCAN_IFLAG1_FIFO_MASK;
}

static void s32k3x8_flexcan_legacy_update_status(S32K3X8FlexCANState *s)
{
    uint32_t iflag1 = s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)];

    iflag1 &= ~FLEXCAN_IFLAG1_BUF6I_MASK;
    if (s->legacy_fifo_count >= (S32K3X8_FLEXCAN_LEGACY_FIFO_CAPACITY - 1U) &&
        s->legacy_fifo_count > 0U) {
        iflag1 |= FLEXCAN_IFLAG1_BUF6I_MASK;
    }

    s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)] = iflag1;
}

static inline void s32k3x8_flexcan_legacy_dma_irq_fallback(S32K3X8FlexCANState *s)
{
    uint32_t imask1 = s->regs[flexcan_reg_index(FLEXCAN_IMASK1_OFFSET)];

    if ((imask1 & FLEXCAN_IFLAG1_BUF5I_MASK) == 0U) {
        /*
         * MCAL DMA-based legacy FIFO reception expects eDMA requests that are
         * not modeled yet. Unmask BUF5I so FIFO frames can complete through
         * the regular ISR callback path.
         */
        s->regs[flexcan_reg_index(FLEXCAN_IMASK1_OFFSET)] =
            imask1 | FLEXCAN_IFLAG1_BUF5I_MASK;
    }
}

static void s32k3x8_flexcan_legacy_present_front(S32K3X8FlexCANState *s)
{
    uint32_t iflag1 = s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)];

    if (s->legacy_fifo_count == 0U) {
        s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)] =
            iflag1 & ~FLEXCAN_IFLAG1_BUF5I_MASK;
        s->regs[flexcan_reg_index(FLEXCAN_RXFIR_OFFSET)] = 0;
        return;
    }

    if (iflag1 & FLEXCAN_IFLAG1_BUF5I_MASK) {
        return;
    }

    s32k3x8_flexcan_encode_rx_words(
        s,
        flexcan_mb_addr(s, 0),
        &s->legacy_fifo[s->legacy_fifo_head].frame,
        s32k3x8_flexcan_mb_payload_size(s, 0));
    s->regs[flexcan_reg_index(FLEXCAN_RXFIR_OFFSET)] =
        s->legacy_fifo[s->legacy_fifo_head].idhit & FLEXCAN_LEGACY_IDHIT_MASK;
    s32k3x8_flexcan_legacy_dma_irq_fallback(s);
    s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)] |=
        FLEXCAN_IFLAG1_BUF5I_MASK;
}

static void s32k3x8_flexcan_legacy_pop(S32K3X8FlexCANState *s)
{
    if (s->legacy_fifo_count == 0U) {
        return;
    }

    s->legacy_fifo_head =
        (s->legacy_fifo_head + 1U) % S32K3X8_FLEXCAN_LEGACY_FIFO_CAPACITY;
    s->legacy_fifo_count--;

    s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)] &= ~FLEXCAN_IFLAG1_BUF5I_MASK;
    s32k3x8_flexcan_legacy_present_front(s);
    s32k3x8_flexcan_legacy_update_status(s);
}

static bool s32k3x8_flexcan_legacy_filter_accept(
    const S32K3X8FlexCANState *s,
    const qemu_can_frame *frame,
    uint16_t *idhit)
{
    uint32_t mcr = s->regs[flexcan_reg_index(FLEXCAN_MCR_OFFSET)];
    uint32_t idam = (mcr & FLEXCAN_MCR_IDAM_MASK) >> FLEXCAN_MCR_IDAM_SHIFT;

    if (idam != 0U) {
        /* IDAM B/C not modeled yet: accept frame and report default hit. */
        *idhit = 0;
        return true;
    }

    uint32_t filter_count = s32k3x8_flexcan_legacy_filter_count(s);
    bool frame_eff = (frame->can_id & QEMU_CAN_EFF_FLAG) != 0U;
    bool frame_rtr = (frame->can_id & QEMU_CAN_RTR_FLAG) != 0U;
    uint32_t frame_cmp;

    if (frame_eff) {
        frame_cmp = FLEXCAN_FIFO_FILTER_IDE_MASK |
                    (frame_rtr ? FLEXCAN_FIFO_FILTER_RTR_MASK : 0U) |
                    ((frame->can_id & QEMU_CAN_EFF_MASK) << 1);
    } else {
        frame_cmp = (frame_rtr ? FLEXCAN_FIFO_FILTER_RTR_MASK : 0U) |
                    ((frame->can_id & QEMU_CAN_SFF_MASK) << 19);
    }

    for (uint32_t i = 0; i < filter_count; i++) {
        uint32_t entry =
            s->regs[flexcan_reg_index(FLEXCAN_LEGACY_FILTER_OFFSET + (i * 4U))];
        uint32_t mask = s32k3x8_flexcan_legacy_filter_mask(s, i);
        if ((entry & mask) == (frame_cmp & mask)) {
            *idhit = (uint16_t)i;
            return true;
        }
    }

    return false;
}

static bool s32k3x8_flexcan_legacy_enqueue(S32K3X8FlexCANState *s,
                                           const qemu_can_frame *frame)
{
    uint16_t idhit;

    if (!s32k3x8_flexcan_legacy_filter_accept(s, frame, &idhit)) {
        return false;
    }
    if (s->legacy_fifo_count >= S32K3X8_FLEXCAN_LEGACY_FIFO_CAPACITY) {
        s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)] |= FLEXCAN_IFLAG1_BUF7I_MASK;
        return false;
    }

    s->legacy_fifo[s->legacy_fifo_tail].frame = *frame;
    s->legacy_fifo[s->legacy_fifo_tail].idhit = idhit;
    s->legacy_fifo_tail =
        (s->legacy_fifo_tail + 1U) % S32K3X8_FLEXCAN_LEGACY_FIFO_CAPACITY;
    s->legacy_fifo_count++;

    s32k3x8_flexcan_legacy_present_front(s);
    s32k3x8_flexcan_legacy_update_status(s);
    return true;
}

static void s32k3x8_flexcan_enhanced_clear_output(S32K3X8FlexCANState *s)
{
    uint32_t out_idx = flexcan_reg_index(FLEXCAN_ENHANCED_FIFO_OFFSET);

    memset(&s->regs[out_idx], 0, 20 * sizeof(uint32_t));
}

static void s32k3x8_flexcan_enhanced_present_front(S32K3X8FlexCANState *s)
{
    uint32_t out_idx = flexcan_reg_index(FLEXCAN_ENHANCED_FIFO_OFFSET);
    uint32_t *out = &s->regs[out_idx];

    s32k3x8_flexcan_enhanced_clear_output(s);
    if (s->enhanced_fifo_count == 0U) {
        return;
    }

    s32k3x8_flexcan_encode_rx_words(
        s, out, &s->enhanced_fifo[s->enhanced_fifo_head].frame, 64U);
    {
        uint8_t len = s->enhanced_fifo[s->enhanced_fifo_head].frame.can_dlc;
        uint32_t idhit_offset = 2U + ((uint32_t)MIN(len, 64U) + 3U) / 4U;

        out[idhit_offset] = s->enhanced_fifo[s->enhanced_fifo_head].idhit & 0x7fU;
    }
}

static void s32k3x8_flexcan_enhanced_update_status(S32K3X8FlexCANState *s)
{
    uint32_t erfsr = s->regs[flexcan_reg_index(FLEXCAN_ERFSR_OFFSET)] &
                     (FLEXCAN_ERFSR_ERFOVF_MASK | FLEXCAN_ERFSR_ERFUFW_MASK);
    uint32_t watermark =
        s->regs[flexcan_reg_index(FLEXCAN_ERFCR_OFFSET)] & FLEXCAN_ERFCR_ERFWM_MASK;

    erfsr |= s->enhanced_fifo_count & FLEXCAN_ERFSR_ERFEL_MASK;
    if (s->enhanced_fifo_count == 0U) {
        erfsr |= FLEXCAN_ERFSR_ERFE_MASK;
    } else {
        erfsr |= FLEXCAN_ERFSR_ERFDA_MASK;
        if (s->enhanced_fifo_count > watermark) {
            erfsr |= FLEXCAN_ERFSR_ERFWMI_MASK;
        }
    }
    if (s->enhanced_fifo_count >= S32K3X8_FLEXCAN_ENHANCED_FIFO_CAPACITY) {
        erfsr |= FLEXCAN_ERFSR_ERFF_MASK;
    }

    s->regs[flexcan_reg_index(FLEXCAN_ERFSR_OFFSET)] = erfsr;
}

static void s32k3x8_flexcan_reset_enhanced_fifo(S32K3X8FlexCANState *s)
{
    s->enhanced_fifo_head = 0;
    s->enhanced_fifo_tail = 0;
    s->enhanced_fifo_count = 0;
    s->regs[flexcan_reg_index(FLEXCAN_ERFSR_OFFSET)] = 0;
    s32k3x8_flexcan_enhanced_clear_output(s);
}

static void s32k3x8_flexcan_enhanced_pop(S32K3X8FlexCANState *s)
{
    if (s->enhanced_fifo_count == 0U) {
        s->regs[flexcan_reg_index(FLEXCAN_ERFSR_OFFSET)] |= FLEXCAN_ERFSR_ERFUFW_MASK;
    } else {
        s->enhanced_fifo_head =
            (s->enhanced_fifo_head + 1U) % S32K3X8_FLEXCAN_ENHANCED_FIFO_CAPACITY;
        s->enhanced_fifo_count--;
    }

    s32k3x8_flexcan_enhanced_present_front(s);
    s32k3x8_flexcan_enhanced_update_status(s);
}

static bool s32k3x8_flexcan_enhanced_enqueue(S32K3X8FlexCANState *s,
                                             const qemu_can_frame *frame)
{
    if (s->enhanced_fifo_count >= S32K3X8_FLEXCAN_ENHANCED_FIFO_CAPACITY) {
        s->regs[flexcan_reg_index(FLEXCAN_ERFSR_OFFSET)] |= FLEXCAN_ERFSR_ERFOVF_MASK;
        s32k3x8_flexcan_enhanced_update_status(s);
        return false;
    }

    s->enhanced_fifo[s->enhanced_fifo_tail].frame = *frame;
    s->enhanced_fifo[s->enhanced_fifo_tail].idhit = 0;
    s->enhanced_fifo_tail =
        (s->enhanced_fifo_tail + 1U) % S32K3X8_FLEXCAN_ENHANCED_FIFO_CAPACITY;
    s->enhanced_fifo_count++;

    s32k3x8_flexcan_enhanced_present_front(s);
    s32k3x8_flexcan_enhanced_update_status(s);
    return true;
}

static inline bool s32k3x8_flexcan_trace_enabled(const S32K3X8FlexCANState *s)
{
    return s->instance_id == FLEXCAN_TRACE_CAN_INSTANCE;
}

static inline bool s32k3x8_flexcan_trace_rx_mb(unsigned mb_idx)
{
    return (mb_idx == FLEXCAN_TRACE_RX_MB_A) ||
           (mb_idx == FLEXCAN_TRACE_RX_MB_B);
}

static uint32_t s32k3x8_flexcan_trace_mb_code(const S32K3X8FlexCANState *s,
                                              unsigned mb_idx)
{
    const uint32_t *mb = flexcan_mb_addr_const(s, mb_idx);

    if (mb == NULL) {
        return 0xffffffffU;
    }

    return flexcan_cs_code(mb[0]);
}

static void s32k3x8_flexcan_trace_irq_state(S32K3X8FlexCANState *s,
                                            const char *tag,
                                            uint32_t pending_mb,
                                            uint32_t pending_erf,
                                            uint8_t level)
{
    if (!s32k3x8_flexcan_trace_enabled(s)) {
        return;
    }

    qemu_log_mask(CPU_LOG_INT,
                  "flexcan%u:%s imask1=0x%08x iflag1=0x%08x pending_mb=0x%08x "
                  "pending_erf=0x%08x level=%u mb24_code=%u mb25_code=%u\\n",
                  s->instance_id,
                  tag,
                  s->regs[flexcan_reg_index(FLEXCAN_IMASK1_OFFSET)],
                  s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)],
                  pending_mb,
                  pending_erf,
                  level,
                  s32k3x8_flexcan_trace_mb_code(s, FLEXCAN_TRACE_RX_MB_A),
                  s32k3x8_flexcan_trace_mb_code(s, FLEXCAN_TRACE_RX_MB_B));
}

static void s32k3x8_flexcan_update_irq(S32K3X8FlexCANState *s)
{
    if (s32k3x8_flexcan_legacy_fifo_enabled(s) && (s->legacy_fifo_count > 0U)) {
        s32k3x8_flexcan_legacy_dma_irq_fallback(s);
    }

    uint32_t pending_mb = s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)] &
                          s->regs[flexcan_reg_index(FLEXCAN_IMASK1_OFFSET)];
    uint32_t pending_erf = s->regs[flexcan_reg_index(FLEXCAN_ERFSR_OFFSET)] &
                           s->regs[flexcan_reg_index(FLEXCAN_ERFIER_OFFSET)] &
                           FLEXCAN_ERFSR_INT_MASK;
    uint8_t level = (pending_mb | pending_erf) != 0U;

    s32k3x8_flexcan_trace_irq_state(s, "irq", pending_mb, pending_erf, level);
    qemu_set_irq(s->irq, level);
}

static void s32k3x8_flexcan_rearm_rx_mb_from_iflag1(S32K3X8FlexCANState *s,
                                                     uint32_t cleared_mask)
{
    for (unsigned mb_idx = 0; mb_idx < 32U; mb_idx++) {
        uint32_t bit = (uint32_t)1U << mb_idx;
        uint32_t *mb;
        uint32_t cs;

        if ((cleared_mask & bit) == 0U) {
            continue;
        }

        mb = flexcan_mb_addr(s, mb_idx);
        if (mb == NULL) {
            continue;
        }

        cs = mb[0];
        if (flexcan_cs_code(cs) == FLEXCAN_RX_FULL) {
            cs &= ~FLEXCAN_CS_CODE_MASK;
            cs |= (FLEXCAN_RX_EMPTY << FLEXCAN_CS_CODE_SHIFT) &
                  FLEXCAN_CS_CODE_MASK;
            mb[0] = cs;

            if (s32k3x8_flexcan_trace_enabled(s) &&
                s32k3x8_flexcan_trace_rx_mb(mb_idx)) {
                qemu_log_mask(CPU_LOG_INT,
                              "flexcan%u:rearm_rx mb=%u cleared_mask=0x%08x "
                              "new_code=%u\\n",
                              s->instance_id,
                              mb_idx,
                              cleared_mask,
                              flexcan_cs_code(mb[0]));
            }
        }
    }
}

static void s32k3x8_flexcan_update_mcr_state(S32K3X8FlexCANState *s)
{
    uint32_t mcr = s->regs[flexcan_reg_index(FLEXCAN_MCR_OFFSET)];
    bool mdis = (mcr & FLEXCAN_MCR_MDIS_MASK) != 0U;
    bool freeze_req;

    /*
     * In low-power disable mode, software expects the module to remain in a
     * frozen context so FlexCAN_Enable() can wait for FRZACK before exit.
     */
    if (mdis) {
        mcr |= FLEXCAN_MCR_FRZ_MASK | FLEXCAN_MCR_HALT_MASK;
    }

    freeze_req = (mcr & FLEXCAN_MCR_FRZ_MASK) &&
                 (mcr & FLEXCAN_MCR_HALT_MASK);

    mcr &= ~(FLEXCAN_MCR_LPMACK_MASK |
             FLEXCAN_MCR_FRZACK_MASK |
             FLEXCAN_MCR_NOTRDY_MASK |
             FLEXCAN_MCR_SOFTRST_MASK);

    if (mdis) {
        mcr |= FLEXCAN_MCR_LPMACK_MASK |
               FLEXCAN_MCR_FRZACK_MASK |
               FLEXCAN_MCR_NOTRDY_MASK;
    } else if (freeze_req) {
        mcr |= FLEXCAN_MCR_FRZACK_MASK |
               FLEXCAN_MCR_NOTRDY_MASK;
    }

    s->regs[flexcan_reg_index(FLEXCAN_MCR_OFFSET)] = mcr;
}

static void s32k3x8_flexcan_reset_registers(S32K3X8FlexCANState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->timestamp = 0;
    s32k3x8_flexcan_reset_legacy_fifo(s);
    s32k3x8_flexcan_reset_enhanced_fifo(s);

    /* Start disabled with 32 message buffers selected. */
    s->regs[flexcan_reg_index(FLEXCAN_MCR_OFFSET)] = FLEXCAN_MCR_MDIS_MASK |
                                                     31U;
    s->regs[flexcan_reg_index(FLEXCAN_RXMGMASK_OFFSET)] = 0xffffffffU;
    s->regs[flexcan_reg_index(FLEXCAN_RXFGMASK_OFFSET)] = 0xffffffffU;
    s->regs[flexcan_reg_index(FLEXCAN_RX14MASK_OFFSET)] = 0xffffffffU;
    s->regs[flexcan_reg_index(FLEXCAN_RX15MASK_OFFSET)] = 0xffffffffU;

    for (unsigned i = 0; i < 96; i++) {
        s->regs[flexcan_reg_index(FLEXCAN_RXIMR_OFFSET) + i] = 0xffffffffU;
    }

    s32k3x8_flexcan_update_mcr_state(s);
    s32k3x8_flexcan_update_irq(s);
}

static inline bool s32k3x8_flexcan_is_running(const S32K3X8FlexCANState *s)
{
    uint32_t mcr = s->regs[flexcan_reg_index(FLEXCAN_MCR_OFFSET)];

    return !(mcr & FLEXCAN_MCR_MDIS_MASK) && !(mcr & FLEXCAN_MCR_FRZACK_MASK);
}

static inline bool s32k3x8_flexcan_loopback_enabled(const S32K3X8FlexCANState *s)
{
    return (s->regs[flexcan_reg_index(FLEXCAN_CTRL1_OFFSET)] &
            FLEXCAN_CTRL1_LPB_MASK) != 0U;
}

static inline bool s32k3x8_flexcan_self_reception_enabled(
    const S32K3X8FlexCANState *s)
{
    return (s->regs[flexcan_reg_index(FLEXCAN_MCR_OFFSET)] &
            FLEXCAN_MCR_SRXDIS_MASK) == 0U;
}

static uint32_t s32k3x8_flexcan_mb_mask(const S32K3X8FlexCANState *s,
                                        unsigned mb_idx)
{
    uint32_t mcr = s->regs[flexcan_reg_index(FLEXCAN_MCR_OFFSET)];

    if (mcr & FLEXCAN_MCR_IRMQ_MASK) {
        return s->regs[flexcan_reg_index(FLEXCAN_RXIMR_OFFSET) + mb_idx];
    }
    if (mb_idx == 14) {
        return s->regs[flexcan_reg_index(FLEXCAN_RX14MASK_OFFSET)];
    }
    if (mb_idx == 15) {
        return s->regs[flexcan_reg_index(FLEXCAN_RX15MASK_OFFSET)];
    }

    return s->regs[flexcan_reg_index(FLEXCAN_RXMGMASK_OFFSET)];
}

static bool s32k3x8_flexcan_mb_matches_frame(const S32K3X8FlexCANState *s,
                                             unsigned mb_idx,
                                             const qemu_can_frame *frame)
{
    const uint32_t *mb = flexcan_mb_addr_const(s, mb_idx);
    if (mb == NULL) {
        return false;
    }

    uint32_t cs = mb[0];
    uint32_t id = mb[1];
    uint32_t mask;
    bool mb_eff;
    bool frame_eff;

    if (flexcan_cs_code(cs) != FLEXCAN_RX_EMPTY) {
        return false;
    }

    mb_eff = (cs & FLEXCAN_CS_IDE_MASK) != 0U;
    frame_eff = (frame->can_id & QEMU_CAN_EFF_FLAG) != 0U;
    if (mb_eff != frame_eff) {
        return false;
    }

    mask = s32k3x8_flexcan_mb_mask(s, mb_idx);
    if (mb_eff) {
        uint32_t mb_id = id & QEMU_CAN_EFF_MASK;
        uint32_t frame_id = frame->can_id & QEMU_CAN_EFF_MASK;
        uint32_t cmp_mask = mask & QEMU_CAN_EFF_MASK;

        return (mb_id & cmp_mask) == (frame_id & cmp_mask);
    } else {
        uint32_t mb_id = (id >> FLEXCAN_STD_ID_SHIFT) & QEMU_CAN_SFF_MASK;
        uint32_t frame_id = frame->can_id & QEMU_CAN_SFF_MASK;
        uint32_t cmp_mask =
            (mask >> FLEXCAN_STD_ID_SHIFT) & QEMU_CAN_SFF_MASK;

        return (mb_id & cmp_mask) == (frame_id & cmp_mask);
    }
}

static void s32k3x8_flexcan_store_rx_frame(S32K3X8FlexCANState *s,
                                           unsigned mb_idx,
                                           const qemu_can_frame *frame)
{
    uint32_t *mb = flexcan_mb_addr(s, mb_idx);

    if (mb == NULL) {
        return;
    }

    s32k3x8_flexcan_encode_rx_words(s, mb, frame,
                                    s32k3x8_flexcan_mb_payload_size(s, mb_idx));

    s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)] |= (1U << mb_idx);

    if (s32k3x8_flexcan_trace_enabled(s) && s32k3x8_flexcan_trace_rx_mb(mb_idx)) {
        qemu_log_mask(CPU_LOG_INT,
                      "flexcan%u:store_rx mb=%u iflag1=0x%08x imask1=0x%08x\\n",
                      s->instance_id,
                      mb_idx,
                      s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)],
                      s->regs[flexcan_reg_index(FLEXCAN_IMASK1_OFFSET)]);
    }

    s32k3x8_flexcan_update_irq(s);
}

static inline bool s32k3x8_flexcan_mb_is_rx_empty(
    const S32K3X8FlexCANState *s,
    unsigned mb_idx)
{
    const uint32_t *mb = flexcan_mb_addr_const(s, mb_idx);

    return (mb != NULL) && (flexcan_cs_code(mb[0]) == FLEXCAN_RX_EMPTY);
}

static inline bool s32k3x8_flexcan_mb_irq_enabled(
    const S32K3X8FlexCANState *s,
    unsigned mb_idx)
{
    return (s->regs[flexcan_reg_index(FLEXCAN_IMASK1_OFFSET)] &
            (1U << mb_idx)) != 0U;
}

/*
 * RX-by-ISR model used when legacy filtering (FIFO path) is disabled:
 * 1) Try strict mailbox ID/mask matching first.
 * 2) If no match, route to first interrupt-enabled RX_EMPTY mailbox.
 * 3) As a final fallback, route to first RX_EMPTY mailbox.
 */
static bool s32k3x8_flexcan_store_rx_by_isr(S32K3X8FlexCANState *s,
                                            const qemu_can_frame *frame,
                                            unsigned start_mb)
{
    for (unsigned mb = start_mb; mb < FLEXCAN_MB_COUNT; mb++) {
        if (s32k3x8_flexcan_mb_matches_frame(s, mb, frame)) {
            s32k3x8_flexcan_store_rx_frame(s, mb, frame);
            return true;
        }
    }

    for (unsigned mb = start_mb; mb < FLEXCAN_MB_COUNT; mb++) {
        if (!s32k3x8_flexcan_mb_irq_enabled(s, mb)) {
            continue;
        }
        if (!s32k3x8_flexcan_mb_is_rx_empty(s, mb)) {
            continue;
        }

        s32k3x8_flexcan_store_rx_frame(s, mb, frame);
        return true;
    }

    for (unsigned mb = start_mb; mb < FLEXCAN_MB_COUNT; mb++) {
        if (!s32k3x8_flexcan_mb_is_rx_empty(s, mb)) {
            continue;
        }

        s32k3x8_flexcan_store_rx_frame(s, mb, frame);
        return true;
    }

    return false;
}

static void s32k3x8_flexcan_tx_complete(S32K3X8FlexCANState *s,
                                        unsigned mb_idx,
                                        bool keep_abort_code)
{
    uint32_t *mb = flexcan_mb_addr(s, mb_idx);
    if (mb == NULL) {
        return;
    }

    uint32_t cs = mb[0];
    uint32_t code = keep_abort_code ? FLEXCAN_TX_ABORT : FLEXCAN_TX_INACTIVE;

    cs &= ~(FLEXCAN_CS_CODE_MASK | FLEXCAN_CS_TIMESTAMP_MASK);
    cs |= (code << FLEXCAN_CS_CODE_SHIFT) & FLEXCAN_CS_CODE_MASK;
    cs |= (uint32_t)s->timestamp++ & FLEXCAN_CS_TIMESTAMP_MASK;
    mb[0] = cs;

    s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)] |= (1U << mb_idx);
    s32k3x8_flexcan_update_irq(s);
}

static void s32k3x8_flexcan_try_tx_mb(S32K3X8FlexCANState *s, unsigned mb_idx)
{
    uint32_t *mb = flexcan_mb_addr(s, mb_idx);
    const uint8_t *mb_bytes;
    uint32_t cs;
    uint32_t code;
    qemu_can_frame frame = {0};
    uint8_t payload_cap = s32k3x8_flexcan_mb_payload_size(s, mb_idx);
    uint8_t len;
    bool is_fd;
    uint8_t copy_len;

    if (mb == NULL) {
        return;
    }

    cs = mb[0];
    code = flexcan_cs_code(cs);
    mb_bytes = (const uint8_t *)&mb[2];

    if (!s32k3x8_flexcan_is_running(s)) {
        return;
    }

    if (s32k3x8_flexcan_legacy_fifo_enabled(s)) {
        uint32_t occupied_last = 5U + (s32k3x8_flexcan_legacy_filter_count(s) / 4U);

        if (mb_idx <= occupied_last) {
            return;
        }
    }

    if (code == FLEXCAN_TX_ABORT) {
        s32k3x8_flexcan_tx_complete(s, mb_idx, true);
        return;
    }

    if (code != FLEXCAN_TX_DATA_CODE) {
        return;
    }

    if (cs & FLEXCAN_CS_IDE_MASK) {
        frame.can_id = mb[1] & QEMU_CAN_EFF_MASK;
        frame.can_id |= QEMU_CAN_EFF_FLAG;
    } else {
        frame.can_id = (mb[1] >> FLEXCAN_STD_ID_SHIFT) & QEMU_CAN_SFF_MASK;
    }

    if (cs & FLEXCAN_CS_RTR_MASK) {
        frame.can_id |= QEMU_CAN_RTR_FLAG;
    }

    len = can_dlc2len((cs & FLEXCAN_CS_DLC_MASK) >> FLEXCAN_CS_DLC_SHIFT);
    is_fd = ((cs & FLEXCAN_CS_EDL_MASK) != 0U) || (len > 8U);
    frame.can_dlc = len;
    frame.flags = 0U;
    if (is_fd) {
        frame.flags |= QEMU_CAN_FRMF_TYPE_FD;
        if (cs & FLEXCAN_CS_BRS_MASK) {
            frame.flags |= QEMU_CAN_FRMF_BRS;
        }
        if (cs & FLEXCAN_CS_ESI_MASK) {
            frame.flags |= QEMU_CAN_FRMF_ESI;
        }
    }

    copy_len = MIN(len, payload_cap);
    for (uint8_t i = 0; i < copy_len; i++) {
        frame.data[i] = mb_bytes[flexcan_word_swapped_index(i)];
    }
    for (uint8_t i = copy_len; i < len; i++) {
        frame.data[i] = 0xCC;
    }

    if (s32k3x8_flexcan_loopback_enabled(s) ||
        s32k3x8_flexcan_self_reception_enabled(s)) {
        (void)s32k3x8_flexcan_receive(&s->bus_client, &frame, 1);
    }

    if (!s32k3x8_flexcan_loopback_enabled(s) && s->bus_client.bus) {
        (void)can_bus_client_send(&s->bus_client, &frame, 1);
    }

    s32k3x8_flexcan_tx_complete(s, mb_idx, false);
}

static uint64_t s32k3x8_flexcan_read(void *opaque, hwaddr addr, unsigned size)
{
    S32K3X8FlexCANState *s = S32K3X8_FLEXCAN(opaque);
    uint32_t idx;

    if ((size != 1U) && (size != 2U) && (size != 4U)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k3x8-flexcan: invalid read size %u @0x%" HWADDR_PRIx
                      "\n", size, addr);
        return 0;
    }
    if ((addr + size) > S32K3X8_FLEXCAN_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k3x8-flexcan: out-of-range read @0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }

    if (addr == FLEXCAN_TIMER_OFFSET) {
        s->regs[flexcan_reg_index(FLEXCAN_TIMER_OFFSET)] =
            (uint32_t)s->timestamp++;

        if (s32k3x8_flexcan_legacy_fifo_enabled(s) &&
            (s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)] &
             FLEXCAN_IFLAG1_BUF5I_MASK) == 0U) {
            s32k3x8_flexcan_legacy_present_front(s);
            s32k3x8_flexcan_legacy_update_status(s);
            s32k3x8_flexcan_update_irq(s);
        }
    }

    if ((size != 4U) || ((addr & 0x3U) != 0U)) {
        return flexcan_raw_read_le(s, addr, size);
    }

    idx = flexcan_reg_index(addr);
    return s->regs[idx];
}

static void s32k3x8_flexcan_write(void *opaque,
                                  hwaddr addr,
                                  uint64_t value,
                                  unsigned size)
{
    S32K3X8FlexCANState *s = S32K3X8_FLEXCAN(opaque);
    uint32_t v = (uint32_t)value;
    uint32_t idx;
    unsigned mb_idx;

    if ((size != 1U) && (size != 2U) && (size != 4U)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k3x8-flexcan: invalid write size %u @0x%" HWADDR_PRIx
                      "\n", size, addr);
        return;
    }
    if ((addr + size) > S32K3X8_FLEXCAN_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k3x8-flexcan: out-of-range write @0x%" HWADDR_PRIx
                      "\n", addr);
        return;
    }

    if ((size != 4U) || ((addr & 0x3U) != 0U)) {
        flexcan_raw_write_le(s, addr, v, size);
        return;
    }

    idx = flexcan_reg_index(addr);
    switch (addr) {
    case FLEXCAN_MCR_OFFSET:
    {
        bool old_rfen = s32k3x8_flexcan_legacy_fifo_enabled(s);

        s->regs[idx] &= FLEXCAN_MCR_LPMACK_MASK |
                        FLEXCAN_MCR_FRZACK_MASK |
                        FLEXCAN_MCR_NOTRDY_MASK;
        s->regs[idx] |= v & ~(FLEXCAN_MCR_LPMACK_MASK |
                              FLEXCAN_MCR_FRZACK_MASK |
                              FLEXCAN_MCR_NOTRDY_MASK);
        if (v & FLEXCAN_MCR_SOFTRST_MASK) {
            s32k3x8_flexcan_reset_registers(s);
        } else {
            s32k3x8_flexcan_update_mcr_state(s);

            if (s32k3x8_flexcan_legacy_fifo_enabled(s) != old_rfen) {
                s32k3x8_flexcan_reset_legacy_fifo(s);
            }
            if (s32k3x8_flexcan_legacy_fifo_enabled(s) &&
                s32k3x8_flexcan_enhanced_fifo_enabled(s)) {
                s->regs[flexcan_reg_index(FLEXCAN_ERFCR_OFFSET)] &=
                    ~FLEXCAN_ERFCR_ERFEN_MASK;
                s32k3x8_flexcan_reset_enhanced_fifo(s);
            }
        }
        s32k3x8_flexcan_update_irq(s);
        break;
    }
    case FLEXCAN_IMASK1_OFFSET:
    case FLEXCAN_IMASK2_OFFSET:
    case FLEXCAN_IMASK3_OFFSET:
        s->regs[idx] = v;
        if ((addr == FLEXCAN_IMASK1_OFFSET) && s32k3x8_flexcan_trace_enabled(s)) {
            qemu_log_mask(CPU_LOG_INT,
                          "flexcan%u:imask1_write v=0x%08x iflag1=0x%08x\\n",
                          s->instance_id,
                          v,
                          s->regs[flexcan_reg_index(FLEXCAN_IFLAG1_OFFSET)]);
        }
        s32k3x8_flexcan_update_irq(s);
        break;
    case FLEXCAN_IFLAG1_OFFSET:
    {
        uint32_t iflag1_before = s->regs[idx];

        if (s32k3x8_flexcan_trace_enabled(s)) {
            qemu_log_mask(CPU_LOG_INT,
                          "flexcan%u:iflag1_w1c v=0x%08x before=0x%08x imask1=0x%08x\\n",
                          s->instance_id,
                          v,
                          iflag1_before,
                          s->regs[flexcan_reg_index(FLEXCAN_IMASK1_OFFSET)]);
        }

        if (s32k3x8_flexcan_legacy_fifo_enabled(s) &&
            (v & FLEXCAN_IFLAG1_BUF5I_MASK) &&
            (s->regs[idx] & FLEXCAN_IFLAG1_BUF5I_MASK)) {
            s->regs[idx] &= ~(v & ~FLEXCAN_IFLAG1_BUF5I_MASK);
            s32k3x8_flexcan_legacy_pop(s);
        } else {
            s->regs[idx] &= ~v;
            s32k3x8_flexcan_rearm_rx_mb_from_iflag1(s, v);
        }
        if (v & FLEXCAN_IFLAG1_BUF7I_MASK) {
            s->regs[idx] &= ~FLEXCAN_IFLAG1_BUF7I_MASK;
        }

        if (s32k3x8_flexcan_trace_enabled(s)) {
            qemu_log_mask(CPU_LOG_INT,
                          "flexcan%u:iflag1_after=0x%08x mb24_code=%u mb25_code=%u\\n",
                          s->instance_id,
                          s->regs[idx],
                          s32k3x8_flexcan_trace_mb_code(s, FLEXCAN_TRACE_RX_MB_A),
                          s32k3x8_flexcan_trace_mb_code(s, FLEXCAN_TRACE_RX_MB_B));
        }

        s32k3x8_flexcan_update_irq(s);
        break;
    }
    case FLEXCAN_IFLAG2_OFFSET:
    case FLEXCAN_IFLAG3_OFFSET:
        /* W1C. */
        s->regs[idx] &= ~v;
        s32k3x8_flexcan_update_irq(s);
        break;
    case FLEXCAN_ERFCR_OFFSET:
    {
        bool old_erfen = s32k3x8_flexcan_enhanced_fifo_enabled(s);

        s->regs[idx] = v;
        if (s32k3x8_flexcan_legacy_fifo_enabled(s)) {
            s->regs[idx] &= ~FLEXCAN_ERFCR_ERFEN_MASK;
        }

        if (s32k3x8_flexcan_enhanced_fifo_enabled(s) != old_erfen) {
            s32k3x8_flexcan_reset_enhanced_fifo(s);
            if (s32k3x8_flexcan_enhanced_fifo_enabled(s)) {
                s32k3x8_flexcan_enhanced_update_status(s);
            }
        } else if (s32k3x8_flexcan_enhanced_fifo_enabled(s)) {
            s32k3x8_flexcan_enhanced_update_status(s);
        }

        s32k3x8_flexcan_update_irq(s);
        break;
    }
    case FLEXCAN_ERFIER_OFFSET:
        s->regs[idx] = v;
        s32k3x8_flexcan_update_irq(s);
        break;
    case FLEXCAN_ERFSR_OFFSET:
        if (v & FLEXCAN_ERFSR_ERFCLR_MASK) {
            s32k3x8_flexcan_reset_enhanced_fifo(s);
            if (s32k3x8_flexcan_enhanced_fifo_enabled(s)) {
                s32k3x8_flexcan_enhanced_update_status(s);
            }
        } else if (v & FLEXCAN_ERFSR_ERFDA_MASK) {
            s->regs[idx] &= ~(v & (FLEXCAN_ERFSR_ERFWMI_MASK |
                                   FLEXCAN_ERFSR_ERFOVF_MASK |
                                   FLEXCAN_ERFSR_ERFUFW_MASK));
            s32k3x8_flexcan_enhanced_pop(s);
        } else {
            s->regs[idx] &= ~(v & (FLEXCAN_ERFSR_ERFWMI_MASK |
                                   FLEXCAN_ERFSR_ERFOVF_MASK |
                                   FLEXCAN_ERFSR_ERFUFW_MASK));
            if (s32k3x8_flexcan_enhanced_fifo_enabled(s)) {
                s32k3x8_flexcan_enhanced_update_status(s);
            }
        }
        s32k3x8_flexcan_update_irq(s);
        break;
    case FLEXCAN_ESR1_OFFSET:
        s->regs[idx] &= ~(v & FLEXCAN_ESR1_W1C_MASK);
        break;
    default:
        s->regs[idx] = v;
        break;
    }

    if ((size == 4U) && flexcan_is_mb_cs_addr(s, addr, &mb_idx)) {
        s32k3x8_flexcan_try_tx_mb(s, mb_idx);
    }
}

static const MemoryRegionOps s32k3x8_flexcan_ops = {
    .read = s32k3x8_flexcan_read,
    .write = s32k3x8_flexcan_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static bool s32k3x8_flexcan_can_receive(CanBusClientState *client)
{
    S32K3X8FlexCANState *s = container_of(client, S32K3X8FlexCANState,
                                          bus_client);

    return s32k3x8_flexcan_is_running(s);
}

static ssize_t s32k3x8_flexcan_receive(CanBusClientState *client,
                                       const qemu_can_frame *frames,
                                       size_t frames_cnt)
{
    S32K3X8FlexCANState *s = container_of(client, S32K3X8FlexCANState,
                                          bus_client);
    ssize_t accepted = 0;

    for (size_t fi = 0; fi < frames_cnt; fi++) {
        const qemu_can_frame *frame = &frames[fi];
        bool stored = false;

        if (s32k3x8_flexcan_enhanced_fifo_enabled(s)) {
            stored = s32k3x8_flexcan_enhanced_enqueue(s, frame);
        } else if (s32k3x8_flexcan_legacy_fifo_enabled(s)) {
            uint16_t idhit = 0U;

            if (s32k3x8_flexcan_legacy_filter_accept(s, frame, &idhit)) {
                /* Keep native legacy FIFO behavior, including overflow handling. */
                stored = s32k3x8_flexcan_legacy_enqueue(s, frame);
            } else {
                uint32_t occupied_last =
                    5U + (s32k3x8_flexcan_legacy_filter_count(s) / 4U);
                unsigned first_mb = (unsigned)(occupied_last + 1U);

                /* On FIFO filter miss, fall back to regular RX MB delivery. */
                stored = s32k3x8_flexcan_store_rx_by_isr(s, frame, first_mb);
            }
        } else {
            stored = s32k3x8_flexcan_store_rx_by_isr(s, frame, 0U);
        }

        if (stored) {
            accepted++;
        } else {
            /* Frame dropped when no RX_EMPTY MB matches. */
        }

        s32k3x8_flexcan_update_irq(s);
    }

    return accepted;
}

static CanBusClientInfo s32k3x8_flexcan_bus_client_info = {
    .can_receive = s32k3x8_flexcan_can_receive,
    .receive = s32k3x8_flexcan_receive,
};

static void s32k3x8_flexcan_reset(DeviceState *dev)
{
    S32K3X8FlexCANState *s = S32K3X8_FLEXCAN(dev);

    s32k3x8_flexcan_reset_registers(s);
}

static void s32k3x8_flexcan_realize(DeviceState *dev, Error **errp)
{
    S32K3X8FlexCANState *s = S32K3X8_FLEXCAN(dev);

    s32k3x8_flexcan_reset(dev);

    if (s->canbus) {
        s->bus_client.info = &s32k3x8_flexcan_bus_client_info;
        if (can_bus_insert_client(s->canbus, &s->bus_client) < 0) {
            error_setg(errp, "s32k3x8-flexcan%u: failed to connect canbus",
                       s->instance_id);
            return;
        }
    }
}

static void s32k3x8_flexcan_unrealize(DeviceState *dev)
{
    S32K3X8FlexCANState *s = S32K3X8_FLEXCAN(dev);

    if (s->bus_client.bus) {
        can_bus_remove_client(&s->bus_client);
    }
}

static void s32k3x8_flexcan_init(Object *obj)
{
    S32K3X8FlexCANState *s = S32K3X8_FLEXCAN(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &s32k3x8_flexcan_ops, s,
                          TYPE_S32K3X8_FLEXCAN, S32K3X8_FLEXCAN_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
}

static Property s32k3x8_flexcan_properties[] = {
    DEFINE_PROP_UINT32("can-instance", S32K3X8FlexCANState, instance_id, 0),
    DEFINE_PROP_LINK("canbus", S32K3X8FlexCANState, canbus, TYPE_CAN_BUS,
                     CanBusState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void s32k3x8_flexcan_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s32k3x8_flexcan_realize;
    dc->unrealize = s32k3x8_flexcan_unrealize;
    device_class_set_legacy_reset(dc, s32k3x8_flexcan_reset);
    device_class_set_props(dc, s32k3x8_flexcan_properties);
}

static const TypeInfo s32k3x8_flexcan_info = {
    .name = TYPE_S32K3X8_FLEXCAN,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S32K3X8FlexCANState),
    .instance_init = s32k3x8_flexcan_init,
    .class_init = s32k3x8_flexcan_class_init,
};

static void s32k3x8_flexcan_register_types(void)
{
    type_register_static(&s32k3x8_flexcan_info);
}

type_init(s32k3x8_flexcan_register_types)
