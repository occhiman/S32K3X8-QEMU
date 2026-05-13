#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "sysemu/sysemu.h"
#include "hw/qdev-core.h"
#include "hw/sysbus.h"
#include "hw/arm/s32k3x8evb.h"
#include "hw/char/s32k3x8_uart.h"
#include "hw/ssi/s32k358_spi.h"
#include "hw/net/s32k3x8_flexcan.h"
#include "hw/dma/s32k358_dma.h"
#include "hw/dma/s32k358_dmamux.h"
#include "hw/misc/s32k358_mscm.h"
#include "hw/arm/boot.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "hw/clock.h"
#include "qemu/units.h"
#include "migration/vmstate.h"
#include "hw/irq.h"
#include <stdio.h>

// Memory region definitions - Based on S32K358 linker file
#define INT_ITCM_BASE           0x00000000  // Instruction Tightly Coupled Memory
#define INT_ITCM_SIZE           (64 * KiB)  // 64KB

#define INT_DTCM_BASE           0x20000000  // Data Tightly Coupled Memory
#define INT_DTCM_SIZE           (124 * KiB) // 124KB
#define INT_STACK_DTCM_BASE     0x2001F000  // Stack area in DTCM
#define INT_STACK_DTCM_SIZE     (4 * KiB)   // 4KB

/*flash --774 page*/
#define FLASH_SIZE              0x00822000
#define INT_CODE_FLASH0_BASE    0x00400000
#define INT_CODE_FLASH0_SIZE    0x00200000    // 2 MB 
#define INT_PFLASH_VECTOR_BASE  0x00490400
#define INT_CODE_FLASH1_BASE    0x00600000 
#define INT_CODE_FLASH1_SIZE    0x00200000    // 2 MB 
#define INT_CODE_FLASH2_BASE    0x00800000
#define INT_CODE_FLASH2_SIZE    0x00200000    // 2 MB 
#define INT_CODE_FLASH3_BASE    0x00A00000
#define INT_CODE_FLASH3_SIZE    0x00200000    // 2 MB 
#define INT_DATA_FLASH_BASE     0x10000000
#define INT_DATA_FLASH_SIZE     0x00020000    // 128 KB 
#define INT_UTEST_NVM_FLASH_BASE 0x1B000000
#define INT_UTEST_NVM_FLASH_SIZE 0x00002000    // 8 KB 


/*sram -- 762 page*/
#define SRAM_SIZE               0xC0000
#define INT_SRAM_STANDBY_BASE   0x20400000  // SRAM_standby, SPLIT FROM SRAM0
#define INT_SRAM_STANDBY_SIZE   0x10000     // 64KB
#define INT_SRAM_0_BASE         0x20410000
#define INT_SRAM_0_SIZE         0x30000     // 256KB-64KB = 192KB
#define INT_SRAM_1_BASE         0x20440000
#define INT_SRAM_1_SIZE         0x40000     // 256KB
#define INT_SRAM_2_BASE         0x20480000
#define INT_SRAM_2_SIZE         0x40000     // 256KB


// Peripheral base address definitions
#define S32K3_PERIPH_BASE        0x40000000

//lpuart instance addresses
#define S32K3_UART_BASE        0x40328000      // memory mapping file provides
#define S32K3_LPUART1_BASE     0x4032C000
#define S32K3_LPUART2_BASE     0x40330000
#define S32K3_LPUART3_BASE     0x40334000
#define S32K3_LPUART4_BASE     0x40338000
#define S32K3_LPUART5_BASE     0x4033C000
#define S32K3_LPUART6_BASE     0x40340000
#define S32K3_LPUART7_BASE     0x40344000
#define S32K3_LPUART8_BASE     0x4048C000
#define S32K3_LPUART9_BASE     0x40490000
#define S32K3_LPUART10_BASE    0x40494000
#define S32K3_LPUART11_BASE    0x40498000
#define S32K3_LPUART12_BASE    0x4049C000
#define S32K3_LPUART13_BASE    0x404A0000
#define S32K3_LPUART14_BASE    0x404A4000
#define S32K3_LPUART15_BASE    0x404A8000
// System frequency definitions
#define S32K3_SYSCLK_FREQ        (160 * 1000 * 1000)  // 160MHz

