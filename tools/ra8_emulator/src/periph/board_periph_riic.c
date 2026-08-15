/**
 * @file board_periph_riic.c
 * @brief RIIC (IIC) controller + PI4IOE5V6408 I/O-expander + OV5640 SCCB model
 *
 * @details
 * Models the RA8D2 RIIC peripheral -- the classic Renesas I2C Bus Interface
 * (HUM Ch 39), distinct from the I3C unified IP modelled in
 * board_periph_i2c.c -- that the ra8_i2c.c polling driver drives, plus the
 * on-board PI4IOE5V6408 I/O port expander (U15) that answers on the modelled
 * bus. So the i2c_loopback example's real ra8_i2c_init / ra8_i2c_write /
 * ra8_i2c_scan path against U15 at 7-bit address 0x43 round-trips (the scan ACKs,
 * the U15 register writes ACK) without a function-level stub.
 *
 * Four cooperating pieces live here, mirroring board_periph_i2c.c:
 *  1. The RIIC controller transfer state machine (START / RESTART / address /
 *     write / read / STOP) the driver clocks through ICCR2 / ICDRT / ICDRR /
 *     ICSR2.
 *  2. A small I2C bus device registry mapping a 7-bit address to read / write /
 *     stop callbacks.
 *  3. The PI4IOE5V6408 device: an auto-incrementing register pointer the
 *     controller writes first, then a payload (or a read of the addressed
 *     register), so the expander ACKs every probe and register write.
 *  4. The OV5640 camera SCCB device (channel 1, 7-bit 0x3C): a 16-bit
 *     big-endian register pointer whose chip-ID registers (0x300A / 0x300B)
 *     answer 0x56 / 0x40 so ``camera_capture``'s VERIFY-FIRST sensor probe reads
 *     0x5640 and reaches its CEU capture path (board_periph_ceu.c); config
 *     writes ACK and the firmware's post-configuration verifier registers
 *     read back their last written values. The register/value path is synthetic
 *     (no analog sensor) -- a run-headless enabler, not a claim the board's
 *     camera streams.
 *
 * Self-registers its descriptor (address range + read / write / reset /
 * report) with the board_periph core from a file-scope constructor.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_console.h"
#include "board_periph_block.h"
#include "board_periph_riic_devices_internal.h"
#include "emu_host_io_internal.h"

/** @brief Console-tap line buffer capacity for a RIIC transaction summary. */
typedef enum : uint32_t {
  k_riic_console_line_cap = 48U, /**< Max chars in a "RIIC addr=.. .." line. */
} riic_console_t;

/**
 * @brief RIIC block geometry (ra8_i2c_regs.h, byte-wide register file).
 *
 * @details
 * The RA8D2 has three RIIC channels at 0x4025E000 + 0x100 * n. The U15 I/O
 * expander and the system I2C bus live on channel 1 (P512 SCL1 / P511 SDA1),
 * which the i2c_loopback example and ra8_board_io_expander_apply_project_sw4_defaults
 * drive. Every RIIC register is a single byte; the polling driver
 * (libs/ra8_hal/src/ra8_i2c.c) only touches the registers named here. Offsets
 * match the @c r_i2c_regs_t struct in ra8_i2c_regs.h.
 */
typedef enum : uint64_t {
  k_riic_base      = 0x4025E000UL,  /**< IIC0 base (channel 0).             */
  k_riic_stride    = 0x100UL,       /**< Bytes per RIIC channel.            */
  k_riic_count     = 3UL,           /**< IIC0 / IIC1 / IIC2.                */
  k_riic_span      = 0x100UL * 3UL, /**< All three channel windows.         */
  k_riic_off_iccr1 = 0x00UL,        /**< ICCR1 control 1 (ICE / IICRST).    */
  k_riic_off_iccr2 = 0x01UL,        /**< ICCR2 control 2 (ST/RS/SP/BBSY).   */
  k_riic_off_icmr3 = 0x04UL,        /**< ICMR3 mode 3 (ACKWP/ACKBT/WAIT).   */
  k_riic_off_icser = 0x06UL,        /**< ICSER own-address enable (SARyE).  */
  k_riic_off_icsr1 = 0x08UL,        /**< ICSR1 status 1 (AASy own-addr).    */
  k_riic_off_icsr2 = 0x09UL,        /**< ICSR2 status 2 (TDRE/TEND/...).    */
  k_riic_off_sarl0 = 0x0AUL,        /**< SARL0 own-address low, slot 0.     */
  k_riic_off_sarl1 = 0x0CUL,        /**< SARL1 own-address low, slot 1.     */
  k_riic_off_sarl2 = 0x0EUL,        /**< SARL2 own-address low, slot 2.     */
  k_riic_off_icdrt = 0x12UL,        /**< ICDRT transmit data register.      */
  k_riic_off_icdrr = 0x13UL,        /**< ICDRR receive data register.       */
  k_riic_reg_bytes = 0x16UL,        /**< Shadow byte count for one channel. */
} riic_map_t;

