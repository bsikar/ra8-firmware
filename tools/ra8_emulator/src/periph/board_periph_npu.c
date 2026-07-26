/**
 * @file board_periph_npu.c
 * @brief Arm Ethos-U55 NPU command/queue execution model (RA8P1-only) for board_sim
 *
 * @details
 * The RA8P1 (R7KA8P1KFLCAC) integrates an Arm Ethos-U55 micro-NPU that the
 * RA8D2 does not have; it occupies a 4 KiB register window at 0x40140000 -- the
 * confirmed base + extent from libs/ra8_hal/inc/ra8_npu_regs.h (cross-sourced
 * there against the RA8P1 FSP CMSIS header and the Zephyr device tree). This
 * block gives that window a WORKING model so the ra8_npu driver's
 * submit -> run -> poll -> read-output protocol runs end-to-end in the emulator.
 * It is tagged ::k_board_block_dev_ra8p1 so it is dispatched ONLY when board_sim
 * runs the RA8P1 profile (``--device ra8p1``); on the RA8D2 the window falls
 * through to the sparse fallback, so the RA8D2 run is unchanged.
 *
 * ## What is modelled (register interface, TRM-sourced)
 *
 * The Ethos-U55 command/queue interface is the Arm "Ethos-U55 NPU Technical
 * Reference Manual" architectural APB map; the offsets and field bits below
 * MIRROR libs/ra8_hal/inc/ra8_npu_regs.h (the primary-source transcription -- that
 * header is guarded to RA8P1 firmware builds and cannot be included in this host
 * build, so the constants are restated with an Ethos-U55 TRM citation, exactly as
 * board_periph_pdm.c / board_periph_ceu.c restate their register maps). Offsets
 * and bit indices that trace to general knowledge of the published Ethos-U55
 * interface rather than a datasheet opened here are flagged ``[CONFIRM]``.
 *
 * Modelled registers: NPU_ID (returns a documented Ethos-U55 id), NPU_STATUS
 * (state / cmd_end / irq / fault bits, computed from job state), NPU_CMD
 * (run / clear_irq), NPU_RESET (clears job state), QBASE / QSIZE (command-stream
 * pointer + length), and BASEP0..7 (per-tensor AXI region bases).
 *
 * ## The "execution" is a DETERMINISTIC BOARD_SIM STAND-IN, not real Vela
 *
 * board_sim is NOT a Vela interpreter (real Vela-compiled inference is the
 * follow-up on the RA8P1 NPU epic). When the driver kicks a job
 * (``CMD.transition_to_running_state``) this block reads the command stream at
 * QBASE and applies a tiny, DOCUMENTED sim-only convention -- see
 * libs/ra8_hal/inc/ra8_npu_sim_cmd.h -- to the tensor arenas at BASEPn: the first
 * command word carries a "SE55" magic marker plus a COPY or add-constant opcode,
 * and the model moves the referenced bytes from the source region to the
 * destination region so the OUTPUT arena holds a checkable, deterministic result.
 * On success it sets STATUS.cmd_end + STATUS.irq_raised and raises the NPU_IRQ
 * ELC event (::k_npu_event_irq), so ``ra8_npu_poll`` / ``ra8_npu_read_status`` see
 * completion.
 *
 * HONEST-MODEL rule (the ra8_rsip "invented register" history is the cautionary
 * tale): a command stream that does NOT carry the sim magic is a real Vela
 * program this model cannot interpret, so it latches STATUS.cmd_parse (a fault)
 * rather than faking a completion. Nothing here claims real NPU numerics.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_console.h"
#include "board_periph_block.h"
#include "ra8_npu_sim_cmd.h"

/**
 * @brief Ethos-U55 NPU register-window geometry (mirrors ra8_npu_regs.h).
 *
 * @details Base + 4 KiB extent are the confirmed constants from ra8_npu_regs.h;
 *          the register offsets are the Ethos-U55 TRM APB map. Offsets restated
 *          from general knowledge of the published interface are ``[CONFIRM]``.
 */
