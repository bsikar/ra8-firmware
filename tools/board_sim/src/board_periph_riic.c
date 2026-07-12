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
 *     writes ACK. The register/value path is synthetic (no analog sensor) --
 *     a run-headless enabler, not a claim the board's camera streams.
 *
 * Self-registers its descriptor (address range + read / write / reset /
 * report) with the board_periph core from a file-scope constructor.
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
 * the bus; headless, board_sim IS that controller. The stimulus drives a fixed
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
static const uint8_t k_riic_tgt_payload[k_riic_tgt_payload_len] = {
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
  k_riic_dev_max    = 4U,    /**< Device-registry capacity on the bus.  */
} riic_addr_t;

/**
 * @brief PI4IOE5V6408 I/O-expander constants (ra8_board_ek_ra8d2.c U15).
 *
 * @details
 * The EK-RA8D2 v1 carries the PI4IOE5V6408 at 7-bit address 0x43 on RIIC
 * channel 1. ra8_board_io_expander_apply_project_sw4_defaults programs it with
 * three @c {register, value} writes (output / Hi-Z / direction); the device is
 * auto-incrementing, so the first transmitted byte is the register pointer and
 * the rest are payload. Reads return the addressed register's shadow value
 * (the device-id register answers 0xA0, the PI4IOE5V6408 reset default).
 */
typedef enum : uint32_t {
  k_pi4ioe_addr_7b   = 0x43U, /**< EK-RA8D2 U15 default 7-bit address.       */
  k_pi4ioe_reg_max   = 0x10U, /**< Register-file size modelled (0x00..0x0F). */
  k_pi4ioe_reg_devid = 0x01U, /**< Device-id register.                       */
  k_pi4ioe_devid_val = 0xA0U, /**< Device-id reset default the part reports. */
} pi4ioe_const_t;

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

/**
 * @brief One PI4IOE5V6408 I/O expander on the modelled I2C bus.
 *
 * @details
 * Holds the auto-incrementing register pointer (set by the first transmitted
 * byte) and a small register-file shadow the controller's writes land in. A
 * read returns the addressed register and advances the pointer, so a probe or
 * register write always ACKs and a read-back reflects what was written.
 */
typedef struct {
  uint8_t  reg_ptr;                /**< Active auto-incrementing register pointer. */
  bool     ptr_set;                /**< First byte of this transfer set the ptr.   */
  uint8_t  file[k_pi4ioe_reg_max]; /**< Register-file shadow.                      */
  uint32_t writes;                 /**< Register writes the controller landed.     */
} pi4ioe_state_t;

/**
 * @brief A device on the modelled I2C bus: 7-bit address + read/write callbacks.
 *
 * @details
 * The write callback receives each byte the controller transmits after the
 * address (register pointer, then payload). The read callback fills @p buf with
 * up to @p max response bytes for the device's current state and returns the
 * count; the bus model serves them to the controller one ICDRR read at a time.
 * @c present false means no device answers that address (the bus NACKs).
 */
typedef struct {
  bool    present;                                         /**< Slot occupied.      */
  uint8_t addr_7b;                                         /**< 7-bit address.      */
  void (*write)(void* ctx, uint8_t byte);                  /**< Controller->device. */
  uint32_t (*read)(void* ctx, uint8_t* buf, uint32_t max); /**< Device->controller. */
  void (*stop)(void* ctx);                                 /**< STOP/transfer end.  */
  void* ctx;                                               /**< Device state.       */
} riic_device_t;

/** @brief The modelled RIIC channels and the shared bus device registry. */
static riic_state_t   s_riic[k_riic_count];
static riic_device_t  s_riic_dev[k_riic_dev_max];
static pi4ioe_state_t s_pi4ioe;

/* =============================================================================
 * I2C bus device registry -- map a 7-bit address to a device model.
 * =============================================================================
 */

/** @brief Find the registered device answering @p addr_7b, or NULL. */
static riic_device_t* riic_device_find(uint8_t addr_7b)
{
  for (uint32_t i = 0U; i < (uint32_t)k_riic_dev_max; i++) {
    if (s_riic_dev[i].present && (s_riic_dev[i].addr_7b == addr_7b)) {
      return &s_riic_dev[i];
    }
  }
  return nullptr;
}