/** @brief ICCR2 (control 2) bits the model acts on (ra8_i2c_regs.h). */
typedef enum : uint32_t {
  k_riic_iccr2_st   = 0x02U, /**< ST: issue START (bit 1).            */
  k_riic_iccr2_rs   = 0x04U, /**< RS: issue repeated-START (bit 2).   */
  k_riic_iccr2_sp   = 0x08U, /**< SP: issue STOP (bit 3).             */
  k_riic_iccr2_trs  = 0x20U, /**< TRS: transmit/receive mode (bit 5). */
  k_riic_iccr2_mst  = 0x40U, /**< MST: controller/peripheral (bit 6). */
  k_riic_iccr2_bbsy = 0x80U, /**< BBSY: bus-busy flag, RO (bit 7).    */
} riic_iccr2_bit_t;

/** @brief ICSR2 (status 2) flags the driver polls / clears (ra8_i2c_regs.h). */
typedef enum : uint32_t {
  k_riic_icsr2_tmof  = 0x01U, /**< TMOF: timeout detected (bit 0).          */
  k_riic_icsr2_al    = 0x02U, /**< AL: arbitration lost (bit 1).            */
  k_riic_icsr2_start = 0x04U, /**< START: start-condition detected (bit 2). */
  k_riic_icsr2_stop  = 0x08U, /**< STOP: stop-condition detected (bit 3).   */
  k_riic_icsr2_nackf = 0x10U, /**< NACKF: NACK detected (bit 4).            */
  k_riic_icsr2_rdrf  = 0x20U, /**< RDRF: receive data full (bit 5).         */
  k_riic_icsr2_tend  = 0x40U, /**< TEND: transmit end (bit 6).              */
  k_riic_icsr2_tdre  = 0x80U, /**< TDRE: transmit data empty (bit 7).       */
} riic_icsr2_bit_t;

/** @brief ICSR1 (status 1) own-address match flags (ra8_i2c_regs.h). */
typedef enum : uint32_t {
  k_riic_icsr1_aas0 = 0x01U, /**< AAS0: own-address slot 0 matched (bit 0). */
} riic_icsr1_bit_t;

/** @brief ICSER (status enable) own-address slot-enable bits (ra8_i2c_regs.h). */
typedef enum : uint32_t {
  k_riic_icser_sar0e = 0x01U, /**< SAR0E: own-address slot 0 enable (bit 0). */
  k_riic_icser_sar1e = 0x02U, /**< SAR1E: own-address slot 1 enable (bit 1). */
  k_riic_icser_sar2e = 0x04U, /**< SAR2E: own-address slot 2 enable (bit 2). */
  k_riic_icser_slots = 0x07U, /**< Any own-address slot enabled (SARyE).     */
} riic_icser_bit_t;

/**
 * @brief RIIC target (peripheral) role: phases + synthetic external controller.
 *
 * @details
 * The RA8D2 answers as an I2C target once the ra8_i2c target driver programmes an
 * own address (SARLy) and enables its slot (ICSER.SARyE) -- the
 * ``i2c_peripheral_responder`` example. On the bench a remote controller drives
 * the bus; headless, ra8_emulator IS that controller. The stimulus drives a fixed
 * write-then-read script: it writes a known payload to the target (the driver's
 * ``ra8_i2c_peripheral_receive`` path drains it), then reads the target back (the
 * ``_transmit`` path echoes it), for ::k_riic_tgt_cycles cycles, and verifies the
 * echo matches. The addressing byte position (dummy ICDRR read), RDRF/STOP
 * receive handshake, and TDRE/TEND transmit handshake mirror HUM Ch 39.3.5 /
 * 39.3.6, so the genuine target driver runs unmodified.
 */