typedef enum : uint64_t {
  k_npu_base       = 0x40140000UL,   /**< R_NPU register-window base (Ethos-U55). */
  k_npu_span       = 0x1000UL,       /**< 4 KiB register window.                  */
  k_npu_words      = 0x1000UL / 4UL, /**< Backing-store word count.               */
  k_npu_off_id     = 0x0000UL,       /**< NPU_ID (Arm arch id/revision).          */
  k_npu_off_status = 0x0004UL,       /**< NPU_STATUS (state + fault flags).       */
  k_npu_off_cmd    = 0x0008UL,       /**< NPU_CMD (run / clear-irq). [CONFIRM]    */
  k_npu_off_reset  = 0x000CUL,       /**< NPU_RESET (software reset). [CONFIRM]   */
  k_npu_off_qbase  = 0x0010UL,       /**< QBASE[31:0] (cmd-stream address).       */
  k_npu_off_qsize  = 0x0020UL,       /**< QSIZE (cmd-stream length bytes).        */
  k_npu_off_basep0 = 0x0080UL,       /**< BASEP0[31:0] (region 0 AXI base).       */
} npu_map_t;

/**
 * @brief Ethos-U55 64-bit-register geometry + region count (mirrors ra8_npu_regs.h).
 */
typedef enum : uint32_t {
  k_npu_reg_hi_off   = 0x0004U, /**< Low->high word gap in a 64-bit register. */
  k_npu_basep_stride = 0x0008U, /**< Bytes between BASEPn pairs (lo + hi).    */
  k_npu_region_count = 8U,      /**< BASEP0..7 region base-pointer pairs.     */
  k_npu_addr_hi_pos  = 32U,     /**< uint64 address -> high 32 bits.          */
} npu_geom_t;

/**
 * @brief NPU_CMD / NPU_STATUS bit positions (mirror ra8_npu_regs.h). [CONFIRM]
 *
 * @details Bit indices trace to the Ethos-U55 TRM CMD / STATUS field layout;
 *          they are flagged ``[CONFIRM]`` as they come from general knowledge of
 *          the published interface, matching ra8_npu_regs.h.
 */
typedef enum : uint32_t {
  k_npu_cmd_run_bit       = 0U, /**< CMD.transition_to_running_state (start). */
  k_npu_cmd_clear_irq_bit = 1U, /**< CMD.clear_irq (write 1 to acknowledge).  */
  k_npu_status_state_bit  = 0U, /**< STATUS.state (1 = running).              */
  k_npu_status_irq_bit    = 1U, /**< STATUS.irq_raised.                       */
  k_npu_status_buserr_bit = 2U, /**< STATUS.bus_error (AXI fault).            */
  k_npu_status_reset_bit  = 3U, /**< STATUS.reset (reset in progress).        */
  k_npu_status_parse_bit  = 4U, /**< STATUS.cmd_parse (parse error).          */
  k_npu_status_cmdend_bit = 5U, /**< STATUS.cmd_end (stream fully consumed).  */
} npu_bit_t;

/**
 * @brief NPU_ID field positions in the Ethos-U55 id register. [CONFIRM]
 *
 * @details The id register packs arch major/minor/patch and product-major
 *          nibbles (Arm Ethos-U id layout). Positions are ``[CONFIRM]`` -- from
 *          general knowledge of the open ethosu_interface.h id bitfield, not a
 *          datasheet opened here.
 */
typedef enum : uint32_t {
  k_npu_id_arch_major_pos = 28U, /**< id[31:28] arch_major_rev.          */
  k_npu_id_arch_minor_pos = 20U, /**< id[27:20] arch_minor_rev.          */
  k_npu_id_arch_patch_pos = 16U, /**< id[19:16] arch_patch_rev.          */
  k_npu_id_product_pos    = 12U, /**< id[15:12] product_major (0 = U55). */
} npu_id_pos_t;