/** @brief Register a device model in the first free bus slot (drop if full). */
static void riic_device_register(uint8_t addr_7b,
                                 void (*wr)(void*, uint8_t),
                                 uint32_t (*rd)(void*, uint8_t*, uint32_t),
                                 void (*stop)(void*),
                                 void* ctx)
{
  for (uint32_t i = 0U; i < (uint32_t)k_riic_dev_max; i++) {
    if (!s_riic_dev[i].present) {
      s_riic_dev[i] = (riic_device_t){.present = true,
                                      .addr_7b = addr_7b,
                                      .write   = wr,
                                      .read    = rd,
                                      .stop    = stop,
                                      .ctx     = ctx};
      return;
    }
  }
}

/* =============================================================================
 * PI4IOE5V6408 I/O-expander device model -- auto-incrementing register file.
 * =============================================================================
 */

/** @brief Controller -> expander: first byte is the register pointer, then data. */
static void pi4ioe_write(void* ctx, uint8_t byte)
{
  pi4ioe_state_t* p = (pi4ioe_state_t*)ctx;
  if (!p->ptr_set) {
    p->reg_ptr = (uint8_t)(byte % (uint8_t)k_pi4ioe_reg_max);
    p->ptr_set = true;
    return;
  }
  /* Payload byte lands in the addressed register; the pointer auto-increments. */
  p->file[p->reg_ptr] = byte;
  p->reg_ptr          = (uint8_t)((p->reg_ptr + 1U) % (uint8_t)k_pi4ioe_reg_max);
  p->writes++;
}

/** @brief Expander -> controller: serve the addressed register, then advance. */
static uint32_t pi4ioe_read(void* ctx, uint8_t* buf, uint32_t max)
{
  pi4ioe_state_t* p = (pi4ioe_state_t*)ctx;
  if (max == 0U) {
    return 0U;
  }
  const uint32_t n = (max < (uint32_t)k_pi4ioe_reg_max) ? max : (uint32_t)k_pi4ioe_reg_max;
  for (uint32_t i = 0U; i < n; i++) {
    const uint8_t r = (uint8_t)((p->reg_ptr + i) % (uint8_t)k_pi4ioe_reg_max);
    buf[i]          = p->file[r];
  }
  return n;
}

/** @brief STOP / transfer end: reset the expander's pointer-capture latch. */
static void pi4ioe_stop(void* ctx)
{
  pi4ioe_state_t* p = (pi4ioe_state_t*)ctx;
  p->ptr_set        = false;
}

/* =============================================================================
 * OV5640 camera SCCB device -- 16-bit register pointer + chip-ID responder.
 * =============================================================================
 */

/** @brief OV5640 SCCB sensor constants (cam_ov5640.c / camera_capture). */
typedef enum : uint16_t {
  k_ov5640_addr_7b   = 0x3CU,   /**< SCCB 7-bit address (SID low; alt 0x3D). */
  k_ov5640_reg_id_hi = 0x300AU, /**< Chip-ID high-byte register.             */
  k_ov5640_reg_id_lo = 0x300BU, /**< Chip-ID low-byte register.              */
  k_ov5640_id_hi     = 0x56U,   /**< Chip-ID high byte (0x5640 >> 8).        */
  k_ov5640_id_lo     = 0x40U,   /**< Chip-ID low byte (0x5640 & 0xFF).       */
  k_ov5640_ptr_bytes = 2U,      /**< 16-bit big-endian register pointer.     */
} ov5640_const_t;

/**
 * @brief One OV5640 camera sensor on the SCCB (I2C) bus.
 *
 * @details
 * The OV5640 addresses its register file with a 16-bit big-endian pointer: each
 * transfer writes the pointer high byte then low byte, then either a data byte
 * (register write) or, after a repeated-START, one read byte (register read).
 * The model answers the chip-ID registers so the firmware's VERIFY-FIRST probe
 * reads 0x5640; every other register reads 0 and config writes are accepted.
 */
typedef struct {
  uint16_t reg_ptr;   /**< Active 16-bit register pointer (MSB-first).  */
  uint8_t  ptr_bytes; /**< Pointer bytes captured this transfer (0..2). */
  uint32_t writes;    /**< Config register writes accepted.             */
  uint32_t id_reads;  /**< Chip-ID bytes served (report).               */
} ov5640_state_t;

