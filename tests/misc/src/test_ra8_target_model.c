/**
 * @file test_ra8_target_model.c
 * @brief Host pin tests for `ra8_target_model.h`, `ra8_seam_symbols.h`, and `ra8_board_memmap.h`
 *
 * @details
 * Verifies single-source consistency across Core, HAL, MPU, and Board headers:
 *   1. Board memory map regions (`ra8_board_memmap.h`) match `ra8_device.h`,
 *      `ra8_sdramc_regs.h`, `ra8_sram_regs.h`, and `ra8_mpu_boot_map()`.
 *   2. Architectural peripheral bus, IDAU NS offset, CPSCU/PSCU/SAMON bases,
 *      Cortex-M85 PPB block addresses, and derived SYSC register addresses match
 *      their primary register headers (`ra8_system_regs.h`, `ra8_lpm_regs.h`,
 *      `ra8_reset_regs.h`, `ra8_lpm_safe_boot.h`).
 *   3. Canonical ELF, linker, bootloader, debug-channel, and RTOS inspection
 *      seam symbol strings in `ra8_seam_symbols.h` match their documented ABI.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2_peripherals.h"
#include "ra8_board_memmap.h"
#include "ra8_device.h"
#include "ra8_mpu.h"
#include "ra8_seam_symbols.h"
#include "ra8_target_model.h"
#include "unity_minimal.h"

/** @brief Reference bus and MPU map constants to pin target-model consistency. */
typedef enum : uintptr_t {
  k_t_expected_peri_base     = 0x40000000UL, /**< Secure peripheral bus base.  */
  k_t_expected_ns_offset     = 0x10000000UL, /**< IDAU bit[28] NS alias delta. */
  k_t_expected_peri_ns_base  = 0x50000000UL, /**< NS peripheral bus alias.     */
  k_t_expected_pscu_base     = 0x40204000UL, /**< R_PSCU base.                 */
  k_t_expected_samon_cms     = 0x40204030UL, /**< R_PSCU CMSAMON register.     */
  k_t_expected_samon_sfs     = 0x4020403CUL, /**< R_PSCU SFSAMON register.     */
  k_t_expected_ppb_base      = 0xE0000000UL, /**< Cortex-M85 PPB base.         */
  k_t_expected_dwt_base      = 0xE0001000UL, /**< Cortex-M85 DWT base.         */
  k_t_expected_fpb_base      = 0xE0002000UL, /**< Cortex-M85 FPB base.         */
  k_t_expected_scs_base      = 0xE000E000UL, /**< Cortex-M85 SCS base.         */
  k_t_expected_itns_base     = 0xE000E380UL, /**< Cortex-M85 NVIC_ITNS base.   */
  k_t_expected_scb_base      = 0xE000ED00UL, /**< Cortex-M85 SCB base.         */
  k_t_expected_dcb_base      = 0xE000EDF0UL, /**< Cortex-M85 DCB base.         */
  k_t_expected_scs_ns_base   = 0xE002E000UL, /**< Cortex-M85 SCS NS alias.     */
  k_t_expected_scb_ns_base   = 0xE002ED00UL, /**< Cortex-M85 SCB NS alias.     */
  k_t_expected_xspi_cs0_base = 0x80000000UL, /**< Octo/xSPI CS0 flash window.  */
} target_model_expect_addr_t;

/** @brief Expected MPU boot-map region indices and sizes. */
typedef enum : uint32_t {
  k_t_mpu_region_count   = 5U,          /**< Canonical 5-region MPU map. */
  k_t_mpu_idx_mram       = 0U,          /**< Region 0: Code MRAM.        */
  k_t_mpu_idx_sram       = 1U,          /**< Region 1: M85 private SRAM. */
  k_t_mpu_idx_sdram      = 2U,          /**< Region 2: External SDRAM.   */
  k_t_mpu_idx_peri       = 3U,          /**< Region 3: Peripheral bus.   */
  k_t_mpu_idx_shram      = 4U,          /**< Region 4: Shared NS SRAM.   */
  k_t_expected_sram_1mib = 0x00100000U, /**< SRAM0+1 size incl NOINIT.   */
} target_model_mpu_const_t;