typedef enum : uint8_t {
  k_riic_tgt_idle  = 0U, /**< No own address armed; target model inert.      */
  k_riic_tgt_write = 1U, /**< Controller is writing the payload (RDRF/STOP). */
  k_riic_tgt_read  = 2U, /**< Controller is reading the echo (TDRE/TEND).    */
  k_riic_tgt_done  = 3U, /**< Script complete; the bus is quiet (no match).  */
} riic_tgt_phase_t;

/** @brief Target-mode synthetic-controller payload + script size. */
typedef enum : uint8_t {
  k_riic_tgt_payload0    = 0xDEU, /**< First byte the synthetic controller writes.  */
  k_riic_tgt_payload1    = 0xADU, /**< Second byte the synthetic controller writes. */
  k_riic_tgt_payload_len = 2U,    /**< Bytes per write (and expected echo length).  */
  k_riic_tgt_cap         = 8U,    /**< Echo-capture buffer bound.                   */
  k_riic_tgt_cycles      = 4U,    /**< Write+read cycles the stimulus drives.       */
} riic_tgt_stim_t;

/** @brief The synthetic controller's write payload (echoed back on read). */
static const uint8_t s_k_riic_tgt_payload[k_riic_tgt_payload_len] = {
  (uint8_t)k_riic_tgt_payload0,
  (uint8_t)k_riic_tgt_payload1,
};

/** @brief I2C address-byte layout on the wire (addr<<1 | R/W). */
typedef enum : uint32_t {
  k_riic_addr_shift = 1U,    /**< 7-bit address occupies bits [7:1].    */
  k_riic_addr_rnw   = 0x01U, /**< LSB: 1 == read, 0 == write.           */
  k_riic_addr_mask7 = 0x7FU, /**< 7-bit target-address mask.            */
  k_riic_byte_mask  = 0xFFU, /**< One data byte.                        */
  k_riic_dev_rx_max = 64U,   /**< Per-read device response staging cap. */
} riic_addr_t;

/**
 * @brief One RIIC channel: register shadow + a controller transfer machine.
 *
 * @details
 * The model owns the registers the polling driver actually touches; the rest
 * of the window reflects writes through @c reg. @c busy spans START..STOP
 * (ICCR2.BBSY reports it), @c addr_done latches once the address byte after a
 * (re)START has selected a device, @c reading records the transfer direction,
 * @c target_7b records the selection, @c icsr2 carries the TDRE / TEND / RDRF /
 * NACKF flags the driver waits on, and the @c rx staging buffer holds the bytes
 * the addressed device produced for the current read (drained one per ICDRR
 * read, after the FSP-style dummy first read).
 */
typedef struct {
  uint8_t  reg[k_riic_reg_bytes]; /**< Reflect-on-read register shadow.         */
  uint32_t icsr2;                 /**< ICSR2 flags (TDRE/TEND/RDRF/NACKF).      */
  bool     busy;                  /**< True between START and STOP.             */
  bool     addr_done;             /**< Address phase of the (re)START done.     */
  bool     acked;                 /**< The addressed target ACKed.              */
  bool     reading;               /**< Current transfer direction is read.      */
  uint8_t  target_7b;             /**< 7-bit address selected this transfer.    */
  uint8_t  rx[k_riic_dev_rx_max]; /**< Staged device response for a read.       */
  uint32_t rx_len;                /**< Valid bytes in @c rx.                    */
  uint32_t rx_pos;                /**< Next byte index served from @c rx.       */
  bool     rx_primed;             /**< The dummy first ICDRR read was consumed. */
  /* Target (peripheral) role: this model is the synthetic external controller. */
  bool     tgt_armed;              /**< Own address enabled (ICSER.SARyE set).     */
  uint8_t  tgt_own_addr;           /**< Firmware's 7-bit own address (SARLy>>1).   */
  uint8_t  tgt_phase;              /**< ::riic_tgt_phase_t of the current script.  */
  uint32_t tgt_icsr2;              /**< Target-role ICSR2 flags (RDRF/STOP/...).   */
  uint32_t tgt_wr_pos;             /**< ICDRR serves consumed this write (0=addr). */
  uint32_t tgt_rd_len;             /**< Bytes the firmware transmitted this read.  */
  uint32_t tgt_cycles;             /**< Completed write+read cycles.               */
  bool     tgt_echo_bad;           /**< A firmware echo did not match the write.   */
  uint8_t  tgt_rd[k_riic_tgt_cap]; /**< Captured echo bytes (bounds-checked).      */
} riic_state_t;