#define S32K3_LPSPI0_BASE        (S32K3_PERIPH_BASE + 0x358000) // 0x40358000
#define S32K3_LPSPI1_BASE        (S32K3_PERIPH_BASE + 0x35C000) // 0x4035C000
#define S32K3_LPSPI2_BASE        (S32K3_PERIPH_BASE + 0x360000) // 0x40360000
#define S32K3_LPSPI3_BASE        (S32K3_PERIPH_BASE + 0x364000) // 0x40364000
#define S32K3_LPSPI4_BASE        (S32K3_PERIPH_BASE + 0x4BC000) // 0x404BC000
#define S32K3_LPSPI5_BASE        (S32K3_PERIPH_BASE + 0x4C0000) // 0x404C0000

//LPSPI interrupt number definitions
#define S32K3_LPSPI0_IRQ         69
#define S32K3_LPSPI1_IRQ         70
#define S32K3_LPSPI2_IRQ         71
#define S32K3_LPSPI3_IRQ         72
#define S32K3_LPSPI4_IRQ         73
#define S32K3_LPSPI5_IRQ         74

/* eDMA channel interrupt range: DMATCD0..DMATCD31. */
#define S32K3_DMATCD0_IRQ        4

/* MSCM base address (CPXNUM at +0x4 used for core ID). */
#define S32K3_MSCM_BASE          0x40260000U

/* DMAMUX instances used by S32K3x8: DMAMUX_0 and DMAMUX_1. */
#define S32K3_DMAMUX0_BASE       0x40280000U
#define S32K3_DMAMUX1_BASE       0x40284000U

/* Clocking/power control blocks used by Clock_Ip_Init on S32K344. */
#define S32K3_FIRC_BASE          0x402D0000U
#define S32K3_FIRC_SIZE          0x4000U
#define S32K3_SIRC_BASE          0x402C8000U
#define S32K3_SIRC_SIZE          0x4000U
#define S32K3_SXOSC_BASE         0x402CC000U
#define S32K3_SXOSC_SIZE         0x4000U
#define S32K3_FXOSC_BASE         0x402D4000U
#define S32K3_FXOSC_SIZE         0x4000U
#define S32K3_MC_CGM_BASE        0x402D8000U
#define S32K3_MC_CGM_SIZE        0x4000U
#define S32K3_MC_ME_BASE         0x402DC000U
#define S32K3_MC_ME_SIZE         0x4000U
#define S32K3_PLL_BASE           0x402E0000U
#define S32K3_PLL_SIZE           0x4000U
#define S32K3_CMU_BASE           0x402BC000U
#define S32K3_CMU_SIZE           0x4000U
#define S32K3_RTC_BASE           0x40288000U
#define S32K3_RTC_SIZE           0x4000U
#define S32K3_MC_RGM_BASE        0x4028C000U
#define S32K3_MC_RGM_SIZE        0x4000U
#define S32K3_CONFIGURATION_GPR_BASE 0x4039C000U
#define S32K3_CONFIGURATION_GPR_SIZE 0x4000U
#define S32K3_PRAMC0_BASE        0x40264000U
#define S32K3_PRAMC0_SIZE        0x4000U
#define S32K3_PRAMC1_BASE        0x40464000U
#define S32K3_PRAMC1_SIZE        0x4000U
#define S32K3_FLASH_CTRL_BASE    0x402EC000U
#define S32K3_FLASH_CTRL_SIZE    0x4000U

/* FlexCAN instances used by this machine: CAN0..CAN7 (S32K358). */
#define S32K3_FLEXCAN0_BASE      (S32K3_PERIPH_BASE + 0x304000) /* 0x40304000 */
#define S32K3_FLEXCAN1_BASE      (S32K3_PERIPH_BASE + 0x308000) /* 0x40308000 */
#define S32K3_FLEXCAN2_BASE      (S32K3_PERIPH_BASE + 0x30C000) /* 0x4030C000 */
#define S32K3_FLEXCAN3_BASE      (S32K3_PERIPH_BASE + 0x310000) /* 0x40310000 */
#define S32K3_FLEXCAN4_BASE      (S32K3_PERIPH_BASE + 0x314000) /* 0x40314000 */
#define S32K3_FLEXCAN5_BASE      (S32K3_PERIPH_BASE + 0x318000) /* 0x40318000 */
#define S32K3_FLEXCAN6_BASE      (S32K3_PERIPH_BASE + 0x31C000) /* 0x4031C000 */
#define S32K3_FLEXCAN7_BASE      (S32K3_PERIPH_BASE + 0x320000) /* 0x40320000 */

