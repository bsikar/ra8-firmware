/**
 * @file examples/ek_ra8d2/hw_pending/secure_boot_ns_hil/vector_table.c
 * @brief Cortex-M85 vector table and default interrupt handlers for RA8D2
 *
 * @details
 * Emits the interrupt vector table that the Cortex-M85 reads at reset
 * from the address in its VTOR register. Slot 0 is the initial main
 * stack pointer, slot 1 is the `Reset_Handler`, and slots 2..15 are
 * the standard ARM core exceptions. Slots 16+ are peripheral IRQs
 * routed through the ICU's IELSR table.
 *
 * Every handler is declared weak and aliased to `Default_Handler`, so
 * application code can override any of them by declaring a non-weak
 * function with the same name.
 *
 * ## Linker-provided symbols
 *
 * - `_stack_top`: end of the main stack, set in the linker script.
 * - `_etext`, `_sdata`, `_edata`, `_sbss`, `_ebss`: section boundaries
 *   used by `Reset_Handler` to copy initialized data from flash/MRAM
 *   to SRAM and zero the BSS.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_boot_entry.h"

/* =============================================================================
 * Linker-provided symbols
 * =============================================================================
 *
 * Project convention: linker symbols carry the `g_ra8_ls_` prefix so they
 * are not in the reserved leading-underscore namespace that ISO C (and
 * cert-dcl37-c / bugprone-reserved-identifier) reject. The linker script
 * in secure_boot_ns_hil/linker_script.ld defines them with this exact spelling.
 */

extern uint32_t g_ra8_ls_stack_top; /**< Top of main stack (linker symbol). */
extern uint32_t g_ra8_ls_sdata;     /**< Start of .data in SRAM.            */
extern uint32_t g_ra8_ls_edata;     /**< End of .data in SRAM.              */
extern uint32_t g_ra8_ls_sidata;    /**< Source of .data in MRAM.           */
extern uint32_t g_ra8_ls_sbss;      /**< Start of .bss in SRAM.             */
extern uint32_t g_ra8_ls_ebss;      /**< End of .bss in SRAM.               */

/* =============================================================================
 * main()
 * =============================================================================
 */

extern int32_t main(void);

/* =============================================================================
 * Handler declarations
 * =============================================================================
 */

typedef void (*exc_handler_t)(void);

void Reset_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);

/* Core exceptions (Cortex-M85). HardFault / MemManage / BusFault /
 * UsageFault each get a dedicated naked trampoline below which
 * forwards to ra8_exception_report(). The rest are weak-aliased to
 * Default_Handler and may be overridden by application code.
 *
 * Mach-O (macOS host) does not support `__attribute__((alias(...)))`.
 * On the host syntax-check / unit-test build we drop the alias and
 * keep only `weak`; the host build never branches through these
 * symbols (the firmware vector table is not exercised on the test
 * host), so we just need them to exist for the `&` references in
 * `g_ra8_vector_table_start` to be well-formed. */
#ifndef __APPLE__
[[gnu::weak, gnu::alias("Default_Handler")]] void NMI_Handler(void);
[[gnu::weak, gnu::alias("Default_Handler")]] void SecureFault_Handler(void);
[[gnu::weak, gnu::alias("Default_Handler")]] void SVC_Handler(void);
[[gnu::weak, gnu::alias("Default_Handler")]] void DebugMon_Handler(void);
[[gnu::weak, gnu::alias("Default_Handler")]] void PendSV_Handler(void);
#else
[[gnu::weak]] void NMI_Handler(void);
[[gnu::weak]] void SecureFault_Handler(void);
[[gnu::weak]] void SVC_Handler(void);
[[gnu::weak]] void DebugMon_Handler(void);
[[gnu::weak]] void PendSV_Handler(void);
#endif

/* SysTick provided by libs/ra8_core/src/ra8_time.c (weak) so RTOS apps may override. */
extern void SysTick_Handler(void);

/* Each peripheral IRQ trampoline forwards to the ra8_isr substrate dispatcher so
 * drivers that called ra8_isr_register run in NVIC handler-mode. */
extern void ra8_isr_dispatch(uint16_t slot);