/** @brief The three modelled RIIC controller channels. */
static riic_state_t s_riic[k_riic_count];

/* =============================================================================
 * RIIC controller model -- the transfer state machine the ra8_i2c.c polling
 * driver drives (START / RESTART / addr / write / read / STOP).
 * =============================================================================
 */

/**
 * @brief Begin a transaction (START or repeated-START): arm the address phase.
 * @details Begin a transaction (start or repeated-start): arm the address phase; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @pre Arguments satisfy the ranges documented for RIIC open transfer. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_open_transfer(riic_state_t* s)
{
  s->busy      = true;
  s->addr_done = false;
  s->acked     = false;
  s->reading   = false;
  s->rx_len    = 0U;
  s->rx_pos    = 0U;
  s->rx_primed = false;
  /* Transmit buffer is empty so the driver can write the address byte; clear
   * the stale condition / fault flags so the new phase reports its own. */
  s->icsr2 = (uint32_t)k_riic_icsr2_tdre;
}

/**
 * @brief Close a transaction (STOP): release the bus and notify the device.
 * @details Close a transaction (stop): release the bus and notify the device; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @pre Arguments satisfy the ranges documented for RIIC close transfer. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_close_transfer(riic_state_t* s)
{
  if (s->acked) {
    riic_device_t* dev = priv_riic_device_find(s->target_7b);
    if (dev != nullptr) {
      if (dev->stop != nullptr) {
        dev->stop(dev->ctx);
      }
    }
    /* Console I2C tab: one line per completed (ACKed) RIIC transaction at STOP
     * (RIIC shares the I2C tab with the IIC_B model). */
    char ln[k_riic_console_line_cap];
    if (s->reading) {
      (void)snprintf(ln,
                     sizeof(ln),
                     "RIIC addr=0x%02X R %uB",
                     (unsigned)s->target_7b,
                     (unsigned)s->rx_len);
    } else {
      (void)snprintf(ln, sizeof(ln), "RIIC addr=0x%02X W", (unsigned)s->target_7b);
    }
    board_console_push(k_board_console_ch_i2c, ln);
  }
  s->busy      = false;
  s->addr_done = false;
  s->icsr2     = (uint32_t)k_riic_icsr2_stop;
}

/**
 * @brief Consume the address byte after a (re)START: select + ACK a device.
 * @details Consume the address byte after a (re)start: select + ack a device; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @param[in] address_byte Address byte input used by the operation.
 * @pre Arguments satisfy the ranges documented for RIIC address phase. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_address_phase(riic_state_t* s, uint8_t address_byte)
{
  s->target_7b =
    (uint8_t)((uint32_t)address_byte >> (uint32_t)k_riic_addr_shift) & (uint8_t)k_riic_addr_mask7;
  s->reading   = ((uint32_t)address_byte & (uint32_t)k_riic_addr_rnw) != 0U;
  s->addr_done = true;

  riic_device_t* dev = priv_riic_device_find(s->target_7b);
  if (dev == nullptr) {
    /* No device at this address: NACK it (scan reports ack=0). */
    s->acked = false;
    s->icsr2 |=
      ((uint32_t)k_riic_icsr2_nackf | (uint32_t)k_riic_icsr2_tend | (uint32_t)k_riic_icsr2_tdre);
    return;
  }
  s->acked = true;
  /* Address ACKed: TEND lets the scan / write proceed, TDRE re-arms TX. */
  s->icsr2 |= ((uint32_t)k_riic_icsr2_tend | (uint32_t)k_riic_icsr2_tdre);
  if (s->reading) {
    /* Pre-fetch the device's response for this read so ICDRR reads serve it. */
    s->rx_len    = dev->read(dev->ctx, s->rx, (uint32_t)k_riic_dev_rx_max);
    s->rx_pos    = 0U;
    s->rx_primed = false;
    s->icsr2 |= (uint32_t)k_riic_icsr2_rdrf; /* RX data ready */
  }
}