/* Message Buffer interrupt line 0-31 for CAN0..CAN7. */
#define S32K3_FLEXCAN0_MB_IRQ    110
#define S32K3_FLEXCAN1_MB_IRQ    114
#define S32K3_FLEXCAN2_MB_IRQ    117
#define S32K3_FLEXCAN3_MB_IRQ    120
#define S32K3_FLEXCAN4_MB_IRQ    122
#define S32K3_FLEXCAN5_MB_IRQ    124
#define S32K3_FLEXCAN6_MB_IRQ    126
#define S32K3_FLEXCAN7_MB_IRQ    128

static bool s32k3x8evb_get_vector_from_int_pflash(Object *obj, Error **errp)
{
    return S32K3X8EVB_MACHINE(obj)->vector_from_int_pflash;
}

static void s32k3x8evb_set_vector_from_int_pflash(Object *obj, bool value,
                                                   Error **errp)
{
    S32K3X8EVB_MACHINE(obj)->vector_from_int_pflash = value;
}

static uint32_t s32k3x8evb_startup_vector_base(const S32K3X8EVBState *s)
{
    if (s->vector_from_int_pflash) {
        return INT_PFLASH_VECTOR_BASE;
    }
    return INT_ITCM_BASE;
}

/*Memory mapping initialization function*/
static void s32k3x8_initialize_memory_regions(MemoryRegion *system_memory)
{
    qemu_log_mask(CPU_LOG_INT, "\n------------------ Initialization of the memory regions ------------------\n");
    
    /* Allocate all memory regions */
    MemoryRegion *itcm = g_new(MemoryRegion, 1);
    MemoryRegion *dtcm = g_new(MemoryRegion, 1);
    MemoryRegion *dtcm_stack = g_new(MemoryRegion, 1);

    
    MemoryRegion *C0flash = g_new(MemoryRegion, 1);
    MemoryRegion *C1flash = g_new(MemoryRegion, 1);
    MemoryRegion *C2flash = g_new(MemoryRegion, 1);
    MemoryRegion *C3flash = g_new(MemoryRegion, 1);
    MemoryRegion *Dflash = g_new(MemoryRegion, 1);
    MemoryRegion *UNVMflash = g_new(MemoryRegion, 1);

    MemoryRegion *sram_standby = g_new(MemoryRegion, 1);
    MemoryRegion *sram0 = g_new(MemoryRegion, 1);
    MemoryRegion *sram1 = g_new(MemoryRegion, 1);
    MemoryRegion *sram2 = g_new(MemoryRegion, 1);
    MemoryRegion *firc = g_new(MemoryRegion, 1);
    MemoryRegion *sirc = g_new(MemoryRegion, 1);
    MemoryRegion *sxosc = g_new(MemoryRegion, 1);
    MemoryRegion *fxosc = g_new(MemoryRegion, 1);
    MemoryRegion *mc_cgm = g_new(MemoryRegion, 1);
    MemoryRegion *mc_me = g_new(MemoryRegion, 1);
    MemoryRegion *pll = g_new(MemoryRegion, 1);
    MemoryRegion *cmu = g_new(MemoryRegion, 1);
    MemoryRegion *rtc = g_new(MemoryRegion, 1);
    MemoryRegion *mc_rgm = g_new(MemoryRegion, 1);
    MemoryRegion *configuration_gpr = g_new(MemoryRegion, 1);
    MemoryRegion *pramc0 = g_new(MemoryRegion, 1);
    MemoryRegion *pramc1 = g_new(MemoryRegion, 1);
    MemoryRegion *flash_ctrl = g_new(MemoryRegion, 1);
      
    /* ITCM init - RAM */
    qemu_log_mask(CPU_LOG_INT, "Initializing ITCM...\n");
    memory_region_init_ram(itcm, NULL, "s32k3x8.itcm", INT_ITCM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, INT_ITCM_BASE, itcm);
    
    /* DTCM init - RAM */
    qemu_log_mask(CPU_LOG_INT, "Initializing DTCM...\n");
    memory_region_init_ram(dtcm, NULL, "s32k3x8.dtcm", INT_DTCM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, INT_DTCM_BASE, dtcm);
    
    /* DTCM stack region */
    memory_region_init_ram(dtcm_stack, NULL, "s32k3x8.dtcm_stack", INT_STACK_DTCM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, INT_STACK_DTCM_BASE, dtcm_stack);
    
    /* Program Flash initial - ROM */
    qemu_log_mask(CPU_LOG_INT, "Initializing Program Flash...\n");
    memory_region_init_rom(C0flash, NULL, "s32k3x8.C0flash", INT_CODE_FLASH0_SIZE, &error_fatal);
    memory_region_init_rom(C1flash, NULL, "s32k3x8.C1lash", INT_CODE_FLASH1_SIZE, &error_fatal);
    memory_region_init_rom(C2flash, NULL, "s32k3x8.C2lash", INT_CODE_FLASH2_SIZE, &error_fatal);
    memory_region_init_rom(C3flash, NULL, "s32k3x8.C3lash", INT_CODE_FLASH3_SIZE, &error_fatal);
    memory_region_init_rom(Dflash, NULL, "s32k3x8.Dflash", INT_DATA_FLASH_SIZE, &error_fatal);
    memory_region_init_rom(UNVMflash, NULL, "s32k3x8.UNVMflash", INT_UTEST_NVM_FLASH_SIZE, &error_fatal);

    memory_region_add_subregion(system_memory, INT_CODE_FLASH0_BASE, C0flash);
    memory_region_add_subregion(system_memory, INT_CODE_FLASH1_BASE, C1flash);
    memory_region_add_subregion(system_memory, INT_CODE_FLASH2_BASE, C2flash);
    memory_region_add_subregion(system_memory, INT_CODE_FLASH3_BASE, C3flash);
    memory_region_add_subregion(system_memory, INT_DATA_FLASH_BASE, Dflash);
    memory_region_add_subregion(system_memory, INT_UTEST_NVM_FLASH_BASE, UNVMflash);
    
    /* SRAM region init */
    qemu_log_mask(CPU_LOG_INT, "Initializing SRAM regions...\n");
    memory_region_init_ram(sram_standby, NULL, "s32k3x8.sram_standby", INT_SRAM_STANDBY_SIZE, &error_fatal);
    memory_region_init_ram(sram0, NULL, "s32k3x8.sram0", INT_SRAM_0_SIZE, &error_fatal);
    memory_region_init_ram(sram1, NULL, "s32k3x8.sram1", INT_SRAM_1_SIZE, &error_fatal);
    memory_region_init_ram(sram2, NULL, "s32k3x8.sram2", INT_SRAM_2_SIZE, &error_fatal);

    memory_region_add_subregion(system_memory, INT_SRAM_STANDBY_BASE, sram_standby);
    memory_region_add_subregion(system_memory, INT_SRAM_0_BASE, sram0);
    memory_region_add_subregion(system_memory, INT_SRAM_1_BASE, sram1);
    memory_region_add_subregion(system_memory, INT_SRAM_2_BASE, sram2);

    /*
     * Stub the clock/power-control address space so Clock_Ip_Init accesses
     * do not fault while the full register-level models are still pending.
     */
    qemu_log_mask(CPU_LOG_INT, "Initializing clock control MMIO stubs...\n");
    memory_region_init_ram(firc, NULL, "s32k3x8.firc", S32K3_FIRC_SIZE, &error_fatal);
    memory_region_init_ram(sirc, NULL, "s32k3x8.sirc", S32K3_SIRC_SIZE, &error_fatal);
    memory_region_init_ram(sxosc, NULL, "s32k3x8.sxosc", S32K3_SXOSC_SIZE, &error_fatal);
    memory_region_init_ram(fxosc, NULL, "s32k3x8.fxosc", S32K3_FXOSC_SIZE, &error_fatal);
    memory_region_init_ram(mc_cgm, NULL, "s32k3x8.mc_cgm", S32K3_MC_CGM_SIZE, &error_fatal);
    memory_region_init_ram(mc_me, NULL, "s32k3x8.mc_me", S32K3_MC_ME_SIZE, &error_fatal);
    memory_region_init_ram(pll, NULL, "s32k3x8.pll", S32K3_PLL_SIZE, &error_fatal);
    memory_region_init_ram(cmu, NULL, "s32k3x8.cmu", S32K3_CMU_SIZE, &error_fatal);
    memory_region_init_ram(rtc, NULL, "s32k3x8.rtc", S32K3_RTC_SIZE, &error_fatal);
    memory_region_init_ram(mc_rgm, NULL, "s32k3x8.mc_rgm", S32K3_MC_RGM_SIZE, &error_fatal);
    memory_region_init_ram(configuration_gpr, NULL, "s32k3x8.configuration_gpr", S32K3_CONFIGURATION_GPR_SIZE, &error_fatal);
    memory_region_init_ram(pramc0, NULL, "s32k3x8.pramc0", S32K3_PRAMC0_SIZE, &error_fatal);
    memory_region_init_ram(pramc1, NULL, "s32k3x8.pramc1", S32K3_PRAMC1_SIZE, &error_fatal);
    memory_region_init_ram(flash_ctrl, NULL, "s32k3x8.flash_ctrl", S32K3_FLASH_CTRL_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, S32K3_FIRC_BASE, firc);
    memory_region_add_subregion(system_memory, S32K3_SIRC_BASE, sirc);
    memory_region_add_subregion(system_memory, S32K3_SXOSC_BASE, sxosc);
    memory_region_add_subregion(system_memory, S32K3_FXOSC_BASE, fxosc);
    memory_region_add_subregion(system_memory, S32K3_MC_CGM_BASE, mc_cgm);
    memory_region_add_subregion(system_memory, S32K3_MC_ME_BASE, mc_me);
    memory_region_add_subregion(system_memory, S32K3_PLL_BASE, pll);
    memory_region_add_subregion(system_memory, S32K3_CMU_BASE, cmu);
    memory_region_add_subregion(system_memory, S32K3_RTC_BASE, rtc);
    memory_region_add_subregion(system_memory, S32K3_MC_RGM_BASE, mc_rgm);
    memory_region_add_subregion(system_memory, S32K3_CONFIGURATION_GPR_BASE, configuration_gpr);
    memory_region_add_subregion(system_memory, S32K3_PRAMC0_BASE, pramc0);
    memory_region_add_subregion(system_memory, S32K3_PRAMC1_BASE, pramc1);
    memory_region_add_subregion(system_memory, S32K3_FLASH_CTRL_BASE, flash_ctrl);
    qemu_log_mask(CPU_LOG_INT, "Memory regions initialized successfully.\n");

  

}
static void s32k3x8_init_lpspi(S32K3X8EVBState *s, MemoryRegion *system_memory, 
                               Clock *sysclk, ARMv7MState *armv7m)
{
    DeviceState *dev;
    
    qemu_log_mask(CPU_LOG_INT, "Initializing LPSPI devices\n");
    
    dev = qdev_new(TYPE_S32K3X8_LPSPI);
    s->lpspi[0] = dev;
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_abort)) {
        error_report("Failed to realize LPSPI0");
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, S32K3_LPSPI0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, 
                       qdev_get_gpio_in(DEVICE(armv7m), S32K3_LPSPI0_IRQ));
    
    dev = qdev_new(TYPE_S32K3X8_LPSPI);
    s->lpspi[1] = dev;
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_abort)) {
        error_report("Failed to realize LPSPI1");
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, S32K3_LPSPI1_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, 
                       qdev_get_gpio_in(DEVICE(armv7m), S32K3_LPSPI1_IRQ));
    
    dev = qdev_new(TYPE_S32K3X8_LPSPI);
    s->lpspi[2] = dev;
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_abort)) {
        error_report("Failed to realize LPSPI2");
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, S32K3_LPSPI2_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, 
                       qdev_get_gpio_in(DEVICE(armv7m), S32K3_LPSPI2_IRQ));
    
    dev = qdev_new(TYPE_S32K3X8_LPSPI);
    s->lpspi[3] = dev;
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_abort)) {
        error_report("Failed to realize LPSPI3");
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, S32K3_LPSPI3_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, 
                       qdev_get_gpio_in(DEVICE(armv7m), S32K3_LPSPI3_IRQ));
    
    dev = qdev_new(TYPE_S32K3X8_LPSPI);
    s->lpspi[4] = dev;
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_abort)) {
        error_report("Failed to realize LPSPI4");
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, S32K3_LPSPI4_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, 
                       qdev_get_gpio_in(DEVICE(armv7m), S32K3_LPSPI4_IRQ));
    
    dev = qdev_new(TYPE_S32K3X8_LPSPI);
    s->lpspi[5] = dev;
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_abort)) {
        error_report("Failed to realize LPSPI5");
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, S32K3_LPSPI5_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, 
                       qdev_get_gpio_in(DEVICE(armv7m), S32K3_LPSPI5_IRQ));
    
    qemu_log_mask(CPU_LOG_INT, "LPSPI devices initialized successfully\n");
}