/**
 * @brief NPU_ID field values for the Ethos-U55. [CONFIRM]
 *
 * @details Composited into ::s_npu_id below. arch = 1.0.6 and product_major = 0
 *          (0 is believed to denote Ethos-U55, 1 Ethos-U65) are ``[CONFIRM]``
 *          general-knowledge values; the driver only needs a stable non-zero id,
 *          and npu_smoke asserts on the executed OUTPUT, not on this constant.
 */
typedef enum : uint32_t {
  k_npu_id_arch_major  = 1U, /**< arch major revision. [CONFIRM]          */
  k_npu_id_arch_minor  = 0U, /**< arch minor revision. [CONFIRM]          */
  k_npu_id_arch_patch  = 6U, /**< arch patch revision. [CONFIRM]          */
  k_npu_id_product_u55 = 0U, /**< product_major: 0 = Ethos-U55. [CONFIRM] */
} npu_id_val_t;

/**
 * @brief NPU interrupt event number and this block's report/tick order slot.
 *
 * @details ::k_npu_event_irq mirrors ra8_npu_regs.h ``k_ra8_npu_event_irq``.
 *          ::k_npu_block_order sits between GPIO (10) and the timers (20).
 */
typedef enum : uint32_t {
  k_npu_event_irq   = 0x067U, /**< NPU_IRQ ELC/ICU event (RA8P1 elc_event_t). */
  k_npu_block_order = 15U,    /**< Report/tick order slot (relative only).    */
} npu_misc_t;

/**
 * @brief Stand-in "execution" bounds + checkword constants (deterministic).
 *
 * @details ::k_npu_chunk_bytes / ::k_npu_max_chunks give the transfer loop a
 *          static upper bound (::k_ra8_npu_sim_max_bytes total). The FNV-1a
 *          constants fold the output bytes into a report/console checkword.
 */
typedef enum : uint32_t {
  k_npu_chunk_bytes = 256U,        /**< Bytes moved per transfer-loop chunk.     */
  k_npu_max_chunks  = 16U,         /**< k_ra8_npu_sim_max_bytes / chunk (bound). */
  k_npu_fnv_offset  = 0x811C9DC5U, /**< FNV-1a 32-bit offset basis.              */
  k_npu_fnv_prime   = 0x01000193U, /**< FNV-1a 32-bit prime.                     */
  k_npu_line_cap    = 64U,         /**< Console line buffer cap.                 */
} npu_exec_const_t;

/**
 * @brief Result of decoding one submitted command stream.
 */
typedef enum : uint8_t {
  k_npu_dec_ok    = 0U, /**< Recognised sim program; fields valid.      */
  k_npu_dec_parse = 1U, /**< Bad magic / field: real-Vela or malformed. */
  k_npu_dec_bus   = 2U, /**< Guest-memory read of the stream failed.    */
} npu_dec_t;

/**
 * @brief One decoded sim command (see ra8_npu_sim_cmd.h).
 */
typedef struct {
  uint32_t op;    /**< Opcode (COPY / add-constant).      */
  uint32_t src;   /**< Source region index.               */
  uint32_t dst;   /**< Destination region index.          */
  uint32_t count; /**< Byte count to process.             */
  uint32_t konst; /**< Constant addend (add-constant op). */
} npu_cmd_t;

/**
 * @brief NPU model state: register shadow + job tally for the end-of-run report.
 */
typedef struct {
  uint32_t reg[k_npu_words]; /**< Reflect-on-read shadow of raw register writes. */
  uint32_t status;           /**< Computed NPU_STATUS value.                     */
  uint32_t jobs;             /**< Jobs completed (report).                       */
  uint32_t faults;           /**< Faulted kicks (report).                        */
  uint32_t last_op;          /**< Opcode of the last completed job (report).     */
  uint32_t last_bytes;       /**< Bytes moved by the last completed job.         */
  uint32_t last_check;       /**< Output checkword of the last completed job.    */
  uint32_t reads;            /**< Reads observed inside the window.              */
  uint32_t writes;           /**< Writes observed inside the window.             */
} npu_state_t;