/**
 * @brief Handle a write to ICDRT (address byte, then controller TX payload).
 * @details Handle a write to icdrt (address byte, then controller tx payload); this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for RIIC icdrt write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_icdrt_write(riic_state_t* s, uint32_t value)
{
  const uint8_t byte = (uint8_t)(value & (uint32_t)k_riic_byte_mask);
  if (!s->addr_done) {
    internal_riic_address_phase(s, byte);
    return;
  }
  if (!s->acked) {
    return; /* NACKed address: swallow further writes until STOP */
  }
  riic_device_t* dev = priv_riic_device_find(s->target_7b);
  if ((dev != nullptr) && (dev->write != nullptr)) {
    dev->write(dev->ctx, byte);
  }
  /* Buffer empty again and the byte was accepted: TDRE + TEND for the next. */
  s->icsr2 |= ((uint32_t)k_riic_icsr2_tdre | (uint32_t)k_riic_icsr2_tend);
}

/**
 * @brief Serve one ICDRR read from the staged device response.
 * @details Serve one icdrr read from the staged device response; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @return The RIIC icdrr read result produced by the board periph RIIC model.
 * @retval value The operation-specific RIIC icdrr read value.
 * @pre Arguments satisfy the ranges documented for RIIC icdrr read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_riic_icdrr_read(riic_state_t* s)
{
  if (!s->rx_primed) {
    /* The controller-receive flow does one dummy ICDRR read to start the clock
     * before the first real byte (HUM Ch 39.3.4). */
    s->rx_primed = true;
    return 0U;
  }
  uint8_t b = 0U;
  if (s->rx_pos < s->rx_len) {
    b = s->rx[s->rx_pos];
    s->rx_pos++;
  }
  s->icsr2 |= (uint32_t)k_riic_icsr2_rdrf; /* keep RX-ready for the next byte */
  return (uint32_t)b;
}

/**
 * @brief Dispatch an ICCR2 write -> START / repeated-START / STOP.
 * @details Dispatch an iccr2 write -> start / repeated-start / stop; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @param[in] off Register or byte offset addressed by the operation.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for RIIC iccr2 write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_iccr2_write(riic_state_t* s, uint64_t off, uint32_t value)
{
  if ((value & ((uint32_t)k_riic_iccr2_st | (uint32_t)k_riic_iccr2_rs)) != 0U) {
    internal_riic_open_transfer(s);
    /* ST / RS auto-clear once the condition is issued; the driver spins on RS
     * reading 0 after a repeated-START, so drop both request bits. */
    s->reg[off] = (uint8_t)(value & ~((uint32_t)k_riic_iccr2_st | (uint32_t)k_riic_iccr2_rs));
  } else if ((value & (uint32_t)k_riic_iccr2_sp) != 0U) {
    internal_riic_close_transfer(s);
    s->reg[off] = (uint8_t)(value & ~(uint32_t)k_riic_iccr2_sp);
  }
}

/* =============================================================================
 * RIIC target (peripheral) model -- ra8_emulator is the synthetic external
 * controller, driving a write-then-read script at the firmware's own address so
 * the real ra8_i2c_peripheral_* driver (receive / transmit / dispatch) runs.
 * =============================================================================
 */

/**
 * @brief Arm the write phase: present the address-match + receive-ready state.
 * @details Arm the write phase: present the address-match + receive-ready state; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @pre Arguments satisfy the ranges documented for RIIC target begin write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_target_begin_write(riic_state_t* s)
{
  s->tgt_phase  = (uint8_t)k_riic_tgt_write;
  s->tgt_wr_pos = 0U;
  s->tgt_icsr2  = (uint32_t)k_riic_icsr2_rdrf; /* address + first data byte ready */
}

/**
 * @brief Arm the read phase: present transmit-ready + transmit-end for the echo.
 * @details Arm the read phase: present transmit-ready + transmit-end for the echo; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @pre Arguments satisfy the ranges documented for RIIC target begin read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_target_begin_read(riic_state_t* s)
{
  s->tgt_phase  = (uint8_t)k_riic_tgt_read;
  s->tgt_rd_len = 0U;
  s->tgt_icsr2  = (uint32_t)k_riic_icsr2_tdre | (uint32_t)k_riic_icsr2_tend;
}

/**
 * @brief Close one cycle: verify the echo, then loop the script or quiesce the bus.
 * @details Close one cycle: verify the echo, then loop the script or quiesce the bus; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @pre Arguments satisfy the ranges documented for RIIC target complete read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_target_complete_read(riic_state_t* s)
{
  bool ok = (s->tgt_rd_len == (uint32_t)k_riic_tgt_payload_len);
  for (uint32_t i = 0U; i < (uint32_t)k_riic_tgt_payload_len; i++) {
    if (s->tgt_rd[i] != s_k_riic_tgt_payload[i]) {
      ok = false;
    }
  }
  if (!ok) {
    s->tgt_echo_bad = true;
  }
  s->tgt_cycles++;
  if (s->tgt_cycles < (uint32_t)k_riic_tgt_cycles) {
    internal_riic_target_begin_write(s);
  } else {
    s->tgt_phase = (uint8_t)k_riic_tgt_done;
    s->tgt_icsr2 = 0U; /* no match, no STOP: dispatch reports no event forever. */
  }
}

