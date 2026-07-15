/**
 * @file main.c
 * @brief RA8D2 board emulator -- boot a real .elf on a CPU emulator, with ticks
 *
 * @details
 * Loads an EK-RA8D2 firmware ELF into an emulated Cortex-M memory map (Unicorn,
 * QEMU's CPU core as a library) and boots it from the vector table, with the
 * RA8D2 peripheral space modelled as logged MMIO.
 *
 * Feasibility (proven): Unicorn 2.x tops out at Cortex-M33 (Armv8-M) while the
 * RA8D2 is M85 (Armv8.1-M), yet the GCC-built firmware executes on the M33 core
 * -- no v8.1-M-only opcode (e.g. low-overhead loops) is emitted on the boot
 * path. The invalid-instruction trap below still reports exactly where and what
 * if that ever changes.
 *
 * Time: bare-metal delays here are SysTick-driven (``ra8_time`` enables SysTick
 * with TICKINT and counts exceptions). Nothing advances time on a plain memory
 * model, so the run loop is chunked and, between chunks, cooperatively invokes
 * the firmware's installed SysTick_Handler as a function -- its tick-counter
 * memory write persists while the interrupted context's registers are restored,
 * which is precisely a real SysTick IRQ's observable effect. This carries the
 * firmware past ``ra8_delay_ms`` so it reaches its main loop (e.g. driving the
 * GLCDC), instead of spinning forever on a tick that never increments.
 *
 *   board_sim <firmware.elf>
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <capstone/capstone.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unicorn/unicorn.h>
#include <unistd.h>

#include "board_console.h"
#include "board_input.h"
#include "board_net.h"
#include "board_overlay.h"
#include "board_periph.h"
#include "board_periph_sd.h"
#include "board_usb.h"
#include "board_usb_host.h"
#include "board_view.h"

/* RA8D2 memory map (EK board) -- from the linker script / HUM R01UH1065EJ. */
typedef struct {
  const char* name;
  uint64_t    base;
  uint64_t    size;
} mem_region_t;

static const mem_region_t k_regions[] = {
  {"ITCM", 0x00000000UL, 0x00010000UL},     /* 64 KiB tightly-coupled code */
  {"MRAM", 0x02000000UL, 0x00100000UL},     /* 1 MiB code flash + vectors  */
  {"TRIM", 0x02C1E000UL, 0x00001000UL},     /* MRAM factory-trim page: the
                                             * on-chip temperature-sensor
                                             * calibration cells (TSCDR/TSCDR2 at
                                             * 0x02C1EDA0) live here. Mapped +
                                             * seeded (see k_tsn_cal_*) so
                                             * ra8_tsn reads a real two-point pair
                                             * instead of bus-faulting on the
                                             * previously-unmapped read. */
  {"OFS", 0x0300A000UL, 0x00001000UL},      /* option-setting flash        */
  {"DTCM", 0x20000000UL, 0x00010000UL},     /* 64 KiB tightly-coupled data */
  {"SRAM", 0x22000000UL, 0x00400000UL},     /* On-chip SRAM: CPU0 1 MiB + shared mailbox +
                                             * CPU1 bank. Intentionally a wide 4 MiB window so
                                             * the placeholder NS_SRAM and every dual-core
                                             * example map cleanly; it does NOT reflect the
                                             * 1.6 MB silicon limit (real ECC SRAM is
                                             * 0x22000000..0x221A0000). */
  {"NS_SRAM2", 0x32100000UL, 0x00080000UL}, /* SRAM2 Non-secure alias (bit[28]=1): the
                                             * TrustZone NS image run region. The Secure
                                             * boot copies the NS image here then BLXNS-es
                                             * to it; mapping it lets two-image TZ apps
                                             * (src/app) run their NS world in board_sim. */
  {"DATA_FLASH", 0x27000000UL, 0x00004000UL},
  {"SDRAM", 0x68000000UL, 0x04000000UL},    /* 64 MiB external SDRAM (Secure
                                             * physical view). Host-backed so the
                                             * NS alias below mirrors the bytes:
                                             * the GLCDC model scans the
                                             * framebuffer from here while the NS
                                             * world draws it through 0x78000000. */
  {"NS_SDRAM", 0x78000000UL, 0x04000000UL}, /* External SDRAM Non-secure alias
                                             * (IDAU bit[28]=1): the SAME 64 MiB
                                             * array as SDRAM. The Non-secure
                                             * e-reader writes framebuffer pixels
                                             * through this alias; sharing one
                                             * host buffer makes them visible to
                                             * the Secure GLCDC read at
                                             * 0x68000000. Must follow SDRAM in
                                             * this table so the shared host
                                             * buffer is allocated first. */
  {"OSPI", 0x80000000UL, 0x04000000UL},     /* 64 MiB Octo-SPI XIP flash (Secure
                                             * physical view). Memory-mapped and
                                             * executable: an NS reader image runs
                                             * execute-in-place from here. Host-
                                             * backed and shared with NS_OSPI so
                                             * the alias below mirrors the bytes. */
  {"NS_OSPI", 0x90000000UL, 0x04000000UL},  /* OSPI XIP Non-secure alias
                                             * (IDAU bit[28]=1): the SAME 64 MiB
                                             * flash array as OSPI. The XIP-linked
                                             * NS image's .text/.rodata live at
                                             * 0x90000000; the M85 fetches NS
                                             * instructions from this alias. Must
                                             * follow OSPI in this table so the
                                             * shared host buffer is allocated
                                             * before the alias maps onto it. */
  {"PPB", 0xE0000000UL, 0x00100000UL},      /* ARM private peripheral bus */
};

/* Renesas peripheral space -- modelled as logged MMIO (returns all-ones so
 * "wait for ready bit" polls fall through instead of spinning forever). */
typedef enum : uint64_t {
  k_periph_base = 0x40000000UL,
  k_periph_size = 0x10000000UL, /* 0x40000000-0x4FFFFFFF: all Renesas peripherals */
} periph_map_t;

/* Octo-SPI (XSPI) execute-in-place flash window. The XSPI controller's register
 * command-engine is modelled in peripheral space (board_periph_xspi.c, base
 * 0x40268000); this is the SEPARATE memory-mapped, executable view of the 64 MiB
 * flash array the controller exposes once the firmware enters XIP mode. The
 * Secure physical window sits at 0x80000000; its TrustZone Non-secure alias
 * (IDAU bit[28]=1) sits at 0x90000000. Both views address the same flash array,
 * so board_sim backs them with a single host buffer (mirrors the SRAM/NS_SRAM2
 * alias pair). A Non-secure reader image linked for XIP places .text/.rodata at
 * 0x90000000 and the CPU fetches Non-secure instructions from there. */
typedef enum : uint64_t {
  k_ospi_xip_base = 0x80000000UL, /* OSPI XIP window: Secure physical base.      */
  k_ospi_ns_base  = 0x90000000UL, /* OSPI XIP window: NS alias (IDAU bit[28]=1). */
  k_ospi_xip_size = 0x04000000UL, /* 64 MiB execute-in-place flash array.        */
} ospi_xip_map_t;

/* On-chip temperature-sensor factory calibration. The TSN two-point trim words
 * TSCDR (code at the high reference) and TSCDR2 (code at the low reference) live
 * in the MRAM factory-trim region at 0x02C1EDA0 (HUM Ch 55.2.2 p 3498-3499).
 * Real silicon ships them factory-programmed; board_sim's blank map left the
 * region unreadable, so ra8_tsn_convert_to_milli_c bus-faulted (UC_ERR_READ_-
 * UNMAPPED) reading them and adc_diag_tsn_demo aborted. Seed a deterministic,
 * plausible positive-slope pair (TSCDR @ +125C > TSCDR2 @ -40C) into the mapped
 * TRIM page after mem-map. Paired with the ADC temperature code the ADC model
 * reports (k_adc_temp_code = 1800 in board_periph_adc.c), the two-point math
 * yields ~26 degC. This is factory-constant data, not a masked poll. */
typedef enum : uint32_t {
  k_tsn_cal_addr   = 0x02C1EDA0U, /* TSCDR (+0x00), TSCDR2 (+0x04).        */
  k_tsn_cal_tscdr  = 3000U,       /* 12-bit calibration code at +125 degC. */
  k_tsn_cal_tscdr2 = 1000U,       /* 12-bit calibration code at -40 degC.  */
} tsn_cal_seed_t;

typedef enum : uint32_t {
  k_run_chunk_insns = 500000U, /**< Instructions per emulation chunk.     */
  k_low_power_div   = 4U,      /**< Low-power: shrink the chunk budget by */
                               /**< this (the 4:1 M85:M33 clock ratio) so */
                               /**< the modelled core advances ~1/4 as fast. */
  k_run_max_chunks = 40000U,   /**< Chunk budget. Each chunk offers one */
                               /**< SysTick, so RTOS apps whose threads */
                               /**< sleep on hundreds/thousands of ticks */
                               /**< (e.g. ThreadX tx_thread_sleep) need a */
                               /**< far larger budget than bare-metal. */
  k_idle_spin_insns = 2U,      /**< Budget when the core is parked on a
                                * wait-for-interrupt spin (a "b ." self-branch,
                                * `wfi`, or a `cpsie i` + back-branch poll):
                                * collapse the idle wait to the next SysTick
                                * instead of spinning a whole chunk. */
  k_op_branch_self  = 0xE7FEU, /**< Thumb "b ." (branch-to-self idle loop).     */
  k_op_wfi          = 0xBF30U, /**< Thumb `wfi` (wait-for-interrupt).           */
  k_op_cpsie_i      = 0xB662U, /**< Thumb `cpsie i` (re-enable IRQ in a poll).  */
  k_op_bn_mask      = 0xF800U, /**< Mask selecting a Thumb T2 `b.n` opcode.     */
  k_op_bn_base      = 0xE000U, /**< Thumb T2 unconditional `b.n` base value.    */
  k_op_bn_imm       = 0x07FFU, /**< Thumb T2 `b.n` imm11 field mask.            */
  k_bn_imm_sext_shl = 21U,     /**< Shift imm11 bit10 up to bit31 (sign bit).   */
  k_bn_imm_sext_shr = 20U,     /**< Arith >> sign-extends and scales imm by 2.  */
  k_idle_scan_fwd   = 8U,      /**< Halfwords scanned ahead for a loop edge.    */
  k_idle_loop_max   = 32U,     /**< Largest idle loop (bytes) that may hold PC. */
  k_run_wall_s      = 120U,    /**< Wall-clock safety bound (seconds).          */
  k_run_inner_max   = 4096U,   /**< Per-chunk exception-resolve relaunch cap.   */
  k_mmio_slots      = 2048U,   /**< Distinct MMIO addresses tracked.            */
  k_mmio_settle     = 8U,      /**< Same-addr reads before a poll "settles".    */
  k_mmio_print_max  = 256U,    /**< Max MMIO rows printed in the summary.       */
  k_env_strtol_base = 10U,     /**< Decimal base for env-var integer parse.     */
} sim_budget_t;

/**
 * @enum board_sim_exit_t
 * @brief Process exit codes for the #67 run-every-example matrix.
 *
 * @details The matrix keys off the process exit code (not the stderr banner):
 * a clean run-to-budget returns success; a firmware BKPT, an emulation fault,
 * or the wall-clock timeout each return a distinct non-zero code so a wedged or
 * trapped run is distinguishable from a healthy one.
 */
typedef enum : int {
  k_board_sim_exit_ok      = 0, /**< Clean run-to-budget (no fault/BKPT/timeout).   */
  k_board_sim_exit_fault   = 1, /**< Emulation fault / invalid access ended it.     */
  k_board_sim_exit_bkpt    = 2, /**< Firmware executed a BKPT (assert/give-up).     */
  k_board_sim_exit_timeout = 3, /**< Wall-clock budget reached before a clean stop. */
} board_sim_exit_t;

/* Cortex-M system control space (architectural, all cores) -- inside the PPB.
 * The PPB is mapped as plain RAM here (not callback MMIO), so SCB/NVIC writes
 * the firmware performs land in memory and read back -- the exception model
 * below polls/edits these words to model what real M-profile hardware would do
 * with the NVIC that Unicorn does not implement. */
typedef enum : uint64_t {
  k_syst_csr          = 0xE000E010UL, /**< SysTick control/status (SYST_CSR).     */
  k_syst_csr_run      = 0x3UL,        /**< ENABLE | TICKINT both set.             */
  k_scb_icsr          = 0xE000ED04UL, /**< Interrupt control/state (ICSR).        */
  k_scb_vtor          = 0xE000ED08UL, /**< Vector table offset register.          */
  k_scb_aircr         = 0xE000ED0CUL, /**< App interrupt/reset control (AIRCR).   */
  k_aircr_sysresetreq = 2UL,          /**< AIRCR.SYSRESETREQ bit (request reset). */
  k_scb_shpr2         = 0xE000ED1CUL, /**< System handler priority 2 (SVC=b3).    */
  k_scb_shpr3         = 0xE000ED20UL, /**< System handler priority 3 (PSV/SYT).   */
  k_mpu_type          = 0xE000ED90UL, /**< MPU_TYPE (DREGION in bits 15:8).       */
  k_mpu_type_seed     = 0x00000800UL, /**< 8 data regions (matches the M85 MPU).  */
  k_mpu_ctrl          = 0xE000ED94UL, /**< MPU_CTRL (ENABLE in bit 0).            */
  k_mpu_rnr           = 0xE000ED98UL, /**< MPU_RNR (region number select).        */
  k_mpu_rbar          = 0xE000ED9CUL, /**< MPU_RBAR (BASE[31:5]|SH|AP[2:1]|XN).   */
  k_mpu_rlar          = 0xE000EDA0UL, /**< MPU_RLAR (LIMIT[31:5]|AttrIdx|EN).     */
  k_scb_ccr           = 0xE000ED14UL, /**< Configuration and Control (CCR).       */
  k_ccr_div_0_trp     = 0x10UL,       /**< CCR.DIV_0_TRP bit4: divide-by-0 traps. */
  k_scb_cfsr          = 0xE000ED28UL, /**< Config Fault Status (MMFSR low byte).  */
  k_scb_mmfar         = 0xE000ED34UL, /**< MemManage Fault Address Register.      */
  k_icsr_pendsvset    = 28UL,         /**< ICSR.PENDSVSET bit (request PendSV).   */
  k_icsr_pendstset    = 26UL,         /**< ICSR.PENDSTSET bit (request SysTick).  */
  k_exc_usagefault    = 6UL,          /**< UsageFault exception / vector index.   */
  k_exc_memmanage     = 4UL,          /**< MemManage exception / vector index.    */
  k_exc_svcall        = 11UL,         /**< SVCall exception / vector index.       */
  k_exc_pendsv        = 14UL,         /**< PendSV exception / vector index.       */
  k_exc_systick       = 15UL,         /**< SysTick exception / vector index.      */
  k_nvic_ipr_base     = 0xE000E400UL, /**< NVIC IPR priority bytes (one per IRQ). */
  k_nvic_ispr_base    = 0xE000E200UL, /**< NVIC ISPR set-pending (per-IRQ bit).   */
  k_nvic_iser_base    = 0xE000E100UL, /**< NVIC ISER set-enable array base.       */
  k_nvic_icer_base    = 0xE000E180UL, /**< NVIC ICER clear-enable array base.     */
  k_nvic_en_words     = 8UL,          /**< ISER/ICER words modelled (256 lines).  */
  k_nvic_en_span      = 8UL * 4UL,    /**< Byte span of one set/clear array.      */
  k_exc_irq_vec0      = 16UL,         /**< Vector index of IRQ0 (16 + IRQn).      */
} cortexm_scs_t;

/* Armv7E-M / Armv8-M exception model constants (the part Unicorn's Cortex-M33
 * core leaves to software here -- it has no NVIC/exception unit). EXC_RETURN
 * magic values steer the unstack: bit2 picks the return stack (1 = PSP, 0 =
 * MSP), bit3 the return mode (1 = Thread, 0 = Handler), bit4 the frame type
 * (1 = basic 8-word frame, 0 = FP extended frame). On Armv8-M the value also
 * carries bit6 = S (1 = return to Secure, 0 = Non-Secure) and bit5 = DCRS, so a
 * Non-Secure thread return is 0xFFFFFFBC -- NOT one of the Armv7-M 0xFFFFFFF_
 * values. is_exc_return() therefore matches the whole bits[31:7]-set EXC_RETURN
 * prefix (0xFFFFFF80..FF), and exc_enter/exc_return branch on bit4 (FType) to
 * stack/unstack the S0-S15 + FPSCR words whenever an FP extended frame is in
 * play (ThreadX's PendSV mirrors this with its own TST LR,#0x10 on S16-S31). */
typedef enum : uint32_t {
  k_exc_frame_words   = 8U,          /**< {R0-R3,R12,LR,PC,xPSR} basic frame.    */
  k_exc_frame_bytes   = 32U,         /**< 8 words * 4 bytes.                     */
  k_exc_ret_v8_mask   = 0xFFFFFF80U, /**< Armv8-M EXC_RETURN prefix: bits[31:7]. */
  k_exc_ret_ftype     = 0x10U,       /**< EXC_RETURN bit4: 1 = basic, 0 = FP.    */
  k_exc_ret_handler   = 0xFFFFFFF1U, /**< Return to Handler mode, MSP.           */
  k_exc_ret_msp       = 0xFFFFFFF9U, /**< Return to Thread mode, MSP.            */
  k_exc_ret_psp       = 0xFFFFFFFDU, /**< Return to Thread mode, PSP.            */
  k_exc_ret_spsel     = 0x4U,        /**< EXC_RETURN bit2: return stack = PSP.   */
  k_exc_ret_mode      = 0x8U,        /**< EXC_RETURN bit3: return to Thread.     */
  k_control_spsel     = 0x2U,        /**< CONTROL.SPSEL: thread SP = PSP.        */
  k_control_fpca      = 0x4U,        /**< CONTROL.FPCA: FP context is active.    */
  k_xpsr_t_bit        = 0x01000000U, /**< xPSR.T (Thumb) -- must stay set.       */
  k_xpsr_align9       = 0x00000200U, /**< xPSR bit9: stack-frame realignment.    */
  k_xpsr_ipsr_mask    = 0x000001FFU, /**< xPSR[8:0] = IPSR (active exception).   */
  k_exc_prio_none     = 0x100U,      /**< Sentinel "no handler active" prio.     */
  k_exc_prio_max      = 0xFFU,       /**< Lowest configurable priority value.    */
  k_exc_nest_max      = 4U,          /**< Tracked active-exception nesting cap.  */
  k_byte_bits         = 8U,          /**< Bits per byte (SHPR field width).      */
  k_frame_off_r3      = 12U,         /**< Basic exception-frame offset of R3.    */
  k_frame_off_lr      = 20U,         /**< Basic exception-frame offset of LR.    */
  k_frame_off_pc      = 24U,         /**< Basic exception-frame offset of PC.    */
  k_frame_off_xpsr    = 28U,         /**< Basic exception-frame offset of xPSR.  */
  k_fp_frame_extra    = 72U,         /**< FP ext frame above basic: S0-15+FPSCR. */
  k_fp_s_words        = 16U,         /**< S0-S15 saved in the FP extended frame. */
  k_frame_off_s0      = 32U,         /**< FP-frame offset of S0 (above basic).   */
  k_frame_off_fpscr   = 96U,         /**< FP-frame offset of FPSCR (32 + 16*4).  */
  k_vector_erased     = 0xFFFFFFFEU, /**< Erased-flash / invalid vector word.    */
  k_nvic_prio_shift   = 4U,          /**< Implemented priority is the 4 MSBs.    */
  k_lo4_mask          = 0xFU,        /**< Low nibble (register / cond field).    */
  k_armv8m_sg_opcode  = 0xE97FE97FU, /**< Armv8-M `SG` secure-gateway opcode.    */
  k_thumb2_insn_bytes = 4U,          /**< 32-bit Thumb-2 instruction width.      */
  k_fpcxtns_push      = 0xCF81ED6DU, /**< `VSTR FPCXTNS,[sp,#-4]!` (LE word).    */
  k_fpcxtns_pop       = 0xCF81ECFDU, /**< `VLDR FPCXTNS,[sp],#4` (LE word).      */
  k_word_bytes        = 4U,          /**< One stacked word.                      */
  k_clrm_hw0          = 0xE89FU,     /**< `CLRM {regs}` first halfword.          */
  k_vscclrm_hw0_s     = 0xEC9FU,     /**< `VSCCLRM {s..,VPR}` first halfword.    */
  k_vscclrm_hw0_d     = 0xECDFU,     /**< `VSCCLRM {d..,VPR}` first halfword.    */
  k_lo16_mask         = 0xFFFFU,     /**< Low halfword of a 32-bit fetch.        */
  k_bkpt_hw_base      = 0xBE00U,     /**< `BKPT #imm8` halfword (imm free).      */
  k_bkpt_hw_mask      = 0xFF00U,     /**< Mask isolating the BKPT opcode.        */
  k_thumb_bx_lr       = 0x4770U,     /**< `BX LR` (stub a function to return).   */
} cortexm_exc_t;

/**
 * @enum board_sim_misc_t
 * @brief Named constants for ELF parsing, Thumb decode, and assorted literals.
 */
typedef enum : uint32_t {
  /* ELF32 layout, in bytes. */
  k_elf_ehdr_size      = 52U,   /**< ELF32 file-header size.            */
  k_elf_em_arm         = 40U,   /**< e_machine == EM_ARM.               */
  k_elf_e_phoff_off    = 28U,   /**< e_phoff in the file header.        */
  k_elf_ph_offset_off  = 4U,    /**< p_offset in a program header.      */
  k_elf_ph_vaddr_off   = 8U,    /**< p_vaddr (VMA) in a program header. */
  k_elf_ph_paddr_off   = 12U,   /**< p_paddr in a program header.       */
  k_elf_ph_filesz_off  = 16U,   /**< p_filesz in a program header.      */
  k_elf_ph_flags_off   = 24U,   /**< p_flags in a program header.       */
  k_elf_pf_x           = 1U,    /**< PF_X: segment is executable.       */
  k_elf_pt_load        = 1U,    /**< p_type == PT_LOAD.                 */
  k_elf_shentsize_min  = 40U,   /**< ELF32 section-header entry size.   */
  k_elf_sh_size_off    = 20U,   /**< sh_size in a section header.       */
  k_elf_sh_link_off    = 24U,   /**< sh_link in a section header.       */
  k_elf_sh_entsize_off = 36U,   /**< sh_entsize in a section header.    */
  k_elf_sym_info_off   = 12U,   /**< st_info in a symbol-table entry.   */
  k_elf_st_type_mask   = 0x0FU, /**< Low nibble of st_info is the type. */
  /* Thumb / conditional-select instruction decode. */
  k_thumb_op5_shift = 11U,   /**< op5 = hw0[15:11].                  */
  k_thumb_op5_mask  = 0x1FU, /**< 5-bit op5 field.                   */
  k_thumb32_op5_min = 0x1DU, /**< op5 >= this -> 32-bit instruction. */
  k_cs_op_shift     = 12U,   /**< CSEL-family op = hw2[13:12].       */
  k_cs_op_mask      = 0x3U,  /**< 2-bit op field.                    */
  /* Assorted. */
  k_u32_all_ones     = 0xFFFFFFFFU, /**< All bits set (MMIO read toggle).     */
  k_ra8_err_no_data  = 0x10AU,      /**< ra8_err_t value: no RX data.         */
  k_ra8_err_inval_st = 0x104U,      /**< ra8_err_t value: invalid state.      */
  k_strtol_base10    = 10U,         /**< Base-10 radix for strtol.            */
  k_max_panel_px     = 4096U,       /**< Largest accepted --size dimension.   */
  k_record_dir_mode  = 0755U,       /**< mkdir mode for the --record dir.     */
  k_byte_mask        = 0xFFU,       /**< Low 8 bits of a value (one byte).    */
  k_dump_sym_max     = 8U,          /**< Max --dump-sym globals per run.      */
  k_trace_sym_max    = 16U,         /**< Max --trace-sym functions per run.   */
  k_sectors_per_mib  = 2048U,       /**< 512-byte sectors per MiB (--sd-new). */
} board_sim_misc_t;

/** @brief 64-bit byte/size units used by --sd-new card sizing. */
typedef enum : uint64_t {
  k_bytes_per_sector = 512ULL,        /**< SD logical sector size in bytes.    */
  k_size_kib         = 1024ULL,       /**< One kibibyte (k suffix multiplier). */
  k_sd_u32_max       = 0xFFFFFFFFULL, /**< 32-bit sector-count ceiling.        */
  k_fat32_min_mib    = 512ULL,        /**< FAT32 default threshold, in MiB.    */
} board_sim_size_t;

/** @brief EK-RA8D2 user-switch GPIO coordinates (active-low): SW1 P009, SW2 P008. */
typedef enum : uint8_t {
  k_sim_sw_port = 0U, /**< Both user switches sit on PORT0. */
  k_sim_sw1_pin = 9U, /**< SW1 -> P009.                     */
  k_sim_sw2_pin = 8U, /**< SW2 -> P008.                     */
} sim_sw_pin_t;

/* Touch on the EK-RA8D2 is now modelled end-to-end: the firmware's real
 * ra8_touch_open / ra8_touch_read run unchanged and drive the GoodIX GT911 over
 * ra8_i3c_transfer (the I3C peripheral in legacy I2C mode), which board_periph
 * models as an I2C bus with a GT911 device. board_sim feeds --click / window
 * clicks into that device (board_periph_touch_inject), so a tap returns through
 * the genuine ra8_touch -> I3C -> GT911 path -- there is no function-level stub. */

/* Renesas peripheral quirks that the generic sparse model cannot reproduce.
 *
 * MRMS frequency latches: the CGC driver (libs/ra8_hal/src/ra8_cgc.c,
 * internal_wait_mrm_freq) writes ``key | freq_mhz`` to MRCFREQ / MREFREQ and
 * spins until the register reads back == freq_mhz. Real silicon validates the
 * upper key byte then strips it, so the readback is the bare frequency. The
 * generic model reflects the full written word (key still in bits[31:24]), so
 * the readback never equals freq and the poll runs to its 0x40000 timeout ->
 * lcd_panic_halt. Model the hardware: on readback of these two registers,
 * return the stored value with the key byte masked off. */
typedef enum : uint64_t {
  k_mrms_mrcfreq   = 0x4013C004UL, /**< MRICLK freq latch (write key 0x1E). */
  k_mrms_mrefreq   = 0x4013C008UL, /**< MRPCLK freq latch (write key 0xE1). */
  k_mrms_freq_mask = 0x00FFFFFFUL, /**< Key byte (bits[31:24]) stripped.    */
} mrms_quirk_t;

/* GLCDC observation point. The lcd_color_cycle demo proves a live panel by
 * re-writing BG_BGC (background colour) every frame and pulsing BG_EN.VEN to
 * commit it. The generic model only keeps the last value per address, so the
 * distinct colours cycled are tracked separately as the tool's success witness.
 * GLCDC base 0x40342000 + 0x1014 = BG_BGC (HUM Ch 63). */
typedef enum : uint64_t {
  k_glcdc_bg_bgc  = 0x40343014UL, /**< GLCDC BG.BGC background colour.    */
  k_bgc_track_max = 32UL,         /**< Distinct BG_BGC values remembered. */
} glcdc_obs_t;

/* Live-view (--view) and snapshot (--ppm) presentation settings. */
typedef enum : uint32_t {
  k_view_default_w     = 1024U,    /**< Default window width (EK-RA8D2 panel).      */
  k_view_default_h     = 600U,     /**< Default window height (EK-RA8D2 panel).     */
  k_view_present_every = 16U,      /**< Present the frame every Nth chunk.          */
  k_view_max_chunks    = 4000000U, /**< Cap in --view; closing the window ends.     */
  k_view_idle_us       = 16000U,   /**< ~60 Hz idle pump after the run ends.        */
  k_view_frame_us      = 16000U,   /**< Min wall-us between live presents (~60 Hz). */
  k_view_yield_us      = 2000U,    /**< Yield this long when a present is skipped.  */
  k_us_per_s           = 1000000U, /**< Microseconds per second.                    */
  k_ns_per_us          = 1000U,    /**< Nanoseconds per microsecond.                */
  k_uart_log_max       = 64U,      /**< Console ring depth (mirrors SCI model).     */
  k_reboot_settle      = 1500U,    /**< Chunks to run before a scheduled --reboot.  */
  /* --record settings. One outer chunk advances SysTick once (~1 ms of emulated
   * time at the firmware's 1 kHz tick), so ~1000 chunks == one emulated second.
   * Recording dumps a frame every k_record_every chunks for k_record_fps fps. */
  k_record_ms_per_sec = 1000U, /**< Emulated ms (= chunks) per second.        */
  k_record_fps        = 20U,   /**< Recorded frames per emulated second.      */
  k_record_every      = 50U,   /**< Chunks between recorded frames (1000/20). */
} view_cfg_t;

/* GLCDC graphics-layer 1 (GR1) framebuffer registers + field decode. The HAL
 * programs FLM6.FORMAT[30:28], FLM3.LNOFF[31:16] (line stride in bytes), and
 * FLM5.LNNUM[26:16] (lines - 1); reverse those to recover the framebuffer. */
typedef enum : uint64_t {
  k_glcdc_gr1_saddr = 0x4034310CUL, /**< GR[0].FLM2 framebuffer base.      */
  k_glcdc_gr1_flm3  = 0x40343110UL, /**< GR[0].FLM3 line stride (LNOFF).   */
  k_glcdc_gr1_flm5  = 0x40343118UL, /**< GR[0].FLM5 lines (LNNUM/DATANUM). */
  k_glcdc_gr1_fmt   = 0x4034311CUL, /**< GR[0].FLM6 pixel FORMAT.          */
} glcdc_gr_t;

typedef enum : uint32_t {
  k_glcdc_fmt_rgb565  = 2U,      /**< FLM6.FORMAT code for RGB565.             */
  k_glcdc_fmt_shift   = 28U,     /**< FORMAT[30:28].                           */
  k_glcdc_fmt_mask    = 0x7U,    /**< FORMAT field width.                      */
  k_glcdc_high_shift  = 16U,     /**< FLM3 stride / FLM5 lnnum live in [*:16]. */
  k_glcdc_stride_mask = 0xFFFFU, /**< FLM3.LNOFF is 16 bits.                   */
  k_glcdc_lnnum_mask  = 0x7FFU,  /**< FLM5.LNNUM is 11 bits.                   */
} glcdc_decode_t;

/* Sparse model of the Renesas peripheral space. Each touched address gets a
 * slot: control writes are reflected back on read so "configure then verify"
 * works, but once the firmware spins reading one address (a "wait for
 * ready/idle" poll) past k_mmio_settle, reads alternate 0 / all-ones so a
 * single-bit poll for either edge (flag set OR flag clear) completes instead
 * of running to its timeout. */
static uint64_t s_mmio_addr[k_mmio_slots];
static uint32_t s_mmio_val[k_mmio_slots];
static bool     s_mmio_written[k_mmio_slots];
static uint32_t s_mmio_rcount[k_mmio_slots];
static uint32_t s_mmio_wcount[k_mmio_slots];
static uint32_t s_mmio_n;
static uint32_t s_mmio_reads;
static uint32_t s_mmio_writes;
static uint32_t s_mmio_toggle;

/* Run-state telemetry the board view shows (updated by the run loop each
 * present): the current PC, the emulation-chunk counter, and whether the run
 * loop is still live (set false once the run parks / faults / exits). */
static uint32_t s_view_pc;
static uint32_t s_view_chunks;
static bool     s_view_running = true;
/* Console scrollback state, Arduino-Serial-Monitor style. s_view_scroll is the
 * offset in lines back from the newest line (0 = the live tail). When
 * s_view_autoscroll is true the view follows the tail (offset pinned to 0);
 * scrolling up pauses autoscroll and the view holds its ABSOLUTE lines as new
 * output arrives (the offset is advanced by each frame's new-line delta, tracked
 * via s_view_log_seen). Scrolling back down to the tail re-enables autoscroll. */
static uint32_t s_view_scroll;
static bool     s_view_autoscroll = true;
static uint32_t s_view_log_seen;
/* Active console tab: which board_console channel the console panel shows. The
 * tab bar (ALL | UART | ITM | SPI | I2C) switches it on a click; switching
 * resets the scrollback to the live tail of the newly selected channel. */
static board_console_ch_t s_view_console_ch = k_board_console_ch_all;

/** @brief Which core the firmware targets: gates the M85-only instruction seams. */
typedef enum : uint8_t {
  k_core_m85 = 0U, /**< Cortex-M85 primary (default): MVE/long-shift seams armed.    */
  k_core_m33 = 1U, /**< Cortex-M33 primary: the M85-only instruction seams stay off. */
} board_primary_core_t;

/* Core-control state (#152 dual-core CLI/GUI asks). s_primary_core gates the
 * M85-only instruction seams and relabels telemetry; s_low_power shrinks the
 * run-chunk budget to model the M33's 4:1-slower clock (Model A), and the GUI
 * low-power button + the --low-power flag both drive it. */
static board_primary_core_t s_primary_core = k_core_m85;
static bool                 s_low_power    = false;

static int      s_mmio_cache    = -1; /**< 1-entry address->slot lookup cache. */
static int      s_mmio_run_slot = -1; /**< Slot of the current read run.       */
static uint32_t s_mmio_run;           /**< Consecutive reads of that slot.     */
static uint32_t s_systick_fires;

/* Hand-modelled Cortex-M exception state. Unicorn's M33 core has no NVIC /
 * exception unit, so board_sim takes SysTick / PendSV / SVCall by hand: it
 * tracks the active-exception priority stack here (everything else -- MSP/PSP/
 * CONTROL/xPSR/PRIMASK -- is read straight from Unicorn). s_exc_stack holds the
 * priority of each handler currently active so a higher-priority exception
 * (e.g. SysTick, prio 0x40) can pre-empt a lower one (PendSV, prio 0xFF) but
 * not vice-versa, exactly as the real priority logic would nest them. */
static uint32_t s_exc_stack[k_exc_nest_max]; /**< Active-handler priorities.        */
static uint32_t s_exc_depth;                 /**< Number of active handlers.        */
static uint32_t s_pendsv_takes;              /**< PendSV exceptions taken.          */
static uint32_t s_svc_takes;                 /**< SVCall exceptions taken.          */
static uint64_t s_exc_return_pc;             /**< Pending EXC_RETURN to unstack.    */
static bool     s_exc_return_hit;            /**< An EXC_RETURN branch was seen.    */
static bool     s_systick_pending;           /**< SysTick exception is pended.      */
static bool     s_pendsv_stop;               /**< Chunk ended on a PENDSVSET.       */
static bool     s_bkpt_hit;                  /**< Firmware executed a BKPT.         */
static uint32_t s_bkpt_pc;                   /**< PC of the BKPT that halted.       */
static bool     s_reboot_request;            /**< AIRCR.SYSRESETREQ -> warm reboot. */

/**
 * @enum mpu_field_t
 * @brief Armv8-M MPU register field bits board_sim enforces.
 *
 * @details
 * The firmware programs the MPU via ra8_mpu (RNR -> RBAR -> RLAR per region,
 * then CTRL). board_sim watches those writes (no banked per-region storage
 * in the flat-RAM PPB, so each region is captured at its RLAR write) and,
 * while ``CTRL.ENABLE`` is set, faults a privileged store into any
 * read-only region with a synthesised MemManage -- the part Unicorn's core
 * does not model. Inert unless the firmware enables the MPU.
 */
typedef enum : uint32_t {
  k_mpu_ctrl_enable_bit  = 1U << 0U,    /**< MPU_CTRL.ENABLE.                  */
  k_mpu_rbar_ap_ro_bit   = 1U << 2U,    /**< RBAR.AP[2]=1 -> read-only.        */
  k_mpu_rlar_en_bit      = 1U << 0U,    /**< RLAR.EN region-enable.            */
  k_mpu_addr_mask        = 0xFFFFFFE0U, /**< BASE/LIMIT [31:5] (32-byte gran). */
  k_mpu_region_low_mask  = 0x1FU,       /**< Low 5 bits of an inclusive limit. */
  k_mpu_rnr_mask         = 0x7U,        /**< 8 regions -> 3-bit select.        */
  k_mpu_max_regions      = 8U,          /**< Modelled data regions (DREGION).  */
  k_cfsr_mmfsr_daccviol  = 1U << 1U,    /**< CFSR.MMFSR.DACCVIOL.              */
  k_cfsr_mmfsr_mmarvalid = 1U << 7U,    /**< CFSR.MMFSR.MMARVALID.             */
} mpu_field_t;

/**
 * @struct mpu_region_t
 * @brief One captured MPU data region (base/limit + permission/enable).
 */
typedef struct {
  uint32_t base;  /**< Inclusive region base (32-byte aligned).    */
  uint32_t limit; /**< Inclusive region top.                       */
  bool     ro;    /**< AP encodes read-only (no privileged write). */
  bool     en;    /**< RLAR.EN set.                                */
} mpu_region_t;

static mpu_region_t s_mpu_region[k_mpu_max_regions];  /**< Per-region shadow.    */
static bool         s_mpu_enabled;                    /**< CTRL.ENABLE active.   */
static uc_hook      s_mpu_ro_hook[k_mpu_max_regions]; /**< RO-range write hooks. */
static uint32_t     s_mpu_ro_hook_n;                  /**< Installed hook count. */
static bool         s_mpu_fault;                      /**< RO write trapped.     */
static uint32_t     s_mpu_fault_pc;                   /**< PC of faulting store. */
static uint32_t     s_mpu_fault_addr;                 /**< Address written.      */
static bool         s_div0_fault;                     /**< Trapping div-0 seen.  */
static uint32_t     s_div0_fault_pc;                  /**< PC of the divide.     */
static uint64_t     s_div0_traps;                     /**< Div-0 UsageFaults.    */

/**
 * @enum dual_core_addr_t
 * @brief CPU_CTRL registers + activation bit for the second core (cpu1).
 *
 * @details
 * The RA8D2 boots cpu0 (Cortex-M85); the application releases cpu1
 * (Cortex-M33) by writing CPU1INITVTOR (its vector table) then
 * CPU1ACTCSR.ACTREQ. board_sim emulates cpu1 in a second Unicorn engine
 * that shares the on-chip SRAM with cpu0 (host-backed), so cpu1_pingpong's
 * cross-core IPC over shared SRAM actually runs. Inert unless the firmware
 * carries a cpu1 image and asserts ACTREQ.
 */
typedef enum : uint64_t {
  k_cpu1_initvtor_addr = 0x4000F044UL, /**< CPU_CTRL.CPU1INITVTOR (32-bit). */
  k_cpu1_actcsr_addr   = 0x4000F064UL, /**< CPU_CTRL.CPU1ACTCSR  (16-bit).  */
  k_cpu1_mram_base     = 0x020C0000UL, /**< MRAM_CPU1: cpu1 image base.     */
  k_cpu1_mram_end      = 0x02100000UL, /**< MRAM_CPU1 end (256 KiB).        */
} dual_core_addr_t;

typedef enum : uint32_t {
  k_cpu1_actcsr_actreq    = 1U << 0U, /**< CPU1ACTCSR.ACTREQ -> release cpu1.   */
  k_cpu1_actcsr_key       = 0xA5U,    /**< KEY[15:8] required to honor a write. */
  k_cpu1_actcsr_key_shift = 8U,       /**< KEY byte position (bits [15:8]).     */
  k_cpu1_actcsr_key_mask  = 0xFFU,    /**< KEY byte mask after the shift.       */
  k_cpu1_chunk_insns      = 100000U,  /**< cpu1 instructions per interleave.    */
} dual_core_bits_t;

static uint8_t*   s_sram_buf;         /**< Host-backed on-chip SRAM (shared).   */
static uint8_t*   s_ospi_buf;         /**< Host-backed OSPI XIP flash (shared). */
static uint8_t*   s_sdram_buf;        /**< Host-backed external SDRAM (shared). */
static uc_engine* s_cpu1_uc;          /**< 2nd engine for cpu1 (NULL if N/A).   */
static bool       s_cpu1_active;      /**< cpu1 released and stepping.          */
static bool       s_cpu1_release_req; /**< CPU1ACTCSR.ACTREQ observed.          */
static uint32_t   s_cpu1_initvtor;    /**< Captured CPU1INITVTOR value.         */
static uint32_t   s_cpu1_pc;          /**< cpu1 run PC across interleaves.      */

/* BG_BGC colour-cycle witness: total writes and the distinct values seen. */
static uint32_t s_bgc_writes;
static uint32_t s_bgc_distinct[k_bgc_track_max];
static uint32_t s_bgc_distinct_n;

/** @brief Record a BG_BGC write; remember the value if it is a new colour. */
static void bgc_track(uint32_t value)
{
  s_bgc_writes++;
  for (uint32_t i = 0U; i < s_bgc_distinct_n; i++) {
    if (s_bgc_distinct[i] == value) {
      return;
    }
  }
  if (s_bgc_distinct_n < (uint32_t)k_bgc_track_max) {
    s_bgc_distinct[s_bgc_distinct_n++] = value;
  }
}

/** @brief Find (or add) a slot for a distinct MMIO address; -1 if table full. */
static int mmio_index(uint64_t addr)
{
  if ((s_mmio_cache >= 0) && (s_mmio_addr[s_mmio_cache] == addr)) {
    return s_mmio_cache;
  }
  for (uint32_t i = 0U; i < s_mmio_n; i++) {
    if (s_mmio_addr[i] == addr) {
      s_mmio_cache = (int)i;
      return (int)i;
    }
  }
  if (s_mmio_n < (uint32_t)k_mmio_slots) {
    s_mmio_addr[s_mmio_n] = addr;
    s_mmio_cache          = (int)s_mmio_n;
    return (int)(s_mmio_n++);
  }
  return -1;
}

static uint64_t mmio_read(uc_engine* uc, uint64_t offset, unsigned size, void* user)
{
  (void)user;
  s_mmio_reads++;
  /* A modelled peripheral block answers first; the sparse fallback below is
   * only reached for addresses no block in board_periph owns. */
  bool           handled = false;
  const uint64_t modeled = board_periph_read(uc, (uint64_t)k_periph_base + offset, size, &handled);
  if (handled) {
    return modeled;
  }
  const int idx = mmio_index((uint64_t)k_periph_base + offset);
  if (idx >= 0) {
    s_mmio_rcount[idx]++;
    if (idx == s_mmio_run_slot) {
      s_mmio_run++;
    } else {
      s_mmio_run_slot = idx;
      s_mmio_run      = 1U;
    }
    /* Reflect a written control value until a spin-poll forces it to settle. */
    if (s_mmio_written[idx] && (s_mmio_run <= (uint32_t)k_mmio_settle)) {
      const uint64_t addr = (uint64_t)k_periph_base + offset;
      /* MRMS frequency latches strip the write key byte on readback so the
       * driver's "wait until reg == freq" poll completes (see mrms_quirk_t). */
      if ((addr == (uint64_t)k_mrms_mrcfreq) || (addr == (uint64_t)k_mrms_mrefreq)) {
        return (uint64_t)(s_mmio_val[idx] & (uint32_t)k_mrms_freq_mask);
      }
      return (uint64_t)s_mmio_val[idx];
    }
  }
  s_mmio_toggle ^= (uint32_t)k_u32_all_ones;
  return (uint64_t)s_mmio_toggle;
}

/**
 * @brief Side-effect-free read of the last value written to a peripheral reg.
 *
 * @details
 * Returns the value last written to @p addr, or 0 if it was never written --
 * searching the MMIO shadow WITHOUT allocating a slot and WITHOUT advancing the
 * spin-settle toggle that ::mmio_read uses. board_sim's own introspection (e.g.
 * ::build_frame reading GLCDC registers to compose the panel) must see stable
 * state: a firmware that never programs the GLCDC (blink, USB, UART demos) would
 * otherwise read the status-poll fallthrough (an alternating 0/0xFFFFFFFF), which
 * made the panel strobe black<->white every frame. A real read of an unwritten
 * register reset-defaults to 0 here, so the panel is a steady background.
 */
static uint32_t mmio_peek(uint64_t addr)
{
  for (uint32_t i = 0U; i < s_mmio_n; i++) {
    if (s_mmio_addr[i] == addr) {
      return s_mmio_written[i] ? s_mmio_val[i] : 0U;
    }
  }
  return 0U;
}

static void mmio_write(uc_engine* uc, uint64_t offset, unsigned size, uint64_t value, void* user)
{
  (void)user;
  s_mmio_writes++;
  const uint64_t mmio_abs = (uint64_t)k_periph_base + offset;
  /* Dual-core release: the firmware stages cpu1's vector table in
   * CPU1INITVTOR, then asserts CPU1ACTCSR.ACTREQ to start cpu1. Capture both
   * so the run loop can boot the second engine (see cpu1_engine_init). */
  if (mmio_abs == (uint64_t)k_cpu1_initvtor_addr) {
    s_cpu1_initvtor = (uint32_t)value;
  } else if (mmio_abs == (uint64_t)k_cpu1_actcsr_addr) {
    /* HUM Ch 2.9.1.9 "CPU1ACTCSR" p 130 -- a write is honored only with
     * KEY=0xA5 in bits [15:8]; the firmware writes 0xA501 (KEY | ACTREQ).
     * Model the key gate so a keyless ACTREQ does not release cpu1. */
    const uint32_t key =
      ((uint32_t)value >> (uint32_t)k_cpu1_actcsr_key_shift) & (uint32_t)k_cpu1_actcsr_key_mask;
    if ((key == (uint32_t)k_cpu1_actcsr_key) &&
        (((uint32_t)value & (uint32_t)k_cpu1_actcsr_actreq) != 0U)) {
      s_cpu1_release_req = true;
    }
  }
  if (mmio_abs == (uint64_t)k_glcdc_bg_bgc) {
    bgc_track((uint32_t)value);
  }
  /* A modelled peripheral block consumes the write first (so GPIO latches,
   * timer control, and ICU event links take real effect); the sparse fallback
   * still records the write for the MMIO table and unmodelled blocks. */
  bool handled = false;
  board_periph_write(uc, (uint64_t)k_periph_base + offset, size, value, &handled);
  if (handled) {
    return;
  }
  const int idx = mmio_index((uint64_t)k_periph_base + offset);
  if (idx >= 0) {
    s_mmio_wcount[idx]++;
    s_mmio_val[idx]     = (uint32_t)value;
    s_mmio_written[idx] = true;
    if (idx == s_mmio_run_slot) {
      s_mmio_run++; /* same-addr read-modify-write spin accumulates toward settle */
    } else {
      s_mmio_run_slot = idx; /* new register -> following reads should see its value */
      s_mmio_run      = 1U;
    }
  }
}

/* Armv8.1-M conditional-select family (CSEL/CSINC/CSINV/CSNEG). The RA8D2 is
 * Cortex-M85 (Armv8.1-M) but Unicorn's nearest core is M33 (Armv8-M), which
 * lacks these. GCC emits them for branchless index/modulo math -- notably in
 * the firmware's event_post on the touch path -- so the invalid-instruction
 * hook decodes and executes them and lets the real firmware path continue.
 * Encoding (T1, two halfwords, little-endian): hw1 = 0xEA5n (n = Rn);
 * hw2 bit15=1, bit14=0, op=bits[13:12], Rd=bits[11:8], cond=bits[7:4],
 * Rm=bits[3:0]. Verified against the GNU assembler for armv8.1-m.main. */
typedef enum : uint32_t {
  k_cs_hw1_mask  = 0xFFF0U, /**< hw1 high 12 bits identify the group. */
  k_cs_hw1_match = 0xEA50U, /**< hw1[15:4] == 0xEA5 for this family.  */
  k_cs_hw2_b15   = 0x8000U, /**< hw2 bit15 must be 1.                 */
  k_cs_hw2_b14   = 0x4000U, /**< hw2 bit14 must be 0.                 */
  k_cs_op_csel   = 0U,      /**< op == 00: Rd = c ? Rn : Rm.          */
  k_cs_op_csinc  = 1U,      /**< op == 01: Rd = c ? Rn : Rm + 1.      */
  k_cs_op_csinv  = 2U,      /**< op == 10: Rd = c ? Rn : ~Rm.         */
  k_cs_op_csneg  = 3U,      /**< op == 11: Rd = c ? Rn : -Rm.         */
  k_cs_insn_len  = 4U,      /**< Both halfwords: 4 bytes.             */
} cond_select_t;

/* Armv8.1-M long-shift family (LSLL/LSRL/ASRL, immediate). Same M85-vs-M33 gap
 * as the conditional-selects above, but with a sharper failure mode: the M33
 * does NOT trap these. Their encoding overlaps ORR.W (register), so the core
 * silently mis-executes the shift and yields a wrong 64-bit result with no
 * fault -- there is no invalid-instruction hook to catch them. GCC emits them
 * for any 64-bit shift that crosses the word boundary (e.g. miniz's ZIP
 * central-directory size math: `(u64)entries * 46` then `>> 32`), so the silent
 * miscompile corrupts the parse. They are instead found by a one-time image scan
 * and emulated at each site (see long_shift_seam_install / on_long_shift).
 * Encoding (T1, two halfwords, little-endian): hw1 = 0xEA5n (n = RdaLo);
 * hw2[3:0] = 0b1111, op = bits[5:4] (0=LSLL, 1=LSRL, 2=ASRL), RdaHi = bits[11:8],
 * imm5 = bits[14:12]:bits[7:6] (0 encodes a shift of 32). Verified against the
 * GNU assembler for armv8.1-m.main. */
typedef enum : uint32_t {
  k_lsh_hw1_mask     = 0xFFF0U, /**< hw1[15:4] selects the group; RdaLo in [3:0]. */
  k_lsh_hw1_match    = 0xEA50U, /**< hw1[15:4] == 0xEA5 for this family.          */
  k_lsh_hw2_lo_mask  = 0x000FU, /**< hw2[3:0] fixed pattern for the imm form.     */
  k_lsh_hw2_lo_match = 0x000FU, /**< hw2[3:0] == 0b1111.                          */
  k_lsh_op_shift     = 4U,      /**< op field at hw2[5:4].                        */
  k_lsh_op_mask      = 0x3U,    /**< op width (value 3 is reserved -> rejected).  */
  k_lsh_op_lsll      = 0U,      /**< 00: 64-bit logical shift left.               */
  k_lsh_op_lsrl      = 1U,      /**< 01: 64-bit logical shift right.              */
  k_lsh_op_asrl      = 2U,      /**< 10: 64-bit arithmetic shift right.           */
  k_lsh_rdahi_shift  = 8U,      /**< RdaHi at hw2[11:8].                          */
  k_lsh_imm3_shift   = 12U,     /**< imm3 at hw2[14:12].                          */
  k_lsh_imm3_mask    = 0x7U,    /**< imm3 width.                                  */
  k_lsh_imm2_shift   = 6U,      /**< imm2 at hw2[7:6].                            */
  k_lsh_imm2_mask    = 0x3U,    /**< imm2 width.                                  */
  k_lsh_imm3_to_imm5 = 2U,      /**< imm5 = (imm3 << 2) | imm2.                   */
  k_lsh_word_bits    = 32U,     /**< Word width; also the imm5==0 shift amount.   */
  k_lsh_insn_len     = 4U,      /**< Thumb-2 instruction length, bytes.           */
} long_shift_t;

/* APSR condition-flag bit positions inside xPSR. */
typedef enum : uint32_t {
  k_apsr_n = 31U, /**< Negative. */
  k_apsr_z = 30U, /**< Zero.     */
  k_apsr_c = 29U, /**< Carry.    */
  k_apsr_v = 28U, /**< Overflow. */
} apsr_bit_t;

/* ARM register index (0..15) -> Unicorn register id. PC(15)/SP(13)/LR(14) are
 * never CSx destinations in practice but are mapped for completeness. */
static const int k_arm_reg_id[16] = {
  UC_ARM_REG_R0,
  UC_ARM_REG_R1,
  UC_ARM_REG_R2,
  UC_ARM_REG_R3,
  UC_ARM_REG_R4,
  UC_ARM_REG_R5,
  UC_ARM_REG_R6,
  UC_ARM_REG_R7,
  UC_ARM_REG_R8,
  UC_ARM_REG_R9,
  UC_ARM_REG_R10,
  UC_ARM_REG_R11,
  UC_ARM_REG_R12,
  UC_ARM_REG_SP,
  UC_ARM_REG_LR,
  UC_ARM_REG_PC,
};

/**
 * @brief Evaluate an ARM condition code against the APSR flags.
 *
 * @param[in] cond 4-bit ARM condition code (0..15).
 * @param[in] xpsr Current xPSR (APSR flags live in the top nibble).
 * @return true if the condition holds.
 */
/**
 * @enum arm_cond_t
 * @brief ARM/Thumb 4-bit condition-code field encodings (cond[3:0]).
 */
typedef enum : uint32_t {
  k_cond_eq = 0x0U, /**< Equal (Z==1).             */
  k_cond_ne = 0x1U, /**< Not equal (Z==0).         */
  k_cond_cs = 0x2U, /**< Carry set / unsigned >=.  */
  k_cond_cc = 0x3U, /**< Carry clear / unsigned <. */
  k_cond_mi = 0x4U, /**< Negative.                 */
  k_cond_pl = 0x5U, /**< Positive or zero.         */
  k_cond_vs = 0x6U, /**< Overflow set.             */
  k_cond_vc = 0x7U, /**< Overflow clear.           */
  k_cond_hi = 0x8U, /**< Unsigned higher.          */
  k_cond_ls = 0x9U, /**< Unsigned lower or same.   */
  k_cond_ge = 0xAU, /**< Signed >=.                */
  k_cond_lt = 0xBU, /**< Signed <.                 */
  k_cond_gt = 0xCU, /**< Signed >.                 */
  k_cond_le = 0xDU, /**< Signed <=.                */
  k_cond_al = 0xEU, /**< Always.                   */
} arm_cond_t;

static bool cond_holds(uint32_t cond, uint32_t xpsr)
{
  const bool n = ((xpsr >> (uint32_t)k_apsr_n) & 1U) != 0U;
  const bool z = ((xpsr >> (uint32_t)k_apsr_z) & 1U) != 0U;
  const bool c = ((xpsr >> (uint32_t)k_apsr_c) & 1U) != 0U;
  const bool v = ((xpsr >> (uint32_t)k_apsr_v) & 1U) != 0U;
  switch (cond & (uint32_t)k_lo4_mask) {
    case k_cond_eq:
      return z;
    case k_cond_ne:
      return !z;
    case k_cond_cs:
      return c;
    case k_cond_cc:
      return !c;
    case k_cond_mi:
      return n;
    case k_cond_pl:
      return !n;
    case k_cond_vs:
      return v;
    case k_cond_vc:
      return !v;
    case k_cond_hi:
      return c && !z;
    case k_cond_ls:
      return !c || z;
    case k_cond_ge:
      return n == v;
    case k_cond_lt:
      return n != v;
    case k_cond_gt:
      return !z && (n == v);
    case k_cond_le:
      return z || (n != v);
    default:
      return true; /* AL (k_cond_al) / 0xF */
  }
}

/**
 * @brief Emulate one Armv8.1-M conditional-select instruction if present at PC.
 *
 * @details
 * Decodes the CSEL/CSINC/CSINV/CSNEG encoding (see ::cond_select_t), evaluates
 * the condition against the APSR, computes Rd, writes it back, and advances PC
 * past the 4-byte instruction. This lets Unicorn's M33 core execute the M85
 * firmware's branchless index math instead of trapping on an opcode it does not
 * implement. Anything that is not this family is left untouched.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   Address of the trapped instruction.
 * @param[in]     code The 4 instruction bytes already read at @p pc.
 * @return true if a conditional-select was recognised, executed, and PC advanced.
 */
static bool emulate_cond_select(uc_engine* uc, uint32_t pc, const uint8_t* code)
{
  const uint16_t hw1 = (uint16_t)(code[0] | ((uint16_t)code[1] << 8));
  const uint16_t hw2 = (uint16_t)(code[2] | ((uint16_t)code[3] << 8));
  if (((hw1 & (uint16_t)k_cs_hw1_mask) != (uint16_t)k_cs_hw1_match) ||
      ((hw2 & (uint16_t)k_cs_hw2_b15) == 0U) || ((hw2 & (uint16_t)k_cs_hw2_b14) != 0U)) {
    return false;
  }
  const uint32_t rn   = (uint32_t)(hw1 & (uint32_t)k_lo4_mask);
  const uint32_t op   = (uint32_t)((hw2 >> (uint32_t)k_cs_op_shift) & (uint32_t)k_cs_op_mask);
  const uint32_t rd   = (uint32_t)((hw2 >> 8) & (uint32_t)k_lo4_mask);
  const uint32_t cond = (uint32_t)((hw2 >> 4) & (uint32_t)k_lo4_mask);
  const uint32_t rm   = (uint32_t)(hw2 & (uint32_t)k_lo4_mask);

  uint32_t xpsr = 0U;
  uint32_t vn   = 0U;
  uint32_t vm   = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_XPSR, &xpsr);
  (void)uc_reg_read(uc, k_arm_reg_id[rn], &vn);
  (void)uc_reg_read(uc, k_arm_reg_id[rm], &vm);

  uint32_t result;
  if (cond_holds(cond, xpsr)) {
    result = vn;
  } else {
    switch (op) {
      case (uint32_t)k_cs_op_csinc:
        result = vm + 1U;
        break;
      case (uint32_t)k_cs_op_csinv:
        result = ~vm;
        break;
      case (uint32_t)k_cs_op_csneg:
        result = (uint32_t)(-(int32_t)vm);
        break;
      case (uint32_t)k_cs_op_csel:
      default:
        result = vm;
        break;
    }
  }
  (void)uc_reg_write(uc, k_arm_reg_id[rd], &result);
  uint32_t next = pc + (uint32_t)k_cs_insn_len;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  return true;
}

/**
 * @brief Emulate an Armv8-M memory barrier (DSB/DMB/ISB) as a NOP if present.
 *
 * @details
 * Some Unicorn builds -- e.g. 2.0.1, as packaged on the Linux CI runner -- do
 * not decode the self-synchronising barrier instructions DSB / DMB / ISB and
 * trap them as invalid, where a newer build executes them. They have no
 * architectural effect in this single-threaded, in-order emulator (there is no
 * real memory ordering or pipeline to enforce), so recognising the encoding and
 * advancing PC past the 4-byte instruction is a faithful NOP. This keeps the
 * firmware's boot-path barriers (e.g. after a clock / SDRAM register write) from
 * faulting regardless of the host Unicorn version. Anything else is left
 * untouched.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   Address of the trapped instruction.
 * @param[in]     code The 4 instruction bytes already read at @p pc.
 * @return true if a DSB/DMB/ISB barrier was recognised and PC advanced past it.
 */
static bool emulate_barrier(uc_engine* uc, uint32_t pc, const uint8_t* code)
{
  enum : uint16_t {
    k_barrier_hw1       = 0xF3BFU, /**< First half-word of DSB/DMB/ISB.       */
    k_barrier_hw2_mask  = 0xFF00U, /**< Fixed high byte of the second h-word. */
    k_barrier_hw2_match = 0x8F00U, /**< 0x8F: the barrier group.              */
    k_barrier_op_mask   = 0x00F0U, /**< Barrier subtype field, bits [7:4].    */
    k_barrier_op_dsb    = 0x0040U, /**< DSB.                                  */
    k_barrier_op_dmb    = 0x0050U, /**< DMB.                                  */
    k_barrier_op_isb    = 0x0060U, /**< ISB.                                  */
    k_barrier_len       = 0x0004U, /**< Thumb-2 barrier instruction length.   */
  };
  const uint16_t hw1 = (uint16_t)(code[0] | ((uint16_t)code[1] << 8));
  const uint16_t hw2 = (uint16_t)(code[2] | ((uint16_t)code[3] << 8));
  if ((hw1 != (uint16_t)k_barrier_hw1) ||
      ((hw2 & (uint16_t)k_barrier_hw2_mask) != (uint16_t)k_barrier_hw2_match)) {
    return false;
  }
  const uint16_t op = (uint16_t)(hw2 & (uint16_t)k_barrier_op_mask);
  if ((op != (uint16_t)k_barrier_op_dsb) && (op != (uint16_t)k_barrier_op_dmb) &&
      (op != (uint16_t)k_barrier_op_isb)) {
    return false;
  }
  const uint32_t next = pc + (uint32_t)k_barrier_len;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  return true;
}

/* Forward declarations for the Cortex-M exception engine (defined below, after
 * the ELF/memory helpers they build on). The memory hooks above need to vector
 * SVCall and recognise EXC_RETURN before those definitions appear. */
static bool     is_exc_return(uint64_t pc);
static uint32_t exc_vector(uc_engine* uc, uint32_t vtor_base, uint32_t exc_num);
static void     exc_enter(uc_engine* uc, uint32_t exc_num, uint32_t handler);
static uint32_t exc_priority(uc_engine* uc, uint32_t exc_num);
static uint32_t exc_active_prio(void);
/* The SCB control-register write hook (on_scb_ctrl_write) arms the div-0 trap on a
 * CCR.DIV_0_TRP write, and on_invalid_insn services the resulting UDF traps; both
 * helpers are defined with the div-0 seam below. */
static void div0_patch_sites(uc_engine* uc);
static bool emulate_div0_patched(uc_engine* uc, uint32_t pc, const uint8_t code[4]);
/* PPB RAM word + register accessors (defined with the memory helpers below); the
 * MPU enforcement hooks above use them before their definitions appear. */
static uint32_t rd32(uc_engine* uc, uint64_t addr);
static void     wr32(uc_engine* uc, uint64_t addr, uint32_t v);
static uint32_t reg_get(uc_engine* uc, int reg);

/**
 * @brief Emulate the Armv8-M security register-scrub ops as NOPs.
 *
 * @details `CLRM {regs}` and `VSCCLRM {s..,VPR}` zero caller-saved core / FP
 * registers on a Non-Secure-Callable return so Secure data cannot leak to the
 * Non-Secure caller. board_sim is a single flat domain with one register/FP
 * bank, so the scrub has no observable effect on a correct caller (the cleared
 * registers are already caller-saved/clobbered) -- model them as NOPs so the
 * cmse veneer epilogue runs. Unicorn's M33 rejects both as invalid.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   Address of the instruction.
 * @param[in]     code Four instruction bytes at @p pc.
 * @return true if @p code was a scrub op (PC advanced past it); false otherwise.
 * @pre @p code holds the 4 bytes at @p pc.
 * @pre @p uc is running.
 * @post On true, PC is advanced one 32-bit instruction.
 * @post On false, no state changes.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool emulate_sec_scrub(uc_engine* uc, uint32_t pc, const uint8_t code[4])
{
  const uint16_t hw0 = (uint16_t)(((uint16_t)code[1] << (uint16_t)k_byte_bits) | (uint16_t)code[0]);
  if ((hw0 != (uint16_t)k_clrm_hw0) && (hw0 != (uint16_t)k_vscclrm_hw0_s) &&
      (hw0 != (uint16_t)k_vscclrm_hw0_d)) {
    return false;
  }
  const uint32_t next = pc + (uint32_t)k_thumb2_insn_bytes;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  return true;
}

/* ============================================================================
 * Minimal MVE (Helium) emulation. The RA8D2 is Cortex-M85 (Armv8.1-M, has MVE),
 * but the closest core Unicorn offers is M33 (Armv8-M, NO MVE), so the Helium
 * instructions GCC's auto-vectoriser emits trap here as "invalid". GCC vectorises
 * the memset / struct-zero idiom with two forms, which this handles:
 *   VMOV.I32 Qd, #imm          -- set all four 32-bit lanes of Qd to imm
 *   VSTRW.32 Qd, [Rn{, #off}]  -- store the 16-byte Qd to memory (no write-back)
 * The Q registers alias the FPU D registers (Qn == D[2n]:D[2n+1]) -- the M33 core
 * has those (the firmware uses the FPU) -- so the vector state is read/written
 * through Unicorn's D registers. Any other MVE form falls through and still
 * reports as invalid, so nothing is silently mis-executed. capstone (already
 * linked, and already decoding these for the error path) provides the operands;
 * the engine handle is opened once and reused. On real silicon all of this just
 * runs natively on Helium -- this only makes the M33-based sim faithful to it.
 * ==========================================================================*/
enum : uint32_t {
  k_mve_insn_len   = 4U,    /**< MVE instructions are 32-bit Thumb-2.          */
  k_mve_q_bytes    = 16U,   /**< Bytes in a Q (128-bit) register.              */
  k_mve_vstrw_pfx  = 5U,    /**< strncmp length for the "vstrw" prefix.        */
  k_mve_lane_shift = 32U,   /**< 32-bit lane width (two lanes per D register). */
  k_mve_max_run    = 4096U, /**< Loop bound: max consecutive MVE ops per trap. */
};
/** @brief Count of MVE instructions emulated this run (run-end telemetry). */
static uint64_t s_mve_emulated = 0U;

/** @brief Map capstone Q-reg @p qreg to its Unicorn D-register half (low/high). */
static int mve_q_d(unsigned int qreg, bool high)
{
  const unsigned int idx = qreg - (unsigned int)ARM_REG_Q0; /* Q index 0..7. */
  return (int)UC_ARM_REG_D0 + (int)(2U * idx) + (high ? 1 : 0);
}

/** @brief Execute one decoded MVE instruction (no PC change); true iff handled. */
static bool mve_exec_one(uc_engine* uc, const cs_insn* insn)
{
  const cs_arm* d     = &insn->detail->arm;
  const bool    op0_q = (d->op_count == 2) && (d->operands[0].type == ARM_OP_REG) &&
                        (d->operands[0].reg >= ARM_REG_Q0) && (d->operands[0].reg <= ARM_REG_Q7);
  if (!op0_q) {
    return false;
  }
  /* VMOV.I32 Qd, #imm -> replicate the 32-bit immediate into all four lanes. */
  if ((insn->id == ARM_INS_VMOV) && (d->operands[1].type == ARM_OP_IMM) &&
      (strstr(insn->mnemonic, ".i32") != nullptr)) {
    const uint32_t imm  = (uint32_t)d->operands[1].imm;
    const uint64_t pair = ((uint64_t)imm << (uint64_t)k_mve_lane_shift) | (uint64_t)imm;
    (void)uc_reg_write(uc, mve_q_d(d->operands[0].reg, false), &pair);
    (void)uc_reg_write(uc, mve_q_d(d->operands[0].reg, true), &pair);
    return true;
  }
  /* VSTRW.32 Qd, [Rn, #off] (no write-back) -> store the 16-byte vector. */
  if ((strncmp(insn->mnemonic, "vstrw", (size_t)k_mve_vstrw_pfx) == 0) && !d->writeback &&
      (d->operands[1].type == ARM_OP_MEM) && (d->operands[1].mem.base >= ARM_REG_R0) &&
      (d->operands[1].mem.base <= ARM_REG_R12)) {
    uint64_t lo = 0U;
    uint64_t hi = 0U;
    (void)uc_reg_read(uc, mve_q_d(d->operands[0].reg, false), &lo);
    (void)uc_reg_read(uc, mve_q_d(d->operands[0].reg, true), &hi);
    uint32_t  base   = 0U;
    const int base_r = (int)UC_ARM_REG_R0 + ((int)d->operands[1].mem.base - (int)ARM_REG_R0);
    (void)uc_reg_read(uc, base_r, &base);
    const uint32_t addr = base + (uint32_t)d->operands[1].mem.disp;
    uint8_t        buf[k_mve_q_bytes];
    (void)memcpy(buf, &lo, sizeof(lo));
    (void)memcpy(buf + sizeof(lo), &hi, sizeof(hi));
    (void)uc_mem_write(uc, (uint64_t)addr, buf, (size_t)k_mve_q_bytes);
    return true;
  }
  return false;
}

/**
 * @brief Emulate a run of consecutive auto-vectoriser MVE instructions.
 *
 * @details
 * Helium ops come in tight back-to-back runs (e.g. one VMOV.I32 then several
 * VSTRW.32 to zero a struct). Emulating only the first and relaunching would land
 * the next launch on another invalid instruction, which Unicorn faults on -- so
 * this consumes every consecutive handled MVE op in one trap and sets PC to the
 * first instruction it does NOT handle, exactly mirroring the cond-select seam's
 * "stop, then relaunch on valid code" contract. Bounded by ::k_mve_max_run.
 *
 * @return true iff at least one MVE instruction was emulated.
 */
static bool emulate_mve(uc_engine* uc, uint32_t pc0, const uint8_t code0[4])
{
  static csh  s_cs;
  static bool s_cs_ok = false;
  if (!s_cs_ok) {
    if (cs_open(CS_ARCH_ARM, (cs_mode)(CS_MODE_THUMB | CS_MODE_MCLASS), &s_cs) != CS_ERR_OK) {
      return false;
    }
    (void)cs_option(s_cs, CS_OPT_DETAIL, CS_OPT_ON);
    s_cs_ok = true;
  }
  uint32_t pc = pc0;
  uint8_t  code[4];
  (void)memcpy(code, code0, sizeof(code));
  uint32_t handled = 0U;
  while (handled < (uint32_t)k_mve_max_run) {
    cs_insn*     insn = nullptr;
    const size_t n    = cs_disasm(s_cs, code, (size_t)k_mve_insn_len, pc, 1, &insn);
    if (n != 1U) {
      break;
    }
    const bool ok = mve_exec_one(uc, &insn[0]);
    cs_free(insn, n);
    if (!ok) {
      break; /* first non-MVE (valid) instruction -- relaunch resumes here. */
    }
    handled++;
    pc += (uint32_t)k_mve_insn_len;
    if (uc_mem_read(uc, (uint64_t)pc, code, sizeof(code)) != UC_ERR_OK) {
      break;
    }
  }
  if (handled > 0U) {
    (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
    s_mve_emulated += handled;
  }
  return handled > 0U;
}

/* ---------------------------------------------------------------------------
 * VSTRW.32 seam. Unlike VMOV.I32 (which traps as invalid above), the MVE vector
 * store decodes as a *valid* legacy coprocessor store (`stc p15, ...`) on the
 * M33 core, so Unicorn would EXECUTE it -- faulting on the absent coprocessor --
 * rather than trap it. capstone mis-reads it the same way, so it cannot be
 * recognised in the invalid-instruction path. Instead, like the long-shift seam,
 * scan the loaded image for VSTRW.32 encodings and hook each site so the store
 * happens before Unicorn reaches the bad instruction.
 *
 * Encoding (Armv8.1-M, U=add): hw1 = 0xED8<Rn>, hw2 = 0bQd[15:13] 1111 0 imm7,
 * i.e. (hw1 & 0xFFF0)==0xED80 and (hw2 & 0x1F80)==0x1F00; the store offset is
 * imm7 * 4 (word-scaled). Verified against the toolchain's own disassembly.
 * ===========================================================================
 */
enum : uint32_t {
  k_mve_sites_max = 8192U,   /**< Cap on hooked VSTRW.32 sites per image.  */
  k_mve_vstrw_hw1 = 0xED80U, /**< VSTRW.32 hw1 fixed bits (Rn in [3:0]).   */
  k_mve_vstrw_h1m = 0xFFF0U, /**< Mask isolating the fixed hw1 bits.       */
  k_mve_vstrw_hw2 = 0x1F00U, /**< VSTRW.32 hw2 fixed bits (Qd in [15:13]). */
  k_mve_vstrw_h2m = 0x1F80U, /**< Mask isolating the fixed hw2 bits.       */
  k_mve_qd_shift  = 13U,     /**< Qd field position in hw2.                */
  k_mve_qd_mask   = 0x7U,    /**< Qd field width (3 bits) after shift.     */
  k_mve_rn_mask   = 0xFU,    /**< Rn field (4 bits) in hw1[3:0].           */
  k_mve_imm7_mask = 0x7FU,   /**< imm7 field (word offset) in hw2.         */
  k_mve_off_scale = 4U,      /**< VSTRW.32 immediate counts 4-byte words.  */
};
/** @brief One UC_HOOK_CODE per VSTRW.32 site (installed by mve_seam_install). */
static uc_hook s_mve_hooks[k_mve_sites_max];

/** @brief Decode VSTRW.32 Qd,[Rn,#imm7*4] (U=add); true iff hw1/hw2 match it. */
static bool mve_vstrw_decode(uint16_t hw1, uint16_t hw2, uint32_t* qd, uint32_t* rn, uint32_t* off)
{
  if (((hw1 & (uint16_t)k_mve_vstrw_h1m) != (uint16_t)k_mve_vstrw_hw1) ||
      ((hw2 & (uint16_t)k_mve_vstrw_h2m) != (uint16_t)k_mve_vstrw_hw2)) {
    return false;
  }
  *qd  = ((uint32_t)hw2 >> (uint32_t)k_mve_qd_shift) & (uint32_t)k_mve_qd_mask;
  *rn  = (uint32_t)hw1 & (uint32_t)k_mve_rn_mask;
  *off = ((uint32_t)hw2 & (uint32_t)k_mve_imm7_mask) * (uint32_t)k_mve_off_scale;
  return true;
}

/** @brief UC_HOOK_CODE at a VSTRW.32 site: store the 16-byte Qd, then relaunch. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_mve_vstrw(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)size;
  (void)user;
  uint8_t code[4] = {};
  if (uc_mem_read(uc, address, code, sizeof(code)) != UC_ERR_OK) {
    return;
  }
  const uint16_t hw1 = (uint16_t)(code[0] | ((uint16_t)code[1] << (uint16_t)k_byte_bits));
  const uint16_t hw2 = (uint16_t)(code[2] | ((uint16_t)code[3] << (uint16_t)k_byte_bits));
  uint32_t       qd  = 0U;
  uint32_t       rn  = 0U;
  uint32_t       off = 0U;
  if (!mve_vstrw_decode(hw1, hw2, &qd, &rn, &off)) {
    return; /* scan false-positive at a never-executed address -- leave it. */
  }
  uint64_t lo = 0U;
  uint64_t hi = 0U;
  (void)uc_reg_read(uc, (int)UC_ARM_REG_D0 + (int)(2U * qd), &lo);
  (void)uc_reg_read(uc, (int)UC_ARM_REG_D0 + (int)(2U * qd) + 1, &hi);
  uint32_t base = 0U;
  (void)uc_reg_read(uc, (int)UC_ARM_REG_R0 + (int)rn, &base);
  uint8_t buf[k_mve_q_bytes];
  (void)memcpy(buf, &lo, sizeof(lo));
  (void)memcpy(buf + sizeof(lo), &hi, sizeof(hi));
  (void)uc_mem_write(uc, (uint64_t)(base + off), buf, (size_t)k_mve_q_bytes);
  const uint32_t next = (uint32_t)address + (uint32_t)k_mve_insn_len;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  s_mve_emulated++;
  (void)uc_emu_stop(uc);
}

/** @brief Scan the loaded image for VSTRW.32 sites and hook each one. */
static void mve_seam_install(uc_engine* uc, const uint8_t* elf, long len)
{
  if ((elf == nullptr) || (len < (long)k_elf_ehdr_size)) {
    return;
  }
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + (uint32_t)k_elf_e_phoff_off, 4);
  const uint16_t phentsize = (uint16_t)(elf[42] | (elf[43] << 8));
  const uint16_t phnum     = (uint16_t)(elf[44] | (elf[45] << 8));
  if (((uint64_t)phoff + ((uint64_t)phnum * (uint64_t)phentsize)) > (uint64_t)len) {
    return;
  }
  uint32_t n_hooks = 0U;
  for (uint16_t i = 0U; i < phnum; i++) {
    const uint8_t* ph       = elf + phoff + ((uint32_t)i * phentsize);
    uint32_t       p_type   = 0U;
    uint32_t       p_offset = 0U;
    uint32_t       p_vaddr  = 0U;
    uint32_t       p_filesz = 0U;
    uint32_t       p_flags  = 0U;
    (void)memcpy(&p_type, ph + 0, 4);
    (void)memcpy(&p_offset, ph + (uint32_t)k_elf_ph_offset_off, 4);
    (void)memcpy(&p_vaddr, ph + (uint32_t)k_elf_ph_vaddr_off, 4);
    (void)memcpy(&p_filesz, ph + (uint32_t)k_elf_ph_filesz_off, 4);
    (void)memcpy(&p_flags, ph + (uint32_t)k_elf_ph_flags_off, 4);
    if ((p_type != (uint32_t)k_elf_pt_load) || (p_filesz < (uint32_t)k_mve_insn_len) ||
        ((p_flags & (uint32_t)k_elf_pf_x) == 0U)) {
      continue;
    }
    if (((uint64_t)p_offset + (uint64_t)p_filesz) > (uint64_t)len) {
      continue;
    }
    for (uint32_t off = 0U; (off + (uint32_t)k_mve_insn_len) <= p_filesz; off += 2U) {
      const uint8_t* p   = elf + p_offset + off;
      const uint16_t hw1 = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
      const uint16_t hw2 = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
      uint32_t       qd  = 0U;
      uint32_t       rn  = 0U;
      uint32_t       o   = 0U;
      if (!mve_vstrw_decode(hw1, hw2, &qd, &rn, &o)) {
        continue;
      }
      if (n_hooks >= (uint32_t)k_mve_sites_max) {
        (void)fprintf(stderr, "  MVE seam: site cap %u reached\n", (unsigned)k_mve_sites_max);
        return;
      }
      const uint64_t va = (uint64_t)p_vaddr + (uint64_t)off;
      (void)
        uc_hook_add(uc, &s_mve_hooks[n_hooks], UC_HOOK_CODE, (void*)on_mve_vstrw, nullptr, va, va);
      n_hooks++;
    }
  }
  if (n_hooks > 0U) {
    (void)fprintf(stderr,
                  "  MVE seam      : hooking %u VSTRW.32 site(s) (M85 Helium the M33 lacks)\n",
                  (unsigned)n_hooks);
  }
}

/* ---------------------------------------------------------------------------
 * Low-Overhead-Branch (LOB) emulation. The M85 has the hardware-loop extension
 * (DLS/WLS/LE) that GCC's -Og uses for counted loops; the M33 does not, so they
 * trap as invalid. Handle the two forms the toolchain emits:
 *   DLS lr, Rn        -- start a loop: LR = Rn (the iteration count)
 *   LE  lr, <label>   -- loop end: LR -= 1; branch back to <label> while LR != 0
 * (the toolchain's <label> offset is (hw2 & 0x7FF) 2-byte units backward from
 * PC+4, verified against its disassembly). Both sit amid valid code, so the same
 * stop-then-relaunch contract as the cond-select seam applies. WLS/LETP/DLSTP are
 * not emitted here and fall through to the normal invalid-instruction report.
 * ===========================================================================
 */
enum : uint32_t {
  k_lob_dls_hw1  = 0xF040U, /**< DLS lr,Rn first half-word (Rn in [3:0]). */
  k_lob_dls_h1m  = 0xFFF0U, /**< Mask isolating the fixed DLS hw1 bits.   */
  k_lob_dls_hw2  = 0xE001U, /**< DLS second half-word (fully fixed).      */
  k_lob_le_hw1   = 0xF00FU, /**< LE lr,label first half-word.             */
  k_lob_le_hw2   = 0xC000U, /**< LE second half-word fixed bits.          */
  k_lob_le_h2m   = 0xF000U, /**< Mask isolating the fixed LE hw2 bits.    */
  k_lob_le_imm10 = 0x03FFU, /**< LE offset high bits hw2[10:1] (#233).    */
  k_lob_le_lsb   = 11U,     /**< LE offset LSB scattered to hw2[11].      */
  k_lob_rn_mask  = 0xFU,    /**< Rn field (4 bits) in hw1[3:0].           */
  k_lob_insn_len = 4U,      /**< LOB instructions are 32-bit Thumb-2.     */
};
/** @brief Count of LOB instructions emulated this run (run-end telemetry). */
static uint64_t s_lob_emulated = 0U;

/** @brief Emulate a DLS/LE Low-Overhead-Branch instruction; true iff handled. */
static bool emulate_lob(uc_engine* uc, uint32_t pc, const uint8_t code[4])
{
  const uint16_t hw1 = (uint16_t)(code[0] | ((uint16_t)code[1] << (uint16_t)k_byte_bits));
  const uint16_t hw2 = (uint16_t)(code[2] | ((uint16_t)code[3] << (uint16_t)k_byte_bits));
  /* DLS lr, Rn -> LR = Rn (start the counted loop). */
  if (((hw1 & (uint16_t)k_lob_dls_h1m) == (uint16_t)k_lob_dls_hw1) &&
      (hw2 == (uint16_t)k_lob_dls_hw2)) {
    /* Map Rn via k_arm_reg_id[] -- Unicorn's UC_ARM_REG_* enum is NOT contiguous
     * (UC_ARM_REG_R0 + n != UC_ARM_REG_Rn), so the prior arithmetic read the
     * wrong register for the loop count -> bogus iteration count -> over-run and
     * a wild branch (#233). This is the same table the CSEL/long-shift seams use. */
    const uint32_t rn = (uint32_t)hw1 & (uint32_t)k_lob_rn_mask;
    uint32_t       v  = 0U;
    (void)uc_reg_read(uc, k_arm_reg_id[rn], &v);
    (void)uc_reg_write(uc, UC_ARM_REG_LR, &v);
    const uint32_t next = pc + (uint32_t)k_lob_insn_len;
    (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
    s_lob_emulated++;
    return true;
  }
  /* LE lr, label -> LR -= 1; branch back while LR != 0, else fall through. */
  if ((hw1 == (uint16_t)k_lob_le_hw1) &&
      ((hw2 & (uint16_t)k_lob_le_h2m) == (uint16_t)k_lob_le_hw2)) {
    uint32_t lr = 0U;
    (void)uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    lr -= 1U;
    (void)uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    uint32_t next = pc + (uint32_t)k_lob_insn_len;
    if (lr != 0U) {
      /* Backward branch to the loop top: the 11-bit immediate is scattered --
       * hw2[10:1] are the high bits and hw2[11] is the offset LSB (2-byte
       * granularity) -- so offset = (imm10 << 2) | (lsb << 1), taken from PC+4.
       * Verified against objdump for all 95 LE sites in the -O1 reader image
       * (#233). The previous (hw2 & 0x7FF) << 1 decode ignored the scatter and
       * mis-branched at 55 of them, running loops the wrong number of times
       * (memory corruption + a wild branch to zeros at -O1). */
      const uint32_t imm10 = ((uint32_t)hw2 >> 1U) & (uint32_t)k_lob_le_imm10;
      const uint32_t lsb   = ((uint32_t)hw2 >> (uint32_t)k_lob_le_lsb) & 1U;
      next -= ((imm10 << 2U) | (lsb << 1U)); /* backward to loop top. */
    }
    (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
    s_lob_emulated++;
    return true;
  }
  return false;
}

/**
 * @brief Decode an Armv8.1-M immediate long-shift (LSLL/LSRL/ASRL) if present.
 *
 * @details
 * Validates the two-halfword encoding (see ::long_shift_t) and, on a match,
 * extracts the destination register pair, the operation, and the shift amount
 * (imm5, where 0 encodes 32). Shared by the image scan that installs the
 * per-site hooks and by the hook that emulates one at run time.
 *
 * @param[in]  hw1   First (low-address) instruction halfword.
 * @param[in]  hw2   Second instruction halfword.
 * @param[out] rdalo Receives the low destination register index (0..12).
 * @param[out] rdahi Receives the high destination register index (0..12).
 * @param[out] op    Receives the op (::k_lsh_op_lsll / _lsrl / _asrl).
 * @param[out] shift Receives the shift amount in bits (1..32).
 * @return true if @p hw1/@p hw2 form an immediate long shift; false otherwise.
 * @pre All out-params are non-null.
 * @post On true, every out-param is written; on false, none are.
 */
static bool long_shift_decode(uint16_t  hw1,
                              uint16_t  hw2,
                              uint32_t* rdalo,
                              uint32_t* rdahi,
                              uint32_t* op,
                              uint32_t* shift)
{
  if (((hw1 & (uint16_t)k_lsh_hw1_mask) != (uint16_t)k_lsh_hw1_match) ||
      ((hw2 & (uint16_t)k_lsh_hw2_lo_mask) != (uint16_t)k_lsh_hw2_lo_match)) {
    return false;
  }
  const uint32_t o = ((uint32_t)hw2 >> (uint32_t)k_lsh_op_shift) & (uint32_t)k_lsh_op_mask;
  if (o > (uint32_t)k_lsh_op_asrl) {
    return false; /* op == 0b11 is reserved -- not a long shift. */
  }
  const uint32_t imm3 = ((uint32_t)hw2 >> (uint32_t)k_lsh_imm3_shift) & (uint32_t)k_lsh_imm3_mask;
  const uint32_t imm2 = ((uint32_t)hw2 >> (uint32_t)k_lsh_imm2_shift) & (uint32_t)k_lsh_imm2_mask;
  const uint32_t imm5 = (imm3 << (uint32_t)k_lsh_imm3_to_imm5) | imm2;
  *rdalo              = (uint32_t)hw1 & (uint32_t)k_lo4_mask;
  *rdahi              = ((uint32_t)hw2 >> (uint32_t)k_lsh_rdahi_shift) & (uint32_t)k_lo4_mask;
  *op                 = o;
  *shift              = (imm5 == 0U) ? (uint32_t)k_lsh_word_bits : imm5;
  return true;
}

/**
 * @brief UC_HOOK_CODE: emulate one immediate long-shift, then relaunch past it.
 *
 * @details
 * Reads the 4 instruction bytes at @p address, decodes the LSLL/LSRL/ASRL (see
 * ::long_shift_decode), forms the 64-bit {RdaHi:RdaLo} operand, applies the
 * shift on the host -- where 64-bit arithmetic is correct -- writes the result
 * back to the register pair, advances PC past the 4-byte instruction, and stops
 * the engine so the chunked run loop resumes at the new PC. This is the same
 * stop-and-relaunch contract the eth/usb seams use; editing PC and continuing
 * in-place corrupts Unicorn's block/Thumb state. The mis-decoding instruction is
 * therefore never executed by the core.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[in]     address Address of the long-shift instruction.
 * @param[in]     size    Reported instruction size (unused).
 * @param[in]     user    Hook user pointer (unused).
 * @return Nothing.
 * @pre @p address holds an immediate long shift (the scan installed this hook).
 * @post PC has advanced one 32-bit instruction and the engine is stopped.
 * @note Not thread-safe.
 * @since 0.1.0
 */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_long_shift(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)size;
  (void)user;
  uint8_t code[4] = {};
  if (uc_mem_read(uc, address, code, sizeof(code)) != UC_ERR_OK) {
    return;
  }
  const uint16_t hw1   = (uint16_t)(code[0] | ((uint16_t)code[1] << (uint16_t)k_byte_bits));
  const uint16_t hw2   = (uint16_t)(code[2] | ((uint16_t)code[3] << (uint16_t)k_byte_bits));
  uint32_t       rdalo = 0U;
  uint32_t       rdahi = 0U;
  uint32_t       op    = 0U;
  uint32_t       shift = 0U;
  if (!long_shift_decode(hw1, hw2, &rdalo, &rdahi, &op, &shift)) {
    return; /* scan false-positive at a never-executed address: leave it alone. */
  }
  uint32_t lo = 0U;
  uint32_t hi = 0U;
  (void)uc_reg_read(uc, k_arm_reg_id[rdalo], &lo);
  (void)uc_reg_read(uc, k_arm_reg_id[rdahi], &hi);
  const uint64_t val = ((uint64_t)hi << (uint64_t)k_lsh_word_bits) | (uint64_t)lo;
  uint64_t       res = 0U;
  switch (op) {
    case (uint32_t)k_lsh_op_lsll:
      res = val << shift;
      break;
    case (uint32_t)k_lsh_op_asrl:
      res = (uint64_t)((int64_t)val >> shift);
      break;
    case (uint32_t)k_lsh_op_lsrl:
    default:
      res = val >> shift;
      break;
  }
  lo = (uint32_t)res;
  hi = (uint32_t)(res >> (uint64_t)k_lsh_word_bits);
  (void)uc_reg_write(uc, k_arm_reg_id[rdalo], &lo);
  (void)uc_reg_write(uc, k_arm_reg_id[rdahi], &hi);
  uint32_t next = (uint32_t)address + (uint32_t)k_lsh_insn_len;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  (void)uc_emu_stop(uc); /* relaunch past the long shift (chunk contract). */
}

/** @brief Max long-shift sites hooked per image (generous; real counts tiny). */
enum : uint32_t {
  k_lsh_sites_max = 4096U,
};
/** @brief Hook handles for the installed long-shift sites (kept for the run). */
static uc_hook s_lsh_hooks[k_lsh_sites_max];

/**
 * @brief Scan the loaded image and install a hook at every immediate long-shift.
 *
 * @details
 * Walks the ELF32 PT_LOAD program headers, and for each executable segment scans
 * its bytes on 2-byte boundaries for the long-shift encoding (::long_shift_decode).
 * A targeted UC_HOOK_CODE is installed at each site's VMA so ::on_long_shift can
 * emulate it. Matches use the segment's VMA (p_vaddr), so a `.sram_text` ramfunc
 * region is hooked at its execution address even though it is not yet copied at
 * install time. A scan false-positive (a halfword pair inside data or mid-
 * instruction that happens to match) is harmless: the core never starts execution
 * there, so the hook never fires. Zero hooks -- hence zero steady-state cost --
 * for firmware that contains no long shifts.
 *
 * @param[in,out] uc  Unicorn engine to install the hooks on.
 * @param[in]     elf In-memory ELF image (still alive at call time).
 * @param[in]     len Length of @p elf in bytes.
 * @return Nothing.
 * @pre @p elf is a 32-bit ARM ELF (already validated by load_elf).
 * @post One UC_HOOK_CODE per long-shift site (up to ::k_lsh_sites_max) is armed.
 * @note Not thread-safe; call once during setup before the run loop.
 * @since 0.1.0
 */
static void long_shift_seam_install(uc_engine* uc, const uint8_t* elf, long len)
{
  if ((elf == nullptr) || (len < (long)k_elf_ehdr_size)) {
    return;
  }
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + (uint32_t)k_elf_e_phoff_off, 4);
  const uint16_t phentsize = (uint16_t)(elf[42] | (elf[43] << 8));
  const uint16_t phnum     = (uint16_t)(elf[44] | (elf[45] << 8));
  /* Bound the program-header table against the file before walking it: a
   * malformed e_phoff/e_phnum would otherwise OOB-read past `elf`. load_elf has
   * already validated the ELF/ARM magic, so this only guards a truncated image. */
  if (((uint64_t)phoff + ((uint64_t)phnum * (uint64_t)phentsize)) > (uint64_t)len) {
    return;
  }
  uint32_t n_hooks = 0U;
  for (uint16_t i = 0U; i < phnum; i++) {
    const uint8_t* ph       = elf + phoff + ((uint32_t)i * phentsize);
    uint32_t       p_type   = 0U;
    uint32_t       p_offset = 0U;
    uint32_t       p_vaddr  = 0U;
    uint32_t       p_filesz = 0U;
    uint32_t       p_flags  = 0U;
    (void)memcpy(&p_type, ph + 0, 4);
    (void)memcpy(&p_offset, ph + (uint32_t)k_elf_ph_offset_off, 4);
    (void)memcpy(&p_vaddr, ph + (uint32_t)k_elf_ph_vaddr_off, 4);
    (void)memcpy(&p_filesz, ph + (uint32_t)k_elf_ph_filesz_off, 4);
    (void)memcpy(&p_flags, ph + (uint32_t)k_elf_ph_flags_off, 4);
    if ((p_type != (uint32_t)k_elf_pt_load) || (p_filesz < (uint32_t)k_lsh_insn_len) ||
        ((p_flags & (uint32_t)k_elf_pf_x) == 0U)) {
      continue;
    }
    if (((uint64_t)p_offset + (uint64_t)p_filesz) > (uint64_t)len) {
      continue; /* truncated / out-of-file segment -- skip. */
    }
    for (uint32_t off = 0U; (off + (uint32_t)k_lsh_insn_len) <= p_filesz; off += 2U) {
      const uint8_t* p     = elf + p_offset + off;
      const uint16_t hw1   = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
      const uint16_t hw2   = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
      uint32_t       rdalo = 0U;
      uint32_t       rdahi = 0U;
      uint32_t       op    = 0U;
      uint32_t       shift = 0U;
      if (!long_shift_decode(hw1, hw2, &rdalo, &rdahi, &op, &shift)) {
        continue;
      }
      if (n_hooks >= (uint32_t)k_lsh_sites_max) {
        (void)fprintf(stderr,
                      "  long-shift seam: site cap %u reached\n",
                      (unsigned)k_lsh_sites_max);
        return;
      }
      const uint64_t va = (uint64_t)p_vaddr + (uint64_t)off;
      (void)
        uc_hook_add(uc, &s_lsh_hooks[n_hooks], UC_HOOK_CODE, (void*)on_long_shift, nullptr, va, va);
      n_hooks++;
    }
  }
  if (n_hooks > 0U) {
    (void)fprintf(stderr,
                  "  long-shift seam: emulating %u Armv8.1-M LSLL/LSRL/ASRL site(s)\n",
                  (unsigned)n_hooks);
  }
}

/** @brief Nanoseconds per second (timespec tv_nsec -> seconds). */
static const double s_nsec_per_sec = 1.0e9;

/** @brief Monotonic wall-clock seconds. */
static double board_now_s(void)
{
  struct timespec ts = {};
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ((double)ts.tv_nsec / s_nsec_per_sec);
}

/* ===========================================================================
 * Firmware profiler (BOARD_SIM_PROFILE). Two modes, bucketed by ELF FUNC symbol:
 *   =1     wall-time sample -- charge each chunk's wall time to its start PC.
 *          Cheap (no per-instruction cost); a flat % list of the dominant cost.
 *   =full  per-instruction -- a code hook tallies every instruction + call entry
 *          AND reconstructs the live call chain (see below), so the run end emits
 *          an Ozone-style breakdown: a boot timeline, an inclusive/self table,
 *          and a speedscope flamechart file. Accurate but ~10x slower; off by
 *          default. The run auto-stops once boot settles into the idle frame
 *          loop, so =full profiles boot work rather than the idle tail.
 * ===========================================================================
 */
enum : uint32_t {
  k_prof_max_syms = 8192U, /**< Cap on profiled FUNC symbols.      */
  k_prof_top_n    = 40U,   /**< Top entries printed in the report. */
};
/** @enum prof_mode_t @brief Profiler mode parsed from BOARD_SIM_PROFILE. */
typedef enum : uint8_t {
  k_prof_off  = 0U, /**< Disabled (no env, zero cost).               */
  k_prof_wall = 1U, /**< =1: cheap chunk-start wall-time sampler.    */
  k_prof_insn = 2U, /**< =full/=insn: exact per-instruction + calls. */
} prof_mode_t;
typedef struct {
  uint32_t    lo;    /**< Function entry (Thumb bit cleared). */
  uint32_t    hi;    /**< Function end (lo + st_size).        */
  const char* name;  /**< Pointer into the ELF string table.  */
  double      secs;  /**< Wall seconds (wall mode).           */
  uint64_t    insns; /**< Instructions executed (insn mode).  */
  uint64_t    calls; /**< Entries to this fn (insn mode).     */
} prof_sym_t;
static prof_sym_t  s_prof[k_prof_max_syms];
static uint32_t    s_prof_n       = 0U;
static double      s_prof_total_s = 0.0;
static uint64_t    s_prof_total_i = 0U;
static prof_mode_t s_prof_mode    = k_prof_off;

/* ---------------------------------------------------------------------------
 * Ozone-style call-stack tracing (insn mode only). On top of the per-function
 * tally above, reconstruct the live call chain straight from the PC stream: a
 * fresh function entry that is not already on the chain is a call (push), and
 * re-entering a function already deeper on the chain is a return (pop down to
 * it). NASA Rule 1 bans recursion in this firmware, so a function appears at
 * most once on the chain and the "already on the chain" test is unambiguous.
 * The chain is sampled at a fixed instruction cadence into a bounded,
 * chronological store and written out as a speedscope "sampled" profile -- open
 * board_sim_profile.speedscope.json at https://speedscope.app for the
 * time-ordered flamechart ("what ran when", the Ozone timeline) plus the
 * sandwich view (self vs total per function). WFI idle naturally weighs ~zero
 * because a halted core retires no instructions, so the picture is boot work,
 * not the idle frame loop. ===============================================
 */
enum : uint32_t {
  k_prof_max_depth   = 64U,    /**< Deepest call chain captured per sample.  */
  k_prof_max_samples = 16384U, /**< Chronological stack samples (decimated). */
  k_prof_samp_every  = 256U,   /**< Default instructions per chain sample.   */
};
static uint16_t s_pstk[k_prof_max_depth];                      /**< Live chain.                */
static uint32_t s_pstk_n = 0U;                                 /**< Chain depth.               */
static uint16_t s_samp[k_prof_max_samples][k_prof_max_depth];  /**< root..leaf.                */
static uint8_t  s_samp_d[k_prof_max_samples];                  /**< Per-sample chain depth.    */
static uint32_t s_samp_w[k_prof_max_samples];                  /**< Per-sample weight (insns). */
static uint32_t s_samp_n        = 0U;                          /**< Stored sample count.       */
static uint64_t s_samp_every    = (uint64_t)k_prof_samp_every; /**< Insns per sample (>>x2).   */
static uint64_t s_samp_acc      = 0U;                          /**< Insns since last sample.   */
static uint32_t s_prof_stop_pc  = 0U;                          /**< BOARD_SIM_STOP_PC (0=off). */
static bool     s_prof_stop_hit = false;                       /**< Set when STOP_PC reached.  */
static uint64_t s_incl[k_prof_max_syms];                       /**< Inclusive weight (report). */
static uint64_t s_self[k_prof_max_syms];                       /**< Self (leaf) weight.        */

/** @brief qsort comparator: order the FUNC symbols by entry address. */
static int prof_cmp(const void* a, const void* b)
{
  const uint32_t la = ((const prof_sym_t*)a)->lo;
  const uint32_t lb = ((const prof_sym_t*)b)->lo;
  return (la < lb) ? -1 : ((la > lb) ? 1 : 0);
}

/** @brief Collect + sort FUNC symbols (BOARD_SIM_PROFILE only) for PC bucketing. */
static void prof_load(const uint8_t* elf, long len)
{
  const char* mode = getenv("BOARD_SIM_PROFILE");
  if (mode == nullptr) {
    s_prof_mode = k_prof_off;
    return;
  }
  s_prof_mode =
    ((strcmp(mode, "full") == 0) || (strcmp(mode, "insn") == 0)) ? k_prof_insn : k_prof_wall;
  if (len < (long)k_elf_ehdr_size) {
    return;
  }
  uint32_t shoff = 0U;
  (void)memcpy(&shoff, elf + 32, 4);
  const uint16_t shentsize = (uint16_t)(elf[46] | (elf[47] << 8));
  const uint16_t shnum     = (uint16_t)(elf[48] | (elf[49] << 8));
  if (shoff == 0U) {
    return;
  }
  for (uint16_t i = 0U; i < shnum; i++) {
    const uint8_t* sh      = elf + shoff + ((uint32_t)i * shentsize);
    uint32_t       sh_type = 0U;
    (void)memcpy(&sh_type, sh + 4, 4);
    if (sh_type != 2U) { /* SHT_SYMTAB */
      continue;
    }
    uint32_t sym_off = 0U, sym_size = 0U, sym_link = 0U, sym_entsize = 0U;
    (void)memcpy(&sym_off, sh + 16, 4);
    (void)memcpy(&sym_size, sh + (uint32_t)k_elf_sh_size_off, 4);
    (void)memcpy(&sym_link, sh + (uint32_t)k_elf_sh_link_off, 4);
    (void)memcpy(&sym_entsize, sh + (uint32_t)k_elf_sh_entsize_off, 4);
    if ((sym_entsize < 16U) || (sym_link >= shnum)) {
      continue;
    }
    const uint8_t* strsh   = elf + shoff + ((uint32_t)sym_link * shentsize);
    uint32_t       str_off = 0U;
    (void)memcpy(&str_off, strsh + 16, 4);
    const uint32_t nsym = sym_size / sym_entsize;
    for (uint32_t s = 0U; (s < nsym) && (s_prof_n < (uint32_t)k_prof_max_syms); s++) {
      const uint8_t* sym     = elf + sym_off + (s * sym_entsize);
      uint32_t       st_name = 0U, st_value = 0U, st_size = 0U;
      (void)memcpy(&st_name, sym + 0, 4);
      (void)memcpy(&st_value, sym + 4, 4);
      (void)memcpy(&st_size, sym + 8, 4);
      if (((sym[k_elf_sym_info_off] & (uint8_t)k_elf_st_type_mask) != 2U) || (st_size == 0U) ||
          (st_name == 0U)) { /* STT_FUNC */
        continue;
      }
      s_prof[s_prof_n].lo    = st_value & ~1U;
      s_prof[s_prof_n].hi    = (st_value & ~1U) + st_size;
      s_prof[s_prof_n].name  = (const char*)(elf + str_off + st_name);
      s_prof[s_prof_n].secs  = 0.0;
      s_prof[s_prof_n].insns = 0U;
      s_prof[s_prof_n].calls = 0U;
      s_prof_n++;
    }
  }
  qsort(s_prof, (size_t)s_prof_n, sizeof(s_prof[0]), prof_cmp);
  (void)fprintf(stderr,
                "  [profile] %s; %u FUNC symbols\n",
                (s_prof_mode == k_prof_insn) ? "per-instruction (exact, slow)" : "wall-time sample",
                (unsigned)s_prof_n);
}

/** @brief Binary-search the FUNC symbol owning @p pc; returns s_prof_n if none. */
static uint32_t prof_find(uint32_t pc)
{
  uint32_t lo = 0U;
  uint32_t hi = s_prof_n;
  while (lo < hi) {
    const uint32_t mid = lo + ((hi - lo) / 2U);
    if (s_prof[mid].lo <= pc) {
      lo = mid + 1U;
    } else {
      hi = mid;
    }
  }
  if (lo == 0U) {
    return s_prof_n;
  }
  const uint32_t idx = lo - 1U;
  return (pc < s_prof[idx].hi) ? idx : s_prof_n;
}

/** @brief Attribute @p dt wall seconds to @p pc's function (wall-sample mode). */
static void prof_add(uint32_t pc, double dt)
{
  if (s_prof_mode != k_prof_wall) {
    return;
  }
  s_prof_total_s += dt;
  const uint32_t idx = prof_find(pc);
  if (idx < s_prof_n) {
    s_prof[idx].secs += dt;
  }
}

/** @brief Halve the sample store (merge adjacent pairs) when it fills up. */
static void prof_decimate(void)
{
  uint32_t dst = 0U;
  for (uint32_t i = 0U; i < s_samp_n; i += 2U) {
    const uint32_t w2 = ((i + 1U) < s_samp_n) ? s_samp_w[i + 1U] : 0U;
    if (dst != i) {
      (void)memcpy(s_samp[dst], s_samp[i], (size_t)s_samp_d[i] * sizeof(uint16_t));
      s_samp_d[dst] = s_samp_d[i];
    }
    s_samp_w[dst] = s_samp_w[i] + w2; /* merged time keeps the total exact. */
    dst++;
  }
  s_samp_n = dst;
  s_samp_every *= 2U; /* coarser cadence keeps the next fill the same span. */
}

/** @brief Append the live call chain as one chronological sample of @p weight insns. */
static void prof_sample(uint32_t weight)
{
  if (s_samp_n >= (uint32_t)k_prof_max_samples) {
    prof_decimate();
  }
  uint32_t d = s_pstk_n;
  if (d > (uint32_t)k_prof_max_depth) {
    d = (uint32_t)k_prof_max_depth;
  }
  for (uint32_t i = 0U; i < d; i++) {
    s_samp[s_samp_n][i] = s_pstk[i];
  }
  s_samp_d[s_samp_n] = (uint8_t)d;
  s_samp_w[s_samp_n] = weight;
  s_samp_n++;
}

/** @brief Fold @p f (PC's owning FUNC index) into the live call chain (push/pop). */
static void prof_stack_update(uint32_t f)
{
  if (f >= s_prof_n) {
    return; /* unknown region -- keep the current leaf (it gets the self time). */
  }
  if ((s_pstk_n > 0U) && (s_pstk[s_pstk_n - 1U] == (uint16_t)f)) {
    return; /* still in the same function -- no call/return transition. */
  }
  for (uint32_t i = s_pstk_n; i > 0U; i--) {
    if (s_pstk[i - 1U] == (uint16_t)f) {
      s_pstk_n = i; /* returned to a frame already on the chain -- unwind to it. */
      return;
    }
  }
  if (s_pstk_n < (uint32_t)k_prof_max_depth) {
    s_pstk[s_pstk_n] = (uint16_t)f; /* a fresh call -- push it. */
    s_pstk_n++;
  }
}

/** @brief UC_HOOK_CODE per instruction: tally instructions + calls + call chain. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void prof_insn_hook(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)size;
  (void)user;
  s_prof_total_i++;
  const uint32_t idx = prof_find((uint32_t)address);
  if (idx < s_prof_n) {
    s_prof[idx].insns++;
    if ((uint32_t)address == s_prof[idx].lo) {
      s_prof[idx].calls++; /* PC at the entry point -> a fresh call (approx). */
    }
  }
  prof_stack_update(idx);
  s_samp_acc++;
  if (s_samp_acc >= s_samp_every) {
    prof_sample((uint32_t)s_samp_acc);
    s_samp_acc = 0U;
  }
  if ((s_prof_stop_pc != 0U) && ((uint32_t)address == s_prof_stop_pc)) {
    s_prof_stop_hit = true; /* BOARD_SIM_STOP_PC reached -- end the run cleanly. */
    (void)uc_emu_stop(uc);
  }
}

/** @brief Write the captured call chains as a speedscope "sampled" profile JSON. */
static void prof_write_speedscope(const char* path)
{
  if ((s_samp_n == 0U) || (s_prof_n == 0U)) {
    return;
  }
  FILE* f = fopen(path, "w");
  if (f == nullptr) {
    return;
  }
  uint64_t total = 0U;
  for (uint32_t i = 0U; i < s_samp_n; i++) {
    total += s_samp_w[i];
  }
  (void)fprintf(f,
                "{\"$schema\":\"https://www.speedscope.app/file-format-schema.json\",\n"
                " \"name\":\"board_sim boot\",\"activeProfileIndex\":0,\n"
                " \"shared\":{\"frames\":[");
  for (uint32_t i = 0U; i < s_prof_n; i++) {
    (void)fputs((i == 0U) ? "{\"name\":\"" : ",{\"name\":\"", f);
    for (const char* p = s_prof[i].name; *p != '\0'; p++) { /* JSON-escape. */
      if ((*p == '"') || (*p == '\\')) {
        (void)fputc('\\', f);
      }
      (void)fputc(*p, f);
    }
    (void)fputs("\"}", f);
  }
  (void)fprintf(f,
                "]},\n"
                " \"profiles\":[{\"type\":\"sampled\",\"name\":\"boot\",\"unit\":\"none\",\n"
                "  \"startValue\":0,\"endValue\":%llu,\n"
                "  \"samples\":[",
                (unsigned long long)total);
  for (uint32_t i = 0U; i < s_samp_n; i++) {
    (void)fputc((i == 0U) ? '[' : ',', f);
    if (i != 0U) {
      (void)fputc('[', f);
    }
    for (uint8_t j = 0U; j < s_samp_d[i]; j++) {
      (void)fprintf(f, (j == 0U) ? "%u" : ",%u", (unsigned)s_samp[i][j]);
    }
    (void)fputc(']', f);
  }
  (void)fputs("],\n  \"weights\":[", f);
  for (uint32_t i = 0U; i < s_samp_n; i++) {
    (void)fprintf(f, (i == 0U) ? "%u" : ",%u", (unsigned)s_samp_w[i]);
  }
  (void)fputs("]}]}\n", f);
  (void)fclose(f);
}

/* Self-contained flamechart viewer markup. The page embeds the profile arrays
 * (written just before this) and renders a time-ordered flame chart on a canvas
 * -- the Ozone timeline, but as a local file that opens in any browser with no
 * upload and no external site. Single-quoted HTML/JS strings keep the C literal
 * free of escapes; pure 7-bit ASCII. */
static const char k_prof_html_head[] =
  "<!doctype html><html><head><meta charset='utf-8'><title>board_sim profile</title>\n"
  "<style>\n"
  "body{margin:0;font:12px Menlo,monospace;background:#1e1e1e;color:#ddd}\n"
  "#bar{padding:7px 10px;background:#2a2a2a;border-bottom:1px solid #444}\n"
  "#bar b{color:#fff}#bar button,#bar input{font:11px monospace;margin-left:10px;"
  "background:#3a3a3a;color:#ddd;border:1px solid #555;padding:2px 6px}\n"
  "#tip{position:fixed;pointer-events:none;background:#000;color:#fff;padding:5px 7px;"
  "border:1px solid #888;display:none;white-space:nowrap;z-index:9;font:11px monospace}\n"
  "canvas{display:block;cursor:crosshair}\n"
  "</style></head><body>\n"
  "<div id='bar'><b id='title'></b><span id='info'></span>"
  "<button onclick='resetView()'>Reset zoom</button>"
  "search:<input id='q' size='18' oninput='onSearch()'></div>\n"
  "<canvas id='fc'></canvas><div id='tip'></div>\n<script>\n";

static const char k_prof_html_js[] =
  "var cv=document.getElementById('fc'),ctx=cv.getContext('2d'),tip=document.getElementById('tip');\n"
  "document.getElementById('title').textContent=TITLE;\n"
  "var ROW=18,total=0,i;for(i=0;i<WEIGHTS.length;i++)total+=WEIGHTS[i];\n"
  "var maxd=0;for(i=0;i<SAMPLES.length;i++)if(SAMPLES[i].length>maxd)maxd=SAMPLES[i].length;\n"
  "var rects=[],incl={},self={};\n"
  "for(var d=0;d<maxd;d++){var cum=0,rs=0,rf=-1,op=false;\n"
  " for(i=0;i<SAMPLES.length;i++){var ff=d<SAMPLES[i].length?SAMPLES[i][d]:-1;\n"
  "  if(!(op&&ff===rf)){if(op&&rf>=0)rects.push({d:d,a:rs,b:cum,f:rf});rs=cum;rf=ff;op=true;}\n"
  "  cum+=WEIGHTS[i];}\n"
  " if(op&&rf>=0)rects.push({d:d,a:rs,b:cum,f:rf});}\n"
  "for(i=0;i<SAMPLES.length;i++){var s=SAMPLES[i],w=WEIGHTS[i];\n"
  " for(var j=0;j<s.length;j++)incl[s[j]]=(incl[s[j]]||0)+w;\n"
  " if(s.length)self[s[s.length-1]]=(self[s[s.length-1]]||0)+w;}\n"
  "var vx0=0,vx1=total,q='';\n"
  "function col(fi){var n=FRAMES[fi],h=0,k;for(k=0;k<n.length;k++)h=(h*31+n.charCodeAt(k))&0xffffff;\n"
  " var lit=(q&&n.toLowerCase().indexOf(q)>=0);return 'hsl('+(h%359)+','+(lit?'90%':'48%')+','+(lit?'62%':'44%')+')';}\n"
  "function resize(){cv.width=window.innerWidth;cv.height=Math.max(maxd*ROW+4,160);draw();}\n"
  "function draw(){ctx.clearRect(0,0,cv.width,cv.height);var span=vx1-vx0;if(span<=0)return;\n"
  " ctx.font='11px monospace';ctx.textBaseline='middle';\n"
  " for(var r=0;r<rects.length;r++){var R=rects[r];if(R.b<=vx0||R.a>=vx1)continue;\n"
  "  var p0=(R.a-vx0)/span*cv.width,p1=(R.b-vx0)/span*cv.width,w=p1-p0;if(w<0.4)continue;\n"
  "  var y=R.d*ROW;ctx.fillStyle=col(R.f);ctx.fillRect(p0,y,Math.max(w-0.6,0.5),ROW-1);\n"
  "  if(w>34){ctx.fillStyle='#111';ctx.fillText(FRAMES[R.f],p0+3,y+ROW/2,w-6);}}}\n"
  "function pick(mx,my){var d=Math.floor(my/ROW),span=vx1-vx0,wx=vx0+mx/cv.width*span,r;\n"
  " for(r=0;r<rects.length;r++){var R=rects[r];if(R.d===d&&wx>=R.a&&wx<R.b)return R;}return null;}\n"
  "cv.onmousemove=function(e){var R=pick(e.offsetX,e.offsetY);if(!R){tip.style.display='none';return;}\n"
  " var n=FRAMES[R.f],to=incl[R.f]||0,se=self[R.f]||0;\n"
  " tip.innerHTML=n+'<br>this block: '+((R.b-R.a)/total*100).toFixed(2)+'% ('+(R.b-R.a)+' insns)'+\n"
  "  '<br>total '+(to/total*100).toFixed(2)+'%  self '+(se/total*100).toFixed(2)+'%';\n"
  " tip.style.display='block';tip.style.left=(e.clientX+14)+'px';tip.style.top=(e.clientY+14)+'px';};\n"
  "cv.onmouseleave=function(){tip.style.display='none';};\n"
  "cv.onclick=function(e){var R=pick(e.offsetX,e.offsetY);if(R){vx0=R.a;vx1=R.b;draw();}};\n"
  "function resetView(){vx0=0;vx1=total;draw();}\n"
  "function onSearch(){q=document.getElementById('q').value.toLowerCase();draw();}\n"
  "document.getElementById('info').textContent=' | '+SAMPLES.length+' samples, '+total+\n"
  "  ' insns  (hover for self/total, click a block to zoom, Reset to zoom out)';\n"
  "window.onresize=resize;resize();\n";

/** @brief Write a self-contained, locally-openable HTML flamechart of the samples. */
static void prof_write_html(const char* path, uint64_t total)
{
  if ((s_samp_n == 0U) || (s_prof_n == 0U)) {
    return;
  }
  FILE* f = fopen(path, "w");
  if (f == nullptr) {
    return;
  }
  (void)fputs(k_prof_html_head, f);
  (void)fputs("var FRAMES=[", f);
  for (uint32_t i = 0U; i < s_prof_n; i++) {
    (void)fputs((i == 0U) ? "'" : ",'", f);
    for (const char* p = s_prof[i].name; *p != '\0'; p++) { /* escape ' and backslash for JS. */
      if ((*p == '\'') || (*p == '\\')) {
        (void)fputc('\\', f);
      }
      (void)fputc(*p, f);
    }
    (void)fputc('\'', f);
  }
  (void)fputs("];\n", f);
  (void)fputs("var SAMPLES=[", f);
  for (uint32_t i = 0U; i < s_samp_n; i++) {
    (void)fputc((i == 0U) ? '[' : ',', f);
    if (i != 0U) {
      (void)fputc('[', f);
    }
    for (uint8_t j = 0U; j < s_samp_d[i]; j++) {
      (void)fprintf(f, (j == 0U) ? "%u" : ",%u", (unsigned)s_samp[i][j]);
    }
    (void)fputc(']', f);
  }
  (void)fputs("];\n", f);
  (void)fputs("var WEIGHTS=[", f);
  for (uint32_t i = 0U; i < s_samp_n; i++) {
    (void)fprintf(f, (i == 0U) ? "%u" : ",%u", (unsigned)s_samp_w[i]);
  }
  (void)fputs("];\n", f);
  (void)fprintf(f,
                "var TITLE='board_sim flamechart -- %llu insns, %u samples';\n",
                (unsigned long long)total,
                (unsigned)s_samp_n);
  (void)fputs(k_prof_html_js, f);
  (void)fputs("</script></body></html>\n", f);
  (void)fclose(f);
}

/** @brief Speedscope export + inclusive/self breakdown + phase timeline (insn mode). */
/** @brief Fraction-to-per-cent scale (fraction * 100.0 == per-cent). */
static const double s_percent_scale = 100.0;

static void prof_report_flamechart(void)
{
  enum : uint32_t {
    k_no_fn        = 0xFFFFFFFFU, /**< Sentinel for "no phase frame".        */
    k_phase_depth  = 2U,          /**< Chain depth used as the boot "phase". */
    k_phase_lines  = 24U,         /**< Cap on printed timeline segments.     */
    k_phase_pct_x1 = 100U,        /**< Per-cent base: keep segments >= 1%.   */
  };
  const char* out = getenv("BOARD_SIM_PROFILE_OUT");
  if ((out == nullptr) || (out[0] == '\0')) {
    out = "board_sim_profile.speedscope.json";
  }
  const char* html = getenv("BOARD_SIM_PROFILE_HTML");
  if ((html == nullptr) || (html[0] == '\0')) {
    html = "board_sim_profile.html";
  }
  prof_write_speedscope(out);

  /* Inclusive (anywhere on the chain) + self (leaf) weights, from the samples.
   * No recursion (NASA Rule 1) -> each function appears at most once per sample,
   * so a straight per-frame add needs no dedup. */
  uint64_t total = 0U;
  for (uint32_t i = 0U; i < s_prof_n; i++) {
    s_incl[i] = 0U;
    s_self[i] = 0U;
  }
  for (uint32_t i = 0U; i < s_samp_n; i++) {
    const uint32_t w = s_samp_w[i];
    const uint8_t  d = s_samp_d[i];
    total += w;
    for (uint8_t j = 0U; j < d; j++) {
      s_incl[s_samp[i][j]] += w;
    }
    if (d > 0U) {
      s_self[s_samp[i][d - 1U]] += w;
    }
  }
  if (total == 0U) {
    return;
  }
  prof_write_html(html, total); /* self-contained local GUI flamechart. */

  /* Phase timeline: collapse each sample's chain to a fixed shallow depth (the
   * major subsystem under main) and print each contiguous run as a boot phase,
   * so the terminal shows "what ran when" even without opening speedscope. */
  (void)fprintf(stderr,
                "  [profile] boot timeline (phase = call depth %u; start%% .. width%%):\n",
                (unsigned)k_phase_depth);
  uint64_t cum   = 0U;
  uint64_t segw  = 0U;
  uint32_t segfn = (uint32_t)k_no_fn;
  uint32_t lines = 0U;
  for (uint32_t i = 0U; i <= s_samp_n; i++) {
    uint32_t fn = (uint32_t)k_no_fn;
    if (i < s_samp_n) {
      const uint8_t d = s_samp_d[i];
      if (d > 0U) {
        const uint8_t pd = ((uint32_t)(d - 1U) < (uint32_t)k_phase_depth) ? (uint8_t)(d - 1U)
                                                                          : (uint8_t)k_phase_depth;
        fn               = s_samp[i][pd];
      }
    }
    if ((i == s_samp_n) || (fn != segfn)) {
      const bool show = (segfn != (uint32_t)k_no_fn) &&
                        (((uint32_t)k_phase_pct_x1 * segw) >= total) &&
                        (lines < (uint32_t)k_phase_lines);
      if (show) {
        (void)fprintf(stderr,
                      "    %5.1f%%  +%4.1f%%  %s\n",
                      s_percent_scale * (double)cum / (double)total,
                      s_percent_scale * (double)segw / (double)total,
                      (segfn < s_prof_n) ? s_prof[segfn].name : "?");
        lines++;
      }
      cum += segw;
      segw  = 0U;
      segfn = fn;
    }
    if (i < s_samp_n) {
      segw += s_samp_w[i];
    }
  }

  /* Inclusive/self table -- the "why is it slow" view (sorted by inclusive). */
  (void)fprintf(stderr,
                "  [profile] flamechart GUI -> %s  (interactive: hover/zoom/search)\n"
                "  [profile] %u samples over %llu insns  (also %s for speedscope.app)\n"
                "       self%%    total%%  function\n",
                html,
                (unsigned)s_samp_n,
                (unsigned long long)total,
                out);
  for (uint32_t k = 0U; k < (uint32_t)k_prof_top_n; k++) {
    uint32_t best  = s_prof_n;
    uint64_t bestv = 0U;
    for (uint32_t i = 0U; i < s_prof_n; i++) {
      if (s_incl[i] > bestv) {
        bestv = s_incl[i];
        best  = i;
      }
    }
    if ((best == s_prof_n) || (bestv == 0U)) {
      break;
    }
    (void)fprintf(stderr,
                  "    %8.2f%%  %7.2f%%  %s\n",
                  s_percent_scale * (double)s_self[best] / (double)total,
                  s_percent_scale * (double)s_incl[best] / (double)total,
                  s_prof[best].name);
    s_incl[best] = 0U; /* consume so the next pick is the runner-up. */
  }
}

/** @brief Print the top hot functions (by wall time or instruction count) at run end. */
static void prof_report(void)
{
  const bool   insn = (s_prof_mode == k_prof_insn);
  const double tot  = insn ? (double)s_prof_total_i : s_prof_total_s;
  if ((s_prof_mode == k_prof_off) || (tot <= 0.0)) {
    return;
  }
  if (insn) {
    (void)fprintf(stderr,
                  "  [profile] %llu instructions; hottest (by instruction count):\n"
                  "     %%insn       instructions       calls  function\n",
                  (unsigned long long)s_prof_total_i);
  } else {
    (void)fprintf(stderr, "  [profile] %.2fs wall sampled; hottest (by wall time):\n", tot);
  }
  for (uint32_t k = 0U; k < (uint32_t)k_prof_top_n; k++) {
    uint32_t best  = s_prof_n;
    double   bestv = 0.0;
    for (uint32_t i = 0U; i < s_prof_n; i++) {
      const double v = insn ? (double)s_prof[i].insns : s_prof[i].secs;
      if (v > bestv) {
        bestv = v;
        best  = i;
      }
    }
    if ((best == s_prof_n) || (bestv <= 0.0)) {
      break;
    }
    if (insn) {
      (void)fprintf(stderr,
                    "    %6.2f%%  %15llu  %10llu  %s\n",
                    s_percent_scale * bestv / tot,
                    (unsigned long long)s_prof[best].insns,
                    (unsigned long long)s_prof[best].calls,
                    s_prof[best].name);
      s_prof[best].insns = 0U;
    } else {
      (void)fprintf(stderr,
                    "    %6.2f%%  %8.2fs  %s\n",
                    s_percent_scale * bestv / tot,
                    bestv,
                    s_prof[best].name);
      s_prof[best].secs = 0.0;
    }
  }
  if (insn && (s_samp_n > 0U)) {
    prof_report_flamechart(); /* speedscope export + inclusive/self + timeline. */
  }
}

/** @brief Disassemble + report an instruction the core could not decode. */
static bool on_invalid_insn(uc_engine* uc, void* user)
{
  (void)user;
  uint32_t pc = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PC, &pc);
  uint8_t code[4] = {};
  (void)uc_mem_read(uc, pc, code, sizeof(code));

  /* Divide-by-zero trap: a UDIV/SDIV overwritten with UDF once the firmware set
   * CCR.DIV_0_TRP. emulate_div0_patched either latches a UsageFault (zero divisor)
   * or emulates the divide and advances PC; either way stop + relaunch. Checked
   * first: it matches only the exact patched addresses, never a real UDF. */
  if (emulate_div0_patched(uc, pc, code)) {
    (void)uc_emu_stop(uc);
    return true;
  }

  /* Armv8-M Security Extension register scrub (CLRM / VSCCLRM) on a cmse
   * Non-Secure-Callable return: a NOP in board_sim's single-domain model. */
  if (emulate_sec_scrub(uc, pc, code)) {
    (void)uc_emu_stop(uc);
    return true;
  }

  /* The RA8D2 firmware is built for Cortex-M85 (Armv8.1-M); the nearest core
   * Unicorn offers is M33 (Armv8-M), which lacks the conditional-select family.
   * GCC emits those for branchless index math on the touch path, so
   * execute them here. emulate_cond_select writes Rd and advances PC past the
   * 4-byte instruction; then uc_emu_stop so the chunked run loop relaunches from
   * the new PC -- editing PC and continuing in-place corrupts Unicorn's block /
   * Thumb state (it then misdecodes the next valid instruction), so the
   * stop-then-relaunch contract the SysTick / touch stubs use is required here. */
  if (emulate_cond_select(uc, pc, code)) {
    (void)uc_emu_stop(uc);
    return true; /* handled -- run loop resumes at the advanced PC */
  }

  /* Older Unicorn builds (the runner's 2.0.1) trap DSB/DMB/ISB as invalid; a
   * barrier is a NOP in this emulator, so advance past it and relaunch. */
  if (emulate_barrier(uc, pc, code)) {
    (void)uc_emu_stop(uc);
    return true; /* handled -- run loop resumes past the barrier */
  }

  /* MVE (Helium): the M85 has it, Unicorn's M33 does not, so the auto-vectoriser's
   * Helium ops trap here. Emulate the handled subset and relaunch past it. */
  if (emulate_mve(uc, pc, code)) {
    (void)uc_emu_stop(uc);
    return true; /* handled -- run loop resumes past the MVE instruction */
  }

  /* Low-Overhead-Branch (DLS/LE): the M85's hardware-loop ops, absent on the M33.
   * Emulate the loop counter / branch and relaunch. */
  if (emulate_lob(uc, pc, code)) {
    (void)uc_emu_stop(uc);
    return true; /* handled -- run loop resumes at the loop top or past the loop */
  }

  (void)fprintf(stderr,
                "  INVALID INSN @ 0x%08X: bytes %02X %02X %02X %02X\n",
                pc,
                code[0],
                code[1],
                code[2],
                code[3]);

  csh cs;
  if (cs_open(CS_ARCH_ARM, (cs_mode)(CS_MODE_THUMB | CS_MODE_MCLASS), &cs) == CS_ERR_OK) {
    cs_insn*     insn = nullptr;
    const size_t n    = cs_disasm(cs, code, sizeof(code), pc, 1, &insn);
    if (n > 0U) {
      (void)fprintf(stderr, "  disasm: %s %s\n", insn[0].mnemonic, insn[0].op_str);
      cs_free(insn, n);
    } else {
      (void)fprintf(stderr, "  disasm: capstone could not decode it either\n");
    }
    cs_close(&cs);
  }
  return false; /* not handled -> stop emulation with UC_ERR_INSN_INVALID */
}

/** @brief Hook fired on access to unmapped memory (peripheral surface gap). */
static bool
on_unmapped(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* user)
{
  (void)size;
  (void)value;
  (void)user;
  /* A FETCH into the EXC_RETURN range is not a fault -- it is the core taking
   * an exception return (the handler ran "BX lr" with an EXC_RETURN magic in
   * LR). Capture it and stop cleanly so the run loop can unstack the frame and
   * resume; the Unicorn fetch-fault error this produces is expected/handled. */
  if ((type == UC_MEM_FETCH_UNMAPPED) && is_exc_return(addr)) {
    s_exc_return_pc  = addr;
    s_exc_return_hit = true;
    (void)uc_emu_stop(uc);
    return false;
  }
  (void)fprintf(stderr,
                "  UNMAPPED %s @ 0x%08llX (extend the memory/peripheral map)\n",
                (type == UC_MEM_READ_UNMAPPED)    ? "read"
                : (type == UC_MEM_WRITE_UNMAPPED) ? "write"
                                                  : "fetch",
                (unsigned long long)addr);
  return false; /* stop emulation and report */
}

/**
 * @brief UC_HOOK_INTR handler: take the SVCall exception on an `svc` opcode.
 *
 * @details
 * Unicorn raises UC_HOOK_INTR when the firmware executes the Thumb `svc`
 * instruction but, lacking an exception unit, does not vector it. This models
 * SVCall (#11): the basic frame is stacked and the core vectors to SVC_Handler
 * via ::exc_enter, then emulation is stopped so the chunked run loop relaunches
 * cleanly from the handler entry (editing PC mid-block and continuing corrupts
 * Unicorn's block/Thumb state -- the same stop-then-relaunch contract the touch
 * and conditional-select stubs use). ThreadX in single-mode never issues an
 * SVC, but bare-metal / future RTOS paths that start the first thread via `svc`
 * are handled correctly here. PRIMASK does not mask SVCall (it is synchronous),
 * matching hardware.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     int_no    Interrupt/exception number reported by Unicorn.
 * @param[in]     user_data Hook user pointer (unused; signature fixed by Unicorn).
 * @return Nothing.
 *
 * @pre @p uc has just executed an `svc` instruction or branched to EXC_RETURN.
 * @pre The vector table (at VTOR or the MRAM fallback) holds SVC_Handler.
 * @post Either an exception was taken/returned (PC updated) or, on a missing
 *       SVC handler, the core is left untouched.
 * @post Emulation is stopped so the run loop resumes from the new PC.
 * @note Only the SVC interrupt class is acted on; other int_no values are
 *       ignored so unrelated traps fall through.
 * @since 0.1.0
 */
static void on_intr(uc_engine* uc, uint32_t int_no, void* user_data)
{
  (void)int_no;
  uint32_t pc = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PC, &pc);

  /* Cortex-M exception RETURN. Unicorn's M-profile core does not pop the
   * exception frame itself, but it DOES trap a branch to an EXC_RETURN magic
   * (0xFFFFFFFx) by raising an interrupt with the magic left in PC (Thumb bit
   * masked off, but the stack/mode selector bits 2/3 intact). Unstack the basic
   * frame and resume the interrupted context. This is how every handler that
   * board_sim vectors in (SysTick / PendSV / SVCall) returns. */
  if (is_exc_return((uint64_t)pc)) {
    s_exc_return_pc  = (uint64_t)pc;
    s_exc_return_hit = true;
    (void)uc_emu_stop(uc);
    return;
  }

  /* Armv8-M secure gateway: every Non-Secure-Callable veneer starts with `SG`
   * (0xE97FE97F) then `B.W __acle_se_<fn>`. Unicorn's M33 has no Security
   * Extension, so it raises INTR on the unrecognised SG instead of switching to
   * Secure state. board_sim has a single flat address space (no S/NS split), so
   * the faithful model is to treat SG as a NOP and let the following branch reach
   * the secure entry directly. Without this the SG is mis-taken as an `svc` and
   * re-taken forever (the firmware has no SVC), looping until the stack
   * underflows -- the tz_nsc_cgc_usb fault. The matching `BXNS`/`BLXNS` returns
   * are handled below. */
  uint32_t insn = 0U;
  (void)uc_mem_read(uc, (uint64_t)pc, &insn, sizeof(insn));

  /* Armv8-M Security Extension instructions Unicorn's M33 does not implement.
   * board_sim is a single flat (no Secure/Non-Secure split) address space, so
   * these reduce to their plain effects here:
   *   - `SG` (secure gateway, 32-bit): a NOP -- the following B.W reaches the
   *     __acle_se_ entry directly.
   *   - `VSTR FPCXTNS,[sp,#-4]!` / `VLDR FPCXTNS,[sp],#4`: the FP context across
   *     the security boundary is meaningless with one FP bank, so model only the
   *     stack push/pop they perform (keeping SP balanced for the C frame).
   * Without this the unrecognised opcode is mis-taken as an `svc`, vectors to
   * Default_Handler's bkpt, and re-traps forever until the stack underflows
   * (the tz_nsc_cgc_usb fault). */
  if (insn == (uint32_t)k_armv8m_sg_opcode) {
    const uint32_t next = pc + (uint32_t)k_thumb2_insn_bytes;
    (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
    (void)uc_emu_stop(uc);
    return;
  }
  if ((insn == (uint32_t)k_fpcxtns_push) || (insn == (uint32_t)k_fpcxtns_pop)) {
    uint32_t sp = 0U;
    (void)uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    if (insn == (uint32_t)k_fpcxtns_push) {
      sp -= (uint32_t)k_word_bytes;
      const uint32_t zero = 0U;
      (void)uc_mem_write(uc, (uint64_t)sp, &zero, sizeof(zero));
    } else {
      sp += (uint32_t)k_word_bytes;
    }
    const uint32_t next = pc + (uint32_t)k_thumb2_insn_bytes;
    (void)uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
    (void)uc_emu_stop(uc);
    return;
  }

  /* A `BKPT` (0xBExx) is a deliberate firmware trap -- Default_Handler's
   * `bkpt #0`, a failed assert, or a fault give-up. It is NOT an `svc`: taking
   * SVCall here would stack a frame, vector to the (often Default_Handler) SVC
   * slot, return to the same bkpt, and re-trap forever -- the stack grows until
   * it underflows (the historical tz_nsc_cgc_usb storm). Model it as a halt:
   * record the site and stop so the run loop ends and the report shows where the
   * firmware trapped. */
  if (((uint16_t)(insn & (uint32_t)k_lo16_mask) & (uint16_t)k_bkpt_hw_mask) ==
      (uint16_t)k_bkpt_hw_base) {
    s_bkpt_hit = true;
    s_bkpt_pc  = pc;
    (void)uc_emu_stop(uc);
    return;
  }

  /* Otherwise it is a synchronous `svc` -- take SVCall (#11). ThreadX in single
   * mode never issues one, but bare-metal / future RTOS first-thread-start
   * paths do. The VTOR fallback is the MRAM vector-table base; the live VTOR
   * (set by SystemInit) is read inside exc_vector. */
  (void)user_data;
  const uint32_t vtor_base = (uint32_t)k_regions[1].base;
  const uint32_t handler   = exc_vector(uc, vtor_base, (uint32_t)k_exc_svcall);
  if (handler != 0U) {
    exc_enter(uc, (uint32_t)k_exc_svcall, handler);
    s_svc_takes++;
  }
  (void)uc_emu_stop(uc);
}

/**
 * @brief UC_HOOK_MEM_WRITE handler for SCB ICSR -- take PendSV promptly.
 *
 * @details
 * On hardware, writing ICSR.PENDSVSET pends PendSV, and -- because ThreadX
 * follows the store with DSB+ISB and PendSV is enabled at a priority above
 * thread level with interrupts unmasked -- the exception activates at the very
 * next instruction. board_sim runs the CPU in long chunks, so without help it
 * would only notice PENDSVSET at the end of a 500k-instruction chunk; by then
 * the requesting thread (e.g. a thread suspending inside tx_queue_receive) has
 * run far past the point where it expected to be switched out and observes
 * inconsistent scheduler state. Stopping the chunk the instant PENDSVSET is
 * written hands control straight back to the run loop, which takes PendSV at
 * that boundary -- restoring next-instruction activation semantics. The store
 * itself has already landed in PPB RAM, so exc_take_pending sees the bit set.
 *
 * @param[in,out] uc    Unicorn engine.
 * @param[in]     type  Memory access type (write); unused.
 * @param[in]     addr  Faulting/observed address (the ICSR word).
 * @param[in]     size  Access width in bytes; unused.
 * @param[in]     value The value being written to ICSR.
 * @param[in]     user  Hook user pointer; unused.
 * @return Nothing.
 *
 * @pre @p uc is mid-chunk executing the store to ICSR.
 * @pre The hook is registered for the 4-byte ICSR word only.
 * @post Emulation is stopped iff the write sets PENDSVSET.
 * @post The PENDSVSET bit is left in PPB RAM for exc_take_pending to read.
 * @note PENDSVCLR / status-only writes do not stop the chunk.
 * @since 0.1.0
 */
static void
on_icsr_write(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* user)
{
  (void)type;
  (void)addr;
  (void)size;
  (void)user;
  if (((uint32_t)value & (1U << (uint32_t)k_icsr_pendsvset)) == 0U) {
    return; /* not a PendSV request (e.g. PENDSVCLR / status write) */
  }
  /* Only end the chunk when PendSV could actually activate now: interrupts
   * unmasked and no equal/higher-priority handler already running. If masked
   * (the store sits inside a ThreadX critical section), do NOT stop -- the run
   * loop would otherwise relaunch from this same store and re-pend forever,
   * since uc_emu_stop here leaves PC on the store. With those guards, the run
   * loop's exc_take_pending takes PendSV and moves PC to the handler, so the
   * store is never re-executed. The masked case is picked up at the next
   * boundary once TX_RESTORE re-enables interrupts. */
  uint32_t primask = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PRIMASK, &primask);
  if ((primask & 1U) != 0U) {
    return;
  }
  if (exc_priority(uc, (uint32_t)k_exc_pendsv) >= exc_active_prio()) {
    return; /* a higher/equal-priority handler is active -- defer */
  }
  /* Advance PC past the storing instruction before stopping so the PendSV we
   * are about to take stacks the return address of the NEXT instruction -- as
   * real hardware does (PendSV activates after the store retires), not the store
   * itself. Re-stacking the store would re-pend PendSV on every return and spin.
   * Thumb length: a halfword whose top 5 bits are 0b111xx with xx != 00 starts a
   * 32-bit instruction; otherwise it is 16-bit. */
  uint32_t pc = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PC, &pc);
  uint16_t hw0 = 0U;
  (void)uc_mem_read(uc, pc, &hw0, sizeof(hw0));
  const uint32_t op5  = (uint32_t)(hw0 >> (uint32_t)k_thumb_op5_shift) & (uint32_t)k_thumb_op5_mask;
  const uint32_t step = (op5 >= (uint32_t)k_thumb32_op5_min) ? 4U : 2U;
  uint32_t       next = pc + step;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  /* Mark this as a context-switch stop so the run loop takes PendSV WITHOUT
   * advancing the SysTick: a context switch consumes no modelled time, so a
   * thread that suspends (e.g. tx_thread_sleep) must not have its tick-based
   * wait expired the instant it yields -- that would starve lower-priority
   * ready threads. Time (SysTick) advances only on a full-budget run (genuine
   * execution / idle spin). See the run loop's inner dispatch. */
  s_pendsv_stop = true;
  (void)uc_emu_stop(uc);
}

/* ===========================================================================
 * ITM (Instrumented Trace Macrocell) echo -- surface ra8_log in the emulator.
 *
 * `[itm] ...` lines are board_sim's echo of the Arm CoreSight ITM stimulus
 * port 0. It is the direct analog of the `[uart] SCI8:` echo: where that
 * surfaces the firmware's UART console, this surfaces ra8_log's debug trace --
 * the same bytes that on real hardware leave through the ITM/SWO pin to the
 * J-Link SWO console. So `[itm]` == "what you would see on the SWO trace
 * console", and `[uart] SCI8:` == "what you would see on the serial console".
 *
 * ra8_log writes log bytes to ITM stimulus port 0 (0xE0000000) after checking
 * DEMCR.TRCENA + ITM TCR/TENR + a non-zero STIM0 "FIFO ready" read. With no
 * debugger attached those PPB bytes are all zero, so internal_itm_ready() returns
 * false and every byte is dropped -- which is why the e-reader (and any ra8_log
 * user) prints nothing in board_sim. We seed the ready bits into PPB RAM at boot
 * (itm_seed_ready: sets DEMCR.TRCENA + TCR.ITMENA + TENR port 0 + a ready STIM0)
 * and hook stimulus-port writes (on_itm_stim_write) to echo the bytes as
 * `[itm] <line>` on stdout. This surfaces ra8_log for every app, not just the
 * TrustZone e-reader.
 * ===========================================================================
 */
typedef enum : uint64_t {
  k_itm_stim0_addr = 0xE0000000UL, /**< ITM stimulus port 0 (byte FIFO).         */
  k_itm_tcr_addr   = 0xE0000E80UL, /**< ITM Trace Control Register (ITMENA).     */
  k_itm_tenr_addr  = 0xE0000E00UL, /**< ITM Trace Enable Register (port 0).      */
  k_scb_demcr_addr = 0xE000EDFCUL, /**< Debug Exception + Monitor Control.       */
  k_sau_type_addr  = 0xE000EDD4UL, /**< SAU_TYPE (SREGION = implemented regs).   */
  k_ns_sram2_base  = 0x32100000UL, /**< SRAM2 Non-secure alias (bit[28]=1).      */
  k_ns_alias_bit   = 0x10000000UL, /**< IDAU bit[28]: NS alias of a Secure addr. */
} itm_addr_t;

/**
 * @brief Fallback NS vector-table base for the BLXNS world switch.
 * @details ::on_blxns first reads the live VTOR_NS word (the Secure boot's
 *          ``ra8_tz_secure_boot_jump_ns`` stores the NS vector base to the
 *          0xE002ED08 alias -- plain PPB RAM here -- right before its BLXNS),
 *          so a single-image TZ app whose NS half lives at its MRAM LMA
 *          (cpu1_pingpong_ipc: 0x02080000) and a two-image app whose NS image
 *          was copied to the SRAM2 run alias both resolve without flags. This
 *          fallback covers a zero VTOR_NS: it defaults to the RAM-resident NS
 *          run alias (@ref k_ns_sram2_base) and is overridden at `--ns` load
 *          to the loaded NS image's minimum PT_LOAD p_vaddr, so an XIP NS
 *          image linked at the OSPI window (0x90000000, VMA == LMA, no copy)
 *          still transitions correctly.
 * @note Single-threaded; set once before the run loop and read in ::on_blxns.
 * @since 0.1.0
 */
static uint32_t s_ns_vector_base = (uint32_t)k_ns_sram2_base;

/** @brief SCB VTOR_NS alias address (ARMv8-M B3.2.4; Secure-state view). */
typedef enum : uint64_t {
  k_scb_vtor_ns_addr = 0xE002ED08UL, /**< VTOR_NS: NS vector-table base. */
} vtor_ns_addr_t;

typedef enum : uint32_t {
  k_itm_line_max     = 240U,        /**< Max chars buffered before a forced flush. */
  k_itm_tcr_itmena   = 0x00000001U, /**< TCR bit 0: ITM global enable.             */
  k_itm_tenr_port0   = 0x00000001U, /**< TENR bit 0: stimulus port 0 enabled.      */
  k_itm_stim_ready   = 0x00000001U, /**< Non-zero STIM0 read = FIFO ready.         */
  k_scb_demcr_trcena = 0x01000000U, /**< DEMCR bit 24: trace subsystem enable.     */
  k_sau_type_regs    = 0x00000008U, /**< SAU_TYPE.SREGION: M85 implements 8.       */
} itm_bits_t;

/** @brief Accumulated current ITM line (flushed on newline or when full). */
static char s_itm_line[k_itm_line_max + 1U];

/** @brief Bytes currently buffered in ::s_itm_line. */
static uint32_t s_itm_len;

/**
 * @brief UC_HOOK_MEM_WRITE handler for ITM stimulus port 0 -- echo the byte.
 *
 * @details Buffers the low byte of each stimulus write and prints `[itm] <line>`
 *          to stdout on a newline (or when the line buffer fills), so ra8_log
 *          output is visible in the emulator. Carriage returns are dropped so
 *          the `\r\n` ra8_log line ending yields one clean line.
 *
 * @param[in] uc    Unicorn engine (unused; the byte rides in @p value).
 * @param[in] type  Memory access type (write); unused.
 * @param[in] addr  Observed address (the stimulus port); unused.
 * @param[in] size  Access width in bytes; unused.
 * @param[in] value The value being written; its low byte is the log character.
 * @param[in] user  Hook user pointer; unused.
 * @return Nothing.
 *
 * @pre The hook is registered for the 4-byte STIM0 word only.
 * @pre @p value holds the character ra8_log is emitting.
 * @post On a newline the buffered line is printed and the buffer reset.
 * @post Non-newline printable bytes are appended to @ref s_itm_line.
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
static void on_itm_stim_write(uc_engine*  uc,
                              uc_mem_type type,
                              uint64_t    addr,
                              int         size,
                              int64_t     value,
                              void*       user)
{
  (void)uc;
  (void)type;
  (void)addr;
  (void)size;
  (void)user;
  const char c = (char)((uint32_t)value & 0xFFU); /* MAGIC-OK: low-byte mask */
  if (c == '\r') {
    return;
  }
  if ((c == '\n') || (s_itm_len >= (uint32_t)k_itm_line_max)) {
    s_itm_line[s_itm_len] = '\0';
    /* Emit one ra8_log line as `[itm] <line>` -- the CoreSight ITM/SWO-trace
     * analog of the `[uart] SCI8:` console echo (see the ITM model block above). */
    (void)fprintf(stdout, "[itm] %s\n", s_itm_line);
    (void)fflush(stdout);
    /* Also route it to the board_console ITM channel so the tabbed board-view
     * console shows ITM/SWO trace in its own tab (a different endpoint than the
     * UART line). The stdout echo above is unchanged -- this is purely additive. */
    board_console_push(k_board_console_ch_itm, s_itm_line);
    s_itm_len = 0U;
    if (c == '\n') {
      return;
    }
  }
  s_itm_line[s_itm_len] = c;
  s_itm_len++;
}

/**
 * @brief Seed the ITM "ready" bits into PPB RAM so ra8_log emits.
 *
 * @details On hardware a debugger sets DEMCR.TRCENA and enables the ITM; with
 *          none attached those PPB registers read zero and ra8_log drops every
 *          byte. board_sim maps the PPB as plain RAM, so writing the enable bits
 *          here makes internal_itm_ready() see a live ITM. The firmware only ever
 *          reads these registers (it never re-disables the ITM), so the seed
 *          persists for the run.
 *
 * @param[in] uc Initialised Unicorn engine with the PPB region mapped.
 * @return Nothing.
 *
 * @pre @p uc has the PPB region (0xE0000000) mapped as RAM.
 * @pre Called once before emulation starts.
 * @post DEMCR.TRCENA, ITM TCR.ITMENA, TENR port-0, and a ready STIM0 are set.
 * @post ra8_log's internal_itm_ready() returns true for the run.
 * @note Not thread-safe; call during single-threaded setup.
 * @since 0.1.0
 */
static void itm_seed_ready(uc_engine* uc)
{
  const uint32_t demcr = (uint32_t)k_scb_demcr_trcena;
  const uint32_t tcr   = (uint32_t)k_itm_tcr_itmena;
  const uint32_t tenr  = (uint32_t)k_itm_tenr_port0;
  const uint32_t stim  = (uint32_t)k_itm_stim_ready;
  (void)uc_mem_write(uc, (uint64_t)k_scb_demcr_addr, &demcr, sizeof(demcr));
  (void)uc_mem_write(uc, (uint64_t)k_itm_tcr_addr, &tcr, sizeof(tcr));
  (void)uc_mem_write(uc, (uint64_t)k_itm_tenr_addr, &tenr, sizeof(tenr));
  (void)uc_mem_write(uc, (uint64_t)k_itm_stim0_addr, &stim, sizeof(stim));
}

/**
 * @enum blxns_op_t
 * @brief Thumb encoding of the BLXNS instruction (scanned in jump_ns).
 * @details BLXNS Rm = 0x4780 | (Rm << 3) | 0x04; masking with k_blxns_mask
 *          isolates the fixed bits (0x4784) so any Rm matches.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_blxns_mask  = 0xFF87U, /**< Mask isolating the BLXNS fixed bits. */
  k_blxns_match = 0x4784U, /**< BLXNS fixed-bit pattern (any Rm).    */
} blxns_op_t;

/** @brief Width of one Thumb halfword, for the BLXNS instruction scan. */
typedef enum : uint32_t {
  k_thumb_hw_bytes = 2U, /**< Bytes per Thumb halfword. */
} blxns_scan_t;

/**
 * @brief UC_HOOK_CODE at the Secure->NS BLXNS -- hand-emulate the world switch.
 *
 * @details Unicorn's emulated M33 is all-Secure with no IDAU, so the real BLXNS
 *          in ra8_tz_secure_boot_jump_ns cannot transition to the Non-Secure
 *          world (it stalls / wanders). This hook fires on that instruction and
 *          performs the switch by hand in board_sim's single flat domain: it
 *          reads the NS initial MSP (NS vector[0]) and the NS reset handler (NS
 *          vector[1]) from the NS run base, sets SP + PC to them (Thumb bit
 *          masked), and stops the chunk so the run loop resumes executing the NS
 *          reset handler -- ThreadX and the e-reader threads then run directly.
 *          Mirrors the existing SG-stub-by-address TrustZone workaround.
 *
 * @param[in] uc      Unicorn engine mid-chunk at the BLXNS.
 * @param[in] address The BLXNS instruction address; unused.
 * @param[in] size    Instruction size in bytes; unused.
 * @param[in] user    Hook user pointer; unused.
 * @return Nothing.
 *
 * @pre The NS vectors are live at @ref s_ns_vector_base -- either copied to the
 *      RAM run alias by the Secure boot, or XIP-resident in the OSPI window.
 * @pre The hook is registered only for the jump_ns BLXNS site (under --ns).
 * @post SP = NS MSP, PC = NS reset handler, and the chunk is stopped.
 * @post The next run-loop chunk executes the Non-Secure reset handler.
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
static void on_blxns(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  /* Prefer the live VTOR_NS: ra8_tz_secure_boot_jump_ns stores the NS vector
   * base to the 0xE002ED08 alias (plain PPB RAM here) right before its BLXNS,
   * so this resolves the NS table wherever the app placed it (MRAM-resident
   * 0x02080000, SRAM2 run alias 0x32100000, or OSPI XIP) with no per-app
   * knowledge. Fall back to ::s_ns_vector_base when the app never wrote it. */
  uint32_t vector_base = 0U;
  (void)uc_mem_read(uc, (uint64_t)k_scb_vtor_ns_addr, &vector_base, sizeof(vector_base));
  if (vector_base == 0U) {
    vector_base = s_ns_vector_base;
  }
  uint32_t ns_msp   = 0U;
  uint32_t ns_reset = 0U;
  (void)uc_mem_read(uc, (uint64_t)vector_base, &ns_msp, sizeof(ns_msp));
  (void)uc_mem_read(uc,
                    (uint64_t)vector_base + (uint64_t)sizeof(uint32_t),
                    &ns_reset,
                    sizeof(ns_reset));
  const uint32_t ns_pc = ns_reset & ~1U; /* mask the Thumb bit for the PC write */
  (void)uc_reg_write(uc, UC_ARM_REG_SP, &ns_msp);
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &ns_pc);
  (void)uc_emu_stop(uc);
}

/**
 * @brief UC_HOOK_MEM_WRITE handler for the SCB control words AIRCR and CCR.
 *
 * @details
 * One write hook spans the SCB control block from AIRCR (0xE000ED0C) through CCR
 * (0xE000ED14) and dispatches strictly by word address, so the two nearby control
 * registers share a single hook rather than each adding its own -- this Unicorn
 * build consults every installed memory hook per access, so folding CCR in here
 * keeps a hot read loop paying the baseline hook count. Intervening words (VTOR,
 * SCR) fall through untouched.
 *
 * - **AIRCR**: writing SYSRESETREQ (with the mandatory 0x05FA key in the upper
 *   half-word) asks the chip to reset. The emulator honours it as a warm reboot:
 *   record the request and stop the chunk; the run loop's reboot wrapper re-runs
 *   the firmware from its reset vector (latching RSTSR1.SWRF). Without this,
 *   ra8_reset_software_reset would spin forever waiting for a reset that never came.
 * - **CCR**: writing DIV_0_TRP arms the divide-by-zero UsageFault by overwriting
 *   the tracked divide sites with UDF (::div0_patch_sites) the instant the firmware
 *   opts in, race-free even when the arming write and the divide share one chunk.
 *
 * @param[in,out] uc    Unicorn engine (stopped to end the chunk on a reset).
 * @param[in]     type  Unused memory-event type.
 * @param[in]     addr  Observed SCB address (dispatched by word).
 * @param[in]     size  Access width (unused).
 * @param[in]     value Value written.
 * @param[in]     user  Unused hook context.
 * @return Nothing.
 * @since 0.1.0
 */
static void on_scb_ctrl_write(uc_engine*  uc,
                              uc_mem_type type,
                              uint64_t    addr,
                              int         size,
                              int64_t     value,
                              void*       user)
{
  (void)type;
  (void)size;
  (void)user;
  const uint32_t word = (uint32_t)addr & ~3U;
  if (word == (uint32_t)k_scb_ccr) {
    if (((uint32_t)value & (uint32_t)k_ccr_div_0_trp) != 0U) {
      div0_patch_sites(uc); /* firmware opted in: overwrite the divides with UDF. */
    }
    return;
  }
  if (word != (uint32_t)k_scb_aircr) {
    return; /* an intervening SCB word (VTOR / SCR): not ours. */
  }
  if (((uint32_t)value & (1U << (uint32_t)k_aircr_sysresetreq)) == 0U) {
    return; /* a non-reset AIRCR write (e.g. priority grouping) */
  }
  s_reboot_request = true;
  (void)uc_emu_stop(uc); /* end the chunk; the run loop performs the reboot */
}

/**
 * @brief UC_HOOK_MEM_WRITE handler for the NVIC ISER / ICER arrays.
 *
 * @details
 * The NVIC set-enable (ISER) and clear-enable (ICER) registers are not normal
 * read/write words: a written 1 sets (ISER) or clears (ICER) that interrupt
 * line and a written 0 has no effect, so independent stores accumulate. The PPB
 * is mapped as plain RAM here, so the raw store would overwrite the whole word
 * and drop every other enabled line -- which breaks any firmware that enables
 * more than one line (e.g. the SCI RXI + TXI + TEI of the interrupt-driven UART
 * path, or several USB controller lines later). This hook decodes the written
 * bits and folds them into board_periph's authoritative enable shadow, which the
 * ICU model consults when deciding whether to pend a line. The raw RAM word is
 * left as-is (nothing reads ISER/ICER back on the modelled paths).
 *
 * @param[in,out] uc    Unicorn engine; unused (state lives in board_periph).
 * @param[in]     type  Memory access type (write); unused.
 * @param[in]     addr  The ISER/ICER word being written.
 * @param[in]     size  Access width in bytes; unused.
 * @param[in]     value The bit-mask the firmware is setting/clearing.
 * @param[in]     user  Hook user pointer; unused.
 * @return Nothing.
 * @since 0.1.0
 */
static void on_nvic_en_write(uc_engine*  uc,
                             uc_mem_type type,
                             uint64_t    addr,
                             int         size,
                             int64_t     value,
                             void*       user)
{
  (void)uc;
  (void)type;
  (void)size;
  (void)user;
  const bool     is_set = (addr >= (uint64_t)k_nvic_iser_base) &&
                          (addr < ((uint64_t)k_nvic_iser_base + (uint64_t)k_nvic_en_span));
  const uint64_t base   = is_set ? (uint64_t)k_nvic_iser_base : (uint64_t)k_nvic_icer_base;
  const uint32_t word   = (uint32_t)((addr - base) / 4U);
  const uint32_t bits   = (uint32_t)value;
  for (uint32_t b = 0U; b < 32U; b++) {
    if ((bits & (1U << b)) != 0U) {
      board_periph_nvic_set_enable((word * 32U) + b, is_set);
    }
  }
}

/**
 * @brief UC_HOOK_MEM_WRITE handler for a write into an enforced RO MPU region.
 *
 * @details
 * Installed (only while the MPU is enabled) on the exact [base, limit] range
 * of each read-only region. A privileged store into that range is an MPU
 * permission violation; record the faulting PC + address and stop the chunk
 * so the run loop can synthesise a MemManage exception at this boundary --
 * Unicorn's core models no MPU, so this is the software stand-in.
 *
 * @param[in,out] uc    Unicorn engine.
 * @param[in]     type  Access type (write); unused.
 * @param[in]     addr  Address being written (the violating address).
 * @param[in]     size  Access width; unused.
 * @param[in]     value Value being written; unused.
 * @param[in]     user  Hook user pointer; unused.
 * @return Nothing.
 *
 * @pre The MPU is enabled and @p addr lies in a read-only region.
 * @pre The PPB CFSR / MMFAR words are mapped as RAM.
 * @post ::s_mpu_fault is set with the PC / address latched; emulation stopped.
 * @post At most one pending MPU fault is tracked at a time.
 * @note PC is captured here (the store instruction) so the run loop stacks it.
 * @since 0.1.0
 */
static void
on_mpu_ro_write(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* user)
{
  (void)type;
  (void)size;
  (void)value;
  (void)user;
  if (s_mpu_fault) {
    return; /* one pending fault is enough; the run loop takes it next */
  }
  s_mpu_fault      = true;
  s_mpu_fault_addr = (uint32_t)addr;
  s_mpu_fault_pc   = reg_get(uc, UC_ARM_REG_PC);
  (void)uc_emu_stop(uc);
}

/**
 * @brief Install a write hook on every enabled, read-only MPU region.
 *
 * @details
 * Called when the firmware sets MPU_CTRL.ENABLE. Memory hooks are evaluated
 * at access time, so a violating store anywhere in a region's [base, limit]
 * range traps via ::on_mpu_ro_write.
 *
 * @param[in,out] uc Unicorn engine.
 * @return Nothing.
 *
 * @pre The per-region shadow ::s_mpu_region has been captured at RLAR writes.
 * @pre No RO hooks are currently installed.
 * @post ::s_mpu_ro_hook_n hooks are installed (one per RO region).
 * @post Non-RO / disabled regions are left unhooked (writable).
 * @note RW and background-region accesses are never hooked, so cost is nil
 *       for apps that do not lock down a region.
 * @since 0.1.0
 */
static void mpu_install_ro_hooks(uc_engine* uc)
{
  s_mpu_ro_hook_n = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_mpu_max_regions; i++) {
    if (!s_mpu_region[i].en || !s_mpu_region[i].ro) {
      continue;
    }
    (void)uc_hook_add(uc,
                      &s_mpu_ro_hook[s_mpu_ro_hook_n],
                      UC_HOOK_MEM_WRITE,
                      (void*)on_mpu_ro_write,
                      nullptr,
                      (uint64_t)s_mpu_region[i].base,
                      (uint64_t)s_mpu_region[i].limit);
    s_mpu_ro_hook_n++;
  }
}

/**
 * @brief Remove every installed RO-region write hook.
 *
 * @details Called when the firmware clears MPU_CTRL.ENABLE, restoring plain
 *          read/write access to the formerly protected ranges.
 *
 * @param[in,out] uc Unicorn engine.
 * @return Nothing.
 *
 * @pre ::s_mpu_ro_hook[0 .. s_mpu_ro_hook_n) hold live hook handles.
 * @pre The engine is not mid-callback for one of those hooks.
 * @post All RO hooks are deleted and ::s_mpu_ro_hook_n is 0.
 * @post Subsequent writes to the ranges no longer trap.
 * @note Idempotent when no hooks are installed.
 * @since 0.1.0
 */
static void mpu_remove_ro_hooks(uc_engine* uc)
{
  for (uint32_t i = 0U; i < s_mpu_ro_hook_n; i++) {
    (void)uc_hook_del(uc, s_mpu_ro_hook[i]);
  }
  s_mpu_ro_hook_n = 0U;
}

/**
 * @brief UC_HOOK_MEM_WRITE handler for MPU_RLAR -- capture one region.
 *
 * @details
 * ra8_mpu programs each region as RNR -> RBAR -> RLAR, so at the RLAR write the
 * region's RNR and RBAR already sit in the (flat-RAM) PPB; read them back and
 * record base / limit / permission / enable into the per-region shadow. The
 * flat PPB has no banked per-region storage, so this capture-at-RLAR is how
 * board_sim reconstructs the table the real MPU keeps internally.
 *
 * @param[in,out] uc    Unicorn engine.
 * @param[in]     type  Access type (write); unused.
 * @param[in]     addr  Observed address (MPU_RLAR); unused.
 * @param[in]     size  Access width; unused.
 * @param[in]     value The RLAR value being written.
 * @param[in]     user  Hook user pointer; unused.
 * @return Nothing.
 *
 * @pre RNR + RBAR for this region were written before this RLAR store.
 * @pre The PPB RNR / RBAR words are mapped as RAM.
 * @post ::s_mpu_region[RNR] mirrors the region (en=0 for an RLAR=0 disable).
 * @post No enforcement changes until MPU_CTRL.ENABLE is (re)written.
 * @note AP[2]=1 in RBAR marks the region read-only (no privileged store).
 * @since 0.1.0
 */
static void on_mpu_rlar_write(uc_engine*  uc,
                              uc_mem_type type,
                              uint64_t    addr,
                              int         size,
                              int64_t     value,
                              void*       user)
{
  (void)type;
  (void)addr;
  (void)size;
  (void)user;
  const uint32_t rnr      = rd32(uc, (uint64_t)k_mpu_rnr) & (uint32_t)k_mpu_rnr_mask;
  const uint32_t rbar     = rd32(uc, (uint64_t)k_mpu_rbar);
  const uint32_t rlar     = (uint32_t)value;
  s_mpu_region[rnr].base  = rbar & (uint32_t)k_mpu_addr_mask;
  s_mpu_region[rnr].limit = (rlar & (uint32_t)k_mpu_addr_mask) | (uint32_t)k_mpu_region_low_mask;
  s_mpu_region[rnr].ro    = (rbar & (uint32_t)k_mpu_rbar_ap_ro_bit) != 0U;
  s_mpu_region[rnr].en    = (rlar & (uint32_t)k_mpu_rlar_en_bit) != 0U;
}

/**
 * @brief UC_HOOK_MEM_WRITE handler for MPU_CTRL -- arm / disarm enforcement.
 *
 * @details
 * On the ENABLE edge, install write hooks over every read-only region; on the
 * disable edge, remove them. Edge-tracked via ::s_mpu_enabled so repeated
 * writes of the same state are no-ops.
 *
 * @param[in,out] uc    Unicorn engine.
 * @param[in]     type  Access type (write); unused.
 * @param[in]     addr  Observed address (MPU_CTRL); unused.
 * @param[in]     size  Access width; unused.
 * @param[in]     value The CTRL value being written.
 * @param[in]     user  Hook user pointer; unused.
 * @return Nothing.
 *
 * @pre The per-region shadow has been captured for the regions in use.
 * @pre The engine permits uc_hook_add / uc_hook_del from a callback.
 * @post Enforcement hooks match the new ENABLE state.
 * @post ::s_mpu_enabled tracks the latest ENABLE bit.
 * @note Apps that never enable the MPU install no hooks (zero overhead).
 * @since 0.1.0
 */
static void on_mpu_ctrl_write(uc_engine*  uc,
                              uc_mem_type type,
                              uint64_t    addr,
                              int         size,
                              int64_t     value,
                              void*       user)
{
  (void)type;
  (void)addr;
  (void)size;
  (void)user;
  const bool enable = ((uint32_t)value & (uint32_t)k_mpu_ctrl_enable_bit) != 0U;
  if (enable && !s_mpu_enabled) {
    mpu_install_ro_hooks(uc);
    s_mpu_enabled = true;
  } else if (!enable && s_mpu_enabled) {
    mpu_remove_ro_hooks(uc);
    s_mpu_enabled = false;
  }
}

/**
 * @brief Seed the on-chip temperature-sensor factory calibration into memory.
 *
 * @details
 * Writes the two-point trim words TSCDR / TSCDR2 (see ::tsn_cal_seed_t) into the
 * mapped MRAM factory-trim page at ::k_tsn_cal_addr. On silicon these cells are
 * factory-programmed; the emulator's map is blank, so without this seed
 * ra8_tsn_convert_to_milli_c reads an unmapped address and the run bus-faults.
 * Must be called after the memory regions (including "TRIM") are mapped.
 *
 * @param[in,out] uc Unicorn engine whose "TRIM" page receives the seed.
 * @return Nothing.
 * @pre The TRIM region covering ::k_tsn_cal_addr has been mem-mapped on @p uc.
 * @post @p uc holds TSCDR at ::k_tsn_cal_addr and TSCDR2 at the next word.
 * @since 0.1.0
 */
static void seed_tsn_calibration(uc_engine* uc)
{
  const uint32_t tscdr  = (uint32_t)k_tsn_cal_tscdr;
  const uint32_t tscdr2 = (uint32_t)k_tsn_cal_tscdr2;
  (void)uc_mem_write(uc, (uint64_t)k_tsn_cal_addr, &tscdr, sizeof(tscdr));
  (void)uc_mem_write(uc, (uint64_t)k_tsn_cal_addr + sizeof(tscdr), &tscdr2, sizeof(tscdr2));
}

static uint8_t* read_file(const char* path, long* out_len)
{
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return nullptr;
  }
  (void)fseek(f, 0, SEEK_END);
  const long len = ftell(f);
  (void)fseek(f, 0, SEEK_SET);
  uint8_t* buf = (uint8_t*)malloc((size_t)len);
  if ((buf != nullptr) && (fread(buf, 1U, (size_t)len, f) != (size_t)len)) {
    free(buf);
    buf = nullptr;
  }
  (void)fclose(f);
  *out_len = len;
  return buf;
}

/* =============================================================================
 * Function-entry seam helpers -- emulate "return <r0> to LR" and hook one ELF
 * symbol's entry to a C callback. Shared by the USB host-mode and virtual-
 * keyboard seams below. The Ethernet frame seam that first introduced them was
 * retired once board_periph_eth modelled the R-Switch (ESWM/ETHA/RMAC/GWCA)
 * registers, so the genuine ra8_eth driver now runs the descriptor DMA path.
 * =============================================================================
 */

/** @brief Resolve a symbol from the ELF .symtab (defined below load_elf). */
static uint32_t elf_sym_addr(const uint8_t* elf, long len, const char* name, uint32_t* size_out);

/** @brief Emulate "return r0;" from a hooked function: set R0, branch to LR. */
static void eth_hook_return(uc_engine* uc, uint32_t r0)
{
  uint32_t lr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_LR, &lr);
  (void)uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  uint32_t pc = lr & ~1U; /* drop the Thumb bit; the M-class core stays Thumb. */
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
  (void)uc_emu_stop(uc); /* relaunch from the returned PC (chunk contract). */
}

/** @brief Hook one symbol (if present) to @p cb; record it for the report. */
static void eth_seam_hook(uc_engine* uc, const uint8_t* elf, long len, const char* name, void* cb)
{
  const uint32_t addr = elf_sym_addr(elf, len, name, nullptr);
  if (addr == 0U) {
    return;
  }
  static uc_hook  handles[24];
  static uint32_t n;
  if (n < (uint32_t)(sizeof(handles) / sizeof(handles[0]))) {
    (void)uc_hook_add(uc, &handles[n], UC_HOOK_CODE, cb, nullptr, addr, addr);
    n++;
  }
}

/**
 * @brief Generic `--trace-sym` hook: log the first time control reaches a named
 *        symbol, plus the calling LR, so a stuck bring-up sequence is visible.
 *
 * @details Installed by ::sym_trace_install over each `--trace-sym <name>`. The
 * UC_HOOK_CODE fires on every execution of the symbol's entry address; we print
 * the entry address and the link register (return address) so a poll loop that
 * re-enters, or a thread that reaches step N but never N+1, is obvious in the
 * log without a full instruction trace.
 *
 * @param[in] uc      Active Unicorn engine (read for LR).
 * @param[in] address Entry address that fired the hook.
 * @param[in] size    Decoded instruction size (unused).
 * @param[in] user    The symbol name passed at install (stable argv pointer).
 *
 * @pre @p user names the hooked symbol.
 * @post One stderr line is emitted per hit.
 *
 * @note Not thread-safe; the run loop is single-threaded host-side.
 * @since 0.1.0
 */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_sym_trace(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)size;
  uint32_t lr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_LR, &lr);
  (void)fprintf(stderr,
                "  [symtrace] %s @ 0x%08X  (LR 0x%08X)\n",
                (const char*)user,
                (unsigned)address,
                lr);
}

/* =============================================================================
 * --fast-sd: serve whole SD blocks in C instead of clocking 512 SPI bytes each.
 * =============================================================================
 *
 * The firmware reads an SD card over SCI0 in Simple-SPI mode one byte at a time:
 * ra8_sci_spi_xfer8() issues a FIXED five MMIO accesses per byte (poll TDRE, write
 * TDR, poll RDRF, read RDR, clear RDRF), and a 512-byte block is 512 of those. A
 * book-sized read is millions of MMIO callbacks -- tens of wall-clock seconds in
 * Unicorn (QEMU-TCG) -- versus ~0.1 s on the real 25 MHz bus, so the cost is a
 * pure emulation artifact, not firmware behaviour.
 *
 * The right granularity to skip is the BLOCK, not the byte: redirecting a hooked
 * function back to its caller needs a uc_emu_stop/relaunch, which is far more
 * expensive than an MMIO callback, so a per-byte hook is a net loss (millions of
 * relaunches). Opt-in --fast-sd instead installs a UC_HOOK_CODE at the entry of
 * ra8_sdmmc_spi_read_block(lba, buf): with a card attached it copies the 512-byte
 * block straight from the image via ::board_sd_read_block -- the byte-identical
 * data the CMD17 path would stream -- writes it to @c buf, returns k_ra8_ok and
 * jumps to LR. One relaunch per sector (hundreds per book) instead of millions
 * per byte, so the rendered framebuffer is byte-for-byte identical while the read
 * drops from tens of seconds to well under one.
 *
 * Faithfulness: this serves block DATA directly, bypassing the SD-over-SPI block
 * protocol (CMD17 / 0xFE token / CRC16 / the per-byte SPI loop) for the data
 * path. The FAT parse, inflate, and render all still run on the real firmware and
 * see identical bytes; the bypassed block protocol is covered by the host unit
 * test (tests/test_ra8_sdmmc_card_reflow.c) and by hardware. It is OFF by default:
 * HIL gates and the default run exercise the full handshake; turn it on only to
 * load a large book fast for an interactive or recorded capture. A card-absent
 * call, an out-of-range LBA, or a firmware without the symbol falls through to
 * the real driver, so nothing else is affected.
 */

/** @enum fast_sd_const_t @brief --fast-sd block-serving constants. */
typedef enum : uint16_t {
  k_fast_sd_block = 512U, /**< SD block size served per hook entry. */
} fast_sd_const_t;

/**
 * @var s_fast_sd
 * @brief True once `--fast-sd` is requested on the command line.
 * @details Gates ::fast_sd_seam_install; when false the hook is never armed and
 *          the SD path runs the full faithful per-byte MMIO block protocol.
 * @warning Single-threaded run loop only.
 * @since 0.1.0
 */
static bool s_fast_sd;

/**
 * @var s_seam_relaunch
 * @brief Set by ::on_sdmmc_read_block to relaunch WITHOUT consuming a chunk.
 * @details The block-read hook stops emulation to return to the caller; left
 *          alone the inner run loop would treat that clean stop as a full
 *          instruction budget elapsing and advance SysTick + charge a chunk. This
 *          flag marks the stop as a zero-time seam relaunch -- the inner loop just
 *          re-enters at the returned PC, exactly like the exception-return path.
 * @warning Single-threaded run loop only; cleared by the inner loop each relaunch.
 * @since 0.1.0
 */
static bool s_seam_relaunch;

/**
 * @brief UC_HOOK_CODE at `ra8_sdmmc_spi_read_block`: serve one 512-byte block in C.
 *
 * @details
 * Reads the AAPCS arguments (r0 = lba, r1 = destination buffer) at the function's
 * entry. With a card attached and the LBA in range it copies the block straight
 * from the image via ::board_sd_read_block -- byte-identical to the CMD17 stream
 * the per-byte path would produce -- writes it to @c buf, sets the return value to
 * k_ra8_ok and jumps to LR, skipping the SPI block protocol. With no card or a
 * bad LBA it returns immediately so Unicorn runs the real driver (which errors as
 * it would on hardware).
 *
 * @param[in,out] uc      Active Unicorn engine.
 * @param[in]     address Hook site (the resolved entry VMA); unused.
 * @param[in]     size    Instruction size at the site; unused.
 * @param[in]     user    Unused hook cookie.
 *
 * @pre @p uc is at ra8_sdmmc_spi_read_block's entry with args still in r0-r1.
 * @post On the fast path, @c buf holds the block, r0 = k_ra8_ok and PC = LR;
 *       otherwise CPU state is intact and the real body runs.
 *
 * @note Not thread-safe; the run loop is single-threaded.
 * @since 0.1.0
 */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_sdmmc_read_block(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t lba     = 0U;
  uint32_t buf_ptr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R0, &lba);
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &buf_ptr);
  uint8_t blk[k_fast_sd_block];
  /* No card, a null buffer, or an out-of-range LBA -> run the real driver so the
   * card-absent / error path stays exactly as on hardware. */
  if (!board_sd_attached() || (buf_ptr == 0U) || !board_sd_read_block(lba, blk)) {
    return;
  }
  (void)uc_mem_write(uc, (uint64_t)buf_ptr, blk, sizeof blk);
  /* Relaunch as a zero-time seam (no chunk / SysTick cost) so a whole book-sized
   * read drains within one settle window. */
  s_seam_relaunch = true;
  eth_hook_return(uc, 0U); /* k_ra8_ok: set r0 + jump to LR, skipping the body. */
}

/**
 * @brief Install the `--fast-sd` block-read hook if opted-in and the symbol exists.
 *
 * @param[in,out] uc  Active Unicorn engine.
 * @param[in]     elf Loaded ELF image (for symbol resolution).
 * @param[in]     len ELF image length in bytes.
 *
 * @pre @p uc is initialised and @p elf holds @p len valid bytes.
 * @post With @c s_fast_sd and the symbol present, a UC_HOOK_CODE fires
 *       ::on_sdmmc_read_block at the function entry; otherwise nothing is armed.
 *
 * @note A firmware without ra8_sdmmc_spi_read_block (no SD path) is reported once
 *       and left on the default per-byte MMIO path.
 * @since 0.1.0
 */
static void fast_sd_seam_install(uc_engine* uc, const uint8_t* elf, long len)
{
  if (!s_fast_sd) {
    return;
  }
  const uint32_t addr = elf_sym_addr(elf, len, "ra8_sdmmc_spi_read_block", nullptr);
  if (addr == 0U) {
    (void)fprintf(
      stderr,
      "  [fast-sd] ra8_sdmmc_spi_read_block not found -- SD stays on the per-byte path\n");
    return;
  }
  static uc_hook h;
  (void)uc_hook_add(uc,
                    &h,
                    UC_HOOK_CODE,
                    (void*)on_sdmmc_read_block,
                    nullptr,
                    (uint64_t)addr,
                    (uint64_t)addr);
  (void)fprintf(stderr,
                "  [fast-sd] ra8_sdmmc_spi_read_block block-hook armed @ 0x%08X "
                "(SD blocks served direct from the image)\n",
                (unsigned)addr);
}

/**
 * @brief Install a `--trace-sym` entry hook for every requested symbol present.
 *
 * @param[in,out] uc    Active Unicorn engine.
 * @param[in]     elf   Loaded ELF image (for symbol resolution).
 * @param[in]     len   ELF image length in bytes.
 * @param[in]     names Symbol names from the CLI (stable for the run).
 * @param[in]     count Number of names in @p names.
 *
 * @pre @p uc is initialised and @p elf holds @p len valid bytes.
 * @post A UC_HOOK_CODE fires ::on_sym_trace at each resolved symbol's entry.
 *
 * @note A name that does not resolve is reported once and skipped.
 * @since 0.1.0
 */
static void sym_trace_install(uc_engine*         uc,
                              const uint8_t*     elf,
                              long               len,
                              const char* const* names,
                              uint32_t           count)
{
  static uc_hook th[k_trace_sym_max];
  for (uint32_t i = 0U; (i < count) && (i < (uint32_t)k_trace_sym_max); i++) {
    const uint32_t addr = elf_sym_addr(elf, len, names[i], nullptr);
    if (addr == 0U) {
      (void)fprintf(stderr, "  [symtrace] %s: symbol not found -- skipped\n", names[i]);
      continue;
    }
    (void)uc_hook_add(uc, &th[i], UC_HOOK_CODE, (void*)on_sym_trace, (void*)names[i], addr, addr);
    (void)fprintf(stderr, "  [symtrace] %s armed @ 0x%08X\n", names[i], (unsigned)addr);
  }
}

/* ============================================================================
 * Virtual USB host-mode device: a HID boot keyboard behind the ra8_usb_host_*
 * seam.
 *
 * board_usb.c models the USBFS controller in DEVICE mode (a virtual host drives
 * the firmware's device stack). The inverse case -- the firmware acting as USB
 * HOST -- drives the USBHS controller (0x40351000, unmodelled) through the
 * first-party `ra8_usb_host_*` primitives, and with no peer the control transfer
 * wedges (SUREQ never acked -> k_ra8_err_busy). Rather than model a second
 * controller register-by-register, seam those primitives the same way ra8_eth_*
 * is seamed to a virtual net peer (the register model "cannot satisfy" that
 * sequence either): present a virtual boot keyboard that answers chapter-9
 * enumeration and streams interrupt-IN reports. This lets a host example
 * (usb_host_keyboard) enumerate + read reports end to end with no hardware --
 * validating the host stack's control/data logic, not silicon timing.
 * ==========================================================================*/

/** @brief bRequest / descriptor-type / sizing constants for the virtual device. */
typedef enum : uint16_t {
  k_vkbd_breq_get_descriptor = 0x06U, /**< Standard GET_DESCRIPTOR bRequest.       */
  k_vkbd_dt_device           = 0x01U, /**< DEVICE descriptor (wValue hi byte).     */
  k_vkbd_dt_config           = 0x02U, /**< CONFIGURATION descriptor.               */
  k_vkbd_dt_string           = 0x03U, /**< STRING descriptor.                      */
  k_vkbd_dt_hid_report       = 0x22U, /**< HID REPORT descriptor.                  */
  k_vkbd_lnst_attached       = 0x02U, /**< SYSSTS0.LNST J-state (device on bus).   */
  k_vkbd_report_len          = 8U,    /**< Boot-keyboard input report width.       */
  k_vkbd_num_keys            = 5U,    /**< Keycodes typed ("R A 8 D 2").           */
  k_vkbd_dev_desc_len        = 18U,   /**< DEVICE descriptor length.               */
  k_vkbd_cfg_desc_len        = 34U,   /**< Full CONFIGURATION descriptor length.   */
  k_vkbd_stop_reports        = 8U,    /**< Reports streamed before USB_STOP fires. */
} vkbd_const_t;

/** @brief 18-byte DEVICE descriptor: class defined at interface, EP0 MPS 64. */
static const uint8_t k_vkbd_device_desc[k_vkbd_dev_desc_len] = {
  0x12,
  0x01,
  0x00,
  0x02,
  0x00,
  0x00,
  0x00,
  0x40, /* len,DEVICE,bcdUSB2.00,class0,MPS64 */
  0x6A,
  0x1A,
  0x88,
  0x42,
  0x00,
  0x01,
  0x00,
  0x00, /* idVendor 0x1A6A, idProduct 0x4288 */
  0x00,
  0x01, /* bcdDevice, iM/iP/iS=0, 1 config */
};

/** @brief 34-byte CONFIGURATION: 1 HID boot-keyboard iface, 1 interrupt-IN EP1. */
static const uint8_t k_vkbd_config_desc[k_vkbd_cfg_desc_len] = {
  0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xA0, 0x32, /* CONFIG: wTotalLen 34, 1 iface */
  0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00, /* IFACE: HID, boot, keyboard    */
  0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00, /* HID: report desc len 0x3F     */
  0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x01,             /* EP1 IN, interrupt, MPS64, 1ms */
};

/** @brief Standard boot-keyboard HID REPORT descriptor (63 bytes, USB HID 1.11 E.6). */
static const uint8_t k_vkbd_report_desc[63] = {
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
  0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
  0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
  0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};

/** @brief HID Usage-Table keycodes (Usage Page 0x07) for the typed string. */
typedef enum : uint8_t {
  k_vkbd_key_r = 0x15U, /**< HID usage for 'R'. */
  k_vkbd_key_a = 0x04U, /**< HID usage for 'A'. */
  k_vkbd_key_8 = 0x25U, /**< HID usage for '8'. */
  k_vkbd_key_d = 0x07U, /**< HID usage for 'D'. */
  k_vkbd_key_2 = 0x1FU, /**< HID usage for '2'. */
} vkbd_keycode_t;

/** @brief HID Usage-Table keycodes the virtual keyboard "types": R A 8 D 2. */
static const uint8_t k_vkbd_keycodes[k_vkbd_num_keys] = {k_vkbd_key_r,
                                                         k_vkbd_key_a,
                                                         k_vkbd_key_8,
                                                         k_vkbd_key_d,
                                                         k_vkbd_key_2};

static uint8_t  s_vkbd_seq           = 0U; /**< Rolling report seq (report byte 0). */
static uint32_t s_vkbd_ctrl_serviced = 0U; /**< Control transfers answered.         */
static uint32_t s_vkbd_reports_sent  = 0U; /**< Interrupt-IN reports streamed.      */

/** @brief Read the 5th (stack-passed) argument of an AAPCS call: mem32[SP]. */
static uint32_t usbh_arg5(uc_engine* uc)
{
  uint32_t sp = 0U;
  uint32_t p  = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_SP, &sp);
  (void)uc_mem_read(uc, (uint64_t)sp, &p, sizeof(p));
  return p;
}

/** @brief Hook a host primitive that just succeeds (init / reset / target / pipe). */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_usbh_ok(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_host_line_state(): report the virtual device attached. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_usbh_line_state(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  eth_hook_return(uc, (uint32_t)k_vkbd_lnst_attached);
}

/** @brief Hook ra8_usb_host_control_xfer(): answer chapter-9 from the virtual device. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_usbh_control_xfer(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t setup_ptr = 0U;
  uint32_t data_ptr  = 0U;
  uint32_t data_len  = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &setup_ptr);
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &data_ptr);
  (void)uc_reg_read(uc, UC_ARM_REG_R3, &data_len);
  const uint32_t out_ptr = usbh_arg5(uc);

  uint8_t s[8] = {};
  (void)uc_mem_read(uc, (uint64_t)setup_ptr, s, sizeof(s));
  const uint8_t  b_request = s[1];
  const uint8_t  desc_type = s[3]; /* wValue high byte = descriptor type. */
  const uint8_t* src       = nullptr;
  uint16_t       src_len   = 0U;
  if (b_request == (uint8_t)k_vkbd_breq_get_descriptor) {
    if (desc_type == (uint8_t)k_vkbd_dt_device) {
      src     = k_vkbd_device_desc;
      src_len = (uint16_t)k_vkbd_dev_desc_len;
    } else if (desc_type == (uint8_t)k_vkbd_dt_config) {
      src     = k_vkbd_config_desc;
      src_len = (uint16_t)k_vkbd_cfg_desc_len;
    } else if (desc_type == (uint8_t)k_vkbd_dt_hid_report) {
      src     = k_vkbd_report_desc;
      src_len = (uint16_t)sizeof(k_vkbd_report_desc);
    }
  }
  uint16_t n = 0U;
  if ((src != nullptr) && (data_ptr != 0U)) {
    n = (src_len < (uint16_t)data_len) ? src_len : (uint16_t)data_len;
    (void)uc_mem_write(uc, (uint64_t)data_ptr, src, n);
  }
  /* No-data control writes (SET_ADDRESS / SET_CONFIGURATION / SET_IDLE /
   * SET_PROTOCOL) just leave n = 0; the host treats that as a successful ack. */
  if (out_ptr != 0U) {
    (void)uc_mem_write(uc, (uint64_t)out_ptr, &n, sizeof(n));
  }
  s_vkbd_ctrl_serviced++;
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_host_bulk_in(): stream one boot-keyboard input report. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_usbh_bulk_in(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t buf     = 0U;
  uint32_t max_len = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &buf);
  (void)uc_reg_read(uc, UC_ARM_REG_R3, &max_len);
  const uint32_t out_ptr = usbh_arg5(uc);
  /* Boot-keyboard report: [seq][reserved 0][keycodes R A 8 D 2][0]. The host
   * ignores byte 0 and pattern-checks bytes 1.. -- it streams "RA8D2". */
  uint8_t rep[k_vkbd_report_len] = {};
  rep[0]                         = s_vkbd_seq++;
  for (uint32_t i = 0U; i < (uint32_t)k_vkbd_num_keys; i++) {
    rep[2U + i] = k_vkbd_keycodes[i];
  }
  uint16_t n = (uint16_t)k_vkbd_report_len;
  if ((uint32_t)n > max_len) {
    n = (uint16_t)max_len;
  }
  if (buf != 0U) {
    (void)uc_mem_write(uc, (uint64_t)buf, rep, n);
  }
  if (out_ptr != 0U) {
    (void)uc_mem_write(uc, (uint64_t)out_ptr, &n, sizeof(n));
  }
  s_vkbd_reports_sent++;
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/* ----------------------------------------------------------------------------
 * Virtual USB host-mode MSC device: a read-only FAT16 disk whose one file
 * MRAM.BIN is the 1 MiB MRAM code window. Seams the first-party `ra8_usb_hmsc_*`
 * class API (one level above the BOT/SCSI bulk transport) so a host MSC app
 * (usb_host_msc_browse) enumerates, READ_CAPACITYs, mounts the FAT16, browses
 * the root directory, and content-verifies MRAM.BIN -- all with no peer device.
 * The boot/FAT/root sectors are a byte-identical replica of the device's
 * selftest_fat_fill_sector; the data region is read live from emulated MRAM, so
 * it matches the host's own MRAM compare byte-for-byte.
 * --------------------------------------------------------------------------*/

/** @brief FAT16 geometry + boot/dir layout for the virtual MSC volume. */
typedef enum : uint32_t {
  k_vmsc_block_size     = 512U,        /**< Logical block size.                   */
  k_vmsc_total_sectors  = 4146U,       /**< 1 reserved + 17 FAT + 32 root + 4096. */
  k_vmsc_root_lba       = 18U,         /**< First root-directory LBA.             */
  k_vmsc_data_lba       = 50U,         /**< First data-region LBA (cluster 2).    */
  k_vmsc_first_cluster  = 2U,          /**< FAT data area starts at cluster 2.    */
  k_vmsc_last_mram_clus = 2049U,       /**< Last cluster of MRAM.BIN.             */
  k_vmsc_entries_per_fs = 256U,        /**< FAT16 entries per 512-byte sector.    */
  k_vmsc_mram_base      = 0x02000000U, /**< MRAM window base (MRAM.BIN data).     */
  k_vmsc_fat_entry0     = 0xFFF8U,     /**< FAT[0]: media F8 + filler.            */
  k_vmsc_fat_eoc        = 0xFFFFU,     /**< End-of-chain marker.                  */
  k_vmsc_file_bytes     = 0x00100000U, /**< MRAM.BIN size: 1 MiB.                 */
  k_vmsc_volid          = 0x52A8D20AU, /**< Boot-sector volume serial.            */
} vmsc_const_t;

/** @brief FAT16 BPB byte offsets, fixed field values, and store shifts. */
typedef enum : uint32_t {
  k_bpb_shift8         = 8U,    /**< Byte 1 store shift.                */
  k_bpb_shift16        = 16U,   /**< Byte 2 store shift.                */
  k_bpb_shift24        = 24U,   /**< Byte 3 store shift.                */
  k_bpb_jmp0           = 0xEBU, /**< BS_jmpBoot[0]: short jump opcode.  */
  k_bpb_jmp1           = 0x3CU, /**< BS_jmpBoot[1]: jump displacement.  */
  k_bpb_jmp2           = 0x90U, /**< BS_jmpBoot[2]: NOP.                */
  k_bpb_off_oem        = 3U,    /**< BS_OEMName offset.                 */
  k_bpb_off_bytspersec = 11U,   /**< BPB_BytsPerSec offset.             */
  k_bpb_off_secperclus = 13U,   /**< BPB_SecPerClus offset.             */
  k_bpb_off_rsvdseccnt = 14U,   /**< BPB_RsvdSecCnt offset.             */
  k_bpb_off_numfats    = 16U,   /**< BPB_NumFATs offset.                */
  k_bpb_off_rootentcnt = 17U,   /**< BPB_RootEntCnt offset.             */
  k_bpb_off_totsec16   = 19U,   /**< BPB_TotSec16 offset.               */
  k_bpb_off_media      = 21U,   /**< BPB_Media offset.                  */
  k_bpb_off_fatsz16    = 22U,   /**< BPB_FATSz16 offset.                */
  k_bpb_off_secpertrk  = 24U,   /**< BPB_SecPerTrk offset.              */
  k_bpb_off_numheads   = 26U,   /**< BPB_NumHeads offset.               */
  k_bpb_off_drvnum     = 36U,   /**< BS_DrvNum offset.                  */
  k_bpb_off_bootsig    = 38U,   /**< BS_BootSig offset.                 */
  k_bpb_off_volid      = 39U,   /**< BS_VolID offset.                   */
  k_bpb_off_vollab     = 43U,   /**< BS_VolLab offset.                  */
  k_bpb_off_filsystype = 54U,   /**< BS_FilSysType offset.              */
  k_bpb_off_sig0       = 510U,  /**< 0x55 signature byte.               */
  k_bpb_off_sig1       = 511U,  /**< 0xAA signature byte.               */
  k_bpb_secperclus_1   = 1U,    /**< 1 sector per cluster.              */
  k_bpb_rsvdseccnt_1   = 1U,    /**< 1 reserved sector.                 */
  k_bpb_numfats_1      = 1U,    /**< 1 FAT copy.                        */
  k_bpb_rootentcnt_512 = 512U,  /**< 512 root-directory entries.        */
  k_bpb_media_f8       = 0xF8U, /**< Fixed-disk media descriptor.       */
  k_bpb_fatsz16_17     = 17U,   /**< 17 sectors per FAT.                */
  k_bpb_secpertrk_32   = 32U,   /**< 32 sectors per track.              */
  k_bpb_numheads_16    = 16U,   /**< 16 heads.                          */
  k_bpb_drvnum_80      = 0x80U, /**< Drive number (first fixed disk).   */
  k_bpb_bootsig_29     = 0x29U, /**< Extended boot signature.           */
  k_bpb_sig0_55        = 0x55U, /**< Boot-sector signature byte 0.      */
  k_bpb_sig1_aa        = 0xAAU, /**< Boot-sector signature byte 1.      */
  k_dir_off_attr       = 11U,   /**< Directory-entry attribute byte.    */
  k_dir_off_entry      = 32U,   /**< Second 32-byte directory entry.    */
  k_dir_off_fstcluslo  = 26U,   /**< DIR_FstClusLO offset within entry. */
  k_dir_off_filesize   = 28U,   /**< DIR_FileSize offset within entry.  */
  k_dir_attr_vollabel  = 0x08U, /**< ATTR_VOLUME_ID.                    */
  k_dir_attr_readonly  = 0x01U, /**< ATTR_READ_ONLY.                    */
} vmsc_bpb_t;

static const uint8_t k_vmsc_oem[8]    = {'R', 'A', '8', 'D', '2', 'F', 'W', ' '};
static const uint8_t k_vmsc_label[11] = {'R', 'A', '8', 'D', '2', ' ', 'M', 'R', 'A', 'M', ' '};
static const uint8_t k_vmsc_fstype[8] = {'F', 'A', 'T', '1', '6', ' ', ' ', ' '};
static const uint8_t k_vmsc_fname[11] = {'M', 'R', 'A', 'M', ' ', ' ', ' ', ' ', 'B', 'I', 'N'};

/** @brief Set once the host attempts a WRITE(10) into the READ-ONLY disk -- the
 *  last host step before usb_host_msc_browse's PASS (read by the USBH_STOP guard).*/
static bool s_vmsc_write_seen = false;

/** @brief True when the virtual disk is writable (usb_host_file_ops links
 *  `fileops_backend_write`); else the disk is read-only and WRITE(10) is rejected.*/
static bool s_vmsc_writable = false;

/**
 * @struct vmsc_overlay_t
 * @brief One overwritten sector of the otherwise-synthesized FAT16 volume.
 * @details A writable host (usb_host_file_ops) creates a file: ra8_fs rewrites a
 * few FAT / root / data sectors. We keep those writes in a small overlay so the
 * read-back reads them back; everything else is still synthesized on the fly.
 */
typedef struct {
  uint32_t lba;                     /**< Overwritten LBA.             */
  bool     valid;                   /**< Slot in use.                 */
  uint8_t  data[k_vmsc_block_size]; /**< The written 512-byte sector. */
} vmsc_overlay_t;

/** @brief Write overlay for the writable disk (file_ops touches only a handful). */
static vmsc_overlay_t s_vmsc_overlay[64];

/** @brief Return an overwritten sector if @p lba is in the overlay. */
static bool vmsc_overlay_get(uint32_t lba, uint8_t* out)
{
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(s_vmsc_overlay) / sizeof(s_vmsc_overlay[0])); i++) {
    if (s_vmsc_overlay[i].valid && (s_vmsc_overlay[i].lba == lba)) {
      (void)memcpy(out, s_vmsc_overlay[i].data, (size_t)k_vmsc_block_size);
      return true;
    }
  }
  return false;
}

/** @brief Record an overwritten sector (update existing slot or take a free one). */
static void vmsc_overlay_put(uint32_t lba, const uint8_t* in)
{
  const uint32_t slots = (uint32_t)(sizeof(s_vmsc_overlay) / sizeof(s_vmsc_overlay[0]));
  for (uint32_t i = 0U; i < slots; i++) {
    if (s_vmsc_overlay[i].valid && (s_vmsc_overlay[i].lba == lba)) {
      (void)memcpy(s_vmsc_overlay[i].data, in, (size_t)k_vmsc_block_size);
      return;
    }
  }
  for (uint32_t i = 0U; i < slots; i++) {
    if (!s_vmsc_overlay[i].valid) {
      s_vmsc_overlay[i].lba   = lba;
      s_vmsc_overlay[i].valid = true;
      (void)memcpy(s_vmsc_overlay[i].data, in, (size_t)k_vmsc_block_size);
      return;
    }
  }
}

/** @brief Little-endian 16-bit store into a sector buffer. */
static void vmsc_put16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & (uint16_t)k_byte_mask);
  p[1] = (uint8_t)((v >> (uint16_t)k_bpb_shift8) & (uint16_t)k_byte_mask);
}

/** @brief Little-endian 32-bit store into a sector buffer. */
static void vmsc_put32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & (uint32_t)k_byte_mask);
  p[1] = (uint8_t)((v >> (uint32_t)k_bpb_shift8) & (uint32_t)k_byte_mask);
  p[2] = (uint8_t)((v >> (uint32_t)k_bpb_shift16) & (uint32_t)k_byte_mask);
  p[3] = (uint8_t)((v >> (uint32_t)k_bpb_shift24) & (uint32_t)k_byte_mask);
}

/** @brief Synthesize the FAT16 boot sector (BPB), mirroring the device side. */
static void vmsc_fill_boot(uint8_t* out)
{
  out[0] = (uint8_t)k_bpb_jmp0;
  out[1] = (uint8_t)k_bpb_jmp1;
  out[2] = (uint8_t)k_bpb_jmp2; /* jmp + nop */
  (void)memcpy(&out[k_bpb_off_oem], k_vmsc_oem, sizeof(k_vmsc_oem));
  vmsc_put16(&out[k_bpb_off_bytspersec], (uint16_t)k_vmsc_block_size);
  out[k_bpb_off_secperclus] = (uint8_t)k_bpb_secperclus_1;                /* sectors/cluster  */
  vmsc_put16(&out[k_bpb_off_rsvdseccnt], (uint16_t)k_bpb_rsvdseccnt_1);   /* reserved sectors */
  out[k_bpb_off_numfats] = (uint8_t)k_bpb_numfats_1;                      /* number of FATs   */
  vmsc_put16(&out[k_bpb_off_rootentcnt], (uint16_t)k_bpb_rootentcnt_512); /* root entries     */
  vmsc_put16(&out[k_bpb_off_totsec16], (uint16_t)k_vmsc_total_sectors);
  out[k_bpb_off_media] = (uint8_t)k_bpb_media_f8;                      /* media descriptor   */
  vmsc_put16(&out[k_bpb_off_fatsz16], (uint16_t)k_bpb_fatsz16_17);     /* sectors per FAT    */
  vmsc_put16(&out[k_bpb_off_secpertrk], (uint16_t)k_bpb_secpertrk_32); /* sectors per track  */
  vmsc_put16(&out[k_bpb_off_numheads], (uint16_t)k_bpb_numheads_16);   /* heads              */
  out[k_bpb_off_drvnum]  = (uint8_t)k_bpb_drvnum_80;                   /* drive number       */
  out[k_bpb_off_bootsig] = (uint8_t)k_bpb_bootsig_29;                  /* ext boot signature */
  vmsc_put32(&out[k_bpb_off_volid], (uint32_t)k_vmsc_volid);
  (void)memcpy(&out[k_bpb_off_vollab], k_vmsc_label, sizeof(k_vmsc_label));
  (void)memcpy(&out[k_bpb_off_filsystype], k_vmsc_fstype, sizeof(k_vmsc_fstype));
  out[k_bpb_off_sig0] = (uint8_t)k_bpb_sig0_55;
  out[k_bpb_off_sig1] = (uint8_t)k_bpb_sig1_aa;
}

/** @brief Synthesize one FAT sector: MRAM.BIN chains clusters 2..2049. */
static void vmsc_fill_fat(uint32_t fat_sector, uint8_t* out)
{
  const uint32_t first = fat_sector * (uint32_t)k_vmsc_entries_per_fs;
  for (uint32_t j = 0U; j < (uint32_t)k_vmsc_entries_per_fs; j++) {
    const uint32_t entry = first + j;
    uint16_t       value = 0U;
    if (entry == 0U) {
      value = (uint16_t)k_vmsc_fat_entry0;
    } else if (entry == 1U) {
      value = (uint16_t)k_vmsc_fat_eoc;
    } else if (entry < (uint32_t)k_vmsc_last_mram_clus) {
      value = (uint16_t)(entry + 1U);
    } else if (entry == (uint32_t)k_vmsc_last_mram_clus) {
      value = (uint16_t)k_vmsc_fat_eoc;
    }
    vmsc_put16(&out[j * 2U], value);
  }
}

/** @brief Synthesize root-directory sector 0: volume label + MRAM.BIN entry. */
static void vmsc_fill_root(uint32_t root_sector, uint8_t* out)
{
  if (root_sector != 0U) {
    return;
  }
  (void)memcpy(&out[0], k_vmsc_label, sizeof(k_vmsc_label));
  out[k_dir_off_attr] = (uint8_t)k_dir_attr_vollabel; /* volume-label attribute */
  uint8_t* e          = &out[k_dir_off_entry];
  (void)memcpy(e, k_vmsc_fname, sizeof(k_vmsc_fname));
  e[k_dir_off_attr] = (uint8_t)k_dir_attr_readonly; /* read-only attribute */
  vmsc_put16(&e[k_dir_off_fstcluslo], (uint16_t)k_vmsc_first_cluster);
  vmsc_put32(&e[k_dir_off_filesize], (uint32_t)k_vmsc_file_bytes);
}

/** @brief Fill one 512-byte volume sector (boot / FAT / root / live MRAM data). */
static void vmsc_fill_sector(uc_engine* uc, uint32_t lba, uint8_t* out)
{
  (void)memset(out, 0, (size_t)k_vmsc_block_size);
  if (lba == 0U) {
    vmsc_fill_boot(out);
  } else if (lba < (uint32_t)k_vmsc_root_lba) {
    vmsc_fill_fat(lba - 1U, out);
  } else if (lba < (uint32_t)k_vmsc_data_lba) {
    vmsc_fill_root(lba - (uint32_t)k_vmsc_root_lba, out);
  } else {
    const uint32_t cluster = (lba - (uint32_t)k_vmsc_data_lba) + (uint32_t)k_vmsc_first_cluster;
    if (cluster <= (uint32_t)k_vmsc_last_mram_clus) {
      const uint32_t off = (cluster - (uint32_t)k_vmsc_first_cluster) * (uint32_t)k_vmsc_block_size;
      (void)
        uc_mem_read(uc, (uint64_t)k_vmsc_mram_base + (uint64_t)off, out, (size_t)k_vmsc_block_size);
    }
  }
}

/** @brief Hook a host MSC primitive that just succeeds (init / close). */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_ok(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief ra8_usb_hmsc_device_t field offsets + reported bulk EP packet/VID/PID. */
typedef enum : uint32_t {
  k_hmsc_off_vid    = 10U,     /**< vid offset in ra8_usb_hmsc_device_t.    */
  k_hmsc_off_pid    = 12U,     /**< pid offset in ra8_usb_hmsc_device_t.    */
  k_hmsc_bulk_mps   = 64U,     /**< Reported bulk-endpoint max packet size. */
  k_hmsc_vendor_id  = 0x1A6AU, /**< Reported USB vendor_id.                 */
  k_hmsc_product_id = 0x4288U, /**< Reported USB product_id.                */
} hmsc_dev_t;

/** @brief Hook ra8_usb_hmsc_enumerate(out_device*): report the virtual disk. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_enumerate(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t dev_ptr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R0, &dev_ptr);
  if (dev_ptr != 0U) {
    /* ra8_usb_hmsc_device_t: addr,bin_ep,bout_ep,max_lun,iface,[pad],in_mps,
     * out_mps,vid,pid. */
    uint8_t d[14] = {};
    d[0]          = 1U;                                          /* device_address      */
    d[1]          = 1U;                                          /* bulk_in_ep          */
    d[2]          = 2U;                                          /* bulk_out_ep         */
    vmsc_put16(&d[6], (uint16_t)k_hmsc_bulk_mps);                /* bulk_in_max_packet  */
    vmsc_put16(&d[8], (uint16_t)k_hmsc_bulk_mps);                /* bulk_out_max_packet */
    vmsc_put16(&d[k_hmsc_off_vid], (uint16_t)k_hmsc_vendor_id);  /* vendor_id           */
    vmsc_put16(&d[k_hmsc_off_pid], (uint16_t)k_hmsc_product_id); /* product_id          */
    (void)uc_mem_write(uc, (uint64_t)dev_ptr, d, sizeof(d));
  }
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_hmsc_read_capacity(lun, *block_count, *block_size). */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_read_capacity(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t bc_ptr = 0U;
  uint32_t bs_ptr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &bc_ptr);
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &bs_ptr);
  const uint32_t block_count = (uint32_t)k_vmsc_total_sectors;
  const uint32_t block_size  = (uint32_t)k_vmsc_block_size;
  if (bc_ptr != 0U) {
    (void)uc_mem_write(uc, (uint64_t)bc_ptr, &block_count, sizeof(block_count));
  }
  if (bs_ptr != 0U) {
    (void)uc_mem_write(uc, (uint64_t)bs_ptr, &block_size, sizeof(block_size));
  }
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_hmsc_read10(lun, lba, count, out_buf): serve sectors. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_read10(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t lba   = 0U;
  uint32_t count = 0U;
  uint32_t buf   = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &lba);
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &count);
  (void)uc_reg_read(uc, UC_ARM_REG_R3, &buf);
  count &= (uint32_t)k_lo16_mask; /* block_count is a uint16_t argument. */
  for (uint32_t i = 0U; (i < count) && (buf != 0U); i++) {
    uint8_t sec[k_vmsc_block_size];
    if (!vmsc_overlay_get(lba + i, sec)) { /* a host WRITE(10) wins over the synthesis. */
      vmsc_fill_sector(uc, lba + i, sec);
    }
    (void)uc_mem_write(uc,
                       (uint64_t)buf + ((uint64_t)i * (uint64_t)k_vmsc_block_size),
                       sec,
                       sizeof(sec));
  }
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_hmsc_write10(): reject -- the volume is read-only. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_write10(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  if (!s_vmsc_writable) {
    /* Read-only disk (usb_host_msc_browse): reject. This is that app's last step
     * before PASS, so flag it for the host early-stop. */
    s_vmsc_write_seen = true;
    eth_hook_return(uc, (uint32_t)k_ra8_err_inval_st); /* write protected */
    return;
  }
  /* Writable disk (usb_host_file_ops): stash the written sectors in the overlay
   * so the host's read-back sees them, and accept. The write is mid-ladder here,
   * so do NOT trip the write-seen early-stop -- that app stops on its banner. */
  uint32_t lba   = 0U;
  uint32_t count = 0U;
  uint32_t buf   = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &lba);
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &count);
  (void)uc_reg_read(uc, UC_ARM_REG_R3, &buf);
  count &= (uint32_t)k_lo16_mask;
  for (uint32_t i = 0U; (i < count) && (buf != 0U); i++) {
    uint8_t sec[k_vmsc_block_size];
    (void)uc_mem_read(uc,
                      (uint64_t)buf + ((uint64_t)i * (uint64_t)k_vmsc_block_size),
                      sec,
                      sizeof(sec));
    vmsc_overlay_put(lba + i, sec);
  }
  eth_hook_return(uc, 0U); /* k_ra8_ok -- write accepted */
}

/**
 * @brief Install the virtual USB host-mode device seam if the host stack is linked.
 *
 * @details Picks the virtual device class from the firmware's linked host stack:
 * an MSC host (links `ra8_usb_hmsc_read10`) gets a read-only FAT16 disk seamed at
 * the `ra8_usb_hmsc_*` class API; otherwise a USB-host-capable firmware (links
 * `ra8_usb_host_control_xfer`) gets a HID boot keyboard seamed at the
 * `ra8_usb_host_*` primitives. Device-mode apps link neither call path, so the
 * hooks are inert there and board_usb.c's device-mode virtual host is untouched.
 *
 * @param[in,out] uc  Active Unicorn engine.
 * @param[in]     elf Loaded ELF image (symbol resolution).
 * @param[in]     len ELF image length in bytes.
 *
 * @pre @p uc is initialised and @p elf holds @p len bytes.
 * @post On a host app, the linked host API answers a virtual device.
 *
 * @note No effect on device-mode apps (the hooked symbols are never called).
 * @return true when a seam family was installed (the register-level USBHS
 *         host model must then stay dormant -- see board_usb_host.h).
 * @retval true  hmsc- or primitive-level seams now shadow the host API.
 * @retval false No usb-host seams; the register path is the real one.
 * @since 0.1.0
 */
static bool usbh_seam_install(uc_engine* uc, const uint8_t* elf, long len)
{
  const uint32_t msc = elf_sym_addr(elf, len, "ra8_usb_hmsc_read10", nullptr);
  if (msc != 0U) {
    /* usb_host_file_ops creates a file (it links fileops_backend_write) -> the
     * virtual disk is writable; usb_host_msc_browse tests a read-only LUN. */
    s_vmsc_writable = (elf_sym_addr(elf, len, "fileops_backend_write", nullptr) != 0U);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_init", (void*)on_hmsc_ok);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_enumerate", (void*)on_hmsc_enumerate);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_read_capacity", (void*)on_hmsc_read_capacity);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_read10", (void*)on_hmsc_read10);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_write10", (void*)on_hmsc_write10);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_close", (void*)on_hmsc_ok);
    (void)fprintf(stderr,
                  "  usb-host seam : hmsc=0x%08X (virtual MSC FAT16 disk, file MRAM.BIN, %s)\n",
                  msc,
                  s_vmsc_writable ? "read-write" : "read-only");
    return true;
  }
  const uint32_t cx = elf_sym_addr(elf, len, "ra8_usb_host_control_xfer", nullptr);
  if (cx == 0U) {
    return false; /* not a USB-host-capable firmware -- nothing to seam. */
  }
  eth_seam_hook(uc, elf, len, "ra8_usb_host_line_state", (void*)on_usbh_line_state);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_control_xfer", (void*)on_usbh_control_xfer);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_bulk_in", (void*)on_usbh_bulk_in);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_bus_reset", (void*)on_usbh_ok);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_set_uact", (void*)on_usbh_ok);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_set_target", (void*)on_usbh_ok);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_pipe_setup", (void*)on_usbh_ok);
  (void)fprintf(stderr,
                "  usb-host seam : control=0x%08X (virtual HID boot keyboard \"RA8D2\")\n",
                cx);
  return true;
}

/** @brief Load ELF32 PT_LOAD segments into emulated memory at their LMA. */
static int load_elf(uc_engine* uc, const uint8_t* elf, long len)
{
  if ((len < (long)k_elf_ehdr_size) ||
      (memcmp(elf,
              "\x7F"
              "ELF",
              4) != 0) ||
      (elf[4] != 1) /* ELFCLASS32 */) {
    (void)fprintf(stderr, "not a 32-bit ELF\n");
    return -1;
  }
  const uint16_t e_machine = (uint16_t)(elf[18] | (elf[19] << 8));
  if (e_machine != (uint16_t)k_elf_em_arm) {
    (void)fprintf(stderr, "ELF e_machine %u != ARM(40)\n", e_machine);
    return -1;
  }
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + (uint32_t)k_elf_e_phoff_off, 4);
  uint16_t phentsize = (uint16_t)(elf[42] | (elf[43] << 8));
  uint16_t phnum     = (uint16_t)(elf[44] | (elf[45] << 8));
  int      loaded    = 0;
  for (uint16_t i = 0U; i < phnum; i++) {
    const uint8_t* ph = elf + phoff + ((uint32_t)i * phentsize);
    uint32_t       p_type;
    uint32_t       p_offset;
    uint32_t       p_paddr;
    uint32_t       p_filesz;
    (void)memcpy(&p_type, ph + 0, 4);
    (void)memcpy(&p_offset, ph + 4, 4);
    (void)memcpy(&p_paddr, ph + (uint32_t)k_elf_ph_paddr_off, 4); /* load address (LMA) */
    (void)memcpy(&p_filesz, ph + 16, 4);
    if ((p_type != 1U /* PT_LOAD */) || (p_filesz == 0U)) {
      continue;
    }
    if (uc_mem_write(uc, p_paddr, elf + p_offset, p_filesz) != UC_ERR_OK) {
      (void)fprintf(stderr, "uc_mem_write seg @0x%08X (%u bytes) failed\n", p_paddr, p_filesz);
      return -1;
    }
    (void)fprintf(stderr, "  loaded %u bytes @ 0x%08X\n", p_filesz, p_paddr);
    loaded++;
  }
  return (loaded > 0) ? 0 : -1;
}

/**
 * @brief Vector-table base of an ELF: lowest executable PT_LOAD VMA.
 *
 * @details
 * Walks the program headers and returns the lowest p_vaddr among non-empty,
 * executable (PF_X) PT_LOAD segments. On Cortex-M the vector table is linked at
 * the start of the executable text region, so this is the image's vector base:
 * 0x32100000 for the RAM-resident NS layout (a single RWE segment runs from the
 * NS_SRAM2 alias) and 0x90000000 for the OSPI-XIP layout (the R-E segment
 * executes in place from the flash window). The executable filter is essential
 * for XIP: that image also carries a writable .data segment whose VMA is
 * 0x32100000 (its initialisers live in flash, copied to RAM at NS startup) --
 * lower than the vectors -- so a plain minimum-VMA scan would wrongly pick the
 * RAM alias. board_sim feeds the result to ::s_ns_vector_base so the BLXNS world
 * switch reads the NS MSP/reset from wherever the loaded image placed them.
 *
 * @param[in] elf Mapped ELF image (little-endian ELF32 ARM).
 * @param[in] len ELF image length in bytes.
 * @return Lowest executable PT_LOAD p_vaddr, or 0 if none / the header is bad.
 *
 * @pre @p elf points to at least @ref k_elf_ehdr_size bytes.
 * @pre The program-header table lies within the first @p len bytes.
 * @post The return value is the VMA of some executable PT_LOAD segment, or 0.
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
static uint32_t elf_vector_base(const uint8_t* elf, long len)
{
  if ((elf == nullptr) || (len < (long)k_elf_ehdr_size)) {
    return 0U;
  }
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + (uint32_t)k_elf_e_phoff_off, 4);
  const uint16_t phentsize = (uint16_t)(elf[42] | (elf[43] << 8));
  const uint16_t phnum     = (uint16_t)(elf[44] | (elf[45] << 8));
  if (((uint64_t)phoff + ((uint64_t)phnum * (uint64_t)phentsize)) > (uint64_t)len) {
    return 0U;
  }
  uint32_t min_vaddr = 0U;
  bool     found     = false;
  for (uint16_t i = 0U; i < phnum; i++) {
    const uint8_t* ph       = elf + phoff + ((uint32_t)i * phentsize);
    uint32_t       p_type   = 0U;
    uint32_t       p_vaddr  = 0U;
    uint32_t       p_filesz = 0U;
    uint32_t       p_flags  = 0U;
    (void)memcpy(&p_type, ph + 0, 4);
    (void)memcpy(&p_vaddr, ph + (uint32_t)k_elf_ph_vaddr_off, 4);
    (void)memcpy(&p_filesz, ph + (uint32_t)k_elf_ph_filesz_off, 4);
    (void)memcpy(&p_flags, ph + (uint32_t)k_elf_ph_flags_off, 4);
    if ((p_type != (uint32_t)k_elf_pt_load) || (p_filesz == 0U) ||
        ((p_flags & (uint32_t)k_elf_pf_x) == 0U)) {
      continue;
    }
    if (!found || (p_vaddr < min_vaddr)) {
      min_vaddr = p_vaddr;
      found     = true;
    }
  }
  return found ? min_vaddr : 0U;
}

/**
 * @brief Resolve a function symbol's entry address from the ELF .symtab.
 *
 * @details
 * Walks the ELF32 section headers for the SHT_SYMTAB table and its linked string
 * table, then matches @p name and returns its st_value with the Thumb bit
 * cleared (so it can be used as a UC_HOOK_CODE address). Used to shim the
 * firmware's ra8_eth_* frame API for the virtual network peer. Returns 0 if the
 * symbol (or a symbol table) is absent.
 *
 * @param[in]  elf      Mapped ELF image.
 * @param[in]  len      ELF image length in bytes.
 * @param[in]  name     NUL-terminated symbol name to find.
 * @param[out] size_out If non-NULL, receives the symbol's st_size (0 if absent).
 * @return Even (Thumb-cleared) symbol address, or 0 if not found.
 */
static uint32_t elf_sym_addr(const uint8_t* elf, long len, const char* name, uint32_t* size_out)
{
  if (size_out != nullptr) {
    *size_out = 0U;
  }
  if (len < (long)k_elf_ehdr_size) {
    return 0U;
  }
  uint32_t shoff = 0U;
  (void)memcpy(&shoff, elf + 32, 4);
  const uint16_t shentsize = (uint16_t)(elf[46] | (elf[47] << 8));
  const uint16_t shnum     = (uint16_t)(elf[48] | (elf[49] << 8));
  const size_t   nlen      = strlen(name) + 1U;
  if ((shoff == 0U) || (shentsize < (uint32_t)k_elf_shentsize_min)) {
    return 0U;
  }
  for (uint16_t i = 0U; i < shnum; i++) {
    const uint8_t* sh = elf + shoff + ((uint32_t)i * shentsize);
    if (((size_t)(sh - elf) + (size_t)k_elf_shentsize_min) > (size_t)len) {
      break;
    }
    uint32_t sh_type = 0U;
    (void)memcpy(&sh_type, sh + 4, 4);
    if (sh_type != 2U /* SHT_SYMTAB */) {
      continue;
    }
    uint32_t sym_off = 0U, sym_size = 0U, sym_link = 0U, sym_entsize = 0U;
    (void)memcpy(&sym_off, sh + 16, 4);
    (void)memcpy(&sym_size, sh + (uint32_t)k_elf_sh_size_off, 4);
    (void)memcpy(&sym_link, sh + (uint32_t)k_elf_sh_link_off, 4);
    (void)memcpy(&sym_entsize, sh + (uint32_t)k_elf_sh_entsize_off, 4);
    if ((sym_entsize < 16U) || (sym_link >= shnum)) {
      continue;
    }
    const uint8_t* strsh   = elf + shoff + ((uint32_t)sym_link * shentsize);
    uint32_t       str_off = 0U;
    (void)memcpy(&str_off, strsh + 16, 4);
    const uint32_t nsym = sym_size / sym_entsize;
    for (uint32_t s = 0U; s < nsym; s++) {
      const uint8_t* sym = elf + sym_off + (s * sym_entsize);
      if (((size_t)(sym - elf) + 16U) > (size_t)len) {
        break;
      }
      uint32_t st_name = 0U, st_value = 0U, st_size = 0U;
      (void)memcpy(&st_name, sym + 0, 4);
      (void)memcpy(&st_value, sym + 4, 4);
      (void)memcpy(&st_size, sym + 8, 4);
      const size_t pos = (size_t)str_off + (size_t)st_name;
      if ((st_name == 0U) || ((pos + nlen) > (size_t)len)) {
        continue;
      }
      if (memcmp(elf + pos, name, nlen) == 0) {
        if (size_out != nullptr) {
          *size_out = st_size;
        }
        return st_value & ~1U; /* clear the Thumb bit for the hook address. */
      }
    }
  }
  return 0U;
}

/** @brief Read a 32-bit little-endian word from emulated memory. */
static uint32_t rd32(uc_engine* uc, uint64_t addr)
{
  uint32_t v = 0U;
  (void)uc_mem_read(uc, addr, &v, sizeof(v));
  return v;
}

/** @brief Write a 32-bit little-endian word to emulated memory. */
static void wr32(uc_engine* uc, uint64_t addr, uint32_t v)
{
  (void)uc_mem_write(uc, addr, &v, sizeof(v));
}

/**
 * @brief Data Watchpoint and Trace (DWT) cycle-counter register model.
 *
 * @details
 * The Armv8-M DWT unit exposes a free-running 32-bit cycle counter (CYCCNT)
 * that the firmware uses as a PRIMASK-immune busy-wait time base: `ra8_delay_ms`
 * (libs/ra8_core/src/ra8_time.c) spins on CYCCNT whenever interrupts are masked,
 * which is exactly the early bring-up window (SystemInit runs `cpsid i` and only
 * `ra8_isr_globals_enable` clears PRIMASK later). The PPB is mapped as plain RAM
 * here, so without a model CYCCNT reads a constant 0, the delta never reaches
 * the target, and any masked-context `ra8_delay_ms` spins the whole run budget --
 * which is what stalled `pdm_mic_demo` and `camera_capture` before their first
 * banner. These constants let ::dwt_cyccnt_advance model the counter's advance.
 */
typedef enum : uint64_t {
  k_dwt_ctrl_addr        = 0xE0001000UL, /**< DWT_CTRL (bit 0 CYCCNTENA).            */
  k_dwt_cyccnt_addr      = 0xE0001004UL, /**< DWT_CYCCNT free-running cycle counter. */
  k_dwt_ctrl_cyccntena   = 0x00000001UL, /**< DWT_CTRL.CYCCNTENA: counter enable.    */
  k_dwt_cyccnt_per_chunk = 500000UL,     /**< Cycles charged per outer chunk (==     */
                                         /**< k_run_chunk_insns: retired insns ~= */
                                         /**< elapsed cycles at ~1 IPC). */
} dwt_model_t;

/**
 * @brief Advance the DWT cycle counter by one outer chunk's worth of cycles.
 *
 * @details
 * Models DWT_CYCCNT as the free-running cycle counter the Armv8-M architecture
 * (DDI0553 D1.2.1) specifies: it counts only while `DEMCR.TRCENA` and
 * `DWT_CTRL.CYCCNTENA` are both set. `ra8_time_init` arms both bits, so once the
 * firmware has initialised its time base the counter advances; an app that never
 * enables the cycle counter sees CYCCNT stay at its firmware-written value (zero
 * by default), so this model is inert for every such app and cannot regress it.
 *
 * The per-chunk increment (::k_dwt_cyccnt_per_chunk) equals the busy chunk's
 * instruction budget, i.e. one chunk of execution is charged one chunk of
 * cycles (~1 instruction per cycle on the M85). A read-modify-write is used so a
 * firmware CYCCNT reset (a `DWT->CYCCNT = 0` at init) is honoured and the count
 * simply resumes from there. Called once per outer chunk, in lockstep with the
 * SysTick period the run loop already advances, so time bases stay consistent.
 *
 * @param[in,out] uc Active Unicorn engine (CYCCNT and its enables live in PPB
 *                   RAM, read/written through @p uc).
 * @return Nothing.
 *
 * @pre @p uc has stopped at an instruction boundary (outer-chunk cadence).
 * @pre The PPB (DEMCR / DWT_CTRL / DWT_CYCCNT) is mapped as RAM.
 * @post CYCCNT is advanced iff the trace subsystem and cycle counter are enabled.
 * @post No PPB word other than DWT_CYCCNT is modified.
 * @note Not thread-safe; the run loop is single-threaded host-side.
 * @since 0.1.0
 */
static void dwt_cyccnt_advance(uc_engine* uc)
{
  const uint32_t demcr = rd32(uc, (uint64_t)k_scb_demcr_addr);
  if ((demcr & (uint32_t)k_scb_demcr_trcena) == 0U) {
    return; /* Trace subsystem off: CYCCNT does not count. */
  }
  const uint32_t ctrl = rd32(uc, (uint64_t)k_dwt_ctrl_addr);
  if ((ctrl & (uint32_t)k_dwt_ctrl_cyccntena) == 0U) {
    return; /* Cycle counter disabled: leave CYCCNT untouched. */
  }
  const uint32_t next = rd32(uc, (uint64_t)k_dwt_cyccnt_addr) + (uint32_t)k_dwt_cyccnt_per_chunk;
  wr32(uc, (uint64_t)k_dwt_cyccnt_addr, next);
}

/** @brief Read a Unicorn 32-bit register by its UC_ARM_REG_* id. */
static uint32_t reg_get(uc_engine* uc, int reg)
{
  uint32_t v = 0U;
  (void)uc_reg_read(uc, reg, &v);
  return v;
}

/** @brief Write a Unicorn 32-bit register by its UC_ARM_REG_* id. */
static void reg_set(uc_engine* uc, int reg, uint32_t v)
{
  (void)uc_reg_write(uc, reg, &v);
}

/** @brief Priority value (lower = higher) of the active handler, or sentinel. */
static uint32_t exc_active_prio(void)
{
  return (s_exc_depth == 0U) ? (uint32_t)k_exc_prio_none : s_exc_stack[s_exc_depth - 1U];
}

/**
 * @brief Read a system-handler priority byte from an SHPR register.
 *
 * @details
 * Cortex-M packs four 8-bit handler priorities per SHPRn word. SVCall (#11) is
 * byte 3 of SHPR2; PendSV (#14) is byte 2 and SysTick (#15) byte 3 of SHPR3.
 * ThreadX's tx_initialize_low_level programs these (SysTick 0x40, PendSV/SVC
 * 0xFF), and the value drives whether one exception may pre-empt another. The
 * PPB is plain RAM here, so the firmware's stores are simply read back.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[in]     exc_num Exception/vector number (11, 14, or 15).
 * @return The 8-bit configured priority (0 = highest, 0xFF = lowest).
 * @retval 0xFF when @p exc_num is not one of the modelled system handlers.
 *
 * @pre @p uc is an initialised engine with the PPB mapped as RAM.
 * @pre SystemInit / tx_initialize_low_level have programmed SHPR2/SHPR3.
 * @post No register or memory state is modified (read-only).
 * @post The returned value is in [0, 0xFF].
 * @note Sub-priority / priority grouping is ignored -- only the raw byte is
 *       compared, which is sufficient for the SysTick > PendSV nesting ThreadX
 *       relies on.
 * @since 0.1.0
 */
static uint32_t exc_priority(uc_engine* uc, uint32_t exc_num)
{
  if (exc_num == (uint32_t)k_exc_svcall) {
    return (rd32(uc, (uint64_t)k_scb_shpr2) >> (3U * (uint32_t)k_byte_bits)) &
           (uint32_t)k_byte_mask;
  }
  if (exc_num == (uint32_t)k_exc_pendsv) {
    return (rd32(uc, (uint64_t)k_scb_shpr3) >> (2U * (uint32_t)k_byte_bits)) &
           (uint32_t)k_byte_mask;
  }
  if (exc_num == (uint32_t)k_exc_systick) {
    return (rd32(uc, (uint64_t)k_scb_shpr3) >> (3U * (uint32_t)k_byte_bits)) &
           (uint32_t)k_byte_mask;
  }
  return (uint32_t)k_exc_prio_max;
}

/**
 * @brief Enter a Cortex-M exception: stack the basic frame and vector in.
 *
 * @details
 * Reproduces Armv7E-M / Armv8-M exception entry that Unicorn's core does not
 * model. The active stack is chosen exactly as hardware would: PSP when in
 * Thread mode with CONTROL.SPSEL set, else MSP. The 8-word basic frame
 * {R0,R1,R2,R3,R12,LR,PC,xPSR} is pushed with 8-byte alignment (the realign
 * pad is recorded in the stacked xPSR bit 9 so exit can undo it), the banked SP
 * is updated, the core is switched to Handler mode on MSP, LR is loaded with
 * the matching EXC_RETURN, IPSR is set to @p exc_num, and PC is vectored to the
 * handler fetched from the VTOR-relative table. The handler's priority is
 * pushed on the active-exception stack so nesting respects priority.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[in]     exc_num Exception number to take (11, 14, or 15).
 * @param[in]     handler Handler entry address (Thumb bit ignored).
 * @return Nothing.
 *
 * @pre @p uc has MSP/PSP/CONTROL/xPSR readable and the target stack mapped.
 * @pre Taking @p exc_num is permitted now (priority/PRIMASK already checked).
 * @post The core is in Handler mode (IPSR == @p exc_num) running on MSP.
 * @post LR holds a valid EXC_RETURN and the outgoing frame is on the old stack.
 * @note When CONTROL.FPCA is set, the FP extended frame (S0-S15 + FPSCR) is
 *       stacked above the basic frame and EXC_RETURN bit4 (FType) is cleared.
 * @since 0.1.0
 */
static void exc_enter(uc_engine* uc, uint32_t exc_num, uint32_t handler)
{
  const uint32_t xpsr_in   = reg_get(uc, UC_ARM_REG_XPSR);
  const uint32_t control   = reg_get(uc, UC_ARM_REG_CONTROL);
  const bool     in_thread = (xpsr_in & (uint32_t)k_xpsr_ipsr_mask) == 0U;
  const bool     use_psp   = in_thread && ((control & (uint32_t)k_control_spsel) != 0U);
  const bool     fp_active = (control & (uint32_t)k_control_fpca) != 0U;

  const int sp_reg = use_psp ? UC_ARM_REG_PSP : UC_ARM_REG_MSP;
  uint32_t  sp     = reg_get(uc, sp_reg);

  /* Hardware aligns the stack pointer to 8 bytes on entry and flags the pad in
   * the stacked xPSR (bit 9) so the matching exception return can remove it. */
  uint32_t frame_xpsr = xpsr_in;
  if ((sp & 0x4U) != 0U) {
    sp -= 4U;
    frame_xpsr |= (uint32_t)k_xpsr_align9;
  } else {
    frame_xpsr &= ~(uint32_t)k_xpsr_align9;
  }
  sp -= (uint32_t)k_exc_frame_bytes;
  if (fp_active) {
    sp -= (uint32_t)k_fp_frame_extra;
  }

  wr32(uc, (uint64_t)sp + 0U, reg_get(uc, UC_ARM_REG_R0));
  wr32(uc, (uint64_t)sp + 4U, reg_get(uc, UC_ARM_REG_R1));
  wr32(uc, (uint64_t)sp + 8U, reg_get(uc, UC_ARM_REG_R2));
  wr32(uc, (uint64_t)sp + (uint64_t)k_frame_off_r3, reg_get(uc, UC_ARM_REG_R3));
  wr32(uc, (uint64_t)sp + 16U, reg_get(uc, UC_ARM_REG_R12));
  wr32(uc, (uint64_t)sp + (uint64_t)k_frame_off_lr, reg_get(uc, UC_ARM_REG_LR));
  wr32(uc, (uint64_t)sp + (uint64_t)k_frame_off_pc, reg_get(uc, UC_ARM_REG_PC));
  wr32(uc, (uint64_t)sp + (uint64_t)k_frame_off_xpsr, frame_xpsr);

  /* Armv8-M FP extended frame: S0-S15 + FPSCR sit directly above the 8-word
   * basic frame (ThreadX's PendSV adds S16-S31 on top of this). */
  if (fp_active) {
    for (uint32_t i = 0U; i < (uint32_t)k_fp_s_words; i++) {
      wr32(uc,
           (uint64_t)sp + (uint64_t)k_frame_off_s0 + (uint64_t)(i * (uint32_t)k_word_bytes),
           reg_get(uc, UC_ARM_REG_S0 + (int)i));
    }
    wr32(uc, (uint64_t)sp + (uint64_t)k_frame_off_fpscr, reg_get(uc, UC_ARM_REG_FPSCR));
  }

  /* Commit the new value of whichever stack the frame went onto. */
  reg_set(uc, sp_reg, sp);

  /* EXC_RETURN encodes where to unstack: Thread/PSP, Thread/MSP, or (when an
   * exception pre-empts another) Handler/MSP. */
  uint32_t exc_ret;
  if (!in_thread) {
    exc_ret = (uint32_t)k_exc_ret_handler;
  } else if (use_psp) {
    exc_ret = (uint32_t)k_exc_ret_psp;
  } else {
    exc_ret = (uint32_t)k_exc_ret_msp;
  }
  if (fp_active) {
    exc_ret &= ~(uint32_t)k_exc_ret_ftype; /* FType=0: an FP frame was stacked. */
  }

  /* Handler mode always runs on MSP with CONTROL.SPSEL clear. */
  reg_set(uc, UC_ARM_REG_CONTROL, control & ~(uint32_t)k_control_spsel);
  reg_set(uc, UC_ARM_REG_SP, reg_get(uc, UC_ARM_REG_MSP));

  uint32_t handler_xpsr =
    (xpsr_in & ~(uint32_t)k_xpsr_ipsr_mask) | (exc_num & (uint32_t)k_xpsr_ipsr_mask);
  handler_xpsr |= (uint32_t)k_xpsr_t_bit; /* M-profile is always Thumb. */
  reg_set(uc, UC_ARM_REG_XPSR, handler_xpsr);
  reg_set(uc, UC_ARM_REG_LR, exc_ret);
  reg_set(uc, UC_ARM_REG_PC, handler & ~1U);

  if (s_exc_depth < (uint32_t)k_exc_nest_max) {
    s_exc_stack[s_exc_depth] = exc_priority(uc, exc_num);
    s_exc_depth++;
  }
}

/**
 * @brief Perform a Cortex-M exception return for an observed EXC_RETURN branch.
 *
 * @details
 * The inverse of ::exc_enter. @p exc_return (the magic value the core branched
 * to) selects the stack to unstack from (bit2: PSP vs MSP) and the mode to
 * return to (bit3: Thread vs Handler). The 8-word basic frame is popped (plus
 * the S0-S15 + FPSCR words when FType, bit4, is clear), the recorded 8-byte
 * realignment (stacked xPSR bit 9) is undone, the banked SP and CONTROL.SPSEL
 * are restored, xPSR (hence IPSR) is reloaded, the active-exception stack is
 * popped, and PC resumes the interrupted instruction stream.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[in]     exc_ret The EXC_RETURN value (prefix bits[31:7] set) returned to.
 * @return Nothing.
 *
 * @pre @p uc is in Handler mode with a valid basic frame on the indicated stack.
 * @pre @p exc_ret has the EXC_RETURN prefix (bits[31:7] all set).
 * @post The core has resumed the unstacked context (PC/SP/xPSR restored).
 * @post The active-exception nesting depth has decreased by one (if non-zero).
 * @note When FType (bit4) is clear, the FP extended frame (S0-S15 + FPSCR) is
 *       unstacked too, matching ::exc_enter.
 * @since 0.1.0
 */
static void exc_return(uc_engine* uc, uint32_t exc_ret)
{
  const bool to_psp    = (exc_ret & (uint32_t)k_exc_ret_spsel) != 0U;
  const bool to_thread = (exc_ret & (uint32_t)k_exc_ret_mode) != 0U;
  const bool fp_frame  = (exc_ret & (uint32_t)k_exc_ret_ftype) == 0U;
  const int  sp_reg    = to_psp ? UC_ARM_REG_PSP : UC_ARM_REG_MSP;
  uint32_t   sp        = reg_get(uc, sp_reg);

  const uint32_t r0   = rd32(uc, (uint64_t)sp + 0U);
  const uint32_t r1   = rd32(uc, (uint64_t)sp + 4U);
  const uint32_t r2   = rd32(uc, (uint64_t)sp + 8U);
  const uint32_t r3   = rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_r3);
  const uint32_t r12  = rd32(uc, (uint64_t)sp + 16U);
  const uint32_t lr   = rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_lr);
  const uint32_t pc   = rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_pc);
  const uint32_t xpsr = rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_xpsr);

  /* Armv8-M FP extended frame (EXC_RETURN bit4 clear): S0-S15 + FPSCR sit above
   * the basic frame. Restore them so the thread's scalar FP state survives, then
   * account for those words when popping. PC/xPSR keep their basic-frame offsets. */
  if (fp_frame) {
    for (uint32_t i = 0U; i < (uint32_t)k_fp_s_words; i++) {
      reg_set(
        uc,
        UC_ARM_REG_S0 + (int)i,
        rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_s0 + (uint64_t)(i * (uint32_t)k_word_bytes)));
    }
    reg_set(uc, UC_ARM_REG_FPSCR, rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_fpscr));
  }

  sp += (uint32_t)k_exc_frame_bytes;
  if (fp_frame) {
    sp += (uint32_t)k_fp_frame_extra;
  }
  if ((xpsr & (uint32_t)k_xpsr_align9) != 0U) {
    sp += 4U; /* undo the entry-time 8-byte realignment pad */
  }
  reg_set(uc, sp_reg, sp);

  reg_set(uc, UC_ARM_REG_R0, r0);
  reg_set(uc, UC_ARM_REG_R1, r1);
  reg_set(uc, UC_ARM_REG_R2, r2);
  reg_set(uc, UC_ARM_REG_R3, r3);
  reg_set(uc, UC_ARM_REG_R12, r12);
  reg_set(uc, UC_ARM_REG_LR, lr);

  /* Restore mode: on return to Thread, CONTROL.SPSEL follows EXC_RETURN bit2;
   * on return to a pre-empted handler the core stays on MSP. */
  uint32_t control = reg_get(uc, UC_ARM_REG_CONTROL);
  if (to_thread && to_psp) {
    control |= (uint32_t)k_control_spsel;
  } else {
    control &= ~(uint32_t)k_control_spsel;
  }
  reg_set(uc, UC_ARM_REG_CONTROL, control);

  uint32_t new_xpsr = xpsr | (uint32_t)k_xpsr_t_bit;
  if (to_thread) {
    new_xpsr &= ~(uint32_t)k_xpsr_ipsr_mask; /* Thread mode: IPSR == 0 */
  }
  reg_set(uc, UC_ARM_REG_XPSR, new_xpsr);

  /* Active SP becomes whichever stack the returned-to context uses. */
  reg_set(uc, UC_ARM_REG_SP, reg_get(uc, to_psp && to_thread ? UC_ARM_REG_PSP : UC_ARM_REG_MSP));
  reg_set(uc, UC_ARM_REG_PC, pc & ~1U);

  if (s_exc_depth > 0U) {
    s_exc_depth--;
  }
}

/**
 * @brief True if @p pc is an EXC_RETURN magic value.
 *
 * @details Matches the Armv8-M EXC_RETURN prefix -- bits[31:7] all set
 * (0xFFFFFF80..0xFFFFFFFF). This covers both the Armv7-M values board_sim
 * itself generates (0xFFFFFFF1/F9/FD) and the Armv8-M Non-Secure thread
 * returns ThreadX uses (0xFFFFFFBC basic, 0xFFFFFFAC with an FP frame), where
 * bit6 (S) is clear. Nothing in this firmware's map executes at 0xFFFFFFxx, so
 * a fetch into that range is always an exception return, never a real branch.
 */
static bool is_exc_return(uint64_t pc)
{
  return (pc & (uint64_t)k_exc_ret_v8_mask) == (uint64_t)k_exc_ret_v8_mask;
}

/**
 * @brief True if @p pc sits in a wait-for-interrupt spin (the core is idle).
 *
 * @details Reports whether the core at @p pc is parked in a loop that can only
 * make progress once an interrupt arrives -- genuine idle, where the next thing
 * that can happen is the periodic SysTick. Two cases are recognised:
 *   1. The instruction AT @p pc is itself a halt: `b .` (0xE7FE, branch-to-self)
 *      or `wfi` (0xBF30).
 *   2. @p pc is ENCLOSED by a wait-for-interrupt poll loop: scanning forward a
 *      few halfwords finds an unconditional backward `b.n` (the loop back-edge)
 *      whose target is at or before @p pc (so the loop wraps around @p pc), and
 *      the loop body holds a `wfi` or a `cpsie i` -- the "re-enable interrupts
 *      and poll" idiom ThreadX's __tx_ts_wait uses (cpsid/ldr/str/cbnz/cpsie/
 *      b .-N, spinning on execute_ptr until a tick makes a thread runnable).
 *
 * The enclosing-loop test is deliberately tight: it requires the back-edge to
 * bracket @p pc, so STRAIGHT-LINE code is never matched even when it sits in
 * memory next to an idle loop (an ISR returns via `bx lr`, not a backward
 * branch over itself -- matching a nearby opcode would wrongly truncate it).
 * A compute/busy loop is also excluded: it exits on a conditional branch and
 * never re-enables interrupts mid-loop, so it carries no wfi/cpsie wait. The
 * run loop uses this to cap the idle chunk's budget to ::k_idle_spin_insns
 * instead of spinning a full ::k_run_chunk_insns to reach the same already-armed
 * tick. Tick COUNT is unchanged; only idle wall-time is skipped.
 *
 * @param[in,out] uc Unicorn engine (instructions are read from its memory).
 * @param[in]     pc Program counter to inspect (Thumb bit ignored).
 * @return true if @p pc is on, or enclosed by, a wait-for-interrupt idle loop.
 *
 * @pre @p uc has the code region containing @p pc mapped.
 * @pre @p pc is halfword-aligned once the Thumb bit is cleared.
 * @post @p uc is unchanged (a read-only probe).
 * @note Detection only; advancing time stays the run loop's job, so the tick
 *       count -- and every tick-based sleep/heartbeat deadline -- is preserved.
 * @since 0.1.0
 */
static bool idle_spin_at(uc_engine* uc, uint32_t pc)
{
  const uint32_t aligned = pc & ~1U;
  uint16_t       hw0     = 0U;
  if (uc_mem_read(uc, (uint64_t)aligned, &hw0, sizeof(hw0)) != UC_ERR_OK) {
    return false;
  }
  /* Case 1: the core is literally on a halt instruction. */
  if ((hw0 == (uint16_t)k_op_branch_self) || (hw0 == (uint16_t)k_op_wfi)) {
    return true;
  }

  /* Case 2: scan forward for the loop's unconditional backward back-edge. */
  for (uint32_t j = 0U; j < (uint32_t)k_idle_scan_fwd; j++) {
    const uint32_t at = aligned + (j * (uint32_t)k_thumb_hw_bytes);
    uint16_t       hw = 0U;
    if (uc_mem_read(uc, (uint64_t)at, &hw, sizeof(hw)) != UC_ERR_OK) {
      return false;
    }
    if ((hw & (uint16_t)k_op_bn_mask) != (uint16_t)k_op_bn_base) {
      continue; /* not an unconditional b.n -- still inside the loop body */
    }
    /* Unconditional b.n: target = at + 4 + sign_extend(imm11) * 2. */
    const uint32_t imm11 = (uint32_t)(hw & (uint16_t)k_op_bn_imm);
    const int32_t  off =
      (int32_t)(imm11 << (uint32_t)k_bn_imm_sext_shl) >> (int32_t)k_bn_imm_sext_shr;
    if (off >= 0) {
      return false; /* forward branch -- not a spin-in-place back-edge */
    }
    const uint32_t target = at + (uint32_t)k_thumb2_insn_bytes + (uint32_t)off;
    if ((target > aligned) || ((aligned - target) > (uint32_t)k_idle_loop_max)) {
      return false; /* back-edge does not bracket pc in a tight loop */
    }
    /* pc is enclosed by [target, at]: require a wait (cpsie/wfi) in the body. */
    for (uint32_t k = target; k <= at; k += (uint32_t)k_thumb_hw_bytes) {
      uint16_t bhw = 0U;
      if (uc_mem_read(uc, (uint64_t)k, &bhw, sizeof(bhw)) != UC_ERR_OK) {
        return false;
      }
      if ((bhw == (uint16_t)k_op_cpsie_i) || (bhw == (uint16_t)k_op_wfi)) {
        return true;
      }
    }
    return false; /* tight loop but no wait signature -- a busy loop, not idle */
  }
  return false;
}

/**
 * @brief Synthesise a MemManage (#4) fault for a trapped RO-region write.
 *
 * @details
 * Called by the run loop after ::on_mpu_ro_write latched a violation. Latches
 * CFSR.MMFSR.DACCVIOL + MMARVALID and MMFAR (so a fault handler -- and the HIL
 * alive probe -- see the architectural status), forces PC back to the faulting
 * store so exc_enter stacks *that* address (a recovering handler skips exactly
 * one store), and vectors into the application's MemManage_Handler. If no
 * handler is installed the violation is dropped (no escalation modelled).
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector base if VTOR reads as 0.
 * @return Nothing.
 *
 * @pre ::s_mpu_fault_pc / ::s_mpu_fault_addr hold the trapped store.
 * @pre The PPB CFSR / MMFAR words and the vector table are mapped as RAM.
 * @post On a valid vector, the core is in the MemManage handler with the
 *       basic frame stacked (stacked PC == the faulting store).
 * @post CFSR.MMFSR and MMFAR reflect a data-access violation.
 * @note Faithful to the recovering-handler contract of mpu_partition_simple.
 * @since 0.1.0
 */
static void mpu_synth_memmanage(uc_engine* uc, uint32_t vtor_base)
{
  const uint32_t cfsr = rd32(uc, (uint64_t)k_scb_cfsr) | (uint32_t)k_cfsr_mmfsr_daccviol |
                        (uint32_t)k_cfsr_mmfsr_mmarvalid;
  wr32(uc, (uint64_t)k_scb_cfsr, cfsr);
  wr32(uc, (uint64_t)k_scb_mmfar, s_mpu_fault_addr);
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &s_mpu_fault_pc);
  const uint32_t handler = exc_vector(uc, vtor_base, (uint32_t)k_exc_memmanage);
  if (handler != 0U) {
    exc_enter(uc, (uint32_t)k_exc_memmanage, handler);
  }
}

/*
 * =============================================================================
 * Divide-by-zero UsageFault (CCR.DIV_0_TRP) -- CPU-model seam.
 *
 * Unicorn's Cortex-M core executes UDIV/SDIV with the Arm default divide-by-zero
 * result (quotient 0) and never raises the UsageFault that real Armv7-M/Armv8-M
 * silicon takes when CCR.DIV_0_TRP is set. board_sim closes that gap by scanning
 * the image for every UDIV/SDIV site and -- only after the firmware sets
 * CCR.DIV_0_TRP (watched via on_scb_ctrl_write) -- overwriting those sites with an
 * undefined instruction (UDF). Each divide then traps through the already-armed
 * ::on_invalid_insn hook into ::emulate_div0_patched, which either takes a decoded
 * UsageFault (divisor zero) or emulates the divide in software (::div0_quotient)
 * and continues. Patching -- rather than a per-site UC_HOOK_CODE -- is the point:
 * a code hook disables Unicorn's engine-wide block chaining and would roughly
 * quarter throughput for ANY busy firmware that arms the trap, whereas the invalid
 * hook has no such cost. A firmware that never opts in keeps its native divides
 * and pays nothing. Faithful in both directions: no faked trap, no masked one.
 * =============================================================================
 */

/** @brief UDIV/SDIV decode masks + fault field bits for the div-0 seam. */
typedef enum : uint32_t {
  k_div0_hw1_mask     = 0xFFF0U,     /**< hw1[15:4] selects the divide opcode.    */
  k_div0_hw1_udiv     = 0xFBB0U,     /**< UDIV T1: hw1[15:4] == 0xFBB.            */
  k_div0_hw1_sdiv     = 0xFB90U,     /**< SDIV T1: hw1[15:4] == 0xFB9.            */
  k_div0_hw2_mask     = 0xF0F0U,     /**< hw2[15:12] and hw2[7:4] must be 1111.   */
  k_div0_hw2_fixed    = 0xF0F0U,     /**< Their required value for a real divide. */
  k_div0_reg_mask     = 0x000FU,     /**< 4-bit register field (Rn / Rm / Rd).    */
  k_div0_rd_shift     = 8U,          /**< hw2[11:8] = Rd (destination register).  */
  k_div0_reg_sp       = 13U,         /**< r13 (SP): UNPREDICTABLE as UDIV d/n/m.  */
  k_div0_reg_pc       = 15U,         /**< r15 (PC): UNPREDICTABLE as UDIV d/n/m.  */
  k_div0_insn_len     = 4U,          /**< UDIV/SDIV are 32-bit Thumb-2.           */
  k_div0_cfsr_divzero = 1U << 25U,   /**< CFSR.UFSR.DIVBYZERO (0x02000000).       */
  k_div0_int32_min    = 0x80000000U, /**< INT32_MIN: the SDIV / -1 overflow edge. */
  k_div0_udf_b0       = 0xF0U,       /**< UDF.W #0 little-endian byte 0.          */
  k_div0_udf_b1       = 0xF7U,       /**< UDF.W #0 little-endian byte 1.          */
  k_div0_udf_b3       = 0xA0U,       /**< UDF.W #0 little-endian byte 3.          */
} div0_field_t;

/**
 * @brief UDF.W #0 -- a permanently-undefined 32-bit Thumb-2 instruction (LE bytes).
 *
 * @details Armv8-M ``UDF.W #0`` is 0xF7F0A000; stored little-endian it is the
 * byte sequence written over an armed divide so its execution raises the
 * undefined-instruction trap that ::emulate_div0_patched services. Byte 2 is 0.
 */
static const uint8_t k_div0_udf[k_div0_insn_len] = {
  (uint8_t)k_div0_udf_b0,
  (uint8_t)k_div0_udf_b1,
  0U,
  (uint8_t)k_div0_udf_b3,
};

/**
 * @struct div0_site_t
 * @brief One tracked UDIV/SDIV site: its address and original encoding halfwords.
 * @details The original encoding is kept so ::emulate_div0_patched can recover the
 * operand registers after the site's bytes have been overwritten with UDF.
 */
typedef struct {
  uint32_t va;  /**< Divide-instruction virtual address. */
  uint16_t hw1; /**< Original first halfword.            */
  uint16_t hw2; /**< Original second halfword.           */
} div0_site_t;

/** @brief Div-0 seam sizing: max tracked divide sites (real counts tiny). */
enum : uint32_t {
  k_div0_sites_max = 4096U,
};
static div0_site_t s_div0_site[k_div0_sites_max]; /**< Tracked divide sites.        */
static uint32_t    s_div0_site_n;                 /**< Count of tracked sites.      */
static bool        s_div0_armed;                  /**< Armed sites overwritten UDF. */

/**
 * @brief Decode a Thumb-2 halfword pair as UDIV/SDIV, recovering all operands.
 *
 * @details
 * Matches the T1 encodings ``1111 1011 1011 Rn : 1111 Rd 1111 Rm`` (UDIV) and
 * ``1111 1011 1001 Rn : 1111 Rd 1111 Rm`` (SDIV) from the Armv8-M Architecture
 * Reference Manual, recovering Rn (dividend), Rd (destination), Rm (divisor) and
 * whether the divide is signed. The two fixed nibbles in hw2 ([15:12] and [7:4]
 * == 1111) are verified so a scan false-positive on data cannot be taken as a
 * divide.
 *
 * The architecture makes UDIV/SDIV with d, n, or m == 13 (SP) or 15 (PC)
 * UNPREDICTABLE, so a real compiler never emits those encodings; a match with
 * any of Rn/Rd/Rm in {13, 15} is therefore a scan false-positive. This matters
 * because the sweep runs on 2-byte boundaries: the high nibbles of an adjacent
 * BL / B.W pair (each halfword 0xFxxx) read as "SDIV ..., ..., r15", and if that
 * window were armed, ::div0_patch_sites would overwrite the two straddled real
 * branches with UDF -- corrupting executed code, not an unreachable divide.
 * Rejecting the reserved register operands keeps the seam matching only the
 * encodings the core can actually take.
 *
 * @param[in]  hw1        First (low-address) instruction halfword.
 * @param[in]  hw2        Second instruction halfword.
 * @param[out] out_rn     Dividend register index [0, 15] on a match.
 * @param[out] out_rd     Destination register index [0, 15] on a match.
 * @param[out] out_rm     Divisor register index [0, 15] on a match.
 * @param[out] out_signed True for SDIV, false for UDIV, on a match.
 *
 * @return Whether @p hw1 / @p hw2 encode a UDIV or SDIV.
 * @retval true  A divide was decoded; all outputs are valid.
 * @retval false Not a divide; the outputs are untouched.
 *
 * @pre All output pointers are non-NULL.
 * @pre @p hw1 / @p hw2 are the two halfwords of one candidate site.
 * @post The outputs are written only when true is returned.
 * @post No engine or global state is mutated (pure).
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static bool udiv_sdiv_decode(uint16_t  hw1,
                             uint16_t  hw2,
                             uint32_t* out_rn,
                             uint32_t* out_rd,
                             uint32_t* out_rm,
                             bool*     out_signed)
{
  if ((out_rn == nullptr) || (out_rd == nullptr) || (out_rm == nullptr) ||
      (out_signed == nullptr)) {
    return false;
  }
  const bool is_udiv = ((uint32_t)hw1 & (uint32_t)k_div0_hw1_mask) == (uint32_t)k_div0_hw1_udiv;
  const bool is_sdiv = ((uint32_t)hw1 & (uint32_t)k_div0_hw1_mask) == (uint32_t)k_div0_hw1_sdiv;
  if (!is_udiv && !is_sdiv) {
    return false;
  }
  if (((uint32_t)hw2 & (uint32_t)k_div0_hw2_mask) != (uint32_t)k_div0_hw2_fixed) {
    return false;
  }
  const uint32_t rn = (uint32_t)hw1 & (uint32_t)k_div0_reg_mask;
  const uint32_t rd = ((uint32_t)hw2 >> (uint32_t)k_div0_rd_shift) & (uint32_t)k_div0_reg_mask;
  const uint32_t rm = (uint32_t)hw2 & (uint32_t)k_div0_reg_mask;
  /* d/n/m == SP or PC is UNPREDICTABLE for UDIV/SDIV: never a real divide, so
   * this window is a false positive straddling two instructions. Reject it so
   * the arming pass does not overwrite executed code with UDF. */
  if ((rn == (uint32_t)k_div0_reg_sp) || (rn == (uint32_t)k_div0_reg_pc) ||
      (rd == (uint32_t)k_div0_reg_sp) || (rd == (uint32_t)k_div0_reg_pc) ||
      (rm == (uint32_t)k_div0_reg_sp) || (rm == (uint32_t)k_div0_reg_pc)) {
    return false;
  }
  *out_rn     = rn;
  *out_rd     = rd;
  *out_rm     = rm;
  *out_signed = is_sdiv;
  return true;
}

/**
 * @brief Compute a UDIV/SDIV quotient with the Arm div-by-zero + overflow rules.
 *
 * @param[in] vn        Dividend value.
 * @param[in] vm        Divisor value (already known non-trapping: non-zero, or
 *                      DIV_0_TRP clear so a zero divisor yields the Arm default 0).
 * @param[in] is_signed True for SDIV, false for UDIV.
 * @return The 32-bit quotient the core would have produced.
 * @retval 0 When @p vm is zero (Arm default divide-by-zero result).
 *
 * @pre None.
 * @pre None.
 * @post No state is mutated (pure).
 * @post Signed INT32_MIN / -1 saturates to INT32_MIN (Arm SDIV overflow rule).
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static uint32_t div0_quotient(uint32_t vn, uint32_t vm, bool is_signed)
{
  if (vm == 0U) {
    return 0U; /* Arm default when DIV_0_TRP is clear. */
  }
  if (!is_signed) {
    return vn / vm;
  }
  if ((vn == (uint32_t)k_div0_int32_min) && ((int32_t)vm == -1)) {
    return (uint32_t)k_div0_int32_min; /* Arm SDIV overflow: INT32_MIN / -1 = INT32_MIN. */
  }
  return (uint32_t)((int32_t)vn / (int32_t)vm);
}

/**
 * @brief Service an undefined-instruction trap that landed on an armed divide.
 *
 * @details
 * Called from ::on_invalid_insn. Divide-by-zero trapping is modelled by
 * overwriting each divide with UDF once the firmware sets CCR.DIV_0_TRP -- unlike
 * a per-site UC_HOOK_CODE this does NOT disable Unicorn's translation-block
 * chaining, so a firmware that arms the trap runs its steady state at the baseline
 * rate. When @p pc is one of those patched sites this recovers the original
 * encoding, reads the operands, and either (a) latches ::s_div0_fault when the
 * divisor is zero and DIV_0_TRP is set -- so the run loop synthesises the decoded
 * UsageFault with @p pc stacked -- or (b) computes the quotient in software
 * (::div0_quotient), writes Rd and steps PC past the 4-byte instruction. A trap at
 * any other address is not ours.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   Address of the trapping instruction.
 * @param[in]     code The 4 bytes at @p pc (the UDF, unused -- the original
 *                     encoding comes from ::s_div0_site).
 * @return Whether the trap was an armed divide this handler serviced.
 * @retval true  @p pc was a patched divide; register/PC state (or the fault latch)
 *               was updated and the caller should stop + resume.
 * @retval false @p pc is not a patched divide; try the next handler.
 *
 * @pre ::s_div0_site[0 .. s_div0_site_n) hold the armed sites.
 * @pre The PPB CCR word is mapped as RAM.
 * @post On a trapping div-0 ::s_div0_fault is set with @p pc; else Rd and PC are
 *       advanced.
 * @note Not thread-safe (single engine).
 * @since 0.1.0
 */
static bool emulate_div0_patched(uc_engine* uc, uint32_t pc, const uint8_t code[4])
{
  (void)code;
  const div0_site_t* site = nullptr;
  for (uint32_t i = 0U; i < s_div0_site_n; i++) {
    if (s_div0_site[i].va == pc) {
      site = &s_div0_site[i];
      break;
    }
  }
  if (site == nullptr) {
    return false; /* not an armed divide -- let the other invalid-insn handlers try. */
  }
  uint32_t rn   = 0U;
  uint32_t rd   = 0U;
  uint32_t rm   = 0U;
  bool     sign = false;
  if (!udiv_sdiv_decode(site->hw1, site->hw2, &rn, &rd, &rm, &sign)) {
    return false; /* a tracked non-divide (scan false-positive): not ours to service. */
  }
  uint32_t vn = 0U;
  uint32_t vm = 0U;
  (void)uc_reg_read(uc, k_arm_reg_id[rn], &vn);
  (void)uc_reg_read(uc, k_arm_reg_id[rm], &vm);
  const uint32_t ccr = rd32(uc, (uint64_t)k_scb_ccr);
  if ((vm == 0U) && ((ccr & (uint32_t)k_ccr_div_0_trp) != 0U)) {
    s_div0_fault    = true;
    s_div0_fault_pc = pc; /* stacked PC == the divide, as real hardware does.          */
    return true;          /* PC left at the site; the run loop synthesises UsageFault. */
  }
  const uint32_t q = div0_quotient(vn, vm, sign);
  (void)uc_reg_write(uc, k_arm_reg_id[rd], &q);
  uint32_t next = pc + (uint32_t)k_div0_insn_len;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  /* Emulating one divide consumes no modelled time: mark the stop a zero-time
   * seam relaunch so the inner run loop re-enters at `next` WITHOUT advancing
   * SysTick or charging an outer chunk (like on_sdmmc_read_block). Without this,
   * a divide in a hot loop (e.g. ra8_keycache_put's modulo hashing) charges one
   * of the 40000 chunks per execution, exhausting the budget mid-render. The
   * zero-divisor trap path above returns earlier and is handled by the
   * s_div0_fault branch, which is already zero-time. */
  s_seam_relaunch = true;
  return true;
}

/**
 * @brief Overwrite every tracked divide with UDF so divide-by-zero can trap.
 *
 * @details
 * Called from ::on_scb_ctrl_write the first time the firmware sets CCR.DIV_0_TRP.
 * The patch is deferred to opt-in so a firmware that never arms the trap keeps its
 * original divides (native, quotient-0 semantics) and pays nothing. Patching to
 * UDF -- rather than installing a UC_HOOK_CODE at each site -- is deliberate:
 * UC_HOOK_CODE disables Unicorn's engine-wide block chaining and would roughly
 * quarter the throughput of any busy loop in a DIV_0_TRP firmware, whereas the
 * already-armed undefined-instruction hook (::on_invalid_insn) has no such cost.
 * Idempotent via ::s_div0_armed. Re-applied after a warm reboot re-loads the image
 * (see the run loop's reboot paths, which clear ::s_div0_armed).
 *
 * @param[in,out] uc Unicorn engine whose memory is patched.
 * @return Nothing.
 *
 * @pre ::s_div0_site[0 .. s_div0_site_n) hold valid divide-site addresses.
 * @pre @p uc permits uc_mem_write to the (host-side) code image.
 * @post ::s_div0_armed is true and each site holds ::k_div0_udf.
 * @post A no-op when already armed.
 * @note Not thread-safe (single engine).
 * @since 0.1.0
 */
static void div0_patch_sites(uc_engine* uc)
{
  if (s_div0_armed) {
    return;
  }
  for (uint32_t i = 0U; i < s_div0_site_n; i++) {
    (void)uc_mem_write(uc, (uint64_t)s_div0_site[i].va, k_div0_udf, sizeof(k_div0_udf));
  }
  s_div0_armed = true;
  (void)fprintf(stderr,
                "  div-0 seam: CCR.DIV_0_TRP set -- patched %u UDIV/SDIV site(s)\n",
                (unsigned)s_div0_site_n);
}

/**
 * @brief Scan the image for UDIV/SDIV sites so the div-0 trap can arm later.
 *
 * @details
 * Walks the ELF32 PT_LOAD executable segments on 2-byte boundaries for the
 * UDIV/SDIV encoding (::udiv_sdiv_decode), recording each site's VMA (p_vaddr
 * based, so a ramfunc is tracked at its execution address) and its original
 * halfwords. Nothing is patched here; the always-on SCB control-write watcher
 * (::on_scb_ctrl_write) overwrites the sites with UDF via ::div0_patch_sites only
 * if the firmware sets CCR.DIV_0_TRP. Tracked for every core (UDIV/SDIV exist on
 * the M85 and the M33 alike). A scan false-positive is harmless: the site is only
 * ever patched after opt-in, and ::emulate_div0_patched re-decodes before acting.
 *
 * @param[in] elf In-memory ELF image (still alive at call time).
 * @param[in] len Length of @p elf in bytes.
 * @return Nothing.
 *
 * @pre @p elf is a 32-bit ARM ELF (already validated by load_elf).
 * @pre @p len is the true byte length of @p elf.
 * @post ::s_div0_site holds up to ::k_div0_sites_max tracked divide sites.
 * @post No site is patched yet (armed later by on_scb_ctrl_write).
 * @note Not thread-safe; call once during setup before the run loop.
 * @since 0.1.0
 */
static void div0_seam_install(const uint8_t* elf, long len)
{
  s_div0_site_n = 0U;
  s_div0_armed  = false;
  if ((elf == nullptr) || (len < (long)k_elf_ehdr_size)) {
    return;
  }
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + (uint32_t)k_elf_e_phoff_off, 4);
  const uint16_t phentsize = (uint16_t)(elf[42] | (elf[43] << 8));
  const uint16_t phnum     = (uint16_t)(elf[44] | (elf[45] << 8));
  if (((uint64_t)phoff + ((uint64_t)phnum * (uint64_t)phentsize)) > (uint64_t)len) {
    return; /* truncated program-header table -- refuse to walk past the file. */
  }
  for (uint16_t i = 0U; i < phnum; i++) {
    const uint8_t* ph       = elf + phoff + ((uint32_t)i * phentsize);
    uint32_t       p_type   = 0U;
    uint32_t       p_offset = 0U;
    uint32_t       p_vaddr  = 0U;
    uint32_t       p_filesz = 0U;
    uint32_t       p_flags  = 0U;
    (void)memcpy(&p_type, ph + 0, 4);
    (void)memcpy(&p_offset, ph + (uint32_t)k_elf_ph_offset_off, 4);
    (void)memcpy(&p_vaddr, ph + (uint32_t)k_elf_ph_vaddr_off, 4);
    (void)memcpy(&p_filesz, ph + (uint32_t)k_elf_ph_filesz_off, 4);
    (void)memcpy(&p_flags, ph + (uint32_t)k_elf_ph_flags_off, 4);
    if ((p_type != (uint32_t)k_elf_pt_load) || (p_filesz < (uint32_t)k_div0_insn_len) ||
        ((p_flags & (uint32_t)k_elf_pf_x) == 0U)) {
      continue;
    }
    if (((uint64_t)p_offset + (uint64_t)p_filesz) > (uint64_t)len) {
      continue; /* truncated / out-of-file segment -- skip. */
    }
    for (uint32_t off = 0U; (off + (uint32_t)k_div0_insn_len) <= p_filesz; off += 2U) {
      const uint8_t* p    = elf + p_offset + off;
      const uint16_t hw1  = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
      const uint16_t hw2  = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
      uint32_t       rn   = 0U;
      uint32_t       rd   = 0U;
      uint32_t       rm   = 0U;
      bool           sign = false;
      if (!udiv_sdiv_decode(hw1, hw2, &rn, &rd, &rm, &sign)) {
        continue;
      }
      if (s_div0_site_n >= (uint32_t)k_div0_sites_max) {
        (void)fprintf(stderr, "  div-0 seam: site cap %u reached\n", (unsigned)k_div0_sites_max);
        break;
      }
      s_div0_site[s_div0_site_n] =
        (div0_site_t){.va = (uint32_t)p_vaddr + off, .hw1 = hw1, .hw2 = hw2};
      s_div0_site_n++;
    }
  }
  /* The CCR write that arms these sites is caught by on_scb_ctrl_write; arming
   * overwrites each with UDF so its execution traps through on_invalid_insn. */
  if (s_div0_site_n > 0U) {
    (void)fprintf(stderr,
                  "  div-0 seam: %u UDIV/SDIV site(s) tracked; armed on CCR.DIV_0_TRP\n",
                  (unsigned)s_div0_site_n);
  }
}

/**
 * @brief Synthesise a UsageFault (#6) for a trapped divide-by-zero.
 *
 * @details
 * Called by the run loop after ::emulate_div0_patched latched a trapping div-0.
 * Latches CFSR.UFSR.DIVBYZERO (so a fault handler -- and the HIL alive probe -- see
 * the
 * architectural status: ``cfsr == 0x02000000``), forces PC back to the faulting
 * divide so ::exc_enter stacks *that* address (a real div-0 UsageFault stacks the
 * divide), and vectors into the application's UsageFault_Handler. If no handler
 * is installed the trap is dropped (no HardFault escalation is modelled -- the
 * firmware that arms DIV_0_TRP always installs the handler).
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector base if VTOR reads as 0.
 * @return Nothing.
 *
 * @pre ::s_div0_fault_pc holds the trapping divide's address.
 * @pre The PPB CFSR word and the vector table are mapped as RAM.
 * @post On a valid vector, the core is in the UsageFault handler with the basic
 *       frame stacked (stacked PC == the faulting divide) and IPSR == 6.
 * @post CFSR.UFSR.DIVBYZERO reads set and ::s_div0_traps is incremented.
 * @note Faithful to Armv8-M CCR.DIV_0_TRP semantics; no time advances (a fault
 *       is synchronous).
 * @since 0.1.0
 */
static void div0_synth_usagefault(uc_engine* uc, uint32_t vtor_base)
{
  const uint32_t cfsr = rd32(uc, (uint64_t)k_scb_cfsr) | (uint32_t)k_div0_cfsr_divzero;
  wr32(uc, (uint64_t)k_scb_cfsr, cfsr);
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &s_div0_fault_pc);
  const uint32_t handler = exc_vector(uc, vtor_base, (uint32_t)k_exc_usagefault);
  if (handler != 0U) {
    s_div0_traps++;
    exc_enter(uc, (uint32_t)k_exc_usagefault, handler);
  }
}

/**
 * @brief Read the handler address for an exception from the vector table.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector base used when VTOR reads as 0.
 * @param[in]     exc_num   Exception/vector index to look up.
 * @return Handler entry address with the Thumb bit cleared.
 * @retval 0 when no usable handler is installed at that vector slot.
 *
 * @pre @p uc has the vector table mapped at VTOR (or @p vtor_base).
 * @pre @p exc_num is a valid vector index (< table length).
 * @post No engine state is modified (read-only).
 * @post The returned address (when non-zero) is halfword-aligned code.
 * @note VTOR lives in PPB RAM here, written by SystemInit at boot.
 * @since 0.1.0
 */
static uint32_t exc_vector(uc_engine* uc, uint32_t vtor_base, uint32_t exc_num)
{
  uint32_t vtor = rd32(uc, (uint64_t)k_scb_vtor);
  if (vtor == 0U) {
    vtor = vtor_base;
  }
  const uint32_t handler = rd32(uc, (uint64_t)vtor + (exc_num * 4U)) & ~1U;
  if ((handler == 0U) || (handler == (uint32_t)k_vector_erased)) {
    return 0U;
  }
  return handler;
}

/**
 * @brief Take one pending peripheral NVIC IRQ the ICU has queued, if allowed.
 *
 * @details
 * The peripheral counterpart to the SysTick / PendSV logic in
 * ::exc_take_pending. board_periph's ICU model queues an IRQ whenever a
 * peripheral event is event-linked through IELSR and its NVIC line is enabled;
 * this pops one and -- if its NVIC priority (IPR byte, top nibble used) outranks
 * the active execution priority -- vectors it in as a real Cortex-M exception
 * (vector 16 + IRQn read from VTOR), exactly the path a hardware IRQ takes. The
 * ISR therefore runs in genuine handler context and returns via the same
 * EXC_RETURN unstack as every other exception. The matching ISPR pending bit is
 * cleared on activation, as hardware does.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector base when VTOR reads as 0.
 * @param[in]     active    Current active-handler priority (sentinel if none).
 * @return true if a peripheral IRQ was taken (PC now points at its ISR).
 *
 * @pre @p uc has stopped at an instruction boundary; PRIMASK already checked.
 * @post At most one IRQ is taken; its ISPR pending bit is cleared if so.
 * @note If no handler is installed at the vector, the IRQ is dropped, not spun.
 * @since 0.1.0
 */
static bool exc_take_periph_irq(uc_engine* uc, uint32_t vtor_base, uint32_t active)
{
  uint32_t irq = 0U;
  if (!board_periph_next_irq(&irq)) {
    return false;
  }
  const uint8_t  prio_byte = (uint8_t)rd32(uc, (uint64_t)k_nvic_ipr_base + irq);
  const uint32_t prio =
    (uint32_t)(prio_byte >> (uint32_t)k_nvic_prio_shift) & (uint32_t)k_lo4_mask; /* 4 MSBs used */
  if (prio >= active) {
    return false; /* an equal/higher-priority handler is active -- defer */
  }
  const uint32_t handler = exc_vector(uc, vtor_base, (uint32_t)k_exc_irq_vec0 + irq);
  if (handler == 0U) {
    return false; /* no ISR installed: drop (default handler would just return) */
  }
  /* Clear the NVIC ISPR pending bit on activation, as hardware does. */
  const uint64_t ispr_word = (uint64_t)k_nvic_ispr_base + ((uint64_t)(irq / 32U) * 4U);
  uint32_t       ispr      = rd32(uc, ispr_word);
  ispr &= ~(1U << (irq % 32U));
  wr32(uc, ispr_word, ispr);

  exc_enter(uc, (uint32_t)k_exc_irq_vec0 + irq, handler);
  board_periph_note_irq_taken(irq);
  return true;
}

/**
 * @brief Take the highest-priority pending exception, if one may activate now.
 *
 * @details
 * The software replacement for the NVIC's "take the highest-priority pending,
 * enabled exception whose priority is greater than the current execution
 * priority" rule -- called at every instruction boundary AND immediately after
 * each exception return (so a lower-priority pend tail-chains exactly as
 * hardware would instead of returning to the interrupted code first). Two
 * sources are modelled:
 *
 *   - SysTick (#15): periodic. ::s_systick_pending is armed once per tick
 *     period by the run loop; this routine consumes it and vectors in #15 so
 *     _tx_timer_interrupt runs in real handler context (correct for ThreadX and
 *     for bare-metal SysTick handlers alike). Arming it elsewhere -- rather than
 *     re-deriving "armed" from SYST_CSR on every call -- is what lets a pending
 *     PendSV run between ticks instead of being starved by a perpetual SysTick.
 *   - PendSV (#14): level-pending via ICSR.PENDSVSET (ThreadX's context-switch
 *     request); the bit is cleared on activation, as hardware does.
 *
 * SysTick (priority 0x40) outranks PendSV (0xFF), so when both are pending
 * SysTick activates first and may even pre-empt a PendSV that is spinning in
 * its idle wait -- exactly the nesting ThreadX relies on to make a sleeping
 * thread runnable. PRIMASK and the active-priority stack are both honoured.
 *
 * @param[in,out] uc            Unicorn engine.
 * @param[in]     vtor_base     Fallback vector base if VTOR reads as 0.
 * @param[in]     allow_systick When false, the armed SysTick is left pending
 *                              (not taken) so modelled time does NOT advance.
 *                              The run loop passes false on a context-switch
 *                              stop (a PENDSVSET write consumes no time), so a
 *                              thread that just suspended on a tick-based wait
 *                              is not woken before lower-priority ready threads
 *                              run. Passes true on a full-budget boundary,
 *                              where genuine execution (or an idle spin) has
 *                              elapsed a tick's worth of time.
 * @return true if an exception was taken (PC now points at a handler).
 *
 * @pre @p uc has stopped at an instruction boundary or just returned.
 * @pre The PPB (SYST_CSR / ICSR / SHPRn / VTOR) is mapped as RAM.
 * @post At most one exception is taken per call (the highest-priority due one).
 * @post ICSR.PENDSVSET / ::s_systick_pending is cleared iff that one was taken.
 * @note SysTick is dropped (not queued) if SYST_CSR is disarmed when its period
 *       elapses, matching a masked/disabled SysTick on hardware.
 * @since 0.1.0
 */
static bool exc_take_pending(uc_engine* uc, uint32_t vtor_base, bool allow_systick)
{
  const uint32_t primask = reg_get(uc, UC_ARM_REG_PRIMASK);
  if ((primask & 1U) != 0U) {
    return false; /* interrupts masked -- no exception may be taken now */
  }
  const uint32_t active = exc_active_prio();

  /* SysTick first: highest-priority of the modelled exceptions, so it can
   * pre-empt a lower-priority PendSV that is spinning for a runnable thread.
   * Skipped on a context-switch stop (allow_systick == false) so the tick does
   * not advance while ready threads still have work to run. */
  if (allow_systick && s_systick_pending) {
    const bool armed =
      (rd32(uc, (uint64_t)k_syst_csr) & (uint32_t)k_syst_csr_run) == (uint32_t)k_syst_csr_run;
    if (!armed) {
      s_systick_pending = false; /* disabled SysTick: drop the pended tick */
    } else if (exc_priority(uc, (uint32_t)k_exc_systick) < active) {
      const uint32_t handler = exc_vector(uc, vtor_base, (uint32_t)k_exc_systick);
      if (handler != 0U) {
        s_systick_pending = false;
        exc_enter(uc, (uint32_t)k_exc_systick, handler);
        s_systick_fires++;
        return true;
      }
    }
  }

  /* PendSV: taken when the firmware has requested a context switch. */
  uint32_t icsr = rd32(uc, (uint64_t)k_scb_icsr);
  if ((icsr & (1U << (uint32_t)k_icsr_pendsvset)) != 0U) {
    if (exc_priority(uc, (uint32_t)k_exc_pendsv) < active) {
      const uint32_t handler = exc_vector(uc, vtor_base, (uint32_t)k_exc_pendsv);
      if (handler != 0U) {
        /* Hardware clears PENDSVSET when PendSV is activated. */
        icsr &= ~(1U << (uint32_t)k_icsr_pendsvset);
        wr32(uc, (uint64_t)k_scb_icsr, icsr);
        exc_enter(uc, (uint32_t)k_exc_pendsv, handler);
        s_pendsv_takes++;
        return true;
      }
    }
  }

  /* Peripheral NVIC IRQs queued by the ICU model (timer overflow / underflow
   * routed through IELSR). Taken last among the modelled exceptions but via the
   * identical real entry/return path, with NVIC IPR priority honoured. */
  return exc_take_periph_irq(uc, vtor_base, active);
}

/**
 * @enum colour_pack_t
 * @brief Field masks/shifts for 24-bit RGB888 <-> 16-bit RGB565 packing.
 */
typedef enum : uint32_t {
  k_rgb888_r_shift = 16U,         /**< Red byte position in 0x00RRGGBB.         */
  k_rgb888_g_shift = 8U,          /**< Green byte position in 0x00RRGGBB.       */
  k_rgb565_r5_keep = 0xF8U,       /**< Top 5 bits of an 8-bit red channel.      */
  k_rgb565_g6_keep = 0xFCU,       /**< Top 6 bits of an 8-bit green channel.    */
  k_rgb565_r_pos   = 8U,          /**< Red field shift when packing RGB565.     */
  k_rgb565_g_pos   = 3U,          /**< Green field shift when packing RGB565.   */
  k_rgb565_b_drop  = 3U,          /**< Bits dropped from an 8-bit blue channel. */
  k_rgb888_mask    = 0x00FFFFFFU, /**< 24-bit colour (BG_BGC low bytes).        */
  k_rgb565_r_shift = 11U,         /**< Red field position in RGB565.            */
  k_rgb565_g_shift = 5U,          /**< Green field position in RGB565.          */
  k_rgb565_5bit    = 0x1FU,       /**< 5-bit channel mask (red / blue).         */
  k_rgb565_6bit    = 0x3FU,       /**< 6-bit channel mask (green).              */
} colour_pack_t;

/** @brief Pack a 0x00RRGGBB colour into RGB565. */
static uint16_t rgb888_to_565(uint32_t rgb)
{
  const uint32_t r = (rgb >> (uint32_t)k_rgb888_r_shift) & (uint32_t)k_byte_mask;
  const uint32_t g = (rgb >> (uint32_t)k_rgb888_g_shift) & (uint32_t)k_byte_mask;
  const uint32_t b = rgb & (uint32_t)k_byte_mask;
  return (uint16_t)(((r & (uint32_t)k_rgb565_r5_keep) << (uint32_t)k_rgb565_r_pos) |
                    ((g & (uint32_t)k_rgb565_g6_keep) << (uint32_t)k_rgb565_g_pos) |
                    (b >> (uint32_t)k_rgb565_b_drop));
}

/**
 * @enum ram_region_t
 * @brief Emulated RAM windows a GLCDC framebuffer may legally live in.
 */
typedef enum : uint32_t {
  k_dtcm_base     = 0x20000000U, /**< Data TCM start.                              */
  k_dtcm_end      = 0x20010000U, /**< Data TCM end.                                */
  k_sram_base     = 0x22000000U, /**< On-chip SRAM start.                          */
  k_sram_end      = 0x22100000U, /**< On-chip SRAM end.                            */
  k_sdram_base    = 0x68000000U, /**< External SDRAM start.                        */
  k_sdram_end     = 0x6C000000U, /**< External SDRAM end.                          */
  k_ns_sdram_base = 0x78000000U, /**< External SDRAM Non-secure alias (bit[28]=1). */
  k_page_size     = 0x1000U,     /**< 4 KiB host-map alignment for SRAM buffer.    */
} ram_region_t;

/** @brief True if addr is in an emulated RAM region a framebuffer could use. */
static bool addr_is_ram(uint32_t addr)
{
  return (((addr >= (uint32_t)k_dtcm_base) && (addr < (uint32_t)k_dtcm_end)) ||
          ((addr >= (uint32_t)k_sram_base) && (addr < (uint32_t)k_sram_end)) ||
          ((addr >= (uint32_t)k_sdram_base) && (addr < (uint32_t)k_sdram_end)));
}

/**
 * @brief Build the current display frame (RGB565) from emulated GLCDC state.
 *
 * @details
 * Fills the buffer with the BG_BGC background colour (what lcd_color_cycle
 * scans out), then, if GR1 has an RGB565 framebuffer programmed in emulated
 * RAM, blits it over the top-left -- so apps that draw real pixels into a
 * graphics layer (e.g. display_pal_animation) show their actual content. The
 * GR1 base/stride/lines are read live each call, so a double-buffered or
 * animating app updates frame to frame.
 *
 * @param[in]  uc Unicorn engine (read-only here).
 * @param[out] fb RGB565 frame buffer of width*height pixels.
 * @param[in]  width_px  Frame width.
 * @param[in]  height_px Frame height.
 */
static void build_frame(uc_engine* uc, uint16_t* fb, uint16_t width_px, uint16_t height_px)
{
  /* Read GLCDC registers from the stable shadow (mmio_peek), never the guest-
   * facing toggling read -- otherwise a firmware that never programs the GLCDC
   * makes the panel strobe black<->white (see mmio_peek). */
  const uint16_t bg = rgb888_to_565(mmio_peek((uint64_t)k_glcdc_bg_bgc) & (uint32_t)k_rgb888_mask);
  const size_t   n  = (size_t)width_px * (size_t)height_px;
  for (size_t i = 0U; i < n; i++) {
    fb[i] = bg;
  }

  const uint32_t saddr = mmio_peek((uint64_t)k_glcdc_gr1_saddr);
  const uint32_t fmt   = (mmio_peek((uint64_t)k_glcdc_gr1_fmt) >> (uint32_t)k_glcdc_fmt_shift) &
                         (uint32_t)k_glcdc_fmt_mask;
  if (!addr_is_ram(saddr) || (fmt != (uint32_t)k_glcdc_fmt_rgb565)) {
    return; /* no graphics layer -- background-only frame */
  }
  const uint32_t stride = (mmio_peek((uint64_t)k_glcdc_gr1_flm3) >> (uint32_t)k_glcdc_high_shift) &
                          (uint32_t)k_glcdc_stride_mask;
  const uint32_t lnnum  = (mmio_peek((uint64_t)k_glcdc_gr1_flm5) >> (uint32_t)k_glcdc_high_shift) &
                          (uint32_t)k_glcdc_lnnum_mask;
  if (stride < 2U) {
    return;
  }
  const uint32_t fb_w = stride / 2U; /* RGB565: 2 bytes per pixel */
  const uint32_t fb_h = lnnum + 1U;
  const uint32_t cw   = (fb_w < (uint32_t)width_px) ? fb_w : (uint32_t)width_px;
  const uint32_t ch   = (fb_h < (uint32_t)height_px) ? fb_h : (uint32_t)height_px;
  for (uint32_t y = 0U; y < ch; y++) {
    (void)uc_mem_read(uc,
                      (uint64_t)saddr + ((uint64_t)y * (uint64_t)stride),
                      &fb[(size_t)y * (size_t)width_px],
                      (size_t)cw * sizeof(uint16_t));
  }
}

/** @brief Write an RGB565 frame to a binary PPM (P6) for headless inspection. */
static int write_ppm(const char* path, const uint16_t* fb, uint16_t width_px, uint16_t height_px)
{
  FILE* f = fopen(path, "wb"); /* alloc-allow: host dev tool, not firmware */
  if (f == nullptr) {
    return -1;
  }
  (void)fprintf(f, "P6\n%u %u\n255\n", (unsigned)width_px, (unsigned)height_px);
  const size_t n = (size_t)width_px * (size_t)height_px;
  for (size_t i = 0U; i < n; i++) {
    const uint16_t p      = fb[i];
    const uint32_t r5     = (uint32_t)((p >> (uint32_t)k_rgb565_r_shift) & (uint32_t)k_rgb565_5bit);
    const uint32_t g6     = (uint32_t)((p >> (uint32_t)k_rgb565_g_shift) & (uint32_t)k_rgb565_6bit);
    const uint32_t b5     = (uint32_t)(p & (uint32_t)k_rgb565_5bit);
    const uint8_t  rgb[3] = {(uint8_t)((r5 << 3) | (r5 >> 2)),
                             (uint8_t)((g6 << 2) | (g6 >> 4)),
                             (uint8_t)((b5 << 3) | (b5 >> 2))};
    (void)fwrite(rgb, 1U, 3U, f);
  }
  (void)fclose(f);
  return 0;
}

/**
 * @brief Snapshot the live peripheral state into a board-view status struct.
 *
 * @details
 * Reads the read-only board_periph / board_usb getters -- per-LED level + lit
 * colour, the USB device-state string, the last captured UART line, the NVIC
 * IRQ totals, and the last drained touch point -- so the overlay can render the
 * status sidebar without reaching into any model's internals.
 *
 * @param[out] st        Status struct to fill.
 * @param[in]  app_name  Window / app title to caption the sidebar with.
 * @return Nothing.
 */
static void fill_status(board_status_t* st, const char* app_name)
{
  static const char* const k_led_labels[k_overlay_led_count] = {"LED1", "LED2", "LED3"};
  *st                                                        = (board_status_t){};
  for (uint32_t i = 0U; i < (uint32_t)k_overlay_led_count; i++) {
    st->leds[i].on    = board_periph_led_level((board_led_id_t)i) != 0U;
    st->leds[i].color = board_periph_led_color_rgb565((board_led_id_t)i);
    st->leds[i].label = k_led_labels[i];
  }
  st->usb_state = board_usb_state_string();
  st->uart_line = board_periph_uart_last_line();
  st->irq_total = board_periph_irq_total();
  st->irq0      = board_periph_irq_count(0U);
  st->irq1      = board_periph_irq_count(1U);
  st->has_touch = board_periph_touch_last(&st->touch_x, &st->touch_y);
  /* User switches are active-low: a held button reads its pin low. */
  st->sw1_pressed = !board_periph_gpio_get_input((uint8_t)k_sim_sw_port, (uint8_t)k_sim_sw1_pin);
  st->sw2_pressed = !board_periph_gpio_get_input((uint8_t)k_sim_sw_port, (uint8_t)k_sim_sw2_pin);
  board_periph_battery_get(&st->battery_soc, &st->battery_charging);
  st->low_power   = s_low_power;
  st->core_is_m33 = (s_primary_core == k_core_m33);
  st->app_name    = app_name;
  board_sd_info(&st->sd_attached, &st->sd_bytes, &st->sd_fat_bits, &st->sd_label);
  /* Tabbed console: each board_console channel (ALL | UART | ITM | SPI | I2C) is
   * a tab; the active one (s_view_console_ch) fills the console panel. Populate
   * the tab-bar metadata (names + live line counts + which tab is active) so the
   * overlay can draw the bar and hit-test clicks. */
  st->console_ch_count  = (uint32_t)k_board_console_ch_count;
  st->console_active_ch = (uint32_t)s_view_console_ch;
  for (uint32_t c = 0U; c < (uint32_t)k_board_console_ch_count; c++) {
    if (c >= (uint32_t)k_overlay_console_tabs_max) {
      break;
    }
    st->console_ch_name[c]        = board_console_name((board_console_ch_t)c);
    st->console_ch_count_lines[c] = board_console_count((board_console_ch_t)c);
  }
  /* Console window: copy up to k_overlay_console_rows lines from the active
   * channel's scrollback ring, starting s_view_scroll lines back from the
   * newest, so the mouse-wheel can page through history. console[0] is the
   * newest visible line (= ring line s_view_scroll). */
  const uint32_t avail = board_console_count(s_view_console_ch);
  const uint32_t total = board_console_total(s_view_console_ch);
  if (s_view_autoscroll) {
    s_view_scroll = 0U; /* follow the live tail */
  } else if (total > s_view_log_seen) {
    /* Paused: new lines pushed the tail forward, so advance the back-offset by
     * the same amount to keep the reader's current (absolute) lines on screen.
     * Capped at the ring depth -- lines older than the ring have aged out. */
    const uint32_t delta = total - s_view_log_seen;
    s_view_scroll += delta;
    if (s_view_scroll > (uint32_t)k_uart_log_max) {
      s_view_scroll = (uint32_t)k_uart_log_max;
    }
  }
  s_view_log_seen = total;
  const uint32_t max_scroll =
    (avail > (uint32_t)k_overlay_console_rows) ? (avail - (uint32_t)k_overlay_console_rows) : 0U;
  if (s_view_scroll > max_scroll) {
    s_view_scroll = max_scroll; /* re-clamp if history shrank or is shallow */
  }
  const uint32_t scroll = s_view_scroll;
  uint32_t       rows   = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_overlay_console_rows; i++) {
    const char* line = board_console_line(s_view_console_ch, scroll + i);
    if (line == nullptr) {
      break;
    }
    (void)snprintf(st->console[i], (size_t)k_overlay_line_cap, "%s", line);
    rows++;
  }
  st->console_count      = rows;
  st->console_scroll     = scroll;
  st->console_total      = avail;
  st->console_autoscroll = s_view_autoscroll;
  /* Run-state telemetry (set by the run loop before each present). */
  st->pc            = s_view_pc;
  st->chunks        = s_view_chunks;
  st->running       = s_view_running;
  st->mmio_reads    = s_mmio_reads;
  st->mmio_writes   = s_mmio_writes;
  st->uart_tx_total = board_periph_uart_tx_total();
}

/**
 * @brief Build the panel framebuffer, then compose it with the status sidebar.
 *
 * @details
 * Renders the GLCDC panel into @p panel_fb (so a display app's region stays
 * pixel-correct), snapshots the live peripheral state, and writes the full
 * composite (panel region + status sidebar) into @p composite -- the single
 * buffer that backs both the live window and the @c --ppm snapshot.
 *
 * @param[in,out] uc        Unicorn engine (read for the GLCDC framebuffer).
 * @param[out]    panel_fb  Panel RGB565 buffer (@p panel_w by @p panel_h).
 * @param[out]    composite Composite RGB565 buffer (overlay total dimensions).
 * @param[in]     panel_w   Panel width in pixels.
 * @param[in]     panel_h   Panel height in pixels.
 * @param[in]     app_name  Window / app title for the sidebar caption.
 * @return Nothing.
 */
/**
 * @enum panel_rotate_t
 * @brief Display rotation (degrees clockwise) applied to the panel for viewing.
 *
 * @details The firmware always renders at its compiled resolution; --rotate only
 * turns the emulated panel for display (e.g. a 1024x600 landscape app shown as a
 * 600x1024 portrait), so a vertically-mounted screen can be previewed. Clicks
 * are mapped back to native coordinates via ::unrotate_click.
 */
typedef enum : uint32_t {
  k_rotate_0   = 0U,   /**< Native orientation.             */
  k_rotate_90  = 90U,  /**< 90 deg clockwise (-> portrait). */
  k_rotate_180 = 180U, /**< Upside down.                    */
  k_rotate_270 = 270U, /**< 90 deg counter-clockwise.       */
} panel_rotate_t;

/** @brief Rotate a row-major RGB565 panel (@p sw x @p sh) into @p dst, @p deg CW. */
static void rotate_panel(const uint16_t* src, uint16_t sw, uint16_t sh, uint16_t* dst, uint32_t deg)
{
  for (uint16_t y = 0U; y < sh; y++) {
    for (uint16_t x = 0U; x < sw; x++) {
      const uint16_t p  = src[(size_t)y * (size_t)sw + (size_t)x];
      uint32_t       dx = x;
      uint32_t       dy = y;
      uint32_t       dw = sw;
      if (deg == (uint32_t)k_rotate_90) {
        dx = (uint32_t)(sh - 1U - y);
        dy = x;
        dw = sh;
      } else if (deg == (uint32_t)k_rotate_180) {
        dx = (uint32_t)(sw - 1U - x);
        dy = (uint32_t)(sh - 1U - y);
      } else if (deg == (uint32_t)k_rotate_270) {
        dx = y;
        dy = (uint32_t)(sw - 1U - x);
        dw = sh;
      }
      dst[(size_t)dy * (size_t)dw + (size_t)dx] = p;
    }
  }
}

/** @brief Map a click in the rotated displayed panel back to native panel coords. */
static void unrotate_click(uint16_t  cx,
                           uint16_t  cy,
                           uint16_t  panel_w,
                           uint16_t  panel_h,
                           uint32_t  deg,
                           uint16_t* nx,
                           uint16_t* ny)
{
  if (deg == (uint32_t)k_rotate_90) {
    *nx = cy;
    *ny = (uint16_t)(panel_h - 1U - cx);
  } else if (deg == (uint32_t)k_rotate_180) {
    *nx = (uint16_t)(panel_w - 1U - cx);
    *ny = (uint16_t)(panel_h - 1U - cy);
  } else if (deg == (uint32_t)k_rotate_270) {
    *nx = (uint16_t)(panel_w - 1U - cy);
    *ny = cx;
  } else {
    *nx = cx;
    *ny = cy;
  }
}

/** @brief Toggle a user switch's pressed level (active-low) for an on-screen button. */
static void set_switch(board_overlay_btn_t btn, bool pressed)
{
  const uint8_t pin =
    (btn == k_board_overlay_btn_sw2) ? (uint8_t)k_sim_sw2_pin : (uint8_t)k_sim_sw1_pin;
  /* SW1/SW2 are momentary push-buttons wired active-low (internal pull-up): held
   * down drives the pin LOW (input false), released lets it return HIGH (true).
   * Driving the level directly -- instead of toggling -- makes a click behave as a
   * real button (press on mouse-down, release on mouse-up), not a latching switch. */
  board_periph_gpio_set_input((uint8_t)k_sim_sw_port, pin, !pressed);
}

/**
 * @brief Apply a POWER-section click to the fuel-gauge model.
 *
 * @details A click on the battery slider track maps the click column to a 0..100
 * percent (::board_overlay_battery_pct_at) and writes it while preserving the
 * charge state; a click on the CHG toggle flips charging while preserving SOC.
 * Shared by the live window and the headless @c --click so both behave alike.
 *
 * @param[in] btn    The POWER button hit (battery slider or CHG toggle).
 * @param[in] cx     Click column in composite pixels (for the slider map).
 * @param[in] disp_w Displayed panel width (the sidebar origin).
 */
static void apply_battery_click(board_overlay_btn_t btn, uint16_t cx, uint16_t disp_w)
{
  if (btn == k_board_overlay_btn_battery) {
    uint8_t pct = 0U;
    if (board_overlay_battery_pct_at(cx, disp_w, &pct)) {
      bool charging = false;
      board_periph_battery_get(nullptr, &charging);
      board_periph_battery_set(pct, charging);
    }
  } else if (btn == k_board_overlay_btn_batt_chg) {
    uint8_t soc      = 0U;
    bool    charging = false;
    board_periph_battery_get(&soc, &charging);
    board_periph_battery_set(soc, !charging);
  } else if (btn == k_board_overlay_btn_lowpower) {
    /* CORE low-power toggle: flip the M33 4:1-clock model live (same effect as
     * the headless --low-power flag). */
    s_low_power = !s_low_power;
  }
}

/**
 * @brief Route a composite-space click to the right input model.
 *
 * @details An on-screen SW1 / SW2 button toggles that user switch (active-low);
 * any other click is mapped back through @ref unrotate_click and injected as a
 * GT911 touch -- the same path the firmware's real ra8_touch_read drains. This is
 * shared by the live window and the headless @c --click so both behave alike.
 *
 * @param[in] cx         Click column in composite pixels.
 * @param[in] cy         Click row in composite pixels.
 * @param[in] panel_w    Native panel width (for the touch unrotate).
 * @param[in] panel_h    Native panel height (for the touch unrotate).
 * @param[in] disp_w     Displayed panel width (the sidebar origin for buttons).
 * @param[in] rotate_deg Active display rotation.
 * @return The button hit, or ::k_board_overlay_btn_none for a panel touch.
 */
static board_overlay_btn_t route_click(uint16_t cx,
                                       uint16_t cy,
                                       uint16_t panel_w,
                                       uint16_t panel_h,
                                       uint16_t disp_w,
                                       uint32_t rotate_deg)
{
  /* A click on the console tab bar switches the active channel (and takes
   * precedence over the console-body autoscroll toggle below, since the tab row
   * sits inside the console region). Switching resets the scrollback to the live
   * tail of the newly selected channel. */
  uint32_t tab_idx = 0U;
  if (board_overlay_hit_console_tab(cx, cy, disp_w, (uint32_t)k_board_console_ch_count, &tab_idx)) {
    s_view_console_ch = (board_console_ch_t)tab_idx;
    s_view_scroll     = 0U;
    s_view_autoscroll = true;
    s_view_log_seen   = board_console_total(s_view_console_ch);
    return k_board_overlay_btn_console;
  }
  const board_overlay_btn_t btn = board_overlay_hit_button(cx, cy, disp_w);
  if (btn == k_board_overlay_btn_console) {
    /* Toggle the console autoscroll, Arduino-Serial-Monitor style: pause it
     * while reading, or click again to jump back to the live tail. */
    s_view_autoscroll = !s_view_autoscroll;
    if (s_view_autoscroll) {
      s_view_scroll = 0U; /* resume following the newest line */
    }
    return btn;
  }
  if ((btn == k_board_overlay_btn_battery) || (btn == k_board_overlay_btn_batt_chg) ||
      (btn == k_board_overlay_btn_lowpower)) {
    apply_battery_click(btn, cx, disp_w);
    return btn;
  }
  if (btn != k_board_overlay_btn_none) {
    set_switch(btn, true); /* mouse-down -> press; mouse-up releases it (run loop). */
    return btn;
  }
  uint16_t nx = cx;
  uint16_t ny = cy;
  unrotate_click(cx, cy, panel_w, panel_h, rotate_deg, &nx, &ny);
  board_periph_touch_inject(nx, ny);
  return k_board_overlay_btn_none;
}

static void build_composite(uc_engine*  uc,
                            uint16_t*   panel_fb,
                            uint16_t*   rot_fb,
                            uint16_t*   composite,
                            uint16_t    panel_w,
                            uint16_t    panel_h,
                            uint16_t    disp_w,
                            uint16_t    disp_h,
                            uint32_t    rotate_deg,
                            const char* app_name)
{
  build_frame(uc, panel_fb, panel_w, panel_h);
  const uint16_t* shown = panel_fb;
  if ((rotate_deg != (uint32_t)k_rotate_0) && (rot_fb != nullptr)) {
    rotate_panel(panel_fb, panel_w, panel_h, rot_fb, rotate_deg);
    shown = rot_fb;
  }
  board_status_t st = {};
  fill_status(&st, app_name);
  board_overlay_compose(composite, shown, disp_w, disp_h, &st);
}

/**
 * @brief Warm-reboot the firmware: re-run from the reset vector in place.
 *
 * @details
 * Mirrors a Cortex-M reset without tearing down the Unicorn engine. Restores the
 * code image and the .data initial values by re-writing the ELF's PT_LOAD
 * segments (the firmware's own Reset_Handler then re-zeroes .bss and re-copies
 * .data), re-reads SP/PC from the vector table, resets the peripheral models
 * (the reset-cause RSTSRn and the VBATT backup domain deliberately survive their
 * reset hooks, so the reboot cause and battery-backed state persist), and clears
 * the host-side exception / scheduler bookkeeping so the next boot starts clean.
 * The installed Unicorn hooks persist across this, so they are NOT re-added, and
 * one-shot CLI input (--keys / --input / --usb-in) is NOT re-injected.
 *
 * @param[in,out] uc    Unicorn engine to reboot (kept alive).
 * @param[in]     elf   The firmware ELF image bytes (re-loaded).
 * @param[in]     len   ELF image length in bytes.
 * @param[in]     trace Whether --trace is active (forwarded to board_periph_init).
 * @return The reset-vector PC to resume the run loop from (Thumb bit set).
 * @since 0.1.0
 */
static uint32_t warm_reboot(uc_engine* uc, const uint8_t* elf, long len, bool trace)
{
  /* 1. Restore the code + .data initial image from the ELF PT_LOAD segments.
   * The same elf/len loaded successfully at startup, so a failure here means the
   * engine's memory writes started failing -- report and return PC=0 so the
   * caller ends the run rather than execute a stale image. */
  if (load_elf(uc, elf, len) != 0) {
    (void)fprintf(stderr, "board_sim: warm_reboot: load_elf failed -- ending run\n");
    return 0U;
  }

  /* 2. Reset the peripheral + network models (RSTSRn / VBATT backup survive). */
  board_periph_init(trace);
  board_net_init(trace);

  /* 3. Clear host-side exception / scheduler bookkeeping. */
  s_exc_depth       = 0U;
  s_systick_pending = true;
  s_bkpt_hit        = false;
  s_exc_return_hit  = false;
  s_pendsv_stop     = false;
  s_mpu_fault       = false;
  s_div0_fault      = false;
  s_div0_armed      = false; /* a warm reboot re-loads the image, un-patching sites. */
  s_systick_fires   = 0U;
  s_pendsv_takes    = 0U;
  s_svc_takes       = 0U;
  /* Clear the multi-channel console store + the in-flight ITM line, and reset
   * the tabbed-console view so the rebooted firmware starts with an empty
   * console on the ALL tab (the SCI model's own reset clears its line buffers). */
  board_console_reset();
  s_itm_len         = 0U;
  s_view_console_ch = k_board_console_ch_all;
  s_view_scroll     = 0U;
  s_view_autoscroll = true;
  s_view_log_seen   = 0U;

  /* 4. Re-read the Cortex-M reset vector (SP = vectors[0], PC = vectors[1]). */
  uint32_t sp = 0U;
  uint32_t pc = 0U;
  (void)uc_mem_read(uc, k_regions[1].base + 0U, &sp, 4);
  (void)uc_mem_read(uc, k_regions[1].base + 4U, &pc, 4);
  pc |= 1U; /* Thumb */
  uint32_t xpsr = (uint32_t)k_xpsr_t_bit;
  (void)uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
  (void)uc_reg_write(uc, UC_ARM_REG_XPSR, &xpsr);
  (void)fprintf(stderr, "board_sim: warm reboot -- reset SP=0x%08X PC=0x%08X\n", sp, pc);
  return pc;
}

typedef enum : uint32_t {
  k_panel_line_max = 256U,  /**< Max panel-config line length.    */
  k_panel_name_max = 64U,   /**< Max panel name (incl NUL).       */
  k_panel_dim_max  = 4096U, /**< Sanity cap on a panel dimension. */
} panel_limits_t;

/** @brief Display descriptor loaded from a flat key=value panel file. */
typedef struct {
  char     name[k_panel_name_max];
  uint16_t width;
  uint16_t height;
} board_panel_t;

/** @brief Trim trailing space/tab/CR/LF in place. */
static void panel_rstrip(char* s)
{
  size_t n = strlen(s);
  while (n > 0U) {
    const char c = s[n - 1U];
    if ((c != ' ') && (c != '\t') && (c != '\r') && (c != '\n')) {
      break;
    }
    s[--n] = '\0';
  }
}

/**
 * @brief Load a panel descriptor (name / width / height) from a TOML-ish file.
 *
 * @details
 * A flat ``key = value`` panel descriptor (see ``tools/board_sim/panels/``), so
 * the board emulator becomes whatever display a config describes -- not just the
 * EK-RA8D2 1024x600. Dependency-free bounded parser (strncmp / strtol, no
 * dynamic allocation beyond the FILE handle); blank lines and '#' comments are
 * ignored and quotes are stripped from the name.
 *
 * @param[in]  path Panel config path.
 * @param[out] out  Filled descriptor on success.
 * @return true if a valid width/height were parsed.
 */
static bool load_panel(const char* path, board_panel_t* out)
{
  (void)memset(out, 0, sizeof(*out));
  FILE* f = fopen(path, "r"); /* alloc-allow: host dev tool, not firmware */
  if (f == nullptr) {
    (void)fprintf(stderr, "board_sim: cannot open panel config %s\n", path);
    return false;
  }
  char line[k_panel_line_max];
  while (fgets(line, (int)sizeof(line), f) != nullptr) {
    char* p = line;
    while ((*p == ' ') || (*p == '\t')) {
      p++;
    }
    if ((*p == '#') || (*p == '\0') || (*p == '\n') || (*p == '\r')) {
      continue;
    }
    char* eq = strchr(p, '=');
    if (eq == nullptr) {
      continue;
    }
    *eq       = '\0';
    char* key = p;
    char* val = eq + 1;
    while ((*val == ' ') || (*val == '\t')) {
      val++;
    }
    panel_rstrip(key);
    panel_rstrip(val);
    if (strncmp(key, "width", sizeof("width")) == 0) {
      out->width = (uint16_t)strtol(val, nullptr, (int)k_strtol_base10);
    } else if (strncmp(key, "height", sizeof("height")) == 0) {
      out->height = (uint16_t)strtol(val, nullptr, (int)k_strtol_base10);
    } else if (strncmp(key, "name", sizeof("name")) == 0) {
      const char* s = val;
      size_t      n = strlen(s);
      if ((n >= 2U) && (s[0] == '"') && (s[n - 1U] == '"')) {
        s += 1;
        n -= 2U;
      }
      if (n >= sizeof(out->name)) {
        n = sizeof(out->name) - 1U;
      }
      (void)memcpy(out->name, s, n);
      out->name[n] = '\0';
    }
  }
  (void)fclose(f);
  if ((out->width == 0U) || (out->width > (uint16_t)k_panel_dim_max) || (out->height == 0U) ||
      (out->height > (uint16_t)k_panel_dim_max)) {
    (void)fprintf(stderr, "board_sim: panel %s: width/height missing or out of range\n", path);
    return false;
  }
  return true;
}

/* Console-UART presentation. The SCI_B model captures every byte the firmware
 * transmits and hands it to console_tx_sink, which echoes it to stdout with a
 * clear [uart] prefix so a console example's output is captured and greppable.
 * The same byte is mirrored raw so piping board_sim's stdout reconstructs the
 * exact serial stream. RX is fed from --input / stdin into the console channel
 * (SCI8 on the EK-RA8D2). */
typedef enum : uint32_t {
  k_uart_line_max = 256U, /**< Pretty-print line buffer for the [uart] prefix. */
} console_cfg_t;

static char     s_uart_line[k_uart_line_max]; /**< Pending [uart] line text.   */
static uint32_t s_uart_line_len;              /**< Chars buffered in the line. */

/** @brief Flush the pending [uart] line to stdout with its channel prefix. */
static void console_flush_line(uint8_t channel)
{
  if (s_uart_line_len == 0U) {
    return;
  }
  s_uart_line[s_uart_line_len] = '\0';
  (void)fprintf(stdout, "[uart] SCI%u: %s\n", channel, s_uart_line);
  (void)fflush(stdout);
  s_uart_line_len = 0U;
}

/**
 * @brief SCI TX sink: print each transmitted byte (prefixed line + raw mirror).
 *
 * @details
 * Installed via board_periph_sci_set_tx_sink. Printable bytes accumulate into a
 * line that is flushed -- with an @c [uart] SCIn: prefix -- on newline or when
 * the buffer fills, so console output reads cleanly in the log; CR is dropped
 * from the pretty line. Every byte is also written verbatim to a raw mirror on
 * stdout so redirecting board_sim reproduces the exact serial stream.
 *
 * @param[in] channel SCI channel that transmitted the byte.
 * @param[in] byte    The transmitted data byte.
 * @return Nothing.
 */
static void console_tx_sink(uint8_t channel, uint8_t byte)
{
  if (byte == (uint8_t)'\n') {
    console_flush_line(channel);
    return;
  }
  if ((byte != (uint8_t)'\r') && (s_uart_line_len < (uint32_t)(k_uart_line_max - 1U))) {
    s_uart_line[s_uart_line_len++] = (char)byte;
  }
  if (s_uart_line_len == (uint32_t)(k_uart_line_max - 1U)) {
    console_flush_line(channel); /* avoid an unbounded line on a stream with no LF */
  }
}

/**
 * @brief Decode a C-style escaped --input string into a raw byte buffer.
 *
 * @details
 * Translates @c \\n / @c \\r / @c \\t / @c \\0 / @c \\\\ in @p in so a shell
 * argument can carry the line endings a console example expects (e.g.
 * @c --input "ping\r\n"); any other character (including an unrecognised
 * escape's backslash) is copied verbatim. Bounded by @p cap; never overruns.
 *
 * @param[in]  in  NUL-terminated source string.
 * @param[out] out Destination byte buffer.
 * @param[in]  cap Capacity of @p out in bytes.
 * @return Number of bytes written to @p out.
 */
static uint32_t decode_escapes(const char* in, uint8_t* out, uint32_t cap)
{
  uint32_t n = 0U;
  for (uint32_t i = 0U; (in[i] != '\0') && (n < cap); i++) {
    char c = in[i];
    if ((c == '\\') && (in[i + 1U] != '\0')) {
      i++;
      switch (in[i]) {
        case 'n':
          c = '\n';
          break;
        case 'r':
          c = '\r';
          break;
        case 't':
          c = '\t';
          break;
        case '0':
          c = '\0';
          break;
        default:
          c = in[i]; /* \\ -> \, and any other escape is taken literally */
          break;
      }
    }
    out[n++] = (uint8_t)c;
  }
  return n;
}

/**
 * @brief Create the second emulator engine for cpu1 (Cortex-M33), if present.
 *
 * @details
 * Only dual-core firmware (one exporting ``cpu1_reset_handler``) gets a cpu1
 * engine, so single-core apps pay nothing. The engine maps the same regions as
 * cpu0 but binds the on-chip SRAM to the shared host buffer (::s_sram_buf) so
 * cross-core IPC over shared SRAM is coherent; the full ELF (which carries
 * cpu1's image at MRAM_CPU1) is loaded so cpu1 can boot from its own vector
 * table when released. The engine is left idle -- the run loop boots it on the
 * CPU1ACTCSR release.
 *
 * @param[in] elf     The firmware image (cpu0 + cpu1).
 * @param[in] elf_len Length of @p elf, bytes.
 *
 * @return The cpu1 engine, or NULL if this is not a dual-core image (or setup
 *         failed -- cpu0 then runs alone, exactly as before).
 * @retval NULL Not a dual-core image / engine setup failed.
 *
 * @pre ::s_sram_buf is allocated (cpu0 SRAM is host-backed).
 * @pre @p elf is a valid ELF still resident (called before it is freed).
 * @post On success a Cortex-M33 engine mirrors cpu0's map with shared SRAM.
 * @post No cpu1 instruction has executed yet (idle until released).
 * @note cpu1's PPB / peripheral writes stay private to its engine.
 * @since 0.1.0
 */
static uc_engine* cpu1_engine_init(const uint8_t* elf, long elf_len)
{
  /* Dual-core only: cpu1's image is embedded as raw bytes (.cpu1_image, not a
   * cpu0 symbol), so detect it by a PT_LOAD segment in the MRAM_CPU1 window. */
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + (uint32_t)k_elf_e_phoff_off, 4);
  const uint16_t phentsize = (uint16_t)(elf[42] | (elf[43] << 8));
  const uint16_t phnum     = (uint16_t)(elf[44] | (elf[45] << 8));
  bool           has_cpu1  = false;
  for (uint16_t i = 0U; i < phnum; i++) {
    const uint8_t* ph = elf + phoff + ((uint32_t)i * phentsize);
    uint32_t       p_type;
    uint32_t       p_paddr;
    (void)memcpy(&p_type, ph + 0, 4);
    (void)memcpy(&p_paddr, ph + (uint32_t)k_elf_ph_paddr_off, 4);
    if ((p_type == 1U) && (p_paddr >= (uint32_t)k_cpu1_mram_base) &&
        (p_paddr < (uint32_t)k_cpu1_mram_end)) {
      has_cpu1 = true;
      break;
    }
  }
  if (!has_cpu1) {
    return nullptr; /* single-core firmware -- no second engine. */
  }
  uc_engine* c1 = nullptr;
  if (uc_open(UC_ARCH_ARM, (uc_mode)(UC_MODE_THUMB | UC_MODE_MCLASS), &c1) != UC_ERR_OK) {
    return nullptr;
  }
  (void)uc_ctl_set_cpu_model(c1, UC_CPU_ARM_CORTEX_M33);
  for (size_t i = 0U; i < (sizeof(k_regions) / sizeof(k_regions[0])); i++) {
    uc_err mr = UC_ERR_OK;
    if (k_regions[i].base == (uint64_t)k_sram_base) {
      mr =
        uc_mem_map_ptr(c1, k_regions[i].base, (size_t)k_regions[i].size, UC_PROT_ALL, s_sram_buf);
    } else if (k_regions[i].base == (uint64_t)k_ns_sram2_base) {
      /* SRAM2 Non-secure alias: the SAME physical bytes as 0x22100000, so back
       * it with the shared SRAM host buffer at that offset (mirroring cpu0's
       * map). cpu1 stamps its bring-up markers through this alias
       * (cpu1_pingpong_ipc's cpu1_main writes 0x321002xx); a private mapping
       * here would hide them from cpu0 and the --dump-sym probe. */
      uint8_t* const ns_host =
        s_sram_buf + ((size_t)k_ns_sram2_base - (size_t)k_ns_alias_bit - (size_t)k_sram_base);
      mr = uc_mem_map_ptr(c1, k_regions[i].base, (size_t)k_regions[i].size, UC_PROT_ALL, ns_host);
    } else {
      mr = uc_mem_map(c1, k_regions[i].base, (size_t)k_regions[i].size, UC_PROT_ALL);
    }
    if (mr != UC_ERR_OK) {
      (void)uc_close(c1);
      return nullptr;
    }
  }
  /* Map the Renesas peripheral space through the SAME board_periph models cpu0
   * uses -- the peripherals are shared hardware, so the M33's MMIO (GPIO LED
   * toggles, SCI, timers, ...) reaches the models and the board view instead of
   * faulting on unmapped space. cpu0 maps this region separately via uc_mmio_map
   * (it is absent from k_regions); cpu1 needs the identical mapping. */
  if (uc_mmio_map(c1,
                  (uint64_t)k_periph_base,
                  (size_t)k_periph_size,
                  mmio_read,
                  nullptr,
                  mmio_write,
                  nullptr) != UC_ERR_OK) {
    (void)uc_close(c1);
    return nullptr;
  }
  /* The M33 runs as a permanent non-secure bus controller (SECEXT off): the
   * peripheral channels it owns -- e.g. the IPC unit it pokes to wake the M85 --
   * are NS-attributed, so its accesses route through the bit[28] non-secure
   * alias (0x5xxxxxxx) rather than the Secure 0x4xxxxxxx the M85 uses. Map that
   * alias to the SAME models with the SAME hooks: the hooks rebuild the absolute
   * address as k_periph_base + window-relative offset, so a 0x50020000 access
   * (offset 0x20000) dispatches to 0x40020000 identically to a Secure one. */
  if (uc_mmio_map(c1,
                  (uint64_t)k_periph_base | (uint64_t)k_ns_alias_bit,
                  (size_t)k_periph_size,
                  mmio_read,
                  nullptr,
                  mmio_write,
                  nullptr) != UC_ERR_OK) {
    (void)uc_close(c1);
    return nullptr;
  }
  if (load_elf(c1, elf, elf_len) != 0) {
    (void)uc_close(c1);
    return nullptr;
  }
  (void)fprintf(stderr, "  cpu1 engine   : Cortex-M33, shared SRAM + peripherals (dual-core)\n");
  return c1;
}

int main(int argc, char** argv)
{
  if (argc < 2) {
    (void)fprintf(
      stderr,
      "usage: board_sim <firmware.elf> [--view] [--ppm <out.ppm>]"
      " [--panel <file.toml>] [--size WxH] [--click X Y] [--input <str>] [--sd <image>]"
      " [--usb-in <str>] [--button <1|2>] [--reboot <N>] [--dump-sym <name>]"
      " [--stop-sym <name> <N>]"
      " [--trace-sym <name>] [--sd-new <N[k|m|g][:fat16|fat32]>] [--save-sd <out>] [--fast-sd]"
      " [--device ra8d2|ra8p1]\n"
      "  --view          open a macOS window: live board view; click panel"
      " (touch) / on-screen SW1/SW2, type -> UART\n"
      "  --ppm <file>    write the final composite (panel + status) to a PPM\n"
      "  --record <dir>  record frames (panel + status) to <dir>/frame_NNNNNN.ppm\n"
      "  --record-secs N headless: record N emulated seconds, then stop (~20 fps)\n"
      "  --rotate DEG    display rotation 90/180/270 (e.g. portrait)\n"
      "  --panel <file>  display descriptor (name/width/height) to size the panel\n"
      "  --size WxH      panel size in pixels; overrides --panel (default 1024x600)\n"
      "  --click X Y     headless: inject one touch at X,Y once the UI is up\n"
      "  --input <str>   feed <str> to the console UART RX (SCI8); \\n / \\r / \\t ok\n"
      "  --keys <str>    type <str> via the window-key path -> console UART RX\n"
      "  --usb-in <str>  feed <str> to the USB CDC bulk OUT pipe (echo test)\n"
      "  --button <1|2>  hold user switch SW1 (P009) / SW2 (P008) pressed\n"
      "  --battery <pct> set the fuel-gauge state-of-charge (0..100; MAX17048 @ 0x36)\n"
      "  --charge        mark the battery charging (fuel-gauge CRATE reads positive)\n"
      "  --reboot <N>    warm-reboot N times (re-run from reset vector; VBATT-backup\n"
      "                  + reset-cause survive) to exercise reset-survival apps\n"
      "  --sd <image>    serve a FAT/exFAT image as the microSD card (read + write)\n"
      "  --sd-new <N[k|m|g][:fat16|fat32]>  blank FAT card of N MiB (k/m/g unit; e.g. 30g)\n"
      "  --save-sd <out> after the run, dump the SD card image (with firmware writes)\n"
      "  --fast-sd       serve SD blocks direct from the image (skip the per-byte SPI\n"
      "                  protocol; byte-identical render) so a big book loads fast; opt-in\n"
      "  --dump-sym <s>  print 32-bit global <s> from memory after the run (memprobe)\n"
      "  --stop-sym <s> <N>  end the run once 32-bit global <s> reaches N (counter memprobe)\n"
      "  --trace-sym <s> log every entry to function <s> (+LR): trace a bring-up path\n"
      "  --trace         log each LED/GPIO transition + NVIC IRQ as it happens\n"
      "  --primary-core m85|m33  label the primary core; m33 leaves the M85-only\n"
      "                  instruction seams (MVE / long-shift) off (default m85)\n"
      "  --device ra8d2|ra8p1  select the emulated part (default ra8d2). ra8p1 adds\n"
      "                  the Ethos-U55 NPU (0x40140000) as a mapped-but-unmodelled\n"
      "                  window; else identical to the RA8D2 model\n"
      "  --low-power     model the M33's 4:1-slower clock (1/4 chunk budget); the\n"
      "                  GUI low-power button toggles it live under --view\n");
    return 2;
  }
  const char* elf_path                         = argv[1];
  bool        want_view                        = false;
  const char* ppm_path                         = nullptr;
  const char* record_dir                       = nullptr;
  uint32_t    record_secs                      = 0U;
  uint32_t    rotate_deg                       = (uint32_t)k_rotate_0;
  const char* panel_path                       = nullptr;
  const char* input_str                        = nullptr;
  const char* keys_str                         = nullptr;
  const char* usb_in_str                       = nullptr;
  const char* dump_sym_names[k_dump_sym_max]   = {}; /* --dump-sym globals to read.  */
  uint32_t    dump_sym_addrs[k_dump_sym_max]   = {}; /* resolved while ELF is alive. */
  uint32_t    dump_sym_n                       = 0U;
  const char* stop_sym_name                    = nullptr; /* --stop-sym watch global.      */
  uint32_t    stop_sym_addr                    = 0U;      /* resolved while ELF is alive.  */
  uint32_t    stop_sym_thresh                  = 0U;      /* stop once *global >= this.    */
  const char* trace_sym_names[k_trace_sym_max] = {};      /* --trace-sym functions to log. */
  uint32_t    trace_sym_n                      = 0U;
  const char* save_sd_path                     = nullptr; /* --save-sd dump path. */
  const char* ns_elf_path                      = nullptr; /* --ns: 2nd (NS) elf.  */
  bool        size_set                         = false;
  bool        want_click                       = false;
  bool        want_trace                       = false;
  int         click_x                          = -1;
  int         click_y                          = -1;
  int         button_press                     = 0;     /* 1=SW1, 2=SW2, 0=none.        */
  int         reboot_count                     = 0;     /* --reboot N: warm reboots.    */
  int         battery_soc                      = -1;    /* --battery <pct>, -1=default. */
  bool        battery_charging                 = false; /* --charge.                    */
  bool        battery_opt                      = false; /* any battery flag given.      */
  uint16_t    view_w                           = (uint16_t)k_view_default_w;
  uint16_t    view_h                           = (uint16_t)k_view_default_h;

  board_device_t sim_device = k_board_device_ra8d2; /* --device (RA8P1 profile). */
  bool           usbhs_loop = false;                /* --usbhs-loop: chip-internal USB self-loop. */
  for (int i = 2; i < argc; i++) {
    if (strncmp(argv[i], "--view", sizeof("--view")) == 0) {
      want_view = true;
    } else if (strncmp(argv[i], "--usbhs-loop", sizeof("--usbhs-loop")) == 0) {
      /* Model the on-chip USBHS host controller (board_periph_usbhs_host.c) and
       * bridge it to the USBFS device, instead of the ra8_usb_host_* function
       * seam: for apps whose HS host enumerates the SAME chip's FS device. */
      usbhs_loop = true;
    } else if (strncmp(argv[i], "--trace", sizeof("--trace")) == 0) {
      want_trace = true;
    } else if (strncmp(argv[i], "--low-power", sizeof("--low-power")) == 0) {
      s_low_power = true;
    } else if ((strncmp(argv[i], "--primary-core", sizeof("--primary-core")) == 0) &&
               ((i + 1) < argc)) {
      s_primary_core = (strncmp(argv[i + 1], "m33", sizeof("m33")) == 0) ? k_core_m33 : k_core_m85;
      i++;
    } else if ((strncmp(argv[i], "--device", sizeof("--device")) == 0) && ((i + 1) < argc)) {
      /* Select the emulated part; anything but "ra8p1" (incl. "ra8d2") = RA8D2. */
      sim_device = (strncmp(argv[i + 1], "ra8p1", sizeof("ra8p1")) == 0) ? k_board_device_ra8p1
                                                                         : k_board_device_ra8d2;
      i++;
    } else if ((strncmp(argv[i], "--ppm", sizeof("--ppm")) == 0) && ((i + 1) < argc)) {
      ppm_path = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--record-secs", sizeof("--record-secs")) == 0) &&
               ((i + 1) < argc)) {
      const long s = strtol(argv[i + 1], nullptr, (int)k_strtol_base10);
      record_secs  = (s > 0L) ? (uint32_t)s : 0U;
      i++;
    } else if ((strncmp(argv[i], "--record", sizeof("--record")) == 0) && ((i + 1) < argc)) {
      record_dir = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--rotate", sizeof("--rotate")) == 0) && ((i + 1) < argc)) {
      const long deg = strtol(argv[i + 1], nullptr, (int)k_strtol_base10);
      if ((deg == (long)k_rotate_90) || (deg == (long)k_rotate_180) ||
          (deg == (long)k_rotate_270)) {
        rotate_deg = (uint32_t)deg;
      }
      i++;
    } else if ((strncmp(argv[i], "--panel", sizeof("--panel")) == 0) && ((i + 1) < argc)) {
      panel_path = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--ns", sizeof("--ns")) == 0) && ((i + 1) < argc)) {
      /* Second ELF: the Non-Secure image of a two-image TrustZone app (src/app).
       * Loaded at its LMA so the Secure boot's NS-image copy + BLXNS land on it. */
      ns_elf_path = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--input", sizeof("--input")) == 0) && ((i + 1) < argc)) {
      input_str = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--keys", sizeof("--keys")) == 0) && ((i + 1) < argc)) {
      keys_str = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--usb-in", sizeof("--usb-in")) == 0) && ((i + 1) < argc)) {
      usb_in_str = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--sd", sizeof("--sd")) == 0) && ((i + 1) < argc)) {
      (void)board_sd_attach(argv[i + 1]); /* serve this FAT image to ra8_sdmmc_spi */
      i++;
    } else if ((strncmp(argv[i], "--sd-new", sizeof("--sd-new")) == 0) && ((i + 1) < argc)) {
      /* "<N>[k|m|g|t][:fat16|fat32]" -- create + attach a blank formatted card.
       * A bare number is MiB; a k/m/g/t suffix sets the unit (so "30g" = 30 GiB).
       * Format defaults by size (FAT32 >= 512 MiB) like a real SD card; FAT16
       * cannot exceed its cluster ceiling, so multi-GB cards are FAT32. */
      char*      endp = nullptr;
      const long num  = strtol(argv[i + 1], &endp, (int)k_strtol_base10);
      uint64_t   mult =
        (uint64_t)k_sectors_per_mib * (uint64_t)k_bytes_per_sector; /* default unit: MiB. */
      const char unit = ((endp != nullptr) && (*endp != '\0')) ? (char)tolower((int)*endp) : 'm';
      if (unit == 'k') {
        mult = (uint64_t)k_size_kib;
      } else if (unit == 'g') {
        mult = (uint64_t)k_size_kib * (uint64_t)k_size_kib * (uint64_t)k_size_kib;
      } else if (unit == 't') {
        mult =
          (uint64_t)k_size_kib * (uint64_t)k_size_kib * (uint64_t)k_size_kib * (uint64_t)k_size_kib;
      }
      const uint64_t bytes   = (num > 0L) ? ((uint64_t)num * mult) : 0ULL;
      const uint64_t sectors = bytes / (uint64_t)k_bytes_per_sector;
      const char*    colon   = (endp != nullptr) ? strchr(endp, ':') : nullptr;
      const uint64_t fat32_min_bytes =
        (uint64_t)k_fat32_min_mib * (uint64_t)k_size_kib * (uint64_t)k_size_kib;
      uint8_t fat = (bytes >= fat32_min_bytes) ? (uint8_t)32U : (uint8_t)16U;
      if (colon != nullptr) {
        fat = (strstr(colon, "32") != nullptr) ? (uint8_t)32U : (uint8_t)16U;
      }
      if ((sectors > 0ULL) && (sectors <= (uint64_t)k_sd_u32_max)) {
        (void)board_sd_attach_blank((uint32_t)sectors, fat, "BOARDSIM");
      } else if (sectors > (uint64_t)k_sd_u32_max) {
        (void)fprintf(stderr, "board_sim: --sd-new: size exceeds the 2 TiB FAT limit\n");
      }
      i++;
    } else if ((strncmp(argv[i], "--save-sd", sizeof("--save-sd")) == 0) && ((i + 1) < argc)) {
      save_sd_path = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--dump-sym", sizeof("--dump-sym")) == 0) && ((i + 1) < argc)) {
      if (dump_sym_n < (uint32_t)k_dump_sym_max) {
        dump_sym_names[dump_sym_n] = argv[i + 1];
        dump_sym_n++;
      }
      i++;
    } else if ((strncmp(argv[i], "--stop-sym", sizeof("--stop-sym")) == 0) && ((i + 2) < argc)) {
      /* End the run the instant 32-bit global <name> reaches <threshold> -- the
       * counter analog of BOARD_SIM_STOP_ON, for the jlink_memprobe SIL mode: a
       * free-running progress counter that hits its floor stops the run at once,
       * so a passing probe finishes in a few chunks instead of the full budget. */
      stop_sym_name   = argv[i + 1];
      stop_sym_thresh = (uint32_t)strtoul(argv[i + 2], nullptr, (int)k_strtol_base10);
      i += 2;
    } else if ((strncmp(argv[i], "--trace-sym", sizeof("--trace-sym")) == 0) && ((i + 1) < argc)) {
      if (trace_sym_n < (uint32_t)k_trace_sym_max) {
        trace_sym_names[trace_sym_n] = argv[i + 1];
        trace_sym_n++;
      }
      i++;
    } else if (strncmp(argv[i], "--fast-sd", sizeof("--fast-sd")) == 0) {
      s_fast_sd = true;
    } else if ((strncmp(argv[i], "--click", sizeof("--click")) == 0) && ((i + 2) < argc)) {
      click_x    = (int)strtol(argv[i + 1], nullptr, (int)k_strtol_base10);
      click_y    = (int)strtol(argv[i + 2], nullptr, (int)k_strtol_base10);
      want_click = (click_x >= 0) && (click_y >= 0);
      i += 2;
    } else if ((strncmp(argv[i], "--button", sizeof("--button")) == 0) && ((i + 1) < argc)) {
      button_press = (int)strtol(argv[i + 1], nullptr, (int)k_strtol_base10);
      i += 1;
    } else if ((strncmp(argv[i], "--battery", sizeof("--battery")) == 0) && ((i + 1) < argc)) {
      battery_soc = (int)strtol(argv[i + 1], nullptr, (int)k_strtol_base10);
      battery_opt = true;
      i += 1;
    } else if (strncmp(argv[i], "--charge", sizeof("--charge")) == 0) {
      battery_charging = true;
      battery_opt      = true;
    } else if ((strncmp(argv[i], "--reboot", sizeof("--reboot")) == 0) && ((i + 1) < argc)) {
      reboot_count = (int)strtol(argv[i + 1], nullptr, (int)k_strtol_base10);
      i += 1;
    } else if ((strncmp(argv[i], "--size", sizeof("--size")) == 0) && ((i + 1) < argc)) {
      char*      end = nullptr;
      const long w   = strtol(argv[i + 1], &end, (int)k_strtol_base10);
      const long h =
        ((end != nullptr) && (*end == 'x')) ? strtol(end + 1, nullptr, (int)k_strtol_base10) : 0L;
      if ((w > 0L) && (w <= (long)k_max_panel_px) && (h > 0L) && (h <= (long)k_max_panel_px)) {
        view_w   = (uint16_t)w;
        view_h   = (uint16_t)h;
        size_set = true;
      }
      i++;
    }
  }

  /* A --panel descriptor sizes the window to that display (so the emulator can
   * present any panel, not just 1024x600); an explicit --size still wins. */
  board_panel_t panel      = {};
  bool          have_panel = false;
  if (panel_path != nullptr) {
    have_panel = load_panel(panel_path, &panel);
    if (have_panel && !size_set) {
      view_w = panel.width;
      view_h = panel.height;
    }
  }
  const char* win_title = (have_panel && (panel.name[0] != '\0')) ? panel.name : "board_sim";

  uc_engine* uc = nullptr;
  if (uc_open(UC_ARCH_ARM, (uc_mode)(UC_MODE_THUMB | UC_MODE_MCLASS), &uc) != UC_ERR_OK) {
    (void)fprintf(stderr, "uc_open failed\n");
    return 1;
  }
  /* Closest emulated core to the M85: M33 (Armv8-M). */
  (void)uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_M33);

  for (size_t i = 0U; i < (sizeof(k_regions) / sizeof(k_regions[0])); i++) {
    /* On-chip SRAM is host-backed so a second engine (cpu1) can share the same
     * physical bytes -- the two cores' IPC over shared SRAM is then coherent.
     * Transparent for cpu0 (uc_mem_map_ptr behaves like uc_mem_map otherwise).*/
    uc_err mr = UC_ERR_OK;
    if (k_regions[i].base == (uint64_t)k_sram_base) {
      s_sram_buf = (uint8_t*)aligned_alloc((size_t)k_page_size, (size_t)k_regions[i].size);
      if (s_sram_buf == nullptr) {
        (void)fprintf(stderr, "SRAM host buffer alloc failed\n");
        return 1;
      }
      (void)memset(s_sram_buf, 0, (size_t)k_regions[i].size);
      mr =
        uc_mem_map_ptr(uc, k_regions[i].base, (size_t)k_regions[i].size, UC_PROT_ALL, s_sram_buf);
    } else if (k_regions[i].base == (uint64_t)k_ns_sram2_base) {
      /* The SRAM2 Non-secure alias (bit[28]=1) is the SAME physical bytes as
       * 0x22100000, so back it with the shared SRAM host buffer at that offset.
       * Keeping the Secure (0x22..) and Non-secure (0x32..) views coherent lets
       * the BLXNS land on the copied NS image whether the core uses the alias or
       * the bit[28]-stripped physical address (s_sram_buf is mapped above first,
       * since SRAM precedes NS_SRAM2 in k_regions). */
      uint8_t* const ns_host =
        s_sram_buf + ((size_t)k_ns_sram2_base - (size_t)k_ns_alias_bit - (size_t)k_sram_base);
      mr = uc_mem_map_ptr(uc, k_regions[i].base, (size_t)k_regions[i].size, UC_PROT_ALL, ns_host);
    } else if (k_regions[i].base == (uint64_t)k_ospi_xip_base) {
      /* OSPI XIP flash (Secure physical view). Host-backed -- like the on-chip
       * SRAM -- so its Non-secure alias (0x90000000) can mirror the SAME bytes:
       * an execute-in-place image is then fetchable through either view. The
       * window is mapped UC_PROT_ALL (read + write + EXEC) so the CPU may fetch
       * instructions from it. */
      s_ospi_buf = (uint8_t*)aligned_alloc((size_t)k_page_size, (size_t)k_regions[i].size);
      if (s_ospi_buf == nullptr) {
        (void)fprintf(stderr, "OSPI host buffer alloc failed\n");
        return 1;
      }
      (void)memset(s_ospi_buf, 0, (size_t)k_regions[i].size);
      mr =
        uc_mem_map_ptr(uc, k_regions[i].base, (size_t)k_regions[i].size, UC_PROT_ALL, s_ospi_buf);
    } else if (k_regions[i].base == (uint64_t)k_ospi_ns_base) {
      /* OSPI XIP Non-secure alias (bit[28]=1): the SAME flash array as
       * 0x80000000, so back it with the shared OSPI host buffer (stripping
       * bit[28] from 0x90000000 yields the Secure base, i.e. offset 0). The NS
       * reader image is linked for XIP at 0x90000000, so the CPU fetches its
       * .text from this view; load_elf writes each PT_LOAD to its p_paddr, which
       * lands in this same backing whether the segment names 0x80.. or 0x90..
       * (s_ospi_buf is mapped above first, since OSPI precedes NS_OSPI in
       * k_regions). */
      uint8_t* const ns_host =
        s_ospi_buf + ((size_t)k_ospi_ns_base - (size_t)k_ns_alias_bit - (size_t)k_ospi_xip_base);
      mr = uc_mem_map_ptr(uc, k_regions[i].base, (size_t)k_regions[i].size, UC_PROT_ALL, ns_host);
    } else if (k_regions[i].base == (uint64_t)k_sdram_base) {
      /* External SDRAM (Secure physical view). Host-backed -- like the on-chip
       * SRAM and OSPI -- so its Non-secure alias (0x78000000) can mirror the
       * SAME bytes. The Secure GLCDC model scans the framebuffer from here while
       * the Non-secure e-reader writes pixels through the alias below; one
       * backing keeps the two views coherent. */
      s_sdram_buf = (uint8_t*)aligned_alloc((size_t)k_page_size, (size_t)k_regions[i].size);
      if (s_sdram_buf == nullptr) {
        (void)fprintf(stderr, "SDRAM host buffer alloc failed\n");
        return 1;
      }
      (void)memset(s_sdram_buf, 0, (size_t)k_regions[i].size);
      mr =
        uc_mem_map_ptr(uc, k_regions[i].base, (size_t)k_regions[i].size, UC_PROT_ALL, s_sdram_buf);
    } else if (k_regions[i].base == (uint64_t)k_ns_sdram_base) {
      /* External SDRAM Non-secure alias (bit[28]=1): the SAME array as
       * 0x68000000, so back it with the shared SDRAM host buffer (stripping
       * bit[28] from 0x78000000 yields the Secure base, i.e. offset 0). The NS
       * world draws the framebuffer through this alias; the Secure GLCDC reads it
       * back from 0x68000000 over the same bytes (s_sdram_buf is mapped above
       * first, since SDRAM precedes NS_SDRAM in k_regions). */
      uint8_t* const ns_host =
        s_sdram_buf + ((size_t)k_ns_sdram_base - (size_t)k_ns_alias_bit - (size_t)k_sdram_base);
      mr = uc_mem_map_ptr(uc, k_regions[i].base, (size_t)k_regions[i].size, UC_PROT_ALL, ns_host);
    } else {
      mr = uc_mem_map(uc, k_regions[i].base, (size_t)k_regions[i].size, UC_PROT_ALL);
    }
    if (mr != UC_ERR_OK) {
      (void)fprintf(stderr,
                    "map %s @0x%08llX failed\n",
                    k_regions[i].name,
                    (unsigned long long)k_regions[i].base);
      return 1;
    }
  }
  /* Factory-programmed on silicon; seed it now the TRIM page is mapped so the
   * temperature-sensor two-point conversion reads real data (see the helper). */
  seed_tsn_calibration(uc);
  if (uc_mmio_map(uc,
                  (uint64_t)k_periph_base,
                  (size_t)k_periph_size,
                  mmio_read,
                  nullptr,
                  mmio_write,
                  nullptr) != UC_ERR_OK) {
    (void)fprintf(stderr, "mmio_map failed\n");
    return 1;
  }
  /* IDAU bit[28]=1 Non-secure peripheral alias (0x50000000): the SAME silicon
   * registers as 0x40000000, reached by TrustZone Non-secure code (an NS image
   * built with RA8_PERIPH_NS_ALIAS -- MSTP at 0x5020_3000, USBFS at 0x5025_0000,
   * USBHS at 0x5035_1000 -- or the NS-side IPC ping-pong at 0x5002_0000). The
   * hooks rebuild the absolute address as k_periph_base + window-relative
   * offset, so a 0x50020000 access (offset 0x20000) dispatches to 0x40020000
   * identically to a Secure one -- the exact mapping the cpu1 engine already
   * carries (see cpu1_engine_init). */
  if (uc_mmio_map(uc,
                  (uint64_t)k_periph_base | (uint64_t)k_ns_alias_bit,
                  (size_t)k_periph_size,
                  mmio_read,
                  nullptr,
                  mmio_write,
                  nullptr) != UC_ERR_OK) {
    (void)fprintf(stderr, "mmio_map (NS alias) failed\n");
    return 1;
  }

  /* Seed read-only/hardwired SCS registers the firmware reads back. The PPB is
   * plain RAM here, so MPU_TYPE would otherwise read 0 (DREGION=0) and
   * ra8_mpu_configure() would reject any region (region_count > 0 implemented),
   * panicking mpu_partition_simple. Hardwire DREGION to the M85's 8 data
   * regions so the configure path validates; board_sim does not yet *enforce*
   * MPU permissions, so the app then takes its documented no-trap host path. */
  wr32(uc, (uint64_t)k_mpu_type, (uint32_t)k_mpu_type_seed);

  /* Reset the peripheral-model framework (GPIO/PORT, AGT/GPT timers, ICU/NVIC,
   * SCI UART) before the firmware boots so its register writes land in real
   * block state. Install the console sink so transmitted bytes reach stdout,
   * and queue any --input as console-channel (SCI8) RX. */
  board_periph_init(want_trace);
  /* Select the emulated part BEFORE the run: gates the RA8P1-only NPU block.
   * Default RA8D2 leaves every RA8D2 run unchanged. */
  board_periph_set_device(sim_device);
  /* --usbhs-loop: activate the on-chip USBHS host model (owns 0x40351000) and
   * hand the USBFS device over to its bridge so the built-in virtual host stands
   * down -- one real host drives the device, closing the chip-internal loop. */
  board_periph_set_usbhs_loop(usbhs_loop);
  board_usb_set_external_host(usbhs_loop);
  board_net_init(want_trace);
  board_periph_sci_set_tx_sink(console_tx_sink);
  /* --button N: hold a user switch pressed (active-low) before the firmware
   * boots, so a button-polling app (e.g. gpio_input_demo: SW1 -> LED1) takes
   * its pressed path. SW1 = P009, SW2 = P008. */
  if (button_press != 0) {
    const uint8_t pin = (button_press == 2) ? (uint8_t)k_sim_sw2_pin : (uint8_t)k_sim_sw1_pin;
    board_periph_gpio_set_input((uint8_t)k_sim_sw_port, pin, false);
    (void)fprintf(stderr,
                  "board_sim: --button %d held (SW pin P00%u low/pressed)\n",
                  button_press,
                  (unsigned)pin);
  }
  if (battery_opt) {
    uint8_t cur_soc = 0U;
    board_periph_battery_get(&cur_soc, nullptr);
    const uint8_t soc = (battery_soc >= 0) ? (uint8_t)battery_soc : cur_soc;
    board_periph_battery_set(soc, battery_charging);
    (void)fprintf(stderr,
                  "board_sim: battery %u%% %s (MAX17048 @ I2C 0x36)\n",
                  (unsigned)soc,
                  battery_charging ? "charging" : "discharging");
  }
  if (input_str != nullptr) {
    uint8_t        rx[k_uart_line_max];
    const uint32_t n = decode_escapes(input_str, rx, (uint32_t)sizeof(rx));
    board_periph_sci_feed_rx(board_periph_sci_console_channel(), rx, n);
    (void)fprintf(stderr,
                  "board_sim: queued %u byte(s) to SCI%u RX from --input\n",
                  n,
                  board_periph_sci_console_channel());
  }
  /* --keys: push a scripted string through the SAME keystroke FIFO the live
   * --view window feeds, so the run loop drains it to the console UART RX over
   * the identical path -- a headless, deterministic test of the keyboard input
   * (no window, no OS key events). */
  if (keys_str != nullptr) {
    uint8_t        kb[k_uart_line_max];
    const uint32_t n = decode_escapes(keys_str, kb, (uint32_t)sizeof(kb));
    for (uint32_t k = 0U; k < n; k++) {
      board_input_push_key((char)kb[k]);
    }
    (void)fprintf(stderr, "board_sim: queued %u keystroke(s) via --keys (window-key path)\n", n);
  }
  /* Queue any --usb-in bytes for the virtual host to push over the CDC bulk OUT
   * pipe once the device is configured; the device echoes them back on bulk IN. */
  if (usb_in_str != nullptr) {
    uint8_t        ub[k_uart_line_max];
    const uint32_t n = decode_escapes(usb_in_str, ub, (uint32_t)sizeof(ub));
    board_usb_feed_bulk_in(ub, n);
    (void)fprintf(stderr, "board_sim: queued %u byte(s) to the USB CDC bulk OUT pipe\n", n);
  }

  long           elf_len = 0;
  uint8_t* const elf     = read_file(elf_path, &elf_len);
  if (elf == nullptr) {
    (void)fprintf(stderr, "cannot read %s\n", elf_path);
    return 1;
  }
  (void)fprintf(stderr, "board_sim: loading %s (%ld bytes)\n", elf_path, elf_len);
  (void)fprintf(stderr,
                "  device        : %s\n",
                (board_periph_device() == k_board_device_ra8p1)
                  ? "RA8P1 (R7KA8P1KFLCAC) -- +Ethos-U55 NPU"
                  : "RA8D2 (R7KA8D2KFLCAC)");
  (void)fprintf(stderr,
                "  primary core  : %s%s\n",
                (s_primary_core == k_core_m33) ? "Cortex-M33 (Armv8-M)"
                                               : "Cortex-M85 (Armv8.1-M, MVE seams armed)",
                s_low_power ? "  [low-power: 1/4 chunk budget]" : "");
  if (load_elf(uc, elf, elf_len) != 0) {
    free(elf);
    return 1;
  }

  /* Two-image TrustZone app (--ns): load the Non-Secure image at its LMA
   * (0x02080000 in MRAM). The Secure boot copies LMA -> the NS_SRAM2 run alias
   * (0x32100000) and BLXNS-es to it, so the NS world (ThreadX + the e-reader)
   * executes real code instead of an empty alias. The host-side buffer stays
   * alive through the --dump-sym / --stop-sym resolution below: those probe
   * counters live in the NS image's OWN symbol table (the Secure ELF has no
   * NS-image symbols), so both tables are consulted. */
  long           ns_len = 0;
  uint8_t* const ns_elf = (ns_elf_path != nullptr) ? read_file(ns_elf_path, &ns_len) : nullptr;
  if (ns_elf_path != nullptr) {
    if (ns_elf == nullptr) {
      (void)fprintf(stderr, "cannot read --ns %s\n", ns_elf_path);
      free(elf);
      return 1;
    }
    (void)fprintf(stderr, "board_sim: loading NS image %s (%ld bytes)\n", ns_elf_path, ns_len);
    if (load_elf(uc, ns_elf, ns_len) != 0) {
      free(ns_elf);
      free(elf);
      return 1;
    }
    /* Track the NS image's actual vector base (lowest executable PT_LOAD VMA) so
     * the BLXNS world switch reads MSP/reset from the right place: 0x32100000
     * for a RAM-resident NS image, 0x90000000 for one that runs XIP from OSPI.
     * Keep the default if the header cannot be parsed. */
    const uint32_t ns_vbase = elf_vector_base(ns_elf, ns_len);
    if (ns_vbase != 0U) {
      s_ns_vector_base = ns_vbase;
    }
    (void)fprintf(stderr, "board_sim: NS vector base @ 0x%08X\n", s_ns_vector_base);
  }
  /* NB: keep the host-side `elf` buffer alive until after the seam installers --
   * load_elf has copied the image into Unicorn memory, but usbh_seam_install /
   * sym_trace_install still scan the ELF symbol table from this buffer (freed
   * right after, below). Ethernet no longer needs it: board_periph_eth models
   * the R-Switch registers, so the genuine ra8_eth driver runs (no symbol seam). */

  /* Resolve any --dump-sym globals to addresses now, while the ELF symbol
   * tables are still resident; the values are read back from Unicorn memory
   * after the run (the software analog of the JLink memprobe HIL mode). A
   * two-image app's probe counters live in the NS image, so a miss in the
   * primary table falls through to the --ns symbol table. */
  for (uint32_t d = 0U; d < dump_sym_n; d++) {
    dump_sym_addrs[d] = elf_sym_addr(elf, elf_len, dump_sym_names[d], nullptr);
    if ((dump_sym_addrs[d] == 0U) && (ns_elf != nullptr)) {
      dump_sym_addrs[d] = elf_sym_addr(ns_elf, ns_len, dump_sym_names[d], nullptr);
    }
    if (dump_sym_addrs[d] == 0U) {
      (void)fprintf(stderr,
                    "board_sim: --dump-sym %s not found in symbol table\n",
                    dump_sym_names[d]);
    }
  }

  /* Resolve the --stop-sym watch global the same way (the address is valid for
   * the whole run; read at each chunk boundary in the run loop below). */
  if (stop_sym_name != nullptr) {
    stop_sym_addr = elf_sym_addr(elf, elf_len, stop_sym_name, nullptr);
    if ((stop_sym_addr == 0U) && (ns_elf != nullptr)) {
      stop_sym_addr = elf_sym_addr(ns_elf, ns_len, stop_sym_name, nullptr);
    }
    if (stop_sym_addr == 0U) {
      (void)fprintf(stderr, "board_sim: --stop-sym %s not found in symbol table\n", stop_sym_name);
    }
  }
  /* NS-image symbol table no longer needed (its bytes are in Unicorn memory). */
  free(ns_elf);

  /* TrustZone NSC pointer validation. The Non-Secure-Callable veneers guard
   * their pointer args with cmse_check_address_range(), which issues Armv8-M
   * `TT`/`TTA` (Test Target) instructions to read an address's security/MPU
   * attribution and then checks the Non-Secure read/write bit. Unicorn's M33
   * has no SAU/IDAU configured (board_sim maps the PPB as plain RAM, so the
   * core's internal SAU stays at its reset all-Secure state); a native TT thus
   * reports every address as Secure, the NS range-check fails, and the veneer
   * returns k_ra8_err_invalid_arg -- stalling CGC/SD bring-up. board_sim collapses
   * the Secure/Non-Secure split into one flat, fully-accessible domain, so every
   * NS pointer the veneers pass (each already null-checked before the range check)
   * is valid. Model that by patching the routine's entry to `BX LR`: r0 still
   * holds the first argument (the pointer `p`) at entry, so an immediate return
   * yields p != NULL == "address OK". This is a one-time 2-byte memory patch
   * (the function image is already copied into Unicorn memory by load_elf), not a
   * UC_HOOK_CODE -- a code hook forces Unicorn to single-step the whole run
   * (~10x slower), whereas the patch has zero steady-state cost. Absent in
   * non-TZ firmware (symbol not found -> no patch). */
  const uint32_t cmse_check_addr = elf_sym_addr(elf, elf_len, "cmse_check_address_range", nullptr);
  if (cmse_check_addr != 0U) {
    const uint16_t bx_lr = (uint16_t)k_thumb_bx_lr;
    (void)uc_mem_write(uc, (uint64_t)cmse_check_addr, &bx_lr, sizeof(bx_lr));
  }

  /* Cortex-M reset: SP = vectors[0], PC = vectors[1] (Thumb, clear bit0). */
  uint32_t sp = 0U;
  uint32_t pc = 0U;
  (void)uc_mem_read(uc, k_regions[1].base + 0U, &sp, 4); /* MRAM[0]                 */
  (void)uc_mem_read(uc, k_regions[1].base + 4U, &pc, 4); /* MRAM[4] (Thumb: bit0=1) */
  /* M-profile is always Thumb (EPSR.T must be 1). Keep the reset vector's bit0
   * and set the xPSR Thumb bit so Unicorn enters Thumb, not ARM, decoding. */
  pc |= 1U;
  uint32_t xpsr = (uint32_t)k_xpsr_t_bit; /* xPSR.T */
  (void)uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
  (void)uc_reg_write(uc, UC_ARM_REG_XPSR, &xpsr);
  (void)fprintf(stderr,
                "board_sim: reset SP=0x%08X PC=0x%08X -- running (<= %u x %u insns, %u s wall)\n",
                sp,
                pc,
                (unsigned)k_run_max_chunks,
                (unsigned)k_run_chunk_insns,
                (unsigned)k_run_wall_s);

  /* MRAM holds the vector table; the run loop passes this as the VTOR fallback
   * to exc_take_pending (the live VTOR, set by SystemInit, is read inside). */
  const uint32_t vtor_base = (uint32_t)k_regions[1].base;

  uc_hook h_invalid;
  uc_hook h_unmapped;
  uc_hook h_intr;
  uc_hook h_icsr;
  (void)uc_hook_add(uc, &h_invalid, UC_HOOK_INSN_INVALID, (void*)on_invalid_insn, nullptr, 1, 0);
  (void)uc_hook_add(uc, &h_unmapped, UC_HOOK_MEM_UNMAPPED, (void*)on_unmapped, nullptr, 1, 0);
  /* SVCall / exception-return: Unicorn raises UC_HOOK_INTR on a Thumb `svc` and
   * on a branch to an EXC_RETURN magic; on_intr vectors / unstacks accordingly. */
  (void)uc_hook_add(uc, &h_intr, UC_HOOK_INTR, (void*)on_intr, nullptr, 1, 0);
  /* Watch the ICSR word so a PENDSVSET store ends the chunk at once, giving
   * PendSV next-instruction activation (see on_icsr_write). ICSR lives in PPB
   * RAM, so this memory-write hook is the only way to observe the request. */
  (void)uc_hook_add(uc,
                    &h_icsr,
                    UC_HOOK_MEM_WRITE,
                    (void*)on_icsr_write,
                    nullptr,
                    (uint64_t)k_scb_icsr,
                    (uint64_t)k_scb_icsr + 3U);
  /* Seed the ITM "ready" bits in PPB RAM and echo stimulus-port writes, so
   * ra8_log output (e.g. the TrustZone e-reader's ra8_nsc_log_emit lines) prints
   * as `[itm] ...`. STIM0 is in PPB RAM, so the write-hook is the only way to
   * observe the log bytes (see itm_seed_ready / on_itm_stim_write). */
  itm_seed_ready(uc);
  uc_hook h_itm;
  (void)uc_hook_add(uc,
                    &h_itm,
                    UC_HOOK_MEM_WRITE,
                    (void*)on_itm_stim_write,
                    nullptr,
                    (uint64_t)k_itm_stim0_addr,
                    (uint64_t)k_itm_stim0_addr + 3U);
  /* TrustZone S->NS boot seams -- armed whenever the firmware links the secure
   * boot's ra8_tz_secure_boot_jump_ns: a two-image --ns app, OR a single-image
   * app whose NS half is embedded at its MRAM LMA (cpu1_pingpong_ipc). The
   * Secure boot bails to its fallback main() unless SAU_TYPE.SREGION >= 4/5;
   * board_sim maps the PPB as plain RAM (SAU_TYPE reads 0), so seed the M85's
   * 8-region count to let the real SAU programming + NS-image copy + BLXNS
   * run. Firmware without the symbol keeps its current (all-Secure) path. */
  {
    uint32_t       jn_size = 0U;
    const uint32_t jump_ns = elf_sym_addr(elf, elf_len, "ra8_tz_secure_boot_jump_ns", &jn_size);
    if ((jump_ns != 0U) && (jn_size >= (uint32_t)k_thumb_hw_bytes)) {
      const uint32_t sau_type = (uint32_t)k_sau_type_regs;
      (void)uc_mem_write(uc, (uint64_t)k_sau_type_addr, &sau_type, sizeof(sau_type));

      /* Hand-emulate the Secure->NS BLXNS in ra8_tz_secure_boot_jump_ns.
       * Unicorn's all-Secure M33 cannot really switch worlds, so resolve the
       * BLXNS site from the Secure symtab (scan the function for the BLXNS
       * opcode) and hook it to enter NS manually (see on_blxns). Without this
       * the BLXNS stalls and the NS world never runs. */
      uint32_t blxns_at = 0U;
      for (uint32_t a = jump_ns; (a + (uint32_t)k_thumb_hw_bytes) <= (jump_ns + jn_size);
           a += (uint32_t)k_thumb_hw_bytes) {
        uint16_t hw = 0U;
        (void)uc_mem_read(uc, (uint64_t)a, &hw, sizeof(hw));
        if (((uint32_t)hw & (uint32_t)k_blxns_mask) == (uint32_t)k_blxns_match) {
          blxns_at = a;
          break;
        }
      }
      if (blxns_at != 0U) {
        uc_hook h_blxns;
        (void)uc_hook_add(uc,
                          &h_blxns,
                          UC_HOOK_CODE,
                          (void*)on_blxns,
                          nullptr,
                          (uint64_t)blxns_at,
                          (uint64_t)blxns_at);
        (void)fprintf(stderr, "board_sim: TZ BLXNS seam armed @ 0x%08X\n", blxns_at);
      } else {
        (void)fprintf(stderr,
                      "board_sim: TZ warning: no BLXNS found in ra8_tz_secure_boot_jump_ns\n");
      }
    }
  }
  /* Watch the SCB control words AIRCR..CCR with ONE hook (on_scb_ctrl_write): a
   * SYSRESETREQ store triggers a warm reboot and a CCR.DIV_0_TRP store arms the
   * div-0 trap (which then patches divides -- no per-access cost). Both are PPB
   * RAM, so a write-hook is the only way to observe them. */
  uc_hook h_scb;
  (void)uc_hook_add(uc,
                    &h_scb,
                    UC_HOOK_MEM_WRITE,
                    (void*)on_scb_ctrl_write,
                    nullptr,
                    (uint64_t)k_scb_aircr,
                    (uint64_t)k_scb_ccr + 3U);
  /* NVIC ISER / ICER are set-enable / clear-enable: fold each written bit into
   * board_periph's enable shadow so enabling several lines does not clobber the
   * earlier ones (see on_nvic_en_write). The PPB is RAM, so this hook is the
   * only place the W1S/W1C semantics can be applied. */
  uc_hook h_nvic_iser;
  uc_hook h_nvic_icer;
  (void)uc_hook_add(uc,
                    &h_nvic_iser,
                    UC_HOOK_MEM_WRITE,
                    (void*)on_nvic_en_write,
                    nullptr,
                    (uint64_t)k_nvic_iser_base,
                    (uint64_t)k_nvic_iser_base + (uint64_t)k_nvic_en_span - 1U);
  (void)uc_hook_add(uc,
                    &h_nvic_icer,
                    UC_HOOK_MEM_WRITE,
                    (void*)on_nvic_en_write,
                    nullptr,
                    (uint64_t)k_nvic_icer_base,
                    (uint64_t)k_nvic_icer_base + (uint64_t)k_nvic_en_span - 1U);
  /* Armv8-M MPU enforcement: capture each region at its RLAR write, then arm /
   * disarm read-only-region write traps on MPU_CTRL.ENABLE edges. Inert unless
   * the firmware programs and enables the MPU (mpu_partition_simple); a trapped
   * store synthesises MemManage in the run loop (see on_mpu_ro_write). */
  uc_hook h_mpu_rlar;
  uc_hook h_mpu_ctrl;
  (void)uc_hook_add(uc,
                    &h_mpu_rlar,
                    UC_HOOK_MEM_WRITE,
                    (void*)on_mpu_rlar_write,
                    nullptr,
                    (uint64_t)k_mpu_rlar,
                    (uint64_t)k_mpu_rlar + 3U);
  (void)uc_hook_add(uc,
                    &h_mpu_ctrl,
                    UC_HOOK_MEM_WRITE,
                    (void*)on_mpu_ctrl_write,
                    nullptr,
                    (uint64_t)k_mpu_ctrl,
                    (uint64_t)k_mpu_ctrl + 3U);
  /* Ethernet: the ra8_eth / ra8_etha / ra8_rmac / ra8_eth_gwca register path runs
   * for real against the board_periph_eth R-Switch model -- no frame-API seam.
   * The virtual peer (board_net) is fed by that model's descriptor DMA. */
  /* Virtual USB host-mode device (HID boot keyboard): inert unless the firmware
   * links the ra8_usb_host_* primitives, so device-mode apps are unaffected.
   * Skipped under --usbhs-loop: there the real ra8_usb_host_* functions must run
   * so they drive the modelled USBHS host controller (board_periph_usbhs_host.c),
   * which bridges to the on-chip USBFS device -- the chip-internal self-loop. */
  bool usbh_seamed = false;
  if (!usbhs_loop) {
    usbh_seamed = usbh_seam_install(uc, elf, elf_len);
  }
  /* Register-level USBHS host model (board_usb_host.c): only an UNSEAMED,
   * non-loop firmware may engage it. A C-level seam family already shadows a
   * seamed app's host API with a virtual device, and under --usbhs-loop the
   * loop-only registered block (board_periph_usbhs_host.c) owns the USBHS
   * window, so the two register models must not both answer it. The TrustZone
   * two-image NS host (tz_nsc_cgc_usb) is unseamed by construction: the
   * installer scans only the primary (Secure) ELF, which links no host
   * symbols. */
  board_usb_host_set_allowed(!usbh_seamed && !usbhs_loop);
  /* Arm any --trace-sym entry hooks (debugging instrument: watch a bring-up
   * sequence reach -- or stall before -- a given function). Done while the
   * host-side `elf` buffer is still alive for symbol resolution. */
  sym_trace_install(uc, elf, elf_len, trace_sym_names, trace_sym_n);
  /* The long-shift (LSLL/LSRL/ASRL) and MVE (Helium VSTRW.32) seams emulate
   * Armv8.1-M instructions that only the Cortex-M85 emits but Unicorn's M33
   * core mis-executes. With --primary-core m33 the firmware is pure Armv8-M, so
   * leave the M85-only seams off (they would be inert anyway -- no sites -- but
   * gating keeps the M33 model honest and the telemetry accurate). The
   * cond-select (CSEL) emulation rides the invalid-instruction hook and stays
   * armed; it is likewise inert for an M33 image. */
  if (s_primary_core == k_core_m85) {
    long_shift_seam_install(uc, elf, elf_len);
    mve_seam_install(uc, elf, elf_len);
  }
  /* Divide-by-zero UsageFault (CCR.DIV_0_TRP): track every UDIV/SDIV site for
   * every core -- the sites are overwritten with UDF by on_scb_ctrl_write only once
   * the firmware sets DIV_0_TRP, so a normal app pays nothing (see div0_seam_install
   * / div0_patch_sites / emulate_div0_patched). */
  div0_seam_install(elf, elf_len);
  /* --fast-sd (opt-in): serve whole SD blocks from the image in one C hook entry
   * instead of clocking 512 SPI bytes each, so a book-sized read loads fast.
   * Inert without the flag or without an SD-capable firmware (see fast_sd). */
  fast_sd_seam_install(uc, elf, elf_len);
  /* BOARD_SIM_PROFILE: collect FUNC symbols for the profiler; in per-instruction
   * mode (=full) arm a code hook that tallies every instruction + call. */
  prof_load(elf, elf_len);
  if (s_prof_mode == k_prof_insn) {
    static uc_hook h_prof;
    (void)uc_hook_add(uc, &h_prof, UC_HOOK_CODE, (void*)prof_insn_hook, nullptr, 1, 0);
  }
  /* Spin up the cpu1 engine for dual-core firmware (shares SRAM with cpu0).
   * NULL for single-core apps -- cpu0 then runs exactly as before. The elf
   * buffer is kept alive for the whole run (freed after the run loop) because a
   * warm reboot re-loads its PT_LOAD segments from it; cpu1_engine_init also
   * reads cpu1's image from it. */
  s_cpu1_uc = cpu1_engine_init(elf, elf_len);

  /* The board view is the panel framebuffer (panel_w x panel_h, left) plus a
   * status sidebar (LEDs / USB / UART / IRQ / touch); the composite buffer is
   * what both the live window and the --ppm snapshot show. panel_fb holds the
   * GLCDC render before it is composited into the sidebar-widened composite. */
  const uint16_t panel_w = view_w;
  const uint16_t panel_h = view_h;
  /* --rotate turns the displayed panel for viewing (90/270 swap W and H, so a
   * landscape app can be shown portrait); the firmware still renders panel_w x
   * panel_h, which build_composite rotates into rot_fb. */
  const bool rot_swap =
    (rotate_deg == (uint32_t)k_rotate_90) || (rotate_deg == (uint32_t)k_rotate_270);
  const uint16_t disp_w    = rot_swap ? panel_h : panel_w;
  const uint16_t disp_h    = rot_swap ? panel_w : panel_h;
  const uint16_t comp_w    = board_overlay_total_width(disp_w);
  const uint16_t comp_h    = board_overlay_total_height(disp_h);
  board_view_t*  view      = nullptr;
  uint16_t*      panel_fb  = nullptr;
  uint16_t*      rot_fb    = nullptr;
  uint16_t*      composite = nullptr;
  if (want_view) {
    view = board_view_open(comp_w, comp_h, win_title);
    if (view == nullptr) {
      (void)fprintf(stderr, "board_sim: could not open window; continuing headless\n");
    }
  }
  if ((view != nullptr) || (ppm_path != nullptr) || want_click || (record_dir != nullptr)) {
    panel_fb  = (uint16_t*)malloc((size_t)panel_w * (size_t)panel_h * sizeof(uint16_t));
    composite = (uint16_t*)malloc((size_t)comp_w * (size_t)comp_h * sizeof(uint16_t));
    if (rotate_deg != (uint32_t)k_rotate_0) {
      rot_fb = (uint16_t*)malloc((size_t)disp_w * (size_t)disp_h * sizeof(uint16_t));
    }
  }

  if (want_click) {
    (void)fprintf(stderr, "board_sim: --click armed at (%d,%d)\n", click_x, click_y);
  }
  enum : uint32_t {
    /* ra8_delay_ms(16) in the render loop spins on SysTick, which advances one
     * tick per chunk, so ~16 chunks == one loop iteration. Give the injected
     * tap many iterations to flow DOWN -> UP -> CLICKED -> tab switch -> repaint. */
    k_click_settle_chunks = 512U, /**< Extra chunks after the click lands. */
  };
  /* BOARD_SIM_CLICK_SETTLE=N: widen the post-click drain for a tap that kicks off
   * a long operation (e.g. opening a big book from SD: read + inflate + decode +
   * render can need far more than the default window). Mirrors BOARD_SIM_MAX_CHUNKS;
   * unset keeps the default so ordinary tap captures stay snappy. */
  uint32_t click_settle_chunks = (uint32_t)k_click_settle_chunks;
  {
    const char* e_settle = getenv("BOARD_SIM_CLICK_SETTLE");
    if (e_settle != nullptr) {
      const long v = strtol(e_settle, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        click_settle_chunks = (uint32_t)v;
      }
    }
  }

  /* Chunked run: emulate a block, take a SysTick (and any pending PendSV),
   * repeat. Within a chunk, exception returns (a handler's "BX lr" into an
   * EXC_RETURN magic) and SVCalls are resolved inline -- the inner loop unstacks
   * / vectors and relaunches from the new PC without consuming a chunk -- so a
   * context switch does not cost a whole scheduling quantum and the once-per-
   * chunk SysTick cadence (one ThreadX tick) is preserved. Headless runs stop on
   * a chunk budget + wall-clock guard; in --view the loop runs until the window
   * is closed, presenting the live GLCDC output every k_view_present_every. */
  uint32_t max_chunks =
    (view != nullptr) ? (uint32_t)k_view_max_chunks : (uint32_t)k_run_max_chunks;
  /* Headless runs of heavy apps (e.g. sd_font_render: a 400 KB SD font read
   * plus stb_truetype rasterisation) can need more than the default budget.
   * BOARD_SIM_WALL_S / BOARD_SIM_MAX_CHUNKS override the guards without a
   * recompile; they have no effect in --view (window-driven) mode.
   *
   * The wall-clock guard is CPU-time (clock(), see the run loop), so a
   * heavily-loaded host burns it faster than wall time -- which made it
   * truncate the otherwise instruction-deterministic chunk budget under CI load
   * and silently drop a late-printing banner (#168). A run that must stay
   * deterministic regardless of host load sets BOARD_SIM_WALL_S=0 to DISABLE the
   * guard entirely and rely on the deterministic BOARD_SIM_MAX_CHUNKS bound (and
   * BOARD_SIM_STOP_ON) instead. (Previously WALL_S=0 silently fell back to the
   * default -- the footgun that hid the #168 root cause.) */
  double wall_s        = (double)k_run_wall_s;
  bool   wall_guard_on = true;
  {
    const char* e_wall = getenv("BOARD_SIM_WALL_S");
    if (e_wall != nullptr) {
      const long v = strtol(e_wall, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        wall_s = (double)v;
      } else if (v == 0L) {
        wall_guard_on = false; /* explicit opt-out: bound the run by chunks only */
      }
    }
    const char* e_chunks = getenv("BOARD_SIM_MAX_CHUNKS");
    if ((e_chunks != nullptr) && (view == nullptr)) {
      const long v = strtol(e_chunks, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        max_chunks = (uint32_t)v;
      }
    }
  }
  /* Headless --record-secs bounds the run to exactly the recording window so the
   * dumped frame sequence spans the requested emulated duration. */
  if ((record_dir != nullptr) && (record_secs > 0U) && (view == nullptr)) {
    max_chunks = record_secs * (uint32_t)k_record_ms_per_sec;
  }
  if (record_dir != nullptr) {
    (void)mkdir(record_dir, (mode_t)k_record_dir_mode);
    (void)fprintf(stderr,
                  "board_sim: recording to %s/frame_NNNNNN.ppm (every %u chunks, ~%u fps)\n",
                  record_dir,
                  (unsigned)k_record_every,
                  (unsigned)k_record_fps);
  }
  /* BOARD_SIM_IDLE_STOP=N: in a plain headless run, stop early once observable
   * state has not changed for N consecutive chunks -- the firmware has reached
   * steady-state idle (an RTOS idle spin, a `while(1) wfi`), so there is nothing
   * left to run. Off (0) by default, so normal runs are unaffected; opt in for
   * RTOS/idle apps that would otherwise burn the whole wall-clock budget. The
   * tracked counters (peripheral MMIO reads/writes, PendSV/SVCall, peripheral
   * IRQs) are all monotonic, so an unchanged sum means none of them advanced. */
  uint32_t idle_stop_chunks = 0U;
  {
    const char* e_idle = getenv("BOARD_SIM_IDLE_STOP");
    if ((e_idle != nullptr) && (view == nullptr)) {
      const long v = strtol(e_idle, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        idle_stop_chunks = (uint32_t)v;
      }
    }
  }
  /* BOARD_SIM_USB_STOP=N: stop a plain headless run N chunks after the virtual USB
   * host reports the device CONFIGURED. The USB device apps never go idle -- HID
   * jiggles its boot mouse forever, MSC keeps answering host polls -- so the
   * idle-stop above never fires for them. This makes the smoke gate fast and
   * deterministic: it runs exactly long enough to confirm enumeration + the first
   * class traffic, then stops, instead of burning the whole chunk/wall budget. */
  uint32_t usb_stop_settle = 0U;
  {
    const char* e_usb = getenv("BOARD_SIM_USB_STOP");
    if ((e_usb != nullptr) && (view == nullptr)) {
      const long v = strtol(e_usb, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        usb_stop_settle = (uint32_t)v;
      }
    }
  }
  /* BOARD_SIM_USBH_STOP=N: as above but for a USB HOST-mode app -- stop N chunks
   * after the virtual host-mode keyboard has streamed its reports. Kept distinct
   * from USB_STOP so a host app that also runs a device worker is not stopped by
   * that worker reaching CONFIGURED before the host side completes. */
  uint32_t usbh_stop_settle = 0U;
  {
    const char* e_usbh = getenv("BOARD_SIM_USBH_STOP");
    if ((e_usbh != nullptr) && (view == nullptr)) {
      const long v = strtol(e_usbh, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        usbh_stop_settle = (uint32_t)v;
      }
    }
  }
  /* BOARD_SIM_STOP_ON="<substr>": stop the headless run as soon as the console
   * UART's last line contains <substr> -- a generic "stop on a banner" guard for
   * apps that loop forever after a success line (e.g. usb_host_file_ops retries
   * its ladder every 5 s). Empty / unset disables it. */
  const char* stop_on = getenv("BOARD_SIM_STOP_ON");
  if ((stop_on != nullptr) && ((stop_on[0] == '\0') || (view != nullptr))) {
    stop_on = nullptr;
  }
  /* BOARD_SIM_STOP_PC=0x...: end the run the first time PC reaches this address
   * (effective in profile insn mode, via prof_insn_hook). Lets the profiler cover
   * exactly the boot path -- the idle main-loop PC the firmware parks at -- when
   * the build-stable compute-idle auto-stop below is not specific enough. */
  {
    const char* e_spc = getenv("BOARD_SIM_STOP_PC");
    if (e_spc != nullptr) {
      const unsigned long v = strtoul(e_spc, nullptr, (int)k_env_strtol_base);
      s_prof_stop_pc        = (uint32_t)(v & ~1UL); /* clear the Thumb bit if supplied. */
    }
  }
  /* BOARD_SIM_PROFILE compute-idle stop (insn mode, build-stable): once boot has
   * fallen into the steady frame loop the firmware retires almost no instructions
   * per chunk (each frame polls a little, then WFI-halts), whereas boot chunks are
   * compute-heavy. Stop after k_prof_idle_need consecutive chunks that each retire
   * fewer than k_prof_idle_insns instructions, so the profile spans boot + the
   * first rendered frame and not the idle tail. Armed only after k_prof_idle_arm
   * chunks so an early cheap chunk cannot trip it. */
  enum : uint32_t {
    k_prof_idle_insns = 4000U, /**< Per-chunk insns below which a chunk is idle. */
    k_prof_idle_need  = 600U,  /**< Consecutive idle chunks that end the run.    */
    k_prof_idle_arm   = 16U,   /**< Chunks to run before the stop is armed.      */
  };
  /* All three are overridable so the boot window can be tuned per app (a long
   * boot-time settle delay is a run of cheap chunks that must not be mistaken
   * for the steady idle loop). NEED defaults high enough to clear the settle
   * delays in these apps; raise it (or set BOARD_SIM_STOP_PC) for a longer boot. */
  uint32_t prof_idle_insns = (uint32_t)k_prof_idle_insns;
  uint32_t prof_idle_need  = (uint32_t)k_prof_idle_need;
  uint32_t prof_idle_arm   = (uint32_t)k_prof_idle_arm;
  {
    const char* e_pi = getenv("BOARD_SIM_PROFILE_IDLE_INSNS");
    const char* e_pn = getenv("BOARD_SIM_PROFILE_IDLE_NEED");
    const char* e_pa = getenv("BOARD_SIM_PROFILE_IDLE_ARM");
    if (e_pi != nullptr) {
      prof_idle_insns = (uint32_t)strtoul(e_pi, nullptr, (int)k_strtol_base10);
    }
    if (e_pn != nullptr) {
      prof_idle_need = (uint32_t)strtoul(e_pn, nullptr, (int)k_strtol_base10);
    }
    if (e_pa != nullptr) {
      prof_idle_arm = (uint32_t)strtoul(e_pa, nullptr, (int)k_strtol_base10);
    }
  }
  uint64_t            prof_idle_prev_i = 0U;
  uint32_t            prof_idle_run    = 0U;
  bool                prof_stopped     = false;
  uint64_t            idle_sig_prev    = 0U;
  uint32_t            idle_run         = 0U;
  bool                idle_stopped     = false;
  uint32_t            usb_stop_run     = 0U;
  uint32_t            usbh_stop_run    = 0U;
  bool                usb_stopped      = false;
  bool                stop_sym_hit     = false; /* --stop-sym threshold reached.           */
  uint32_t            rec_frames       = 0U;    /* frames written when --record is active. */
  const clock_t       t0               = clock();
  uc_err              err              = UC_ERR_OK;
  uint32_t            run_pc           = pc;
  uint32_t            chunks           = 0U;
  bool                timed_out        = false;
  bool                closed           = false;
  uint32_t            settle_left      = 0U;    /* >0 once the click landed: chunks to drain. */
  uint32_t            last_boot_chunk  = 0U;    /* chunk of the last (re)boot for --reboot.   */
  bool                slider_grab      = false; /* true while a press grabbed the battery slider. */
  board_overlay_btn_t held_btn = k_board_overlay_btn_none; /* SW held down (released on up). */
  uint64_t            last_present_us = 0U; /* wall-us of the last live --view present. */
  /* Classify a headless --click once. A click on the console tab bar switches the
   * active console channel (a one-shot view change, same as the window path); an
   * on-screen sidebar button toggles a user switch (fired once); anything else is
   * a panel touch (re-armed until drained). */
  bool     click_was_tab = false;
  uint32_t click_tab_idx = 0U;
  if (want_click) {
    if (board_overlay_hit_console_tab((uint16_t)click_x,
                                      (uint16_t)click_y,
                                      disp_w,
                                      (uint32_t)k_board_console_ch_count,
                                      &click_tab_idx)) {
      s_view_console_ch = (board_console_ch_t)click_tab_idx;
      s_view_scroll     = 0U;
      s_view_autoscroll = true;
      s_view_log_seen   = board_console_total(s_view_console_ch);
      click_was_tab     = true;
    }
  }
  const board_overlay_btn_t click_btn =
    (want_click && !click_was_tab)
      ? board_overlay_hit_button((uint16_t)click_x, (uint16_t)click_y, disp_w)
      : k_board_overlay_btn_none;
  bool     button_fired = false;
  uint32_t prof_prev_pc = 0U;
  double   prof_prev_t  = 0.0;
  for (; chunks < max_chunks; chunks++) {
    /* BOARD_SIM_PROFILE: charge the wall time of the previous chunk to the
     * function its execution started in (so a cheap WFI-halt chunk and an
     * expensive compute chunk are weighted by real time, not by chunk count). */
    if (s_prof_mode == k_prof_wall) {
      const double now = board_now_s();
      if (prof_prev_t > 0.0) {
        prof_add(prof_prev_pc, now - prof_prev_t);
      }
      prof_prev_pc = run_pc;
      prof_prev_t  = now;
    }
    /* Publish run telemetry for the board view (PC + chunk counter). */
    s_view_pc     = run_pc;
    s_view_chunks = chunks;

    /* --reboot N: after each boot runs a settle window (enough to exercise its
     * first-boot path -- e.g. plant a backup-RAM sentinel), force a power-on warm
     * reboot. The reset-retained models (VBATT backup) survive, so an app proves
     * reset-survival across the reboot. This is a clean power-on (PORF stays). */
    if ((reboot_count > 0) && ((chunks - last_boot_chunk) >= (uint32_t)k_reboot_settle)) {
      reboot_count--;
      board_periph_reset_set_cause(true, false, false, false); /* power-on reboot */
      run_pc = warm_reboot(uc, elf, elf_len, want_trace);
      if (run_pc == 0U) {
        break; /* reboot failed to reload the image -- end the run */
      }
      last_boot_chunk = chunks;
      continue;
    }
    /* A peripheral (the watchdog) may have requested a reset on its tick: latch
     * the watchdog cause and warm-reboot so the next boot reads reset_by=wdt. */
    bool wdt_rst  = false;
    bool iwdt_rst = false;
    if (board_periph_reset_take_request(&wdt_rst, &iwdt_rst)) {
      board_periph_reset_set_cause(false, false, wdt_rst, iwdt_rst);
      run_pc = warm_reboot(uc, elf, elf_len, want_trace);
      if (run_pc == 0U) {
        break; /* reboot failed to reload the image -- end the run */
      }
      last_boot_chunk = chunks;
      continue;
    }

    /* Each outer chunk is one SysTick period: arm the periodic tick so the
     * boundary (or a tail-chain) takes it once interrupts permit. */
    s_systick_pending = true;

    /* Advance the free-running DWT cycle counter one chunk's worth of cycles so
     * a masked-context ra8_delay_ms (which spins on CYCCNT while PRIMASK is set)
     * makes progress instead of stalling the whole run. Inert unless the
     * firmware enabled the counter, so this cannot change any other app. */
    dwt_cyccnt_advance(uc);

    /* Advance the modelled timers one tick-period and let the ICU pend any IRQ
     * a counter wrap raised; the exception layer below takes it like SysTick. */
    board_periph_tick(uc);
    board_net_tick();

    /* Drain any buffered keystrokes into the console UART RX (the same SCI
     * channel --input targets). Sources are the live --view window (keyDown)
     * and the headless --keys injector, both via board_input -- so the keyboard
     * path is identical whether typed in the window or scripted on the CLI. */
    char key_byte = 0;
    while (board_input_pop_key(&key_byte)) {
      const uint8_t kb = (uint8_t)key_byte;
      board_periph_sci_feed_rx(board_periph_sci_console_channel(), &kb, 1U);
    }

    /* Headless --click: keep one contact armed in the GT911 model until the
     * firmware's real ra8_touch_read drains it (board_periph_touch_reported
     * increments). Re-arming each chunk is needed because ra8_touch_open clears
     * the GT911 status byte during bring-up; once the render loop reads the
     * point it is consumed once and never re-armed. */
    if (want_click && (click_btn != k_board_overlay_btn_none)) {
      if (!button_fired) {
        if ((click_btn == k_board_overlay_btn_battery) ||
            (click_btn == k_board_overlay_btn_batt_chg) ||
            (click_btn == k_board_overlay_btn_lowpower)) {
          apply_battery_click(click_btn, (uint16_t)click_x, disp_w); /* SOC / CHG / low-power. */
        } else {
          set_switch(click_btn, true); /* headless --click SW1/SW2: press + hold. */
        }
        button_fired = true;
      }
    } else if (want_click && !click_was_tab && (board_periph_touch_reported() == 0U)) {
      uint16_t cnx = (uint16_t)click_x;
      uint16_t cny = (uint16_t)click_y;
      unrotate_click((uint16_t)click_x,
                     (uint16_t)click_y,
                     panel_w,
                     panel_h,
                     rotate_deg,
                     &cnx,
                     &cny);
      board_periph_touch_inject(cnx, cny);
    }

    /* Inner loop: run a chunk, then service exceptions to a steady state before
     * the next chunk. An EXC_RETURN branch is unstacked and the NVIC re-checked
     * so a still-pending lower-priority exception tail-chains (e.g. PendSV right
     * after SysTick) exactly as hardware would, instead of briefly resuming the
     * interrupted code. SVCall (taken in on_intr) just leaves PC at the handler,
     * which the next relaunch runs. */
    bool faulted = false;
    for (uint32_t inner = 0U; inner < (uint32_t)k_run_inner_max; inner++) {
      s_pendsv_stop = false; /* set by on_icsr_write iff this run ends on PENDSVSET */
      /* Idle fast-forward: when the core is parked on a wait-for-interrupt spin
       * (ThreadX's __tx_wait_here `b .`, or a `wfi` halt), running a full
       * k_run_chunk_insns just burns wall time to reach the SAME SysTick that is
       * already armed once per outer chunk. Cap the budget to k_idle_spin_insns
       * so the spin returns at once and the boundary below takes the tick now.
       * This skips only idle wall-time: the tick stays armed once per outer
       * chunk and fired once, so ThreadX's tick COUNT -- and every sleep/heartbeat
       * deadline measured in ticks -- is identical to a full idle chunk. Busy
       * firmware never parks on these opcodes, so it runs the full budget. */
      /* Low-power (Model A): the M33 runs ~4x slower than the M85, so a busy
       * chunk advances 1/4 as many instructions per modelled tick. Idle spins
       * still collapse to k_idle_spin_insns regardless. */
      size_t busy_budget = (size_t)k_run_chunk_insns;
      if (s_low_power) {
        busy_budget = (size_t)k_run_chunk_insns / (size_t)k_low_power_div;
      }
      const size_t run_budget = idle_spin_at(uc, run_pc) ? (size_t)k_idle_spin_insns : busy_budget;
      err                     = uc_emu_start(uc, (uint64_t)run_pc | 1U, 0, 0, run_budget);
      (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
      if (s_seam_relaunch) {
        /* A --fast-sd byte-exchange returned to its caller. This consumed no
         * modelled time, so relaunch from the returned PC without advancing the
         * SysTick or charging a chunk (the inner-loop cap still bounds the run). */
        s_seam_relaunch = false;
        continue;
      }
      if (s_prof_stop_hit) {
        break; /* BOARD_SIM_STOP_PC reached (set in prof_insn_hook) -- end the run. */
      }
      if (s_reboot_request) {
        break; /* AIRCR.SYSRESETREQ -- the outer wrapper performs the reboot. */
      }
      if (s_bkpt_hit) {
        faulted = true; /* firmware trapped on a BKPT -- end the run, report it. */
        break;
      }
      if (s_exc_return_hit) {
        s_exc_return_hit = false;
        exc_return(uc, (uint32_t)s_exc_return_pc);
        /* Tail-chain the next pend, but do NOT advance the SysTick: an exception
         * return consumes no modelled time, so time advances only on a full
         * instruction budget (below). This keeps a deferred tick from firing the
         * instant a PendSV context switch unstacks into the new thread. */
        (void)exc_take_pending(uc, vtor_base, false);
        (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
        continue;
      }
      if (err != UC_ERR_OK) {
        faulted = true;
        break;
      }
      if (s_mpu_fault) {
        /* A store hit a read-only MPU region (on_mpu_ro_write stopped us).
         * Synthesise MemManage at this boundary -- no time advances (a fault is
         * synchronous), and the stacked PC is the faulting store. */
        s_mpu_fault = false;
        mpu_synth_memmanage(uc, vtor_base);
        (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
        continue;
      }
      if (s_div0_fault) {
        /* A UDIV/SDIV by zero with CCR.DIV_0_TRP set (emulate_div0_patched stopped
         * us). Synthesise UsageFault at this boundary -- synchronous, no time
         * advances, and the stacked PC is the trapping divide. */
        s_div0_fault = false;
        div0_synth_usagefault(uc, vtor_base);
        (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
        continue;
      }
      if (s_pendsv_stop) {
        /* Context-switch stop: a thread wrote PENDSVSET to yield. Take PendSV
         * but do NOT advance the SysTick -- a context switch consumes no
         * modelled time, so a thread that just suspended on a tick wait keeps
         * waiting and the scheduler runs the highest-priority READY thread. This
         * is what lets a low-priority worker (e.g. the NetX echo thread) run to
         * completion before a higher-priority sleeper's tick expires. */
        (void)exc_take_pending(uc, vtor_base, false);
        (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
        continue;
      }
      /* Full instruction budget elapsed: a tick's worth of genuine execution (or
       * an idle spin) has passed, so advance time -- take the highest-priority
       * pending exception (SysTick this period, and/or PendSV) and resolve it in
       * this same chunk so a context switch does not cost a scheduling quantum. */
      if (exc_take_pending(uc, vtor_base, true)) {
        (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
        continue;
      }
      break;
    }
    if (faulted) {
      break;
    }
    if (s_prof_stop_hit) {
      prof_stopped = true; /* BOARD_SIM_STOP_PC reached -- end the profiled run. */
      break;
    }
    /* AIRCR.SYSRESETREQ requested a reset: latch a software-reset cause and warm
     * reboot the firmware from its reset vector, then keep running. */
    if (s_reboot_request) {
      board_periph_reset_set_cause(false, true, false, false); /* software reset */
      run_pc = warm_reboot(uc, elf, elf_len, want_trace);
      /* Cleared after the reboot (the engine is idle during warm_reboot, so no
       * AIRCR write can be lost here, but clearing last keeps it robust if
       * warm_reboot ever runs code). */
      s_reboot_request = false;
      if (run_pc == 0U) {
        break; /* reboot failed to reload the image -- end the run */
      }
      continue;
    }

    /* Dual-core: boot cpu1 on the CPU1ACTCSR release (SP/PC from its vector
     * table at CPU1INITVTOR), then step it interleaved with cpu0. cpu1 shares
     * the on-chip SRAM, so its poll of the ping struct sees cpu0's writes and
     * its pong reply is visible back to cpu0 -- a real second core, not a model.
     * cpu1 is a tight poll loop (no interrupts), so a plain stepped run suffices;
     * a cpu1 fault just stops cpu1 (cpu0 keeps running). */
    if (s_cpu1_uc != nullptr) {
      if (s_cpu1_release_req && !s_cpu1_active) {
        s_cpu1_release_req     = false;
        const uint32_t cpu1_sp = rd32(s_cpu1_uc, (uint64_t)s_cpu1_initvtor);
        s_cpu1_pc              = rd32(s_cpu1_uc, (uint64_t)s_cpu1_initvtor + 4U);
        (void)uc_reg_write(s_cpu1_uc, UC_ARM_REG_SP, &cpu1_sp);
        s_cpu1_active = true;
      }
      if (s_cpu1_active) {
        const uc_err e1 =
          uc_emu_start(s_cpu1_uc, (uint64_t)s_cpu1_pc | 1U, 0, 0, (size_t)k_cpu1_chunk_insns);
        (void)uc_reg_read(s_cpu1_uc, UC_ARM_REG_PC, &s_cpu1_pc);
        if (e1 != UC_ERR_OK) {
          s_cpu1_active = false; /* cpu1 hit a fault -- halt it, cpu0 continues */
        }
      }
    }
    /* --record: dump the composite (panel + status) every k_record_every chunks
     * as a numbered PPM, so the run becomes a frame sequence (assemble to a video
     * with ffmpeg, e.g. `ffmpeg -framerate 20 -i frame_%06d.ppm out.mp4`). */
    if ((record_dir != nullptr) && (composite != nullptr) &&
        ((chunks % (uint32_t)k_record_every) == 0U)) {
      build_composite(uc,
                      panel_fb,
                      rot_fb,
                      composite,
                      panel_w,
                      panel_h,
                      disp_w,
                      disp_h,
                      rotate_deg,
                      win_title);
      char fpath[1024];
      (void)snprintf(fpath, sizeof(fpath), "%s/frame_%06u.ppm", record_dir, (unsigned)rec_frames);
      if (write_ppm(fpath, composite, comp_w, comp_h) == 0) {
        rec_frames++;
      }
    }
    if (view != nullptr) {
      /* Live window: mouse-down on an on-screen SW1/SW2 PRESSES that user switch
       * (route_click drives it active-low); the matching mouse-up releases it, so
       * the on-screen buttons act as momentary push-buttons, not latching switches.
       * A press anywhere on the panel arms one GT911 contact instead. */
      uint16_t cx = 0U;
      uint16_t cy = 0U;
      if (board_view_poll_click(view, &cx, &cy)) {
        /* A press on the battery slider "grabs" it, so a subsequent drag keeps
         * setting the SOC from the cursor column even if the mouse leaves the
         * track row -- standard slider grab semantics. */
        const board_overlay_btn_t hit = route_click(cx, cy, panel_w, panel_h, disp_w, rotate_deg);
        slider_grab                   = (hit == k_board_overlay_btn_battery);
        held_btn = ((hit == k_board_overlay_btn_sw1) || (hit == k_board_overlay_btn_sw2))
                     ? hit
                     : k_board_overlay_btn_none;
      }
      /* Mouse-up: release a held push-button and drop any slider grab. */
      if (board_view_poll_release(view)) {
        if (held_btn != k_board_overlay_btn_none) {
          set_switch(held_btn, false);
          held_btn = k_board_overlay_btn_none;
        }
        slider_grab = false;
      }
      uint16_t dx = 0U;
      uint16_t dy = 0U;
      if (board_view_poll_drag(view, &dx, &dy) && slider_grab) {
        apply_battery_click(k_board_overlay_btn_battery, dx, disp_w);
      }
      /* Mouse-wheel pages the console scrollback, Arduino-Serial-Monitor style:
       * scrolling up reveals older lines AND pauses autoscroll (so new output no
       * longer yanks the view to the bottom); scrolling back down to the tail
       * re-enables autoscroll. fill_status clamps the offset + holds the absolute
       * position while paused. */
      const int32_t notches = board_view_poll_scroll(view);
      if (notches > 0) {
        s_view_autoscroll = false; /* user scrolled up -> pause the live follow */
        s_view_scroll += (uint32_t)notches;
      } else if (notches < 0) {
        const uint32_t dec = (uint32_t)(-notches);
        if (s_view_scroll > dec) {
          s_view_scroll -= dec;
        } else {
          s_view_scroll     = 0U;
          s_view_autoscroll = true; /* scrolled back to the tail -> resume follow */
        }
      }
      if ((chunks % (uint32_t)k_view_present_every) == 0U) {
        /* Compositing + uploading the full panel+sidebar frame is the dominant
         * host cost. The firmware advances far faster than a display needs, so
         * cap the live present to ~60 Hz wall-clock and yield the CPU when a
         * present is skipped -- otherwise an idle app (cheap chunks) would spin a
         * host core redrawing identical frames thousands of times a second, which
         * reads as window lag for "nothing happening". */
        struct timespec ts_now = {};
        (void)clock_gettime(CLOCK_MONOTONIC, &ts_now);
        const uint64_t now_us = ((uint64_t)ts_now.tv_sec * (uint64_t)k_us_per_s) +
                                ((uint64_t)ts_now.tv_nsec / (uint64_t)k_ns_per_us);
        if ((now_us - last_present_us) >= (uint64_t)k_view_frame_us) {
          build_composite(uc,
                          panel_fb,
                          rot_fb,
                          composite,
                          panel_w,
                          panel_h,
                          disp_w,
                          disp_h,
                          rotate_deg,
                          win_title);
          board_view_present(view, composite, comp_w, comp_h);
          last_present_us = now_us;
        } else {
          (void)usleep((useconds_t)k_view_yield_us);
        }
      }
      if (board_view_pump(view)) {
        closed = true;
        break;
      }
    } else if (want_click) {
      /* Headless --click: run a bounded tail after the input lands (a drained
       * touch, or a fired on-screen button), then stop deterministically so the
       * dumped frame shows the switched tab / lit LED. */
      const bool click_acted = (click_btn != k_board_overlay_btn_none)
                                 ? button_fired
                                 : (board_periph_touch_reported() > 0U);
      if (click_acted) {
        settle_left = (settle_left == 0U) ? click_settle_chunks : (settle_left - 1U);
        if (settle_left == 1U) {
          break;
        }
      }
      if (wall_guard_on && (((double)(clock() - t0) / (double)CLOCKS_PER_SEC) >= wall_s)) {
        timed_out = true;
        break;
      }
    } else { /* plain headless run (no window, no scripted click) */
      /* Profiler compute-idle early-stop (insn mode): a chunk that retires very
       * few instructions is an idle frame (poll + WFI-halt), whereas boot chunks
       * are compute-heavy. After enough consecutive idle chunks the firmware has
       * reached its steady frame loop -- end the run so the profile is boot, not
       * the idle tail. Build-stable (no per-app address), unlike STOP_PC. */
      if ((s_prof_mode == k_prof_insn) && (record_dir == nullptr) && (chunks >= prof_idle_arm)) {
        const uint64_t d = s_prof_total_i - prof_idle_prev_i;
        prof_idle_prev_i = s_prof_total_i;
        if (d < (uint64_t)prof_idle_insns) {
          prof_idle_run++;
          if (prof_idle_run >= prof_idle_need) {
            prof_stopped = true;
            break;
          }
        } else {
          prof_idle_run = 0U;
        }
      }
      /* Steady-state idle early-stop (opt-in; not while recording, which must
       * span its full window). All tracked counters are monotonic, so an
       * unchanged sum means no MMIO / IRQ / context switch happened this chunk. */
      if ((idle_stop_chunks > 0U) && (record_dir == nullptr)) {
        const uint64_t idle_sig = (uint64_t)s_mmio_reads + (uint64_t)s_mmio_writes +
                                  (uint64_t)s_pendsv_takes + (uint64_t)s_svc_takes +
                                  (uint64_t)board_periph_irq_total();
        if (idle_sig == idle_sig_prev) {
          idle_run++;
          if (idle_run >= idle_stop_chunks) {
            idle_stopped = true;
            break;
          }
        } else {
          idle_sig_prev = idle_sig;
          idle_run      = 0U;
        }
      }
      /* USB device-mode early-stop: once the device reaches CONFIGURED, run a
       * short settle window (for the first class traffic + report line), stop. */
      if ((usb_stop_settle > 0U) && (record_dir == nullptr) && board_usb_configured()) {
        usb_stop_run++;
        if (usb_stop_run >= usb_stop_settle) {
          usb_stopped = true;
          break;
        }
      }
      /* USB host-mode early-stop: once the virtual device has served the host
       * its last request -- the keyboard streamed its reports, or the MSC host
       * reached its read-only WRITE(10) test (the step right before PASS) --
       * settle (for the PASS banner) and stop. Separate from USB_STOP because a
       * host app may ALSO run a device worker whose CONFIGURED would otherwise
       * stop the run before the host side finishes. */
      const bool usbh_done =
        (s_vkbd_reports_sent >= (uint32_t)k_vkbd_stop_reports) || s_vmsc_write_seen;
      if ((usbh_stop_settle > 0U) && (record_dir == nullptr) && usbh_done) {
        usbh_stop_run++;
        if (usbh_stop_run >= usbh_stop_settle) {
          usb_stopped = true;
          break;
        }
      }
      /* Generic banner stop: the firmware printed its success line to the
       * console. Check BOTH endpoints -- the UART (the SCI-TX `[uart]` banners)
       * and the ITM/SWO stimulus stream (the `[itm]` ra8_log lines surfaced by
       * on_itm_stim_write). ra8_log-only apps -- e.g. the dual-core demos, whose
       * PASS verdict is an `ra8_log_info` line that never touches the UART --
       * would otherwise run to the full chunk budget; matching the ITM line lets
       * BOARD_SIM_STOP_ON end the run the instant the verdict is logged, exactly
       * as it already does for the UART-banner apps. */
      if (stop_on != nullptr) {
        const char* last_uart = board_periph_uart_last_line();
        if ((last_uart != nullptr) && (strstr(last_uart, stop_on) != nullptr)) {
          usb_stopped = true;
          break;
        }
        const char* last_itm = board_console_line(k_board_console_ch_itm, 0U);
        if ((last_itm != nullptr) && (strstr(last_itm, stop_on) != nullptr)) {
          usb_stopped = true;
          break;
        }
      }
      /* --stop-sym: end the run the instant the watched 32-bit global reaches its
       * threshold (the jlink_memprobe counter floor). Checked at the chunk
       * boundary like STOP_ON, so the stop is deterministic and host-load
       * independent -- a passing probe stops in a handful of chunks. */
      if (stop_sym_addr != 0U) {
        uint32_t sv = 0U;
        if ((uc_mem_read(uc, (uint64_t)stop_sym_addr, &sv, sizeof(sv)) == UC_ERR_OK) &&
            (sv >= stop_sym_thresh)) {
          stop_sym_hit = true;
          break;
        }
      }
      if (wall_guard_on && (((double)(clock() - t0) / (double)CLOCKS_PER_SEC) >= wall_s)) {
        timed_out = true;
        break;
      }
    }
  }

  /* The run has ended: the board view's final / held frame shows "parked". */
  s_view_running = false;
  s_view_pc      = run_pc;

  (void)fprintf(stderr,
                "\nboard_sim: stopped -- %s%s%s%s%s%s\n",
                uc_strerror(err),
                timed_out ? " (wall-clock budget reached)" : "",
                idle_stopped ? " (idle steady-state)" : "",
                usb_stopped ? " (USB enumerated)" : "",
                stop_sym_hit ? " (--stop-sym threshold reached)" : "",
                prof_stopped ? " (profile: boot complete)" : "");
  (void)fprintf(stderr, "  final PC      : 0x%08X\n", run_pc);
  if (s_bkpt_hit) {
    (void)fprintf(stderr,
                  "  => firmware executed a BKPT @ 0x%08X (deliberate trap: "
                  "Default_Handler / failed assert / fault give-up)\n",
                  s_bkpt_pc);
  }
  (void)fprintf(stderr, "  chunks run    : %u   SysTick ticks: %u\n", chunks, s_systick_fires);
  if (s_mve_emulated > 0U) {
    (void)fprintf(stderr,
                  "  MVE (Helium)  : %llu instruction(s) emulated (M85 vector ops the M33 "
                  "core lacks)\n",
                  (unsigned long long)s_mve_emulated);
  }
  if (s_lob_emulated > 0U) {
    (void)fprintf(stderr,
                  "  LOB (loop)    : %llu DLS/LE instruction(s) emulated (M85 hardware-loop "
                  "ops)\n",
                  (unsigned long long)s_lob_emulated);
  }
  prof_report();
  (void)fprintf(stderr,
                "  exceptions    : %u PendSV  %u SVCall (real Cortex-M entry/return)\n",
                s_pendsv_takes,
                s_svc_takes);
  (void)fprintf(stderr,
                "  touch clicks  : %u drained via ra8_touch -> I3C -> GT911\n",
                board_periph_touch_reported());
  /* Emit any console bytes still buffered without a trailing newline. */
  console_flush_line(board_periph_sci_console_channel());
  /* Peripheral-model observability: LED transitions, timer totals, IRQ counts,
   * SCI byte totals. */
  board_periph_report(uc);
  board_net_report();
  (void)fprintf(stderr,
                "  MMIO reads    : %u   writes: %u   distinct addrs: %u\n",
                s_mmio_reads,
                s_mmio_writes,
                s_mmio_n);
  /* Device summary: surface the attached microSD card so a headless run shows
   * its size / format the same way the --view sidebar does. */
  {
    bool        sd_att = false;
    uint64_t    sd_b   = 0U;
    uint8_t     sd_f   = 0U;
    const char* sd_l   = nullptr;
    board_sd_info(&sd_att, &sd_b, &sd_f, &sd_l);
    const uint64_t gib_bytes = (uint64_t)k_size_kib * (uint64_t)k_size_kib * (uint64_t)k_size_kib;
    const uint64_t mib_bytes = (uint64_t)k_size_kib * (uint64_t)k_size_kib;
    const bool     sd_gb     = (sd_b >= gib_bytes);
    const unsigned long sd_sz =
      sd_gb ? (unsigned long)(sd_b / gib_bytes) : (unsigned long)(sd_b / mib_bytes);
    const char* sd_u = sd_gb ? "GB" : "MB";
    if (sd_att && (sd_f != 0U)) {
      (void)fprintf(stderr,
                    "  SD card       : %lu %s FAT%u '%s' (created by --sd-new)\n",
                    sd_sz,
                    sd_u,
                    (unsigned)sd_f,
                    (sd_l != nullptr) ? sd_l : "");
    } else if (sd_att) {
      (void)fprintf(stderr, "  SD card       : %lu %s image attached\n", sd_sz, sd_u);
    }
  }
  /* GLCDC colour-cycle witness: BG_BGC write count + the distinct colours. */
  (void)fprintf(stderr,
                "  BG_BGC writes : %u   distinct colours: %u   [",
                s_bgc_writes,
                s_bgc_distinct_n);
  for (uint32_t i = 0U; i < s_bgc_distinct_n; i++) {
    (void)fprintf(stderr, "%s0x%06X", (i == 0U) ? "" : " ", s_bgc_distinct[i]);
  }
  (void)fprintf(stderr, "]\n");
  (void)fprintf(stderr, "    %-12s %10s %10s %12s\n", "addr", "reads", "writes", "last-write");
  const bool     truncated = (s_mmio_n > (uint32_t)k_mmio_print_max);
  const uint32_t shown     = truncated ? (uint32_t)k_mmio_print_max : s_mmio_n;
  for (uint32_t i = 0U; i < shown; i++) {
    if (s_mmio_written[i]) {
      (void)fprintf(stderr,
                    "    0x%08llX %10u %10u   0x%08X\n",
                    (unsigned long long)s_mmio_addr[i],
                    s_mmio_rcount[i],
                    s_mmio_wcount[i],
                    s_mmio_val[i]);
    } else {
      (void)fprintf(stderr,
                    "    0x%08llX %10u %10u %12s\n",
                    (unsigned long long)s_mmio_addr[i],
                    s_mmio_rcount[i],
                    s_mmio_wcount[i],
                    "-");
    }
  }
  if (truncated) {
    (void)fprintf(stderr, "    ... (%u more)\n", s_mmio_n - shown);
  }
  if (timed_out && !idle_stopped && !usb_stopped && !stop_sym_hit && !prof_stopped && !s_bkpt_hit) {
    /* The wall-clock guard fired (clock() is CPU-time, so a heavily-loaded host
     * burns the budget faster). This is a TRUNCATED run, NOT a completed one --
     * say so plainly, and do NOT print the "EXECUTED to the run budget" line a
     * full-budget run prints. Conflating the two let a load-correlated truncation
     * masquerade as success, dropping a deterministic banner without failing the
     * gate (#168). A caller that wants the run bounded by a deterministic event
     * (not by CPU-time) should pass BOARD_SIM_STOP_ON / BOARD_SIM_MAX_CHUNKS. */
    (void)fprintf(stderr,
                  "  => board_sim TRUNCATED by the wall-clock guard at chunk %u of %u "
                  "(%.0fs CPU-time elapsed); this is NOT a full-budget run (host "
                  "overloaded -- see #168).\n",
                  chunks,
                  max_chunks,
                  wall_s);
  } else if (((err == UC_ERR_OK) || idle_stopped || usb_stopped || stop_sym_hit || prof_stopped) &&
             !s_bkpt_hit) {
    if (idle_stopped) {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (idle steady-state: no "
                    "observable change for %u chunks, stopped at chunk %u).\n",
                    idle_stop_chunks,
                    chunks);
    } else if (usb_stopped) {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (USB enumerated: device "
                    "CONFIGURED, stopped at chunk %u).\n",
                    chunks);
    } else {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (no invalid opcode / fault).\n");
    }
  }

  /* --dump-sym: read each resolved global from Unicorn memory and print its
   * 32-bit value (and the address), so a test can probe firmware state (e.g. an
   * init-step or mismatch counter) after the run without a debugger. */
  for (uint32_t d = 0U; d < dump_sym_n; d++) {
    if (dump_sym_addrs[d] == 0U) {
      (void)fprintf(stderr, "  dump-sym      : %s = <unresolved>\n", dump_sym_names[d]);
      continue;
    }
    uint32_t v = 0U;
    if (uc_mem_read(uc, (uint64_t)dump_sym_addrs[d], &v, sizeof(v)) == UC_ERR_OK) {
      (void)fprintf(stderr,
                    "  dump-sym      : %s @0x%08X = %u (0x%08X)\n",
                    dump_sym_names[d],
                    dump_sym_addrs[d],
                    v,
                    v);
    } else {
      (void)fprintf(stderr,
                    "  dump-sym      : %s @0x%08X = <unreadable>\n",
                    dump_sym_names[d],
                    dump_sym_addrs[d]);
    }
  }

  if ((ppm_path != nullptr) && (composite != nullptr)) {
    build_composite(uc,
                    panel_fb,
                    rot_fb,
                    composite,
                    panel_w,
                    panel_h,
                    disp_w,
                    disp_h,
                    rotate_deg,
                    win_title);
    if (write_ppm(ppm_path, composite, comp_w, comp_h) == 0) {
      (void)fprintf(stderr, "  wrote %s (%ux%u)\n", ppm_path, (unsigned)comp_w, (unsigned)comp_h);
    } else {
      (void)fprintf(stderr, "  could not write %s\n", ppm_path);
    }
  }
  if (record_dir != nullptr) {
    (void)fprintf(stderr,
                  "  recorded %u frame(s) to %s (%ux%u, ~%u fps)\n",
                  rec_frames,
                  record_dir,
                  (unsigned)comp_w,
                  (unsigned)comp_h,
                  (unsigned)k_record_fps);
  }
  if (view != nullptr) {
    if (!closed) { /* run ended on its own -- keep the last frame up until closed */
      build_composite(uc,
                      panel_fb,
                      rot_fb,
                      composite,
                      panel_w,
                      panel_h,
                      disp_w,
                      disp_h,
                      rotate_deg,
                      win_title);
      board_view_present(view, composite, comp_w, comp_h);
      (void)fprintf(stderr, "board_sim: run ended; close the window to exit\n");
      while (!board_view_pump(view)) {
        (void)usleep((useconds_t)k_view_idle_us);
      }
    }
    board_view_close(view);
  }
  /* --save-sd: dump the (possibly firmware-modified) SD card image for reuse. */
  if (save_sd_path != nullptr) {
    (void)board_sd_save(save_sd_path);
  }
  free(panel_fb);
  free(rot_fb);
  free(composite);
  free(elf); /* kept alive for the whole run so a warm reboot can re-load it */
  (void)uc_close(uc);

  /* Exit status for the #67 run-every-example matrix: a clean run-to-budget is
   * 0; a firmware BKPT, an emulation fault, or the wall-clock timeout each map
   * to a distinct non-zero code so the matrix can flag a wedged or trapped run
   * by exit code alone. */
  board_sim_exit_t exit_code = k_board_sim_exit_ok;
  if (s_bkpt_hit) {
    exit_code = k_board_sim_exit_bkpt;
  } else if (err != UC_ERR_OK) {
    exit_code = k_board_sim_exit_fault;
  } else if (timed_out) {
    exit_code = k_board_sim_exit_timeout;
  }
  return (int)exit_code;
}