static void s32k3x8_init_dma(S32K3X8EVBState *s,
                             MemoryRegion *system_memory,
                             ARMv7MState *armv7m)
{
    Error *local_err = NULL;
    DeviceState *dev = qdev_new(TYPE_S32K358_DMA);

    s->dma = dev;
    object_property_set_link(OBJECT(dev), "memory",
                             OBJECT(system_memory), &error_abort);

    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &local_err)) {
        error_reportf_err(local_err, "Failed to realize eDMA: ");
        return;
    }

    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, EDMA_REGS_BASE_ADDR);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, EDMA_TCD1_BASE_ADDR);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 2, EDMA_TCD2_BASE_ADDR);

    for (int i = 0; i < S32K358_NUM_DMA_CH; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i,
                           qdev_get_gpio_in(DEVICE(armv7m),
                                            S32K3_DMATCD0_IRQ + i));
    }

    qemu_log_mask(CPU_LOG_INT, "eDMA initialized and mapped\n");
}

static void s32k3x8_init_mscm(S32K3X8EVBState *s)
{
    Error *local_err = NULL;
    DeviceState *dev = qdev_new(TYPE_S32K358_MSCM);

    s->mscm = dev;
    qdev_prop_set_uint32(dev, "core-id", 0U);

    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &local_err)) {
        error_reportf_err(local_err, "Failed to realize MSCM: ");
        return;
    }

    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, S32K3_MSCM_BASE);
    qemu_log_mask(CPU_LOG_INT, "MSCM initialized and mapped\n");
}