/**
 * @brief (Dis)arm target mode on an ICSER write; latch the firmware's own address.
 * @details (dis)arm target mode on an icser write; latch the firmware's own address; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @param[in] icser Icser input used by the operation.
 * @pre Arguments satisfy the ranges documented for RIIC target open. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_target_open(riic_state_t* s, uint32_t icser)
{
  if ((icser & (uint32_t)k_riic_icser_slots) == 0U) {
    s->tgt_armed = false;
    s->tgt_phase = (uint8_t)k_riic_tgt_idle;
    return;
  }
  uint8_t sarl = s->reg[k_riic_off_sarl0];
  if ((icser & (uint32_t)k_riic_icser_sar1e) != 0U) {
    sarl = s->reg[k_riic_off_sarl1];
  } else if ((icser & (uint32_t)k_riic_icser_sar2e) != 0U) {
    sarl = s->reg[k_riic_off_sarl2];
  }
  s->tgt_own_addr = (uint8_t)((uint32_t)sarl >> (uint32_t)k_riic_addr_shift);
  s->tgt_armed    = true;
  internal_riic_target_begin_write(s);
}

/**
 * @brief ICSR1 read in target mode: assert the own-address match while active.
 * @details Icsr1 read in target mode: assert the own-address match while active; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in] s Module state instance processed by the operation.
 * @return The RIIC target icsr1 result produced by the board periph RIIC model.
 * @retval value The operation-specific RIIC target icsr1 value.
 * @pre Arguments satisfy the ranges documented for RIIC target icsr1. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_riic_target_icsr1(const riic_state_t* s)
{
  if (s->tgt_phase == (uint8_t)k_riic_tgt_write) {
    return (uint32_t)k_riic_icsr1_aas0;
  }
  if (s->tgt_phase == (uint8_t)k_riic_tgt_read) {
    return (uint32_t)k_riic_icsr1_aas0;
  }
  return 0U;
}

/**
 * @brief ICCR2 read in target mode: assert TRS only while the controller reads.
 * @details Iccr2 read in target mode: assert trs only while the controller reads; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in] s Module state instance processed by the operation.
 * @return The RIIC target iccr2 result produced by the board periph RIIC model.
 * @retval value The operation-specific RIIC target iccr2 value.
 * @pre Arguments satisfy the ranges documented for RIIC target iccr2. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_riic_target_iccr2(const riic_state_t* s)
{
  uint32_t v = (uint32_t)s->reg[k_riic_off_iccr2] & ~(uint32_t)k_riic_iccr2_trs;
  if (s->tgt_phase == (uint8_t)k_riic_tgt_read) {
    v |= (uint32_t)k_riic_iccr2_trs;
  }
  return v;
}

/**
 * @brief ICDRR read in target mode: serve the address byte, then the payload.
 * @details Icdrr read in target mode: serve the address byte, then the payload; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @return The RIIC target icdrr result produced by the board periph RIIC model.
 * @retval value The operation-specific RIIC target icdrr value.
 * @pre Arguments satisfy the ranges documented for RIIC target icdrr. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_riic_target_icdrr(riic_state_t* s)
{
  if (s->tgt_phase != (uint8_t)k_riic_tgt_write) {
    return 0U; /* read-phase SCL-release dummy read / idle: no receive data. */
  }
  uint8_t b = 0U;
  if (s->tgt_wr_pos == 0U) {
    /* Step 1: the matched address byte the driver dummy-reads to discard. */
    b = (uint8_t)((uint32_t)s->tgt_own_addr << (uint32_t)k_riic_addr_shift);
  } else if (s->tgt_wr_pos <= (uint32_t)k_riic_tgt_payload_len) {
    b = s_k_riic_tgt_payload[s->tgt_wr_pos - 1U];
  }
  s->tgt_wr_pos++;
  if (s->tgt_wr_pos > (uint32_t)k_riic_tgt_payload_len) {
    s->tgt_icsr2 = (uint32_t)k_riic_icsr2_stop; /* all served: STOP, drop RDRF. */
  } else {
    s->tgt_icsr2 = (uint32_t)k_riic_icsr2_rdrf;
  }
  return (uint32_t)b;
}