/** @brief The single modelled OV5640 camera sensor on RIIC channel 1. */
static ov5640_state_t s_ov5640;

/** @brief Controller -> sensor: first two bytes set the pointer, then data. */
static void ov5640_write(void* ctx, uint8_t byte)
{
  ov5640_state_t* p = (ov5640_state_t*)ctx;
  if (p->ptr_bytes < (uint8_t)k_ov5640_ptr_bytes) {
    if (p->ptr_bytes == 0U) {
      p->reg_ptr = 0U; /* Fresh pointer for this transfer. */
    }
    p->reg_ptr = (uint16_t)(((uint16_t)(p->reg_ptr << 8U)) | (uint16_t)byte);
    p->ptr_bytes++;
    return;
  }
  /* Data byte after the pointer: accept (ACK) the config write. */
  p->writes++;
}

/** @brief Sensor -> controller: serve the addressed register (chip-ID or 0). */
static uint32_t ov5640_read(void* ctx, uint8_t* buf, uint32_t max)
{
  ov5640_state_t* p = (ov5640_state_t*)ctx;
  if (max == 0U) {
    return 0U;
  }
  uint8_t v = 0U;
  if (p->reg_ptr == (uint16_t)k_ov5640_reg_id_hi) {
    v = (uint8_t)k_ov5640_id_hi;
    p->id_reads++;
  } else if (p->reg_ptr == (uint16_t)k_ov5640_reg_id_lo) {
    v = (uint8_t)k_ov5640_id_lo;
    p->id_reads++;
  }
  buf[0] = v;
  return 1U;
}

/** @brief STOP / transfer end: reset the sensor's pointer-capture latch. */
static void ov5640_stop(void* ctx)
{
  ov5640_state_t* p = (ov5640_state_t*)ctx;
  p->ptr_bytes      = 0U;
}

/* =============================================================================
 * RIIC controller model -- the transfer state machine the ra8_i2c.c polling
 * driver drives (START / RESTART / addr / write / read / STOP).
 * =============================================================================
 */

/** @brief Begin a transaction (START or repeated-START): arm the address phase. */
static void riic_open_transfer(riic_state_t* s)
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