static void s32k3x8_init_dmamux(S32K3X8EVBState *s)
{
    static const hwaddr dmamux_base[S32K3X8_DMAMUX_COUNT] = {
        S32K3_DMAMUX0_BASE,
        S32K3_DMAMUX1_BASE,
    };

    for (int i = 0; i < S32K3X8_DMAMUX_COUNT; i++) {
        Error *local_err = NULL;
        DeviceState *dev = qdev_new(TYPE_S32K358_DMAMUX);

        s->dmamux[i] = dev;
        if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &local_err)) {
            error_reportf_err(local_err,
                              "Failed to realize DMAMUX instance %d: ", i);
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, dmamux_base[i]);
    }

    qemu_log_mask(CPU_LOG_INT, "DMAMUX0/1 initialized and mapped\n");
}

static void s32k3x8_init_flexcan(S32K3X8EVBState *s, ARMv7MState *armv7m)
{
    static const hwaddr flexcan_bases[S32K3X8_CAN_COUNT] = {
        S32K3_FLEXCAN0_BASE,
        S32K3_FLEXCAN1_BASE,
        S32K3_FLEXCAN2_BASE,
        S32K3_FLEXCAN3_BASE,
        S32K3_FLEXCAN4_BASE,
        S32K3_FLEXCAN5_BASE,
        S32K3_FLEXCAN6_BASE,
        S32K3_FLEXCAN7_BASE,
    };
    static const int flexcan_mb_irq[S32K3X8_CAN_COUNT] = {
        S32K3_FLEXCAN0_MB_IRQ,
        S32K3_FLEXCAN1_MB_IRQ,
        S32K3_FLEXCAN2_MB_IRQ,
        S32K3_FLEXCAN3_MB_IRQ,
        S32K3_FLEXCAN4_MB_IRQ,
        S32K3_FLEXCAN5_MB_IRQ,
        S32K3_FLEXCAN6_MB_IRQ,
        S32K3_FLEXCAN7_MB_IRQ,
    };
    static const uint32_t flexcan_instance[S32K3X8_CAN_COUNT] = {
        0, 1, 2, 3, 4, 5, 6, 7
    };
    Error *local_err = NULL;

    qemu_log_mask(CPU_LOG_INT,
                  "Initializing FlexCAN instances CAN0..CAN7\n");

    for (int i = 0; i < S32K3X8_CAN_COUNT; i++) {
        DeviceState *dev = qdev_new(TYPE_S32K3X8_FLEXCAN);
        s->flexcan[i] = dev;

        qdev_prop_set_uint32(dev, "can-instance", flexcan_instance[i]);
        if (s->canbus[i]) {
            object_property_set_link(OBJECT(dev), "canbus",
                                     OBJECT(s->canbus[i]), &error_abort);
        }

        if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &local_err)) {
            error_reportf_err(local_err,
                              "Failed to realize FlexCAN instance %u: ",
                              flexcan_instance[i]);
            return;
        }

        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, flexcan_bases[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                           qdev_get_gpio_in(DEVICE(armv7m),
                                            flexcan_mb_irq[i]));
    }

    qemu_log_mask(CPU_LOG_INT,
                  "FlexCAN instances CAN0..CAN7 initialized\n");
}