/**
 * @brief ICDRT write in target mode: capture the firmware's echo byte.
 * @details Icdrt write in target mode: capture the firmware's echo byte; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @param[in] byte One data byte received from or sent to the emulated interface.
 * @pre Arguments satisfy the ranges documented for RIIC target icdrt. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_target_icdrt(riic_state_t* s, uint8_t byte)
{
  if (s->tgt_phase != (uint8_t)k_riic_tgt_read) {
    return;
  }
  if (s->tgt_rd_len < (uint32_t)k_riic_tgt_cap) {
    s->tgt_rd[s->tgt_rd_len] = byte;
  }
  s->tgt_rd_len++;
  /* TDRE|TEND stay asserted (begin_read) so fill_tx keeps pushing to completion. */
}

/**
 * @brief ICSR2 W0C write in target mode: advance the write->read->done script.
 * @details Icsr2 w0c write in target mode: advance the write->read->done script; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for RIIC target icsr2 write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_target_icsr2_write(riic_state_t* s, uint32_t value)
{
  s->tgt_icsr2 &= value; /* condition flags are write-0-to-clear. */
  if (s->tgt_phase == (uint8_t)k_riic_tgt_write) {
    internal_riic_target_begin_read(s); /* receive() cleared STOP -> controller now reads. */
  } else if (s->tgt_phase == (uint8_t)k_riic_tgt_read) {
    internal_riic_target_complete_read(s); /* finish_tx cleared NACKF/STOP -> cycle done. */
  }
}

/**
 * @brief Register read while target mode is armed (own-address responder path).
 * @details Register read while target mode is armed (own-address responder path); this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @param[in] off Register or byte offset addressed by the operation.
 * @return The RIIC target reg read result produced by the board periph RIIC model.
 * @retval value The operation-specific RIIC target reg read value.
 * @pre Arguments satisfy the ranges documented for RIIC target reg read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_riic_target_reg_read(riic_state_t* s, uint64_t off)
{
  if (off == (uint64_t)k_riic_off_icsr1) {
    return internal_riic_target_icsr1(s);
  }
  if (off == (uint64_t)k_riic_off_icsr2) {
    return s->tgt_icsr2;
  }
  if (off == (uint64_t)k_riic_off_iccr2) {
    return internal_riic_target_iccr2(s);
  }
  if (off == (uint64_t)k_riic_off_icdrr) {
    return internal_riic_target_icdrr(s);
  }
  return s->reg[off]; /* reflect every other register */
}

/**
 * @brief Read a register from one modelled RIIC channel.
 * @details Read a register from one modelled riic channel; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @param[in] off Register or byte offset addressed by the operation.
 * @return The RIIC reg read result produced by the board periph RIIC model.
 * @retval value The operation-specific RIIC reg read value.
 * @pre Arguments satisfy the ranges documented for RIIC reg read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_riic_reg_read(riic_state_t* s, uint64_t off)
{
  if (s->tgt_armed) {
    return internal_riic_target_reg_read(s, off);
  }
  if (off == (uint64_t)k_riic_off_icsr2) {
    return s->icsr2;
  }
  if (off == (uint64_t)k_riic_off_iccr2) {
    /* BBSY tracks an open transaction; the request bits already read back 0. */
    uint32_t v = s->reg[off] & ~(uint32_t)k_riic_iccr2_bbsy;
    if (s->busy) {
      v |= (uint32_t)k_riic_iccr2_bbsy;
    }
    return v;
  }
  if (off == (uint64_t)k_riic_off_icdrr) {
    return internal_riic_icdrr_read(s);
  }
  return s->reg[off]; /* reflect every other register */
}