/** @brief Close a transaction (STOP): release the bus and notify the device. */
static void riic_close_transfer(riic_state_t* s)
{
  if (s->acked) {
    riic_device_t* dev = riic_device_find(s->target_7b);
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

/** @brief Consume the address byte after a (re)START: select + ACK a device. */
static void riic_address_phase(riic_state_t* s, uint8_t address_byte)
{
  s->target_7b =
    (uint8_t)((uint32_t)address_byte >> (uint32_t)k_riic_addr_shift) & (uint8_t)k_riic_addr_mask7;
  s->reading   = ((uint32_t)address_byte & (uint32_t)k_riic_addr_rnw) != 0U;
  s->addr_done = true;

  riic_device_t* dev = riic_device_find(s->target_7b);
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

/** @brief Handle a write to ICDRT (address byte, then controller TX payload). */
static void riic_icdrt_write(riic_state_t* s, uint32_t value)
{
  const uint8_t byte = (uint8_t)(value & (uint32_t)k_riic_byte_mask);
  if (!s->addr_done) {
    riic_address_phase(s, byte);
    return;
  }
  if (!s->acked) {
    return; /* NACKed address: swallow further writes until STOP */
  }
  riic_device_t* dev = riic_device_find(s->target_7b);
  if ((dev != nullptr) && (dev->write != nullptr)) {
    dev->write(dev->ctx, byte);
  }
  /* Buffer empty again and the byte was accepted: TDRE + TEND for the next. */
  s->icsr2 |= ((uint32_t)k_riic_icsr2_tdre | (uint32_t)k_riic_icsr2_tend);
}

/** @brief Serve one ICDRR read from the staged device response. */
static uint32_t riic_icdrr_read(riic_state_t* s)
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

/** @brief Dispatch an ICCR2 write -> START / repeated-START / STOP. */
static void riic_iccr2_write(riic_state_t* s, uint64_t off, uint32_t value)
{
  if ((value & ((uint32_t)k_riic_iccr2_st | (uint32_t)k_riic_iccr2_rs)) != 0U) {
    riic_open_transfer(s);
    /* ST / RS auto-clear once the condition is issued; the driver spins on RS
     * reading 0 after a repeated-START, so drop both request bits. */
    s->reg[off] = (uint8_t)(value & ~((uint32_t)k_riic_iccr2_st | (uint32_t)k_riic_iccr2_rs));
  } else if ((value & (uint32_t)k_riic_iccr2_sp) != 0U) {
    riic_close_transfer(s);
    s->reg[off] = (uint8_t)(value & ~(uint32_t)k_riic_iccr2_sp);
  }
}

/* =============================================================================
 * RIIC target (peripheral) model -- board_sim is the synthetic external
 * controller, driving a write-then-read script at the firmware's own address so
 * the real ra8_i2c_peripheral_* driver (receive / transmit / dispatch) runs.
 * =============================================================================
 */

/** @brief Arm the write phase: present the address-match + receive-ready state. */
static void riic_target_begin_write(riic_state_t* s)
{
  s->tgt_phase  = (uint8_t)k_riic_tgt_write;
  s->tgt_wr_pos = 0U;
  s->tgt_icsr2  = (uint32_t)k_riic_icsr2_rdrf; /* address + first data byte ready */
}

/** @brief Arm the read phase: present transmit-ready + transmit-end for the echo. */
static void riic_target_begin_read(riic_state_t* s)
{
  s->tgt_phase  = (uint8_t)k_riic_tgt_read;
  s->tgt_rd_len = 0U;
  s->tgt_icsr2  = (uint32_t)k_riic_icsr2_tdre | (uint32_t)k_riic_icsr2_tend;
}

/** @brief Close one cycle: verify the echo, then loop the script or quiesce the bus. */
static void riic_target_complete_read(riic_state_t* s)
{
  bool ok = (s->tgt_rd_len == (uint32_t)k_riic_tgt_payload_len);
  for (uint32_t i = 0U; i < (uint32_t)k_riic_tgt_payload_len; i++) {
    if (s->tgt_rd[i] != k_riic_tgt_payload[i]) {
      ok = false;
    }
  }
  if (!ok) {
    s->tgt_echo_bad = true;
  }
  s->tgt_cycles++;
  if (s->tgt_cycles < (uint32_t)k_riic_tgt_cycles) {
    riic_target_begin_write(s);
  } else {
    s->tgt_phase = (uint8_t)k_riic_tgt_done;
    s->tgt_icsr2 = 0U; /* no match, no STOP: dispatch reports no event forever. */
  }
}

/** @brief (Dis)arm target mode on an ICSER write; latch the firmware's own address. */
static void riic_target_open(riic_state_t* s, uint32_t icser)
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
  riic_target_begin_write(s);
}

/** @brief ICSR1 read in target mode: assert the own-address match while active. */
static uint32_t riic_target_icsr1(const riic_state_t* s)
{
  if (s->tgt_phase == (uint8_t)k_riic_tgt_write) {
    return (uint32_t)k_riic_icsr1_aas0;
  }
  if (s->tgt_phase == (uint8_t)k_riic_tgt_read) {
    return (uint32_t)k_riic_icsr1_aas0;
  }
  return 0U;
}

/** @brief ICCR2 read in target mode: assert TRS only while the controller reads. */
static uint32_t riic_target_iccr2(const riic_state_t* s)
{
  uint32_t v = (uint32_t)s->reg[k_riic_off_iccr2] & ~(uint32_t)k_riic_iccr2_trs;
  if (s->tgt_phase == (uint8_t)k_riic_tgt_read) {
    v |= (uint32_t)k_riic_iccr2_trs;
  }
  return v;
}

/** @brief ICDRR read in target mode: serve the address byte, then the payload. */
static uint32_t riic_target_icdrr(riic_state_t* s)
{
  if (s->tgt_phase != (uint8_t)k_riic_tgt_write) {
    return 0U; /* read-phase SCL-release dummy read / idle: no receive data. */
  }
  uint8_t b = 0U;
  if (s->tgt_wr_pos == 0U) {
    /* Step 1: the matched address byte the driver dummy-reads to discard. */
    b = (uint8_t)((uint32_t)s->tgt_own_addr << (uint32_t)k_riic_addr_shift);
  } else if (s->tgt_wr_pos <= (uint32_t)k_riic_tgt_payload_len) {
    b = k_riic_tgt_payload[s->tgt_wr_pos - 1U];
  }
  s->tgt_wr_pos++;
  if (s->tgt_wr_pos > (uint32_t)k_riic_tgt_payload_len) {
    s->tgt_icsr2 = (uint32_t)k_riic_icsr2_stop; /* all served: STOP, drop RDRF. */
  } else {
    s->tgt_icsr2 = (uint32_t)k_riic_icsr2_rdrf;
  }
  return (uint32_t)b;
}

/** @brief ICDRT write in target mode: capture the firmware's echo byte. */
static void riic_target_icdrt(riic_state_t* s, uint8_t byte)
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

/** @brief ICSR2 W0C write in target mode: advance the write->read->done script. */
static void riic_target_icsr2_write(riic_state_t* s, uint32_t value)
{
  s->tgt_icsr2 &= value; /* condition flags are write-0-to-clear. */
  if (s->tgt_phase == (uint8_t)k_riic_tgt_write) {
    riic_target_begin_read(s); /* receive() cleared STOP -> controller now reads. */
  } else if (s->tgt_phase == (uint8_t)k_riic_tgt_read) {
    riic_target_complete_read(s); /* finish_tx cleared NACKF/STOP -> cycle done. */
  }
}

/** @brief Register read while target mode is armed (own-address responder path). */
static uint64_t riic_target_reg_read(riic_state_t* s, uint64_t off)
{
  if (off == (uint64_t)k_riic_off_icsr1) {
    return riic_target_icsr1(s);
  }
  if (off == (uint64_t)k_riic_off_icsr2) {
    return s->tgt_icsr2;
  }
  if (off == (uint64_t)k_riic_off_iccr2) {
    return riic_target_iccr2(s);
  }
  if (off == (uint64_t)k_riic_off_icdrr) {
    return riic_target_icdrr(s);
  }
  return s->reg[off]; /* reflect every other register */
}

/** @brief Read a register from one modelled RIIC channel. */
static uint64_t riic_reg_read(riic_state_t* s, uint64_t off)
{
  if (s->tgt_armed) {
    return riic_target_reg_read(s, off);
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
    return riic_icdrr_read(s);
  }
  return s->reg[off]; /* reflect every other register */
}

/** @brief Write a register on one modelled RIIC channel. */
static void riic_reg_write(riic_state_t* s, uint64_t off, uint32_t value)
{
  s->reg[off] = (uint8_t)(value & (uint32_t)k_riic_byte_mask);
  if (off == (uint64_t)k_riic_off_icser) {
    /* ICSER arms / disarms the own-address responder; it is the target trigger. */
    riic_target_open(s, value);
    return;
  }
  if (s->tgt_armed) {
    if (off == (uint64_t)k_riic_off_icdrt) {
      riic_target_icdrt(s, (uint8_t)(value & (uint32_t)k_riic_byte_mask));
    } else if (off == (uint64_t)k_riic_off_icsr2) {
      riic_target_icsr2_write(s, value);
    }
    return;
  }
  if (off == (uint64_t)k_riic_off_iccr2) {
    riic_iccr2_write(s, off, value);
  } else if (off == (uint64_t)k_riic_off_icdrt) {
    riic_icdrt_write(s, value);
  } else if (off == (uint64_t)k_riic_off_icsr2) {
    /* Condition / fault flags are write-0-to-clear: keep only the model's
     * internal flags whose bit is still written as 1 by the driver's RMW. */
    s->icsr2 &= value;
  }
}

/** @brief MMIO read inside the RIIC window: route to the addressed channel. */
static uint64_t riic_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint32_t ch  = (uint32_t)((addr - (uint64_t)k_riic_base) / (uint64_t)k_riic_stride);
  const uint64_t off = (addr - (uint64_t)k_riic_base) % (uint64_t)k_riic_stride;
  if (off >= (uint64_t)k_riic_reg_bytes) {
    return 0U; /* outside the modelled register file */
  }
  return riic_reg_read(&s_riic[ch], off);
}

/** @brief MMIO write inside the RIIC window: route to the addressed channel. */
static void riic_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint32_t ch  = (uint32_t)((addr - (uint64_t)k_riic_base) / (uint64_t)k_riic_stride);
  const uint64_t off = (addr - (uint64_t)k_riic_base) % (uint64_t)k_riic_stride;
  if (off >= (uint64_t)k_riic_reg_bytes) {
    return; /* outside the modelled register file */
  }
  riic_reg_write(&s_riic[ch], off, (uint32_t)value);
}