/*board_init*/
static void s32k3x8evb_init(MachineState *machine)
{
    S32K3X8EVBState *s = S32K3X8EVB_MACHINE(machine);
    Error *error_local = NULL;
    DeviceState *dev;
    uint32_t startup_vector_base = s32k3x8evb_startup_vector_base(s);
    
    qemu_log_mask(CPU_LOG_INT, "Initializing S32K3X8EVB board\n");
    
    /*1. Check and get system memory*/
    MemoryRegion *system_memory = get_system_memory();
    if (!system_memory) {
        error_report("Failed to get system memory");
        return;
    }
    
    /*2. Use memory mapping initialization method*/
    s32k3x8_initialize_memory_regions(system_memory);
    
    /*3. Initialize system clock*/
    Clock *sysclk = clock_new(OBJECT(machine), "SYSCLK");
    if (!sysclk) {
        error_report("Failed to create system clock");
        return;
    }
    clock_set_hz(sysclk, S32K3_SYSCLK_FREQ);
    
  
    /*4. Initialize ARM core*/
    object_initialize_child(OBJECT(machine), "armv7m", &s->armv7m, TYPE_ARMV7M);
    
    /*5. Configure CPU*/
    qdev_prop_set_string(DEVICE(&s->armv7m), "cpu-type", ARM_CPU_TYPE_NAME("cortex-m7"));
    qdev_prop_set_uint32(DEVICE(&s->armv7m), "init-svtor", startup_vector_base);
    qdev_prop_set_uint32(DEVICE(&s->armv7m), "init-nsvtor", startup_vector_base);
    qdev_prop_set_uint8(DEVICE(&s->armv7m), "num-prio-bits", 4);  // Cortex-M7 uses 4 priority bits
    qdev_prop_set_uint32(DEVICE(&s->armv7m), "num-irq", 240);     // Number of interrupts for S32K3X8
    qemu_log_mask(CPU_LOG_INT,
                  "Startup vector base set to 0x%08x (%s)\n",
                  startup_vector_base,
                  s->vector_from_int_pflash ? "int_pflash" : "itcm");
    
   
    /*6. Set up system connections*/
    object_property_set_link(OBJECT(&s->armv7m), "memory", 
                           OBJECT(system_memory), &error_abort);
    qdev_connect_clock_in(DEVICE(&s->armv7m), "cpuclk", sysclk);
    
    /* 7. Implement system bus device*/
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), &error_local)) {
        error_reportf_err(error_local, "Failed to realize ARM core: ");
        return;
    }
     /* 8. Load firmware*/
    armv7m_load_kernel(ARM_CPU(first_cpu), machine->kernel_filename, INT_CODE_FLASH0_BASE, FLASH_SIZE);

    /*9. Initialize UART*/
    qemu_log_mask(CPU_LOG_INT, "Initializing UART\n");
    dev = qdev_new(TYPE_S32E8_LPUART);
    if (!dev) {
        error_report("Failed to create UART device");
        return;
    }
    
    /*10. Configure UART*/
    qdev_prop_set_chr(dev, "chardev", serial_hd(0));
    s->uart = dev;
    
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_local)) {
        error_reportf_err(error_local, "Failed to realize UART: ");
        return;
    }
    
    /*11. Map UART - adjusted according to reference manual*/
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, S32K3_UART_BASE);
  
    /*12. Initialize eDMA device*/
    s32k3x8_init_dma(s, system_memory, &s->armv7m);
    /*13. Initialize MSCM*/
    s32k3x8_init_mscm(s);
    /*14. Initialize DMAMUX instances*/
    s32k3x8_init_dmamux(s);
    /*15. Initialize LPSPI devices*/
    s32k3x8_init_lpspi(s, system_memory, sysclk, &s->armv7m);
    /*16. Initialize FlexCAN devices (instances 0..7)*/
    s32k3x8_init_flexcan(s, &s->armv7m);
    qemu_log_mask(CPU_LOG_INT, "S32K3X8EVB board initialization complete\n"); 


}