/* =============================================================================
 * Peripheral IRQ handlers
 * =============================================================================
 *
 * The RA Interrupt Control Unit exposes 112 routable interrupt lines
 * to the NVIC (per HUM table 13.x).
 *
 * Each peripheral IRQ vector forwards to ra8_isr_dispatch(slot) so any
 * (handler, ctx) registered through the substrate (ra8_isr_register)
 * runs in NVIC handler-mode. Slots that were never registered land in
 * the dispatcher's bounds-checked no-op path. The trampolines are
 * weak so a driver can still install a fully-custom IRQ handler by
 * defining a non-weak ``IRQ<n>_Handler``. HUM Ch 13 NVIC + Ch 14 ICU
 * IELSR.
 *
 * NB: a prior revision aliased every IRQn_Handler directly to
 * Default_Handler (which contains ``bkpt #0``). On debug-disabled
 * silicon a stray bkpt escalates to HardFault, so any first IRQ after
 * ra8_isr_register armed the NVIC line would halt the chip. The
 * dispatching form here avoids that. This secure-boot proof registers
 * no IRQs (the Secure side only runs the RoT verify + BLXNS), so the
 * dispatcher's bounds-checked no-op path covers every slot.
 */

enum : uint16_t {
  k_ra8_irq_count = 112U, /**< RA8 IRQ count. */
};

/* On Apple host syntax-check we drop ra8_isr_dispatch (host build
 * never branches through these) and keep only a weak empty body. */