/** @brief Clear the RIIC channels + expander state and (re)populate the bus. */
static void riic_reset(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_riic_count; i++) {
    s_riic[i] = (riic_state_t){};
  }
  s_pi4ioe = (pi4ioe_state_t){};
  /* Seed the expander's device-id register so a read-back of the part probes
   * its reset default, matching the PI4IOE5V6408. */
  s_pi4ioe.file[k_pi4ioe_reg_devid] = (uint8_t)k_pi4ioe_devid_val;
  /* Populate the modelled I2C bus: the EK-RA8D2 carrier's PI4IOE5V6408 I/O
   * expander (U15) answers at its 7-bit address, so the firmware's real
   * ra8_i2c_scan / ra8_i2c_write -> RIIC path ACKs instead of needing a stub. */
  for (uint32_t i = 0U; i < (uint32_t)k_riic_dev_max; i++) {
    s_riic_dev[i] = (riic_device_t){};
  }
  riic_device_register((uint8_t)k_pi4ioe_addr_7b,
                       pi4ioe_write,
                       pi4ioe_read,
                       pi4ioe_stop,
                       &s_pi4ioe);
  /* The camera_capture example's OV5640 sensor answers on channel 1 at SCCB
     0x3C; its chip-ID registers report 0x5640 so the sensor probe passes and
     the CEU capture path (board_periph_ceu.c) runs headless. */
  s_ov5640 = (ov5640_state_t){};
  riic_device_register((uint8_t)k_ov5640_addr_7b,
                       ov5640_write,
                       ov5640_read,
                       ov5640_stop,
                       &s_ov5640);
}