/** @brief Verify board memory map matches Core device map, HAL reg headers, and MPU map. */
RA8_INTERNAL static void internal_test_memmap_and_mpu_pin(void)
{
  TEST_BEGIN("target_model: ra8_board_memmap matches device, regs, and ra8_mpu_boot_map");

  TEST_ASSERT_EQ((uintptr_t)k_ra8_mem_mram_base, (uintptr_t)k_ra8_board_mram_base);
  TEST_ASSERT_EQ((uintptr_t)k_ra8_mem_itcm_base, (uintptr_t)k_ra8_board_itcm_base);
  TEST_ASSERT_EQ((uintptr_t)k_ra8_mem_dtcm_base, (uintptr_t)k_ra8_board_dtcm_base);
  TEST_ASSERT_EQ((uintptr_t)k_ra8_mem_sram_base, (uintptr_t)k_ra8_board_sram_base);
  TEST_ASSERT_EQ((uintptr_t)k_ra8_mem_sdram_base, (uintptr_t)k_ra8_board_sdram_base);
  TEST_ASSERT_EQ((uintptr_t)k_ra8_sram_data_base_addr, (uintptr_t)k_ra8_board_sram_base);
  TEST_ASSERT_EQ((uintptr_t)k_ra8_sdram_base_addr, (uintptr_t)k_ra8_board_sdram_base);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_xspi_cs0_base, (uintptr_t)k_ra8_board_xspi_flash_base);
  TEST_ASSERT_EQ(k_t_expected_sram_1mib, k_ra8_board_sram_size + k_ra8_board_noinit_size);

  uint8_t                 count = 0U;
  const ra8_mpu_region_t* map   = ra8_mpu_boot_map(&count);
  TEST_ASSERT(map != nullptr);
  TEST_ASSERT_EQ((uint8_t)k_t_mpu_region_count, count);

  TEST_ASSERT_EQ((uint32_t)k_ra8_board_mram_base, map[k_t_mpu_idx_mram].base);
  TEST_ASSERT_EQ((uint32_t)k_ra8_board_mram_size, map[k_t_mpu_idx_mram].size);
  TEST_ASSERT_EQ((uint32_t)k_ra8_board_sram_base, map[k_t_mpu_idx_sram].base);
  TEST_ASSERT_EQ((uint32_t)k_ra8_board_sdram_base, map[k_t_mpu_idx_sdram].base);
  TEST_ASSERT_EQ((uint32_t)k_ra8_board_sdram_size, map[k_t_mpu_idx_sdram].size);
  TEST_ASSERT_EQ((uint32_t)k_ra8_periph_bus_base_addr, map[k_t_mpu_idx_peri].base);
  TEST_ASSERT_EQ((uint32_t)(k_ra8_board_noinit_base + k_ra8_board_noinit_size),
                 map[k_t_mpu_idx_shram].base);
  TEST_ASSERT_EQ((uint32_t)k_ra8_board_ns_sram_size, map[k_t_mpu_idx_shram].size);

  TEST_END("target_model: ra8_board_memmap matches device, regs, and ra8_mpu_boot_map");
}

/** @brief Verify SoC peripheral bus, PSCU/SAMON, Core PPB, and SYSC derived bases. */
RA8_INTERNAL static void internal_test_bus_and_sysc_bases(void)
{
  TEST_BEGIN("target_model: peripheral bus, PPB, and SYSC bases match HAL headers");

  TEST_ASSERT_EQ((uintptr_t)k_t_expected_peri_base, (uintptr_t)k_ra8_periph_bus_base_addr);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_ns_offset, (uintptr_t)k_ra8_idau_ns_alias_offset);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_peri_ns_base, (uintptr_t)k_ra8_periph_bus_ns_base_addr);
  TEST_ASSERT_EQ((uintptr_t)k_ra8_sram_cpscu_base_addr, (uintptr_t)k_ra8_cpscu_base_addr);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_pscu_base, (uintptr_t)k_ra8_pscu_base_addr);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_samon_cms, (uintptr_t)k_ra8_samon_cms_addr);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_samon_sfs, (uintptr_t)k_ra8_samon_sfs_addr);

  TEST_ASSERT_EQ((uintptr_t)k_t_expected_ppb_base, (uintptr_t)k_ra8_core_ppb_base_addr);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_ppb_base, (uintptr_t)k_ra8_core_itm_base_addr);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_dwt_base, (uintptr_t)k_ra8_core_dwt_base_addr);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_fpb_base, (uintptr_t)k_ra8_core_fpb_base_addr);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_scs_base, (uintptr_t)k_ra8_core_scs_base_addr);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_itns_base, (uintptr_t)k_ra8_core_itns_base_addr);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_scb_base, (uintptr_t)k_ra8_core_scb_base_addr);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_dcb_base, (uintptr_t)k_ra8_core_dcb_base_addr);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_scs_ns_base, (uintptr_t)k_ra8_core_scs_ns_base_addr);
  TEST_ASSERT_EQ((uintptr_t)k_t_expected_scb_ns_base, (uintptr_t)k_ra8_core_scb_ns_base_addr);

  TEST_ASSERT_EQ((uintptr_t)ra8_sys_vscr(), (uintptr_t)k_ra8_sys_vscr_addr);
  TEST_ASSERT_EQ((uintptr_t)ra8_sys_sckdivcr(), (uintptr_t)k_ra8_sys_sckdivcr_addr);
  TEST_ASSERT_EQ((uintptr_t)ra8_sys_oscsf(), (uintptr_t)k_ra8_sys_oscsf_addr);
  TEST_ASSERT_EQ((uintptr_t)ra8_sys_octackcr(), (uintptr_t)k_ra8_sys_octackcr_addr);
  TEST_ASSERT_EQ((uintptr_t)ra8_sys_rstsr1(), (uintptr_t)k_ra8_sys_rstsr1_addr);
  TEST_ASSERT_EQ((uintptr_t)ra8_sys_prcr(), (uintptr_t)k_ra8_sys_prcr_addr);
  TEST_ASSERT_EQ((uintptr_t)ra8_sys_lococr(), (uintptr_t)k_ra8_sys_lococr_addr);
  TEST_ASSERT_EQ((uintptr_t)ra8_sys_rstsr0(), (uintptr_t)k_ra8_sys_rstsr0_addr);
  TEST_ASSERT_EQ((uintptr_t)ra8_sys_sosccr(), (uintptr_t)k_ra8_sys_sosccr_addr);
  TEST_ASSERT_EQ((uintptr_t)k_ra8_lpm_safe_boot_sbycr_addr,
                 (uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_lpm_sbycr_off);

  TEST_END("target_model: peripheral bus, PPB, and SYSC bases match HAL headers");
}