static npu_state_t s_npu;

/** @brief Composited Ethos-U55 NPU_ID (arch 1.0.6, product 0 = U55). [CONFIRM] */
static const uint32_t s_npu_id =
  ((uint32_t)k_npu_id_arch_major << (uint32_t)k_npu_id_arch_major_pos) |
  ((uint32_t)k_npu_id_arch_minor << (uint32_t)k_npu_id_arch_minor_pos) |
  ((uint32_t)k_npu_id_arch_patch << (uint32_t)k_npu_id_arch_patch_pos) |
  ((uint32_t)k_npu_id_product_u55 << (uint32_t)k_npu_id_product_pos);

/** @brief Word index of a byte offset inside the shadow store. */
static uint32_t npu_word(uint64_t off)
{
  return (uint32_t)(off >> 2U);
}

/** @brief Assemble the 64-bit value of a lo/hi shadow-register pair. */
static uint64_t npu_reg64(uint64_t lo_off)
{
  const uint32_t lo = s_npu.reg[npu_word(lo_off)];
  const uint32_t hi = s_npu.reg[npu_word(lo_off + (uint64_t)k_npu_reg_hi_off)];
  return (uint64_t)lo | ((uint64_t)hi << (uint32_t)k_npu_addr_hi_pos);
}

/** @brief AXI base programmed into BASEP@p idx (0 if unprogrammed). */
static uint64_t npu_region_base(uint32_t idx)
{
  const uint64_t lo_off =
    (uint64_t)k_npu_off_basep0 + ((uint64_t)idx * (uint64_t)k_npu_basep_stride);
  return npu_reg64(lo_off);
}

/** @brief Read one little-endian 32-bit word from guest memory (bool ok). */
static bool npu_guest_word(uc_engine* uc, uint64_t addr, uint32_t* out)
{
  uint8_t b[4] = {};
  if (uc_mem_read(uc, addr, b, sizeof(b)) != UC_ERR_OK) {
    return false;
  }
  /* Little-endian assembly over the fixed 4-byte buffer -- the loop keeps the
   * byte shift a named multiple (i * 8U) instead of a bare 24U high-byte
   * literal, and is statically bounded by sizeof(b) (NASA P10 Rule 2). */
  *out = 0U;
  for (uint32_t i = 0U; i < sizeof(b); ++i) {
    *out |= (uint32_t)b[i] << (i * 8U);
  }
  return true;
}

/** @brief Latch a STATUS fault bit and stop the engine (honest: no fake done). */
static void npu_fault(uint32_t status_bit)
{
  s_npu.status = ((uint32_t)1U << status_bit) | ((uint32_t)1U << (uint32_t)k_npu_status_irq_bit);
  s_npu.faults++;
}

/** @brief Decode the submitted command stream at QBASE per ra8_npu_sim_cmd.h. */
static npu_dec_t npu_decode(uc_engine* uc, npu_cmd_t* out)
{
  const uint64_t qbase = npu_reg64((uint64_t)k_npu_off_qbase);
  const uint32_t qsize = s_npu.reg[npu_word((uint64_t)k_npu_off_qsize)];
  if (qsize < (uint32_t)k_ra8_npu_sim_header_bytes) {
    return k_npu_dec_parse;
  }
  uint32_t w[k_ra8_npu_sim_word_num] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_npu_sim_word_num; i++) {
    if (!npu_guest_word(uc, qbase + ((uint64_t)i * 4UL), &w[i])) {
      return k_npu_dec_bus;
    }
  }
  if ((w[k_ra8_npu_sim_word_op] & (uint32_t)k_ra8_npu_sim_magic_mask) !=
      (uint32_t)k_ra8_npu_sim_magic) {
    return k_npu_dec_parse; /* Real Vela stream / not ours: cannot interpret. */
  }
  out->op    = w[k_ra8_npu_sim_word_op] & (uint32_t)k_ra8_npu_sim_op_mask;
  out->src   = w[k_ra8_npu_sim_word_src];
  out->dst   = w[k_ra8_npu_sim_word_dst];
  out->count = w[k_ra8_npu_sim_word_count];
  out->konst = w[k_ra8_npu_sim_word_const];
  const bool bad_region =
    (out->src >= (uint32_t)k_npu_region_count) || (out->dst >= (uint32_t)k_npu_region_count);
  const bool bad_count = (out->count == 0U) || (out->count > (uint32_t)k_ra8_npu_sim_max_bytes);
  if (bad_region || bad_count) {
    return k_npu_dec_parse;
  }
  return k_npu_dec_ok;
}