/** @brief Print the PI4IOE line and any target-role responder activity. */
static void riic_report(void)
{
  if (s_pi4ioe.writes > 0U) {
    (void)fprintf(stderr,
                  "  RIIC PI4IOE   : U15 expander 0x%02X acked %u register write(s)\n",
                  (unsigned)k_pi4ioe_addr_7b,
                  s_pi4ioe.writes);
  }
  if ((s_ov5640.id_reads > 0U) || (s_ov5640.writes > 0U)) {
    (void)fprintf(stderr,
                  "  RIIC OV5640   : SCCB 0x%02X chip-id 0x5640 (%u id read(s), %u cfg write(s))\n",
                  (unsigned)k_ov5640_addr_7b,
                  s_ov5640.id_reads,
                  s_ov5640.writes);
  }
  for (uint32_t ch = 0U; ch < (uint32_t)k_riic_count; ch++) {
    if (s_riic[ch].tgt_cycles > 0U) {
      (void)fprintf(stderr,
                    "  RIIC target   : own 0x%02X serviced %u controller write+read "
                    "cycle(s), echo=%s\n",
                    (unsigned)s_riic[ch].tgt_own_addr,
                    (unsigned)s_riic[ch].tgt_cycles,
                    s_riic[ch].tgt_echo_bad ? "MISMATCH" : "OK");
    }
  }
}

/** @brief This block's descriptor (static lifetime; the core keeps the pointer). */
static const board_periph_block_t k_riic_block = {
  .base   = (uint64_t)k_riic_base,
  .span   = (uint64_t)k_riic_span,
  .order  = (uint32_t)k_block_order_i2c,
  .read   = riic_read,
  .write  = riic_write,
  .tick   = nullptr,
  .reset  = riic_reset,
  .report = riic_report,
  .name   = "RIIC+PI4IOE",
};

/** @brief Self-register the RIIC block before main runs (decentralized). */
[[gnu::constructor]] static void board_periph_riic_register(void)
{
  board_periph_register_block(&k_riic_block);
}