/** @brief Verify all published ELF, linker, bootloader, and RTOS seam symbol strings. */
RA8_INTERNAL static void internal_test_seam_symbols_contract(void)
{
  TEST_BEGIN("target_model: seam symbol names match linker and firmware ABI");

  TEST_ASSERT_EQ(0, strcmp(K_RA8_SEAM_SYM_RESET_HANDLER, "Reset_Handler"));
  TEST_ASSERT_EQ(0, strcmp(K_RA8_SEAM_SYM_STACK_TOP, "g_ra8_ls_stack_top"));
  TEST_ASSERT_EQ(0, strcmp(K_RA8_SEAM_SYM_STACK_SIZE, "g_ra8_ls_stack_size"));
  TEST_ASSERT_EQ(0, strcmp(K_RA8_SEAM_SYM_NS_BSS_START, "g_ra8_ls_ns_bss_start"));
  TEST_ASSERT_EQ(0, strcmp(K_RA8_SEAM_SYM_NS_BSS_END, "g_ra8_ls_ns_bss_end"));
  TEST_ASSERT_EQ(0, strcmp(K_RA8_SEAM_SYM_NS_STACK_TOP, "g_ra8_ls_ns_stack_top"));
  TEST_ASSERT_EQ(0, strcmp(K_RA8_SEAM_SYM_NS_RUN_START, "g_ra8_ls_ns_run_start"));
  TEST_ASSERT_EQ(0, strcmp(K_RA8_SEAM_SYM_SGSTUBS_START, "g_ra8_ls_sgstubs_start"));
  TEST_ASSERT_EQ(0, strcmp(K_RA8_SEAM_SYM_SGSTUBS_END, "g_ra8_ls_sgstubs_end"));
  TEST_ASSERT_EQ(0, strcmp(K_RA8_SEAM_SYM_DFU_TRIGGER, "g_dfu_trigger"));
  TEST_ASSERT_EQ(0, strcmp(K_RA8_SEAM_SYM_WIDGET_DEBUG_TREE, "ra8_widget_debug_tree"));
  TEST_ASSERT_EQ(0, strcmp(K_RA8_SEAM_SYM_TX_THREAD_CURRENT_PTR, "_tx_thread_current_ptr"));
  TEST_ASSERT_EQ(0, strcmp(K_RA8_SEAM_SYM_TX_THREAD_CREATED_PTR, "_tx_thread_created_ptr"));
  TEST_ASSERT_EQ(0, strcmp(K_RA8_SEAM_SYM_TX_THREAD_CREATED_COUNT, "_tx_thread_created_count"));
  TEST_ASSERT_EQ(0,
                 strcmp(K_RA8_SEAM_SYM_TXM_LOADED_LIST_PTR, "_txm_module_manager_loaded_list_ptr"));
  TEST_ASSERT_EQ(
    0,
    strcmp(K_RA8_SEAM_SYM_TXM_MEMORY_FAULT_INFO, "_txm_module_manager_memory_fault_info"));

  TEST_END("target_model: seam symbol names match linker and firmware ABI");
}

int main(void)
{
  internal_test_memmap_and_mpu_pin();
  internal_test_bus_and_sysc_bases();
  internal_test_seam_symbols_contract();
  return 0;
}