static void s32k3x8evb_instance_init(Object *obj)
{
    S32K3X8EVBState *s = S32K3X8EVB_MACHINE(obj);

    s->vector_from_int_pflash = false;

    object_property_add_link(obj, "canbus0", TYPE_CAN_BUS,
                             (Object **)&s->canbus[0],
                             object_property_allow_set_link,
                             0);
    object_property_add_link(obj, "canbus1", TYPE_CAN_BUS,
                             (Object **)&s->canbus[1],
                             object_property_allow_set_link,
                             0);
    object_property_add_link(obj, "canbus2", TYPE_CAN_BUS,
                             (Object **)&s->canbus[2],
                             object_property_allow_set_link,
                             0);
    object_property_add_link(obj, "canbus3", TYPE_CAN_BUS,
                             (Object **)&s->canbus[3],
                             object_property_allow_set_link,
                             0);
    object_property_add_link(obj, "canbus4", TYPE_CAN_BUS,
                             (Object **)&s->canbus[4],
                             object_property_allow_set_link,
                             0);
    object_property_add_link(obj, "canbus5", TYPE_CAN_BUS,
                             (Object **)&s->canbus[5],
                             object_property_allow_set_link,
                             0);
    object_property_add_link(obj, "canbus6", TYPE_CAN_BUS,
                             (Object **)&s->canbus[6],
                             object_property_allow_set_link,
                             0);
    object_property_add_link(obj, "canbus7", TYPE_CAN_BUS,
                             (Object **)&s->canbus[7],
                             object_property_allow_set_link,
                             0);
}

/*Board class initialization*/
static void s32k3x8evb_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_bool(oc, "vector-from-int-pflash",
                                   s32k3x8evb_get_vector_from_int_pflash,
                                   s32k3x8evb_set_vector_from_int_pflash);
    object_class_property_set_description(oc, "vector-from-int-pflash",
        "Use int_pflash reset vectors at 0x00490400 instead of ITCM vectors at 0x00000000");

    mc->desc = "NXP S32K3X8EVB Development Board (Cortex-M7)";
    mc->init = s32k3x8evb_init;
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = 2;
    mc->default_ram_size = SRAM_SIZE;
}

static const TypeInfo s32k3x8evb_type = {
    .name = MACHINE_TYPE_NAME("s32k3x8evb"),
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(S32K3X8EVBState),
    .instance_init = s32k3x8evb_instance_init,
    .class_init = s32k3x8evb_class_init,
};

/* Register machine type*/
static void s32k3x8evb_machine_init(void)
{
    qemu_log_mask(CPU_LOG_INT, "Registering S32K3X8EVB machine type\n");
    type_register_static(&s32k3x8evb_type);
}

type_init(s32k3x8evb_machine_init)