/** @brief Apply the op to the arenas; fold the output into a checkword (bool ok). */
static bool
npu_apply(uc_engine* uc, const npu_cmd_t* c, uint64_t src, uint64_t dst, uint32_t* out_check)
{
  uint8_t  buf[k_npu_chunk_bytes] = {};
  uint32_t check                  = (uint32_t)k_npu_fnv_offset;
  uint32_t done                   = 0U;
  for (uint32_t chunk = 0U; chunk < (uint32_t)k_npu_max_chunks; chunk++) {
    if (done >= c->count) {
      break;
    }
    uint32_t n = c->count - done;
    if (n > (uint32_t)k_npu_chunk_bytes) {
      n = (uint32_t)k_npu_chunk_bytes;
    }
    if (uc_mem_read(uc, src + done, buf, n) != UC_ERR_OK) {
      return false;
    }
    if (c->op == (uint32_t)k_ra8_npu_sim_op_addk) {
      for (uint32_t i = 0U; i < n; i++) {
        buf[i] = (uint8_t)((buf[i] + c->konst) & (uint32_t)k_ra8_npu_sim_byte_mask);
      }
    }
    if (uc_mem_write(uc, dst + done, buf, n) != UC_ERR_OK) {
      return false;
    }
    for (uint32_t i = 0U; i < n; i++) {
      check = (check ^ (uint32_t)buf[i]) * (uint32_t)k_npu_fnv_prime;
    }
    done += n;
  }
  *out_check = check;
  return true;
}

/** @brief Human label for a sim opcode (report / console). */
static const char* npu_op_name(uint32_t op)
{
  if (op == (uint32_t)k_ra8_npu_sim_op_copy) {
    return "copy";
  }
  if (op == (uint32_t)k_ra8_npu_sim_op_addk) {
    return "add-const";
  }
  return "unknown";
}

/** @brief Push one completed-job summary line to the DMA/compute console lane. */
static void npu_console_job(const npu_cmd_t* c, uint32_t check)
{
  char ln[k_npu_line_cap];
  (void)snprintf(ln,
                 sizeof(ln),
                 "NPU %s r%u->r%u %uB check=0x%08X",
                 npu_op_name(c->op),
                 (unsigned)c->src,
                 (unsigned)c->dst,
                 (unsigned)c->count,
                 (unsigned)check);
  board_console_push(k_board_console_ch_dma, ln);
}

/** @brief Run one submitted job: decode -> execute -> latch STATUS + IRQ. */
static void npu_execute(uc_engine* uc)
{
  npu_cmd_t       c   = {};
  const npu_dec_t dec = npu_decode(uc, &c);
  if (dec == k_npu_dec_bus) {
    npu_fault((uint32_t)k_npu_status_buserr_bit);
    return;
  }
  if (dec != k_npu_dec_ok) {
    npu_fault((uint32_t)k_npu_status_parse_bit);
    return;
  }
  const uint64_t src = npu_region_base(c.src);
  const uint64_t dst = npu_region_base(c.dst);
  if ((src == 0U) || (dst == 0U)) {
    npu_fault((uint32_t)k_npu_status_parse_bit);
    return;
  }
  uint32_t check = 0U;
  if (!npu_apply(uc, &c, src, dst, &check)) {
    npu_fault((uint32_t)k_npu_status_buserr_bit);
    return;
  }
  s_npu.status = ((uint32_t)1U << (uint32_t)k_npu_status_cmdend_bit) |
                 ((uint32_t)1U << (uint32_t)k_npu_status_irq_bit);
  s_npu.jobs++;
  s_npu.last_op    = c.op;
  s_npu.last_bytes = c.count;
  s_npu.last_check = check;
  board_periph_icu_raise_event(uc, (uint16_t)k_npu_event_irq);
  npu_console_job(&c, check);
}