#ifndef __APPLE__
/** @brief RA8 IRQ STUB. */
#define RA8_IRQ_STUB(n)                                                                            \
  void               IRQ##n##_Handler(void);                                                       \
  [[gnu::weak]] void IRQ##n##_Handler(void)                                                        \
  {                                                                                                \
    ra8_isr_dispatch((uint16_t)(n));                                                               \
  }                                                                                                \
  static_assert(1, "ra8_irq_stub_" #n)
#else
/** @brief RA8 IRQ STUB. */
#define RA8_IRQ_STUB(n)                                                                            \
  void               IRQ##n##_Handler(void);                                                       \
  [[gnu::weak]] void IRQ##n##_Handler(void) {}                                                     \
  static_assert(1, "ra8_irq_stub_" #n)
#endif

RA8_IRQ_STUB(0);
RA8_IRQ_STUB(1);
RA8_IRQ_STUB(2);
RA8_IRQ_STUB(3);
RA8_IRQ_STUB(4);
RA8_IRQ_STUB(5);
RA8_IRQ_STUB(6);
RA8_IRQ_STUB(7);
RA8_IRQ_STUB(8);
RA8_IRQ_STUB(9);
RA8_IRQ_STUB(10);
RA8_IRQ_STUB(11);
RA8_IRQ_STUB(12);
RA8_IRQ_STUB(13);
RA8_IRQ_STUB(14);
RA8_IRQ_STUB(15);
RA8_IRQ_STUB(16);
RA8_IRQ_STUB(17);
RA8_IRQ_STUB(18);
RA8_IRQ_STUB(19);
RA8_IRQ_STUB(20);
RA8_IRQ_STUB(21);
RA8_IRQ_STUB(22);
RA8_IRQ_STUB(23);
RA8_IRQ_STUB(24);
RA8_IRQ_STUB(25);
RA8_IRQ_STUB(26);
RA8_IRQ_STUB(27);
RA8_IRQ_STUB(28);
RA8_IRQ_STUB(29);
RA8_IRQ_STUB(30);
RA8_IRQ_STUB(31);
RA8_IRQ_STUB(32);
RA8_IRQ_STUB(33);
RA8_IRQ_STUB(34);
RA8_IRQ_STUB(35);
RA8_IRQ_STUB(36);
RA8_IRQ_STUB(37);
RA8_IRQ_STUB(38);
RA8_IRQ_STUB(39);
RA8_IRQ_STUB(40);
RA8_IRQ_STUB(41);
RA8_IRQ_STUB(42);
RA8_IRQ_STUB(43);
RA8_IRQ_STUB(44);
RA8_IRQ_STUB(45);
RA8_IRQ_STUB(46);
RA8_IRQ_STUB(47);
RA8_IRQ_STUB(48);
RA8_IRQ_STUB(49);
RA8_IRQ_STUB(50);
RA8_IRQ_STUB(51);
RA8_IRQ_STUB(52);
RA8_IRQ_STUB(53);
RA8_IRQ_STUB(54);
RA8_IRQ_STUB(55);
RA8_IRQ_STUB(56);
RA8_IRQ_STUB(57);
RA8_IRQ_STUB(58);
RA8_IRQ_STUB(59);
RA8_IRQ_STUB(60);
RA8_IRQ_STUB(61);
RA8_IRQ_STUB(62);
RA8_IRQ_STUB(63);
RA8_IRQ_STUB(64);
RA8_IRQ_STUB(65);
RA8_IRQ_STUB(66);
RA8_IRQ_STUB(67);
RA8_IRQ_STUB(68);
RA8_IRQ_STUB(69);
RA8_IRQ_STUB(70);
RA8_IRQ_STUB(71);
RA8_IRQ_STUB(72);
RA8_IRQ_STUB(73);
RA8_IRQ_STUB(74);
RA8_IRQ_STUB(75);
RA8_IRQ_STUB(76);
RA8_IRQ_STUB(77);
RA8_IRQ_STUB(78);
RA8_IRQ_STUB(79);
RA8_IRQ_STUB(80);
RA8_IRQ_STUB(81);
RA8_IRQ_STUB(82);
RA8_IRQ_STUB(83);
RA8_IRQ_STUB(84);
RA8_IRQ_STUB(85);
RA8_IRQ_STUB(86);
RA8_IRQ_STUB(87);
RA8_IRQ_STUB(88);
RA8_IRQ_STUB(89);
RA8_IRQ_STUB(90);
RA8_IRQ_STUB(91);
RA8_IRQ_STUB(92);
RA8_IRQ_STUB(93);
RA8_IRQ_STUB(94);
RA8_IRQ_STUB(95);
RA8_IRQ_STUB(96);
RA8_IRQ_STUB(97);
RA8_IRQ_STUB(98);
RA8_IRQ_STUB(99);
RA8_IRQ_STUB(100);
RA8_IRQ_STUB(101);
RA8_IRQ_STUB(102);
RA8_IRQ_STUB(103);
RA8_IRQ_STUB(104);
RA8_IRQ_STUB(105);
RA8_IRQ_STUB(106);
RA8_IRQ_STUB(107);
RA8_IRQ_STUB(108);
RA8_IRQ_STUB(109);
RA8_IRQ_STUB(110);
RA8_IRQ_STUB(111);

#undef RA8_IRQ_STUB

/* =============================================================================
 * Vector table
 * =============================================================================
 *
 * Placed in its own section so the linker script can pin it to the
 * start of MRAM. The Cortex-M85 reads the initial MSP from offset 0
 * and the reset vector from offset 4.
 */

/* Mach-O `section()` requires a `"segment,section"` form, but on the
 * host syntax-check / unit-test build we only need this array to
 * exist as a normal global -- the host never reads it from a fixed
 * vector address. So drop the section pinning on Apple hosts. */
#ifndef __APPLE__
[[gnu::section(".vectors"), gnu::used]]
#else
[[gnu::used]]
#endif
const exc_handler_t g_ra8_vector_table_start[16U + k_ra8_irq_count] = {
  /* Core exceptions (slots 0..15). */
  (exc_handler_t)&g_ra8_ls_stack_top, /* 0  Initial main stack pointer. */
  Reset_Handler,                      /* 1  Reset vector.               */
  NMI_Handler,                        /* 2  NMI.                        */
  HardFault_Handler,                  /* 3  HardFault.                  */
  MemManage_Handler,                  /* 4  MemManage.                  */
  BusFault_Handler,                   /* 5  BusFault.                   */
  UsageFault_Handler,                 /* 6  UsageFault.                 */
  SecureFault_Handler,                /* 7  SecureFault.                */
  0,                                  /* 8  Reserved.                   */
  0,                                  /* 9  Reserved.                   */
  0,                                  /* 10 Reserved.                   */
  SVC_Handler,                        /* 11 SVC.                        */
  DebugMon_Handler,                   /* 12 DebugMon.                   */
  0,                                  /* 13 Reserved.                   */
  PendSV_Handler,                     /* 14 PendSV.                     */
  SysTick_Handler,                    /* 15 SysTick.                    */

/* Peripheral IRQs 0..111. */
/** @brief `E` helper macro. */
#define E(n) IRQ##n##_Handler
  E(0),
  E(1),
  E(2),
  E(3),
  E(4),
  E(5),
  E(6),
  E(7),
  E(8),
  E(9),
  E(10),
  E(11),
  E(12),
  E(13),
  E(14),
  E(15),
  E(16),
  E(17),
  E(18),
  E(19),
  E(20),
  E(21),
  E(22),
  E(23),
  E(24),
  E(25),
  E(26),
  E(27),
  E(28),
  E(29),
  E(30),
  E(31),
  E(32),
  E(33),
  E(34),
  E(35),
  E(36),
  E(37),
  E(38),
  E(39),
  E(40),
  E(41),
  E(42),
  E(43),
  E(44),
  E(45),
  E(46),
  E(47),
  E(48),
  E(49),
  E(50),
  E(51),
  E(52),
  E(53),
  E(54),
  E(55),
  E(56),
  E(57),
  E(58),
  E(59),
  E(60),
  E(61),
  E(62),
  E(63),
  E(64),
  E(65),
  E(66),
  E(67),
  E(68),
  E(69),
  E(70),
  E(71),
  E(72),
  E(73),
  E(74),
  E(75),
  E(76),
  E(77),
  E(78),
  E(79),
  E(80),
  E(81),
  E(82),
  E(83),
  E(84),
  E(85),
  E(86),
  E(87),
  E(88),
  E(89),
  E(90),
  E(91),
  E(92),
  E(93),
  E(94),
  E(95),
  E(96),
  E(97),
  E(98),
  E(99),
  E(100),
  E(101),
  E(102),
  E(103),
  E(104),
  E(105),
  E(106),
  E(107),
  E(108),
  E(109),
  E(110),
  E(111),
#undef E
};

/* =============================================================================
 * Reset handler: copy .data from MRAM to SRAM, zero .bss, call main()
 * =============================================================================
 */

/* NOLINTBEGIN(clang-analyzer-security.ArrayBound,misc-use-internal-linkage) -- the .data/.bss copy loops walk linker-symbol bounds the analyzer cannot model, and Reset_Handler must be extern for the vector table. */
void Reset_Handler(void)
{
  /* Step 1: copy initialized data from flash/MRAM to SRAM. This MUST run
   * before SystemInit(): SystemInit() calls ra8_cgc_init(), which publishes
   * the settled clock-tree rates into the .data-resident cache
   * (ra8_cgc's s_clock_hz[]). If the .data copy ran AFTER SystemInit() it
   * would overwrite that cache with the boot-default (MOCO) LMA image, so
   * ra8_cgc_get_clock_hz(PCLKA) would report ~8 MHz in main() and the SCI8
   * console would refuse to come up (k_ra8_err_not_initialized). The stack
   * is already usable (SP is loaded from the vector table at reset), and
   * SRAM0-3 run at reset, so this copy is safe before SystemInit(). */
  uint32_t* src = &g_ra8_ls_sidata;
  uint32_t* dst = &g_ra8_ls_sdata;
  while (dst < &g_ra8_ls_edata) {
    *dst++ = *src++;
  }

  /* Step 2: zero BSS (also before SystemInit(), for the same reason -- any
   * .bss state SystemInit()'s ra8_cgc_init / ra8_trustzone_init establish
   * must survive into main()). */
  dst = &g_ra8_ls_sbss;
  while (dst < &g_ra8_ls_ebss) {
    *dst++ = 0U;
  }

  /* Step 3: core-level init (VTOR, FPU, caches, priority grouping, interrupts
   * masked) + the Secure clock tree + TrustZone SAU/NS-image copy. Runs with
   * .data/.bss already initialised, so ra8_cgc_init's clock-cache writes and
   * any other global state it (or ra8_trustzone_init) sets now persist. */
  SystemInit();

  /* Step 4: enter C. `main()` is responsible for enabling interrupts
   * via `__enable_irq()` once all drivers are ready. */
  (void)main();

  /* main() should never return; if it does, halt. */
  while (1) {
    __asm__ volatile("wfi");
  }
}

/* =============================================================================
 * HardFault / MemManage / BusFault / UsageFault naked trampolines
 * =============================================================================
 *
 * Each trampoline picks the stack pointer the fault was taken on
 * (MSP if `EXC_RETURN[2]=0`, PSP otherwise), packs an `exception
 * number` in `r1`, and tail-calls `ra8_exception_report()`. See
 * `ra8_exception.h` for the frame layout and HUM reference.
 */

extern void ra8_exception_report(const void* frame, uint32_t exc_number);

/**
 * @enum ra8_vector_exc_t
 * @brief Exception numbers matching the Cortex-M vector table slots.
 */
typedef enum : uint32_t {
  k_ra8_vector_hardfault  = 3U, /**< RA8 vector hardfault.  */
  k_ra8_vector_memmanage  = 4U, /**< RA8 vector memmanage.  */
  k_ra8_vector_busfault   = 5U, /**< RA8 vector busfault.   */
  k_ra8_vector_usagefault = 6U, /**< RA8 vector usagefault. */
} ra8_vector_exc_t;

#ifndef RA8_OFF_TARGET
[[gnu::naked, noreturn]] void HardFault_Handler(void)
{
  __asm__ volatile("tst lr, #4          \n"
                   "ite eq              \n"
                   "mrseq r0, msp       \n"
                   "mrsne r0, psp       \n"
                   "mov   r1, #3        \n"
                   "b     ra8_exception_report\n");
}

[[gnu::naked, noreturn]] void MemManage_Handler(void)
{
  __asm__ volatile("tst lr, #4          \n"
                   "ite eq              \n"
                   "mrseq r0, msp       \n"
                   "mrsne r0, psp       \n"
                   "mov   r1, #4        \n"
                   "b     ra8_exception_report\n");
}

[[gnu::naked, noreturn]] void BusFault_Handler(void)
{
  __asm__ volatile("tst lr, #4          \n"
                   "ite eq              \n"
                   "mrseq r0, msp       \n"
                   "mrsne r0, psp       \n"
                   "mov   r1, #5        \n"
                   "b     ra8_exception_report\n");
}

[[gnu::naked, noreturn]] void UsageFault_Handler(void)
{
  __asm__ volatile("tst lr, #4          \n"
                   "ite eq              \n"
                   "mrseq r0, msp       \n"
                   "mrsne r0, psp       \n"
                   "mov   r1, #6        \n"
                   "b     ra8_exception_report\n");
}
#else
/* Host build: fault handlers trap directly to the reporter. The host
 * compiler does not know `ra8_exception_report` is noreturn through
 * the extern declaration, so we add `__builtin_unreachable()` after
 * each call to keep the `noreturn` attribute on the handler valid. */
[[noreturn]] void HardFault_Handler(void)
{
  ra8_exception_report(nullptr, k_ra8_vector_hardfault);
  __builtin_unreachable();
}
[[noreturn]] void MemManage_Handler(void)
{
  ra8_exception_report(nullptr, k_ra8_vector_memmanage);
  __builtin_unreachable();
}
[[noreturn]] void BusFault_Handler(void)
{
  ra8_exception_report(nullptr, k_ra8_vector_busfault);
  __builtin_unreachable();
}
[[noreturn]] void UsageFault_Handler(void)
{
  ra8_exception_report(nullptr, k_ra8_vector_usagefault);
  __builtin_unreachable();
}
#endif

/* =============================================================================
 * Default trap
 * =============================================================================
 */

void Default_Handler(void)
{
  /* Stop here so an attached debugger can inspect stack / faults. */
#ifndef RA8_OFF_TARGET
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
#endif
}
/* NOLINTEND(clang-analyzer-security.ArrayBound,misc-use-internal-linkage) */
