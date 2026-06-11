/*
 * NXP S32K358 DMAMUX model (minimal CHCFG implementation).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/dma/s32k358_dmamux.h"
#include "hw/dma/s32k358_dma.h"

#define S32K358_DMAMUX_DEBUG 1

#define DMAMUX_PRINT(fmt, args...) do { \
    if (S32K358_DMAMUX_DEBUG >= 1) { \
        qemu_log("[QEMU/S32K358-DMAMUX] %s: " fmt, __func__, ## args); \
    } \
} while (0)

static S32K358DMAMUXState *g_s32k358_dmamux[S32K358_DMAMUX_INSTANCE_COUNT];
static uint32_t g_s32k358_dmamux_instance_seed = 0U;

static uint32_t s32k358_dmamux_gate_offset(uint32_t channel)
{
    return (channel ^ 3U);
}

static uint32_t s32k358_dmamux_channel_to_dma_channel(uint32_t instance,
                                                      uint32_t dmamux_channel)
{
    uint32_t dma_channel;

    dma_channel = dmamux_channel;
    if (S32K358_DMAMUX_INSTANCE_1 == instance) {
        dma_channel += 16U;
    }

    return dma_channel;
}

void s32k358_dmamux_request(uint32_t instance, uint32_t source)
{
    S32K358DMAMUXState *s;
    uint32_t ch_num;
    uint32_t cfg_source;

    if (S32K358_DMAMUX_INSTANCE_COUNT <= instance) {
        return;
    }

    s = g_s32k358_dmamux[instance];
    if (NULL == s) {
        return;
    }

    DMAMUX_PRINT("instance=%u source=%u request\n", instance, source);

    for (ch_num = 0U; ch_num < S32K358_DMAMUX_NUM_CHANNELS; ch_num++) {
        if (0U == (s->chcfg[ch_num] & S32K358_DMAMUX_CHCFG_ENBL_MASK)) {
            continue;
        }

        cfg_source = (uint32_t)(s->chcfg[ch_num] &
                                S32K358_DMAMUX_CHCFG_SOURCE_MASK);
        if (cfg_source != source) {
            continue;
        }

        DMAMUX_PRINT("instance=%u ch=%u cfg=0x%02X -> dma_ch=%u\n",
                     instance,
                     ch_num,
                     s->chcfg[ch_num],
                     s32k358_dmamux_channel_to_dma_channel(instance, ch_num));
        s32k358_dma_hw_request(s32k358_dmamux_channel_to_dma_channel(instance,
                                                                     ch_num));
    }
}

static uint64_t s32k358_dmamux_read(void *opaque, hwaddr addr, unsigned size)
{
    S32K358DMAMUXState *s = S32K358_DMAMUX(opaque);
    uint64_t value = 0;

    if (addr + size > S32K358_DMAMUX_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-dmamux: invalid read addr=0x%" HWADDR_PRIx
                      " size=%u\n", addr, size);
        return 0;
    }

    for (unsigned i = 0; i < size; i++) {
        hwaddr off = addr + i;
        uint8_t byte = 0;
        uint32_t channel;

        if (off < S32K358_DMAMUX_NUM_CHANNELS) {
            channel = s32k358_dmamux_gate_offset((uint32_t)off);
            if (channel < S32K358_DMAMUX_NUM_CHANNELS) {
                byte = s->chcfg[channel];
            }
        }
        value |= ((uint64_t)byte << (8U * i));
    }

    return value;
}

static void s32k358_dmamux_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    S32K358DMAMUXState *s = S32K358_DMAMUX(opaque);

    if (addr + size > S32K358_DMAMUX_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s32k358-dmamux: invalid write addr=0x%" HWADDR_PRIx
                      " size=%u val=0x%" PRIx64 "\n",
                      addr, size, value);
        return;
    }

    for (unsigned i = 0; i < size; i++) {
        hwaddr off = addr + i;
        uint8_t byte = (uint8_t)(value >> (8U * i));
        uint32_t channel;

        if (off < S32K358_DMAMUX_NUM_CHANNELS) {
            channel = s32k358_dmamux_gate_offset((uint32_t)off);
            if (channel < S32K358_DMAMUX_NUM_CHANNELS) {
                s->chcfg[channel] = byte;
                DMAMUX_PRINT("instance=%u write off=%u ch=%u cfg=0x%02X\n",
                             s->instance,
                             (unsigned)off,
                             channel,
                             byte);
            }
        }
    }
}

static const MemoryRegionOps s32k358_dmamux_ops = {
    .read = s32k358_dmamux_read,
    .write = s32k358_dmamux_write,
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

static void s32k358_dmamux_reset(DeviceState *dev)
{
    S32K358DMAMUXState *s = S32K358_DMAMUX(dev);

    memset(s->chcfg, 0, sizeof(s->chcfg));
}

static void s32k358_dmamux_instance_init(Object *obj)
{
    S32K358DMAMUXState *s = S32K358_DMAMUX(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->instance = (uint8_t)g_s32k358_dmamux_instance_seed;
    if (g_s32k358_dmamux_instance_seed < (S32K358_DMAMUX_INSTANCE_COUNT - 1U)) {
        g_s32k358_dmamux_instance_seed++;
    }

    memory_region_init_io(&s->mmio, obj, &s32k358_dmamux_ops, s,
                          TYPE_S32K358_DMAMUX, S32K358_DMAMUX_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    g_s32k358_dmamux[s->instance] = s;
}

static void s32k358_dmamux_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "S32K358 DMAMUX";
    device_class_set_legacy_reset(dc, s32k358_dmamux_reset);
}

static const TypeInfo s32k358_dmamux_info = {
    .name = TYPE_S32K358_DMAMUX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S32K358DMAMUXState),
    .instance_init = s32k358_dmamux_instance_init,
    .class_init = s32k358_dmamux_class_init,
};

static void s32k358_dmamux_register_types(void)
{
    type_register_static(&s32k358_dmamux_info);
}

type_init(s32k358_dmamux_register_types)