/** @brief Act on an NPU_CMD write: run kicks a job, clear_irq de-asserts the IRQ. */
static void npu_on_cmd(uc_engine* uc, uint32_t value)
{
  if ((value & ((uint32_t)1U << (uint32_t)k_npu_cmd_clear_irq_bit)) != 0U) {
    s_npu.status &= ~((uint32_t)1U << (uint32_t)k_npu_status_irq_bit);
  }
  if ((value & ((uint32_t)1U << (uint32_t)k_npu_cmd_run_bit)) != 0U) {
    npu_execute(uc);
  }
}

/** @brief MMIO read inside the NPU window: ID + STATUS are computed, rest shadow. */
static uint64_t npu_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_npu_base;
  if (off >= (uint64_t)k_npu_span) {
    return 0U;
  }
  s_npu.reads++;
  if (off == (uint64_t)k_npu_off_id) {
    return (uint64_t)s_npu_id;
  }
  if (off == (uint64_t)k_npu_off_status) {
    return (uint64_t)s_npu.status; /* STATUS.reset stays 0: reset is instant. */
  }
  return (uint64_t)s_npu.reg[npu_word(off)];
}

/** @brief MMIO write inside the NPU window: latch, then act on CMD / RESET. */
static void npu_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)size;
  const uint64_t off = addr - (uint64_t)k_npu_base;
  if (off >= (uint64_t)k_npu_span) {
    return;
  }
  s_npu.writes++;
  s_npu.reg[npu_word(off)] = (uint32_t)value;
  if (off == (uint64_t)k_npu_off_cmd) {
    npu_on_cmd(uc, (uint32_t)value);
    return;
  }
  if (off == (uint64_t)k_npu_off_reset) {
    s_npu.status = 0U; /* Reset discards job state; STATUS.reset reads 0. */
  }
}

/** @brief Clear the NPU model to power-on state. */
static void npu_reset(void)
{
  s_npu = (npu_state_t){};
}

/** @brief End-of-run NPU section: jobs run + last-result checkword, or touch notice. */
static void npu_report(void)
{
  if ((s_npu.jobs == 0U) && (s_npu.faults == 0U)) {
    if ((s_npu.reads != 0U) || (s_npu.writes != 0U)) {
      (void)fprintf(stderr,
                    "  NPU(Ethos-U55): window touched r=%u w=%u -- no job kicked\n",
                    s_npu.reads,
                    s_npu.writes);
    }
    return;
  }
  (void)fprintf(stderr,
                "  NPU(Ethos-U55): jobs=%u faults=%u last=%s bytes=%u check=0x%08X"
                " (board_sim stand-in, not Vela)\n",
                s_npu.jobs,
                s_npu.faults,
                npu_op_name(s_npu.last_op),
                s_npu.last_bytes,
                s_npu.last_check);
}

/** @brief NPU block descriptor (RA8P1-only; self-registered with the core). */
static const board_periph_block_t k_npu_block = {
  .base   = (uint64_t)k_npu_base,
  .span   = (uint64_t)k_npu_span,
  .order  = (uint32_t)k_npu_block_order,
  .read   = npu_read,
  .write  = npu_write,
  .tick   = nullptr,
  .reset  = npu_reset,
  .report = npu_report,
  .name   = "NPU",
  .device = k_board_block_dev_ra8p1,
};

/** @brief Register the NPU block before main (host constructor). */
[[gnu::constructor]] static void npu_block_register(void)
{
  board_periph_register_block(&k_npu_block);
}