/**
 * @brief Write a register on one modelled RIIC channel.
 * @details Write a register on one modelled riic channel; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @param[in] off Register or byte offset addressed by the operation.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for RIIC reg write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_reg_write(riic_state_t* s, uint64_t off, uint32_t value)
{
  s->reg[off] = (uint8_t)(value & (uint32_t)k_riic_byte_mask);
  if (off == (uint64_t)k_riic_off_icser) {
    /* ICSER arms / disarms the own-address responder; it is the target trigger. */
    internal_riic_target_open(s, value);
    return;
  }
  if (s->tgt_armed) {
    if (off == (uint64_t)k_riic_off_icdrt) {
      internal_riic_target_icdrt(s, (uint8_t)(value & (uint32_t)k_riic_byte_mask));
    } else if (off == (uint64_t)k_riic_off_icsr2) {
      internal_riic_target_icsr2_write(s, value);
    }
    return;
  }
  if (off == (uint64_t)k_riic_off_iccr2) {
    internal_riic_iccr2_write(s, off, value);
  } else if (off == (uint64_t)k_riic_off_icdrt) {
    internal_riic_icdrt_write(s, value);
  } else if (off == (uint64_t)k_riic_off_icsr2) {
    /* Condition / fault flags are write-0-to-clear: keep only the model's
     * internal flags whose bit is still written as 1 by the driver's RMW. */
    s->icsr2 &= value;
  }
}

/**
 * @brief MMIO read inside the RIIC window: route to the addressed channel.
 * @details MMIO read inside the riic window: route to the addressed channel; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @return The RIIC read result produced by the board periph RIIC model.
 * @retval value The operation-specific RIIC read value.
 * @pre Arguments satisfy the ranges documented for RIIC read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_riic_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint32_t ch  = (uint32_t)((addr - (uint64_t)k_riic_base) / (uint64_t)k_riic_stride);
  const uint64_t off = (addr - (uint64_t)k_riic_base) % (uint64_t)k_riic_stride;
  if (off >= (uint64_t)k_riic_reg_bytes) {
    return 0U; /* outside the modelled register file */
  }
  return internal_riic_reg_read(&s_riic[ch], off);
}

/**
 * @brief MMIO write inside the RIIC window: route to the addressed channel.
 * @details MMIO write inside the riic window: route to the addressed channel; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for RIIC write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_riic_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint32_t ch  = (uint32_t)((addr - (uint64_t)k_riic_base) / (uint64_t)k_riic_stride);
  const uint64_t off = (addr - (uint64_t)k_riic_base) % (uint64_t)k_riic_stride;
  if (off >= (uint64_t)k_riic_reg_bytes) {
    return; /* outside the modelled register file */
  }
  internal_riic_reg_write(&s_riic[ch], off, (uint32_t)value);
}

/**
 * @brief Clear the RIIC channels + expander state and (re)populate the bus.
 * @details Clear the riic channels + expander state and (re)populate the bus; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for RIIC reset. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_reset(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_riic_count; i++) {
    s_riic[i] = (riic_state_t){};
  }
  priv_riic_devices_reset();
}

/**
 * @brief Print the PI4IOE line and any target-role responder activity.
 * @details Print the pi4ioe line and any target-role responder activity; this step is contained within the board periph RIIC model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for RIIC report. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph RIIC model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_riic_report(void)
{
  priv_riic_devices_report();
  for (uint32_t ch = 0U; ch < (uint32_t)k_riic_count; ch++) {
    if (s_riic[ch].tgt_cycles > 0U) {
      (void)priv_emu_io_errf("  RIIC target   : own 0x%02X serviced %u controller write+read "
                             "cycle(s), echo=%s\n",
                             (unsigned)s_riic[ch].tgt_own_addr,
                             (unsigned)s_riic[ch].tgt_cycles,
                             s_riic[ch].tgt_echo_bad ? "MISMATCH" : "OK");
    }
  }
}

/** @brief This block's descriptor (static lifetime; the core keeps the pointer). */
static const board_periph_block_t s_k_riic_block = {
  .base   = (uint64_t)k_riic_base,
  .span   = (uint64_t)k_riic_span,
  .order  = (uint32_t)k_block_order_i2c,
  .read   = internal_riic_read,
  .write  = internal_riic_write,
  .tick   = nullptr,
  .reset  = internal_riic_reset,
  .report = internal_riic_report,
  .name   = "RIIC+PI4IOE",
};

/** @brief Self-register the RIIC block before main runs (decentralized). */
[[gnu::constructor]] RA8_INTERNAL static void internal_board_periph_riic_register(void)
{
  board_periph_register_block(&s_k_riic_block);
}
