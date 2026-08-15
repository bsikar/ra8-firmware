/**
 * @file board_periph_i2c.c
 * @brief I3C-in-I2C-mode (IIC_B) controller + GT911 touch + LSM6DSO IMU models
 *        plus an external-controller stimulus for the firmware in target mode
 *
 * @details
 * Models the RA8D2 I3C channel 0 driven in legacy I2C mode (PRTS.PRTMD=1) --
 * the IIC_B controller the ra8_i3c_i2c.c polling driver drives -- plus the GoodIX
 * GT911 touch controller that answers on the modelled bus, so the firmware's
 * real ra8_touch -> ra8_i3c_transfer -> GT911 path returns touch data without a
 * function-level stub (and the i3c_loopback example drives the same block as a
 * plain I2C controller).
 *
 * Four cooperating pieces live here:
 *  1. The IIC_B controller transfer state machine (START / address / write /
 *     read / STOP) the driver clocks through CNDCTL / NTDTBP0 / BST / NTST.
 *  2. A small I2C bus device registry mapping a 7-bit address to read / write /
 *     stop callbacks.
 *  3. The GT911 device: a 16-bit register pointer, the PRODUCT_ID the driver
 *     probes, and one armed contact a status + point0 read delivers exactly once.
 *  4. A target-mode (peripheral) stimulus: when the firmware instead programmes
 *     its own MSDVAD address and waits to be addressed (i3c_i2c_peripheral_demo),
 *     the model plays the EXTERNAL controller -- writing a byte the firmware
 *     drains (NTST.RDBFF0) then reading the firmware's one-byte echo
 *     (NTST.TDBEF0), so the responder driver is exercised end to end with no
 *     wired-up bus. See ::i3c_periph_phase_t.
 *
 * Self-registers its descriptor (address range + read / write / reset / report)
 * with the board_periph core from a file-scope constructor.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_console.h"
#include "board_periph.h"
#include "board_periph_block.h"
#include "board_periph_i2c_internal.h"
#include "emu_host_io_internal.h"

/**
 * @brief I3C-in-I2C-mode (IIC_B) block geometry (ra8_i3c_i2c_regs.h).
 *
 * @details
 * The RA8D2 has one I3C channel at 0x4035F000. ra8_touch drives the GoodIX GT911
 * touch controller through this peripheral in legacy I2C mode (PRTS.PRTMD=1),
 * and the i3c_loopback example drives the same block as an I2C controller. The
 * polling driver (libs/ra8_hal/src/ra8_i3c_i2c.c) only touches the registers named
 * here; everything else in the window reflects writes via the shadow. Offsets
 * match the @c r_i3c_i2c_regs_t struct in ra8_i3c_i2c_regs.h.
 */
typedef enum : uint64_t {
  k_i3c_base        = 0x4035F000UL,  /**< I3C0 base (== IIC_B channel 0).    */
  k_i3c_span        = 0x214UL,       /**< Through BCST at +0x210.            */
  k_i3c_off_msdvad  = 0x018UL,       /**< MSDVAD own/target device address.  */
  k_i3c_off_cndctl  = 0x140UL,       /**< CNDCTL START/RESTART/STOP request. */
  k_i3c_off_ntdtbp0 = 0x158UL,       /**< NTDTBP0 transfer data buffer port. */
  k_i3c_off_bst     = 0x1D0UL,       /**< BST bus status (W0C flags).        */
  k_i3c_off_ntst    = 0x1E0UL,       /**< NTST normal-transfer status.       */
  k_i3c_off_bcst    = 0x210UL,       /**< BCST bus condition status (BFREF). */
  k_i3c_reg_words   = 0x214UL / 4UL, /**< Shadow word count for the window.  */
} i3c_map_t;

/** @brief CNDCTL condition-request bits (ra8_i3c_i2c_regs.h). */
typedef enum : uint32_t {
  k_i3c_cndctl_stcnd = 0x00000001U, /**< STCND issue START.          */
  k_i3c_cndctl_srcnd = 0x00000002U, /**< SRCND issue repeated-START. */
  k_i3c_cndctl_spcnd = 0x00000004U, /**< SPCND issue STOP.           */
} i3c_cndctl_bit_t;

/** @brief BST (Bus Status) flags the polling driver observes / clears (W0C). */
typedef enum : uint32_t {
  k_i3c_bst_stcnddf = 0x00000001U, /**< START-condition detected (bit 0).   */
  k_i3c_bst_spcnddf = 0x00000002U, /**< STOP-condition detected (bit 1).    */
  k_i3c_bst_nackdf  = 0x00000010U, /**< NACK detected (bit 4).              */
  k_i3c_bst_tendf   = 0x00000100U, /**< Transfer-end / address ACK (bit 8). */
  k_i3c_bst_alf     = 0x00010000U, /**< Arbitration lost (bit 16).          */
  k_i3c_bst_todf    = 0x00100000U, /**< Timeout detected (bit 20).          */
} i3c_bst_bit_t;

/** @brief NTST (Normal Transfer Status) flags. */
typedef enum : uint32_t {
  k_i3c_ntst_tdbef0 = 0x00000001U, /**< TX data-buffer empty (bit 0). */
  k_i3c_ntst_rdbff0 = 0x00000002U, /**< RX data-buffer full (bit 1).  */
} i3c_ntst_bit_t;

/** @brief BCST (Bus Condition Status) flags. */
typedef enum : uint32_t {
  k_i3c_bcst_bfref = 0x00000001U, /**< Bus-free flag (bit 0): 1 == idle. */
} i3c_bcst_bit_t;

/** @brief I2C address-byte layout on the wire (addr<<1 | R/W). */
typedef enum : uint32_t {
  k_i3c_addr_shift = 1U,    /**< 7-bit address occupies bits [7:1].    */
  k_i3c_addr_rnw   = 0x01U, /**< LSB: 1 == read, 0 == write.           */
  k_i3c_addr_mask7 = 0x7FU, /**< 7-bit target-address mask.            */
  k_i3c_byte_mask  = 0xFFU, /**< One data byte.                        */
  k_i3c_dev_rx_max = 64U,   /**< Per-read device response staging cap. */
  k_i3c_dev_max    = 4U,    /**< Device-registry capacity on the bus.  */
} i3c_addr_t;

/**
 * @brief Target (peripheral) mode transaction phase.
 *
 * @details
 * When the firmware brings IIC_B up as an addressed TARGET (it programmes its
 * own address into MSDVAD and never issues a controller START via CNDCTL), the
 * model plays the role of the EXTERNAL controller driving it. Each synthetic
 * transaction is a controller write (the firmware sees NTST.RDBFF0 and drains a
 * byte) followed by a controller read (the firmware sees NTST.TDBEF0 and pushes
 * its one-byte echo). The phase advances on the firmware's own NTDTBP0 accesses,
 * so the handshake is paced by the firmware's polling loop -- exactly as a real
 * controller's clocking would pace it.
 */
typedef enum : uint8_t {
  k_i3c_periph_idle     = 0U, /**< No target transaction armed.            */
  k_i3c_periph_rx_armed = 1U, /**< Controller wrote a byte: RDBFF0 raised. */
  k_i3c_periph_tx_armed = 2U, /**< Controller is reading: TDBEF0 raised.   */
} i3c_periph_phase_t;

/** @brief Synthetic controller-write byte stream for the target-mode model. */
typedef enum : uint32_t {
  k_i3c_periph_seed = 0xA5U, /**< First byte the external controller writes. */
  k_i3c_periph_step = 0x11U, /**< Per-transaction increment of that byte.    */
} i3c_periph_stim_t;

/**
 * @brief GoodIX GT911 protocol constants (ra8_touch_gt911_regs.h + ra8_touch.c).
 *
 * @details
 * The GT911 is addressed with a 16-bit big-endian register pointer written
 * first, then read N bytes from that pointer (after a repeated-START). The touch
 * driver reads PRODUCT_ID to confirm the part is alive on open, then each frame
 * reads the STATUS byte (bit7 buffer-ready, bits[3:0] point count) and, when a
 * point is present, the 8-byte POINT[0] record. Writing 0 to STATUS acks the
 * frame. The point record packs x/y little-endian; ra8_touch decodes x at byte 0
 * and y at byte 2 of the public point type.
 */
typedef enum : uint16_t {
  k_gt911_reg_command = 0x8040U, /**< Command register (sleep/wake/ack).   */
  k_gt911_reg_product = 0x8140U, /**< 4-byte ASCII product id "911\0".     */
  k_gt911_reg_status  = 0x814EU, /**< Status: bit7 ready, bits[3:0] count. */
  k_gt911_reg_point0  = 0x814FU, /**< First 8-byte per-point record.       */
} gt911_reg_t;

/** @brief GT911 magic byte values + record geometry. */
typedef enum : uint32_t {
  k_gt911_addr_7b      = 0x5DU, /**< EK-RA8D2 carrier GT911 default address.        */
  k_gt911_id0          = 0x39U, /**< '9' -- first product-id byte ra8_touch checks. */
  k_gt911_id1          = 0x31U, /**< '1'.                                           */
  k_gt911_id2          = 0x31U, /**< '1'.                                           */
  k_gt911_id3          = 0x00U, /**< NUL terminator.                                */
  k_gt911_status_ready = 0x80U, /**< Buffer-ready (bit 7).                          */
  k_gt911_status_one   = 0x01U, /**< One active contact in bits[3:0].               */
  k_gt911_id_bytes     = 4U,    /**< PRODUCT_ID payload length.                     */
  k_gt911_point_bytes  = 8U,    /**< Bytes per per-point record.                    */
  k_gt911_ptr_bytes    = 2U,    /**< 16-bit register-pointer width.                 */
  k_gt911_press        = 0x20U, /**< Synthetic contact pressure (size lsb).         */
  k_gt911_seq_max      = 8U,    /**< Injected raw-point sequence FIFO depth.        */
} gt911_const_t;

/** @brief Byte offsets inside one 8-byte GT911 point record (ra8_touch.c). */
typedef enum : uint32_t {
  k_gt911_pt_track  = 0U, /**< track_id.            */
  k_gt911_pt_x_lsb  = 1U, /**< X low byte.          */
  k_gt911_pt_x_msb  = 2U, /**< X high byte.         */
  k_gt911_pt_y_lsb  = 3U, /**< Y low byte.          */
  k_gt911_pt_y_msb  = 4U, /**< Y high byte.         */
  k_gt911_pt_sz_lsb = 5U, /**< size low (pressure). */
} gt911_pt_off_t;

/**
 * @brief One I3C-in-I2C-mode channel: register shadow + a transfer state machine.
 *
 * @details
 * The model owns the registers the IIC_B polling driver actually touches; the
 * rest of the window reflects writes through @c reg. The transfer fields track
 * one controller transaction: @c busy spans START..STOP (BCST.BFREF reports the
 * complement), @c addr_done latches once the address byte after a (re)START has
 * selected a device, @c target_7b / @c reading record that selection, and the
 * @c rx staging buffer holds the bytes the addressed device produced for the
 * current read (drained one per NTDTBP0 read, after the FSP "dummy" first read).
 */
typedef struct {
  uint32_t reg[k_i3c_reg_words]; /**< Reflect-on-read register shadow.             */
  bool     busy;                 /**< True between START and STOP.                 */
  bool     addr_done;            /**< Address phase of the current (re)START done. */
  bool     acked;                /**< The addressed target ACKed.                  */
  bool     reading;              /**< Current transfer direction is read.          */
  uint8_t  target_7b;            /**< 7-bit address selected this transfer.        */
  uint32_t ntst;                 /**< NTST flags (TDBEF0 / RDBFF0).                */
  uint32_t bst;                  /**< BST flags (NACKDF / TENDF / ...).            */
  uint8_t  rx[k_i3c_dev_rx_max]; /**< Staged device response for a read.           */
  uint32_t rx_len;               /**< Valid bytes in @c rx.                        */
  uint32_t rx_pos;               /**< Next byte index served from @c rx.           */
  bool     rx_primed;            /**< The FSP dummy first read was consumed.       */
  /* Target (peripheral) mode: the model is the EXTERNAL controller. */
  bool     periph_mode;     /**< IIC_B opened as an addressed target (MSDVAD set). */
  uint8_t  periph_addr_7b;  /**< Own 7-bit address the firmware claimed in MSDVAD. */
  uint8_t  periph_phase;    /**< ::i3c_periph_phase_t -- target-xfer position.     */
  uint8_t  periph_rx_byte;  /**< Synthetic controller-write byte for this xfer.    */
  uint32_t periph_xfers;    /**< Completed controller write+read target xfers.     */
  bool     periph_echo_bad; /**< A firmware echo did not match the written byte.   */
} i3c_state_t;

/**
 * @brief One GoodIX GT911 touch device on the modelled I2C bus.
 *
 * @details
 * Holds the current 16-bit register pointer (set MSB-first by the write phase)
 * and one armed contact. A status read reports buffer-ready + one point while a
 * contact is armed; the point0 read returns its x/y and clears the contact
 * (counted in @c reported), so a tap is delivered exactly once -- matching the
 * real GT911, which drops the frame once the controller drains and acks it.
 *
 * The @c seq_* fields hold an optional injected raw-point SEQUENCE (loaded via
 * ::board_periph_touch_seq_push). While the queue is non-empty a status read
 * auto-arms the head point, so a multi-tap flow (e.g. touch_cal's five-target
 * calibration) drains one distinct raw point per frame with no per-chunk
 * re-arm -- the model latches the next queued touch exactly as the real GT911
 * latches the next physical touch after the current frame is acked.
 */
typedef struct {
  uint16_t reg_ptr;                /**< Active 16-bit register pointer.           */
  uint8_t  ptr_bytes;              /**< Pointer bytes captured this write (0..2). */
  bool     click_pending;          /**< A contact is armed and unread.            */
  uint16_t click_x;                /**< Armed contact X.                          */
  uint16_t click_y;                /**< Armed contact Y.                          */
  uint32_t reported;               /**< Contacts the firmware has drained.        */
  uint16_t seq_x[k_gt911_seq_max]; /**< Queued raw-point X sequence.              */
  uint16_t seq_y[k_gt911_seq_max]; /**< Queued raw-point Y sequence.              */
  uint8_t  seq_len;                /**< Points queued in the FIFO.                */
  uint8_t  seq_pos;                /**< Next queued point to arm.                 */
} gt911_state_t;

/* =============================================================================
 * I2C bus device registry -- map a 7-bit address to a device model.
 * =============================================================================
 */

/**
 * @brief A device on the modelled I2C bus: 7-bit address + read/write callbacks.
 *
 * @details
 * The write callback receives each byte the controller transmits after the
 * address (register pointers, then payload). The read callback fills @p buf with
 * up to @p max response bytes for the device's current state and returns the
 * count; the bus model serves them to the controller one NTDTBP0 read at a time.
 * @c present false means no device answers that address (the bus NACKs).
 */
typedef struct {
  bool    present;                                         /**< Slot occupied.      */
  uint8_t addr_7b;                                         /**< 7-bit address.      */
  void (*write)(void* ctx, uint8_t byte);                  /**< Controller->device. */
  uint32_t (*read)(void* ctx, uint8_t* buf, uint32_t max); /**< Device->controller. */
  void (*stop)(void* ctx);                                 /**< STOP/transfer end.  */
  void* ctx;                                               /**< Device state.       */
} i2c_device_t;

static i3c_state_t   s_i3c;
static i2c_device_t  s_i2c_dev[k_i3c_dev_max];
static gt911_state_t s_gt911;

/** @brief Find the registered device answering @p addr_7b, or NULL. */
RA8_INTERNAL static i2c_device_t* internal_i2c_device_find(uint8_t addr_7b)
{
  for (uint32_t i = 0U; i < (uint32_t)k_i3c_dev_max; i++) {
    if (s_i2c_dev[i].present && (s_i2c_dev[i].addr_7b == addr_7b)) {
      return &s_i2c_dev[i];
    }
  }
  return nullptr;
}

/** @brief Register a device model in the first free bus slot (drop if full). */
void priv_i2c_device_register(uint8_t addr_7b,
                              void (*wr)(void*, uint8_t),
                              uint32_t (*rd)(void*, uint8_t*, uint32_t),
                              void (*stop)(void*),
                              void* ctx)
{
  for (uint32_t i = 0U; i < (uint32_t)k_i3c_dev_max; i++) {
    if (!s_i2c_dev[i].present) {
      s_i2c_dev[i] = (i2c_device_t){.present = true,
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
 * GT911 touch device model -- 16-bit register pointer, product id, touch frame.
 * =============================================================================
 */

void board_periph_touch_inject(uint16_t x, uint16_t y)
{
  s_gt911.click_x       = x;
  s_gt911.click_y       = y;
  s_gt911.click_pending = true;
}

void board_periph_touch_seq_reset(void)
{
  s_gt911.seq_len       = 0U;
  s_gt911.seq_pos       = 0U;
  s_gt911.click_pending = false;
}

bool board_periph_touch_seq_push(uint16_t x, uint16_t y)
{
  if (s_gt911.seq_len >= (uint8_t)k_gt911_seq_max) {
    return false;
  }
  s_gt911.seq_x[s_gt911.seq_len] = x;
  s_gt911.seq_y[s_gt911.seq_len] = y;
  s_gt911.seq_len++;
  return true;
}

/**
 * @brief Arm the head of the injected sequence FIFO when nothing is pending.
 * @details Arm the head of the injected sequence fifo when nothing is pending; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in,out] g G state or storage updated in place by the operation.
 * @pre Arguments satisfy the ranges documented for gt911 arm next from seq. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gt911_arm_next_from_seq(gt911_state_t* g)
{
  if (!g->click_pending && (g->seq_pos < g->seq_len)) {
    g->click_x       = g->seq_x[g->seq_pos];
    g->click_y       = g->seq_y[g->seq_pos];
    g->click_pending = true;
    g->seq_pos++;
  }
}

uint32_t board_periph_touch_reported(void)
{
  return s_gt911.reported;
}

bool board_periph_touch_last(uint16_t* x, uint16_t* y)
{
  if ((x == nullptr) || (y == nullptr) || (s_gt911.reported == 0U)) {
    return false;
  }
  *x = s_gt911.click_x;
  *y = s_gt911.click_y;
  return true;
}

/**
 * @brief Controller -> GT911: pointer bytes first (MSB,LSB), then payload.
 * @details Controller -> gt911: pointer bytes first (msb,lsb), then payload; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in,out] ctx Opaque callback context identifying module-owned device state.
 * @param[in] byte One data byte received from or sent to the emulated interface.
 * @pre Arguments satisfy the ranges documented for gt911 write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gt911_write(void* ctx, uint8_t byte)
{
  gt911_state_t* g = (gt911_state_t*)ctx;
  if (g->ptr_bytes < (uint8_t)k_gt911_ptr_bytes) {
    /* 16-bit pointer arrives MSB first: byte 0 is the high byte, byte 1 the low. */
    g->reg_ptr = (uint16_t)(((uint32_t)g->reg_ptr << 8U) | (uint32_t)byte);
    g->ptr_bytes++;
    return;
  }
  /* Payload after the pointer: a 0 written to STATUS acks the current frame so
   * the IC can latch the next one (ra8_touch's priv_ack_frame). */
  if ((g->reg_ptr == (uint16_t)k_gt911_reg_status) && (byte == 0U)) {
    g->click_pending = false;
  }
  if (g->reg_ptr == (uint16_t)k_gt911_reg_command) {
    /* Wake / soft-reset commands are accepted (no observable side effect). */
  }
}

/**
 * @brief Fill @p buf with the GT911 status byte for the current frame.
 * @details Fill @p buf with the gt911 status byte for the current frame; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in,out] g G state or storage updated in place by the operation.
 * @param[in,out] buf Bounded byte buffer read or updated by the operation.
 * @return The gt911 read status result produced by the board periph I2C model.
 * @retval value The operation-specific gt911 read status value.
 * @pre Arguments satisfy the ranges documented for gt911 read status. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_gt911_read_status(gt911_state_t* g, uint8_t* buf)
{
  /* Auto-arm the next queued raw point (multi-tap sequence path) so a status
   * read reports buffer-ready while the FIFO is non-empty. A single-shot
   * --click (seq_len == 0) is unaffected: the arm is a no-op and the directly
   * injected contact still reports ready on its own. */
  internal_gt911_arm_next_from_seq(g);
  buf[0] = g->click_pending ? (uint8_t)(k_gt911_status_ready | k_gt911_status_one) : 0U;
  return 1U;
}

/**
 * @brief Fill @p buf with one GT911 point0 record for the armed contact.
 * @details Fill @p buf with one gt911 point0 record for the armed contact; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in,out] g G state or storage updated in place by the operation.
 * @param[in,out] buf Bounded byte buffer read or updated by the operation.
 * @param[in] max Capacity of the destination or operation in elements.
 * @return The gt911 read point0 result produced by the board periph I2C model.
 * @retval value The operation-specific gt911 read point0 value.
 * @pre Arguments satisfy the ranges documented for gt911 read point0. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t
internal_gt911_read_point0(gt911_state_t* g, uint8_t* buf, uint32_t max)
{
  if (max < (uint32_t)k_gt911_point_bytes) {
    return 0U;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_gt911_point_bytes; i++) {
    buf[i] = 0U;
  }
  buf[k_gt911_pt_track]  = 0U;
  buf[k_gt911_pt_x_lsb]  = (uint8_t)(g->click_x & (uint16_t)k_i3c_byte_mask);
  buf[k_gt911_pt_x_msb]  = (uint8_t)((uint32_t)g->click_x >> 8U);
  buf[k_gt911_pt_y_lsb]  = (uint8_t)(g->click_y & (uint16_t)k_i3c_byte_mask);
  buf[k_gt911_pt_y_msb]  = (uint8_t)((uint32_t)g->click_y >> 8U);
  buf[k_gt911_pt_sz_lsb] = (uint8_t)k_gt911_press;
  /* The coordinate has now been delivered to the firmware -- count it and drop
   * the contact so the next frame reads "not ready", exactly as the GT911 does
   * once a tap is drained. */
  g->click_pending = false;
  g->reported++;
  return (uint32_t)k_gt911_point_bytes;
}

/**
 * @brief GT911 -> controller: answer a read at the current register pointer.
 * @details Gt911 -> controller: answer a read at the current register pointer; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in,out] ctx Opaque callback context identifying module-owned device state.
 * @param[in,out] buf Bounded byte buffer read or updated by the operation.
 * @param[in] max Capacity of the destination or operation in elements.
 * @return The gt911 read result produced by the board periph I2C model.
 * @retval value The operation-specific gt911 read value.
 * @pre Arguments satisfy the ranges documented for gt911 read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_gt911_read(void* ctx, uint8_t* buf, uint32_t max)
{
  gt911_state_t* g = (gt911_state_t*)ctx;
  if (g->reg_ptr == (uint16_t)k_gt911_reg_product) {
    const uint8_t  id[k_gt911_id_bytes] = {(uint8_t)k_gt911_id0,
                                           (uint8_t)k_gt911_id1,
                                           (uint8_t)k_gt911_id2,
                                           (uint8_t)k_gt911_id3};
    const uint32_t n = (max < (uint32_t)k_gt911_id_bytes) ? max : (uint32_t)k_gt911_id_bytes;
    for (uint32_t i = 0U; i < n; i++) {
      buf[i] = id[i];
    }
    return n;
  }
  if (g->reg_ptr == (uint16_t)k_gt911_reg_status) {
    return internal_gt911_read_status(g, buf);
  }
  if (g->reg_ptr == (uint16_t)k_gt911_reg_point0) {
    return internal_gt911_read_point0(g, buf, max);
  }
  return 0U;
}

/**
 * @brief STOP / transfer end: reset the GT911 pointer-capture state.
 * @details Stop / transfer end: reset the gt911 pointer-capture state; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in,out] ctx Opaque callback context identifying module-owned device state.
 * @pre Arguments satisfy the ranges documented for gt911 stop. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gt911_stop(void* ctx)
{
  gt911_state_t* g = (gt911_state_t*)ctx;
  g->ptr_bytes     = 0U;
}

/* =============================================================================
 * LSM6DSO IMU device model -- 8-bit auto-incrementing register file.
 * =============================================================================
 */

/* =============================================================================
 * I3C-in-I2C-mode (IIC_B) controller model -- the transfer state machine the
 * ra8_i3c_i2c.c polling driver drives (START / addr / write / read / STOP).
 * =============================================================================
 */

/**
 * @brief Index of the I3C shadow word for @p off (already range-checked).
 * @details Index of the i3c shadow word for @p off (already range-checked); this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in] off Register or byte offset addressed by the operation.
 * @return The I3C word result produced by the board periph I2C model.
 * @retval value The operation-specific I3C word value.
 * @pre Arguments satisfy the ranges documented for I3C word. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_i3c_word(uint64_t off)
{
  return (uint32_t)(off / 4U);
}

/**
 * @brief Begin a transaction (START or repeated-START): arm the address phase.
 * @details Begin a transaction (start or repeated-start): arm the address phase; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for I3C open transfer. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_i3c_open_transfer(void)
{
  s_i3c.busy      = true;
  s_i3c.addr_done = false;
  s_i3c.acked     = false;
  s_i3c.reading   = false;
  s_i3c.rx_len    = 0U;
  s_i3c.rx_pos    = 0U;
  s_i3c.rx_primed = false;
  /* TX buffer is empty so the driver can write the address byte; clear stale
   * NACK/TEND so the new address phase reports its own outcome. */
  s_i3c.ntst = (uint32_t)k_i3c_ntst_tdbef0;
  s_i3c.bst &= ~((uint32_t)k_i3c_bst_nackdf | (uint32_t)k_i3c_bst_tendf);
}

/** @brief Console-tap line buffer capacity for an I2C transaction summary. */
typedef enum : uint32_t {
  k_i2c_console_line_cap = 48U, /**< Max chars in an "I2C addr=.. .." line. */
} i2c_console_t;

/**
 * @brief Close a transaction (STOP): release the bus and notify the device.
 * @details Close a transaction (stop): release the bus and notify the device; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for I3C close transfer. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_i3c_close_transfer(void)
{
  if (s_i3c.acked) {
    i2c_device_t* dev = internal_i2c_device_find(s_i3c.target_7b);
    if (dev != nullptr) {
      if (dev->stop != nullptr) {
        dev->stop(dev->ctx);
      }
    }
    /* Console I2C tab: one line per completed (ACKed) transaction at STOP --
     * 7-bit address + R/W + read byte count. Bounded to one push per STOP. */
    char ln[k_i2c_console_line_cap];
    if (s_i3c.reading) {
      (void)snprintf(ln,
                     sizeof(ln),
                     "IIC addr=0x%02X R %uB",
                     (unsigned)s_i3c.target_7b,
                     (unsigned)s_i3c.rx_len);
    } else {
      (void)snprintf(ln, sizeof(ln), "IIC addr=0x%02X W", (unsigned)s_i3c.target_7b);
    }
    board_console_push(k_board_console_ch_i2c, ln);
  }
  s_i3c.busy      = false;
  s_i3c.addr_done = false;
  s_i3c.ntst      = (uint32_t)k_i3c_ntst_tdbef0;
}

/**
 * @brief Consume the address byte after a (re)START: select + ACK a device.
 * @details Consume the address byte after a (re)start: select + ack a device; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in] address_byte Address byte input used by the operation.
 * @pre Arguments satisfy the ranges documented for I3C address phase. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_i3c_address_phase(uint8_t address_byte)
{
  s_i3c.target_7b =
    (uint8_t)((uint32_t)address_byte >> (uint32_t)k_i3c_addr_shift) & (uint8_t)k_i3c_addr_mask7;
  s_i3c.reading   = ((uint32_t)address_byte & (uint32_t)k_i3c_addr_rnw) != 0U;
  s_i3c.addr_done = true;

  i2c_device_t* dev = internal_i2c_device_find(s_i3c.target_7b);
  if (dev == nullptr) {
    /* No device at this address: NACK the address (scan reports ack=0). */
    s_i3c.acked = false;
    s_i3c.bst |= (uint32_t)k_i3c_bst_nackdf;
    return;
  }
  s_i3c.acked = true;
  s_i3c.bst |= (uint32_t)k_i3c_bst_tendf; /* address ACKed -> scan sees TENDF */
  if (s_i3c.reading) {
    /* Pre-fetch the device's response for this read so NTDTBP0 reads serve it. */
    s_i3c.rx_len    = dev->read(dev->ctx, s_i3c.rx, (uint32_t)k_i3c_dev_rx_max);
    s_i3c.rx_pos    = 0U;
    s_i3c.rx_primed = false;
    s_i3c.ntst |= (uint32_t)k_i3c_ntst_rdbff0; /* RX data ready */
  } else {
    s_i3c.ntst |= (uint32_t)k_i3c_ntst_tdbef0; /* ready for the first data byte */
  }
}

/* =============================================================================
 * Target (peripheral) mode -- the model plays the EXTERNAL I2C controller that
 * drives the firmware's IIC_B target (i3c_i2c_peripheral_demo). The firmware
 * polls NTST and moves bytes through NTDTBP0; the controller path here is idle
 * (the firmware issues no CNDCTL START), so these handlers own NTST/NTDTBP0 for
 * the channel whenever target mode is active. See ::i3c_periph_phase_t.
 * =============================================================================
 */

/**
 * @brief Enter / leave target mode when the firmware (re)programmes MSDVAD.
 * @details Enter / leave target mode when the firmware (re)programmes msdvad; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in] msdvad Msdvad input used by the operation.
 * @pre Arguments satisfy the ranges documented for I3C periph open. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_i3c_periph_open(uint32_t msdvad)
{
  const uint8_t addr =
    (uint8_t)(((uint32_t)msdvad >> (uint32_t)k_i3c_addr_shift) & (uint32_t)k_i3c_addr_mask7);
  s_i3c.periph_addr_7b = addr;
  s_i3c.periph_mode    = (addr != 0U); /* MSDVAD == 0 (close) leaves target mode. */
  s_i3c.periph_phase   = (uint8_t)k_i3c_periph_idle;
}

/** @brief Target-mode NTST: arm a fresh controller write when idle, else reflect.
 *
 * @details A new transaction starts on the firmware's first status poll after
 *          the previous one completes: the synthetic controller "writes" a byte
 *          (NTST.RDBFF0 raised) that rotates per transaction, so the firmware's
 *          echo-back is a meaningful round-trip rather than a fixed constant.  * @return The I3C periph ntst result produced by the board periph I2C model.
 * @retval value The operation-specific I3C periph ntst value.
 * @pre Arguments satisfy the ranges documented for I3C periph ntst. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_i3c_periph_ntst(void)
{
  if (s_i3c.periph_phase == (uint8_t)k_i3c_periph_idle) {
    s_i3c.periph_rx_byte =
      (uint8_t)(((uint32_t)k_i3c_periph_seed + (s_i3c.periph_xfers * (uint32_t)k_i3c_periph_step)) &
                (uint32_t)k_i3c_byte_mask);
    s_i3c.periph_phase = (uint8_t)k_i3c_periph_rx_armed;
  }
  if (s_i3c.periph_phase == (uint8_t)k_i3c_periph_rx_armed) {
    return (uint32_t)k_i3c_ntst_rdbff0; /* controller-written byte is waiting */
  }
  return (uint32_t)k_i3c_ntst_tdbef0; /* tx_armed: controller is reading the echo */
}

/**
 * @brief Target-mode NTDTBP0 read: hand the firmware the written byte, go to TX.
 * @details Target-mode ntdtbp0 read: hand the firmware the written byte, go to tx; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @return The I3C periph rx read result produced by the board periph I2C model.
 * @retval value The operation-specific I3C periph rx read value.
 * @pre Arguments satisfy the ranges documented for I3C periph rx read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_i3c_periph_rx_read(void)
{
  s_i3c.periph_phase = (uint8_t)k_i3c_periph_tx_armed;
  return (uint32_t)s_i3c.periph_rx_byte;
}

/**
 * @brief Target-mode NTDTBP0 write: capture + verify the firmware echo, end xfer.
 * @details Target-mode ntdtbp0 write: capture + verify the firmware echo, end xfer; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in] byte One data byte received from or sent to the emulated interface.
 * @pre Arguments satisfy the ranges documented for I3C periph tx write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_i3c_periph_tx_write(uint8_t byte)
{
  if (byte != s_i3c.periph_rx_byte) {
    s_i3c.periph_echo_bad = true;
  }
  s_i3c.periph_xfers++;
  s_i3c.periph_phase = (uint8_t)k_i3c_periph_idle;
}

/**
 * @brief Handle a write to NTDTBP0 (address byte, then controller TX payload).
 * @details Handle a write to ntdtbp0 (address byte, then controller tx payload); this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for I3C ntdtbp0 write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_i3c_ntdtbp0_write(uint32_t value)
{
  const uint8_t byte = (uint8_t)(value & (uint32_t)k_i3c_byte_mask);
  if (s_i3c.periph_mode) {
    /* Target mode: the firmware is the peripheral pushing its echo byte. */
    internal_i3c_periph_tx_write(byte);
    return;
  }
  if (!s_i3c.addr_done) {
    internal_i3c_address_phase(byte);
    return;
  }
  if (!s_i3c.acked) {
    return; /* NACKed address: swallow further writes until STOP */
  }
  i2c_device_t* dev = internal_i2c_device_find(s_i3c.target_7b);
  if ((dev != nullptr) && (dev->write != nullptr)) {
    dev->write(dev->ctx, byte);
  }
  s_i3c.ntst |= (uint32_t)k_i3c_ntst_tdbef0; /* buffer empty again for the next */
}

/**
 * @brief Serve one NTDTBP0 read from the staged device response.
 * @details Serve one ntdtbp0 read from the staged device response; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @return The I3C ntdtbp0 read result produced by the board periph I2C model.
 * @retval value The operation-specific I3C ntdtbp0 read value.
 * @pre Arguments satisfy the ranges documented for I3C ntdtbp0 read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_i3c_ntdtbp0_read(void)
{
  if (s_i3c.periph_mode) {
    /* Target mode: the firmware is the peripheral draining the written byte. */
    return internal_i3c_periph_rx_read();
  }
  if (!s_i3c.rx_primed) {
    /* FSP's controller RXI handler drops the first RDBFF0 read before real payload. */
    s_i3c.rx_primed = true;
    return 0U;
  }
  uint8_t b = 0U;
  if (s_i3c.rx_pos < s_i3c.rx_len) {
    b = s_i3c.rx[s_i3c.rx_pos];
    s_i3c.rx_pos++;
  }
  s_i3c.ntst |= (uint32_t)k_i3c_ntst_rdbff0; /* keep RX-ready for the next byte */
  return (uint32_t)b;
}

/**
 * @brief Dispatch a CNDCTL write -> START / repeated-START / STOP.
 * @details Dispatch a cndctl write -> start / repeated-start / stop; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for I3C cndctl write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_i3c_cndctl_write(uint32_t value)
{
  if ((value & ((uint32_t)k_i3c_cndctl_stcnd | (uint32_t)k_i3c_cndctl_srcnd)) != 0U) {
    internal_i3c_open_transfer();
  } else if ((value & (uint32_t)k_i3c_cndctl_spcnd) != 0U) {
    internal_i3c_close_transfer();
  }
}

/**
 * @brief Read a register from the modelled I3C/IIC_B channel.
 * @details Read a register from the modelled i3c/iic_b channel; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in] off Register or byte offset addressed by the operation.
 * @return The I3C reg read result produced by the board periph I2C model.
 * @retval value The operation-specific I3C reg read value.
 * @pre Arguments satisfy the ranges documented for I3C reg read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_i3c_reg_read(uint64_t off)
{
  if (off == (uint64_t)k_i3c_off_ntst) {
    if (s_i3c.periph_mode) {
      return internal_i3c_periph_ntst();
    }
    return s_i3c.ntst;
  }
  if (off == (uint64_t)k_i3c_off_bst) {
    return s_i3c.bst;
  }
  if (off == (uint64_t)k_i3c_off_bcst) {
    /* BFREF: 1 when the bus is free. The driver gates new transactions on it. */
    return s_i3c.busy ? 0U : (uint32_t)k_i3c_bcst_bfref;
  }
  if (off == (uint64_t)k_i3c_off_ntdtbp0) {
    return internal_i3c_ntdtbp0_read();
  }
  return s_i3c.reg[internal_i3c_word(off)]; /* reflect every other register */
}

/**
 * @brief Write a register on the modelled I3C/IIC_B channel.
 * @details Write a register on the modelled i3c/iic_b channel; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in] off Register or byte offset addressed by the operation.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for I3C reg write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_i3c_reg_write(uint64_t off, uint32_t value)
{
  s_i3c.reg[internal_i3c_word(off)] = value; /* shadow keeps "configure then verify" working */
  if (off == (uint64_t)k_i3c_off_msdvad) {
    /* Own-address programming = the firmware is coming up as an addressed
     * target. The controller path never writes MSDVAD, so this cleanly selects
     * target mode (the external-controller stimulus in i3c_periph_*). */
    internal_i3c_periph_open(value);
  } else if (off == (uint64_t)k_i3c_off_cndctl) {
    internal_i3c_cndctl_write(value);
  } else if (off == (uint64_t)k_i3c_off_ntdtbp0) {
    internal_i3c_ntdtbp0_write(value);
  } else if (off == (uint64_t)k_i3c_off_bst) {
    /* BST condition / fault flags are write-0-to-clear: keep only bits still
     * written as 1 (the driver clears by reading then masking the bit out). */
    s_i3c.bst &= value;
  }
}

/**
 * @brief MMIO read inside the I3C/IIC_B window.
 * @details MMIO read inside the i3c/iic_b window; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @return The I3C read result produced by the board periph I2C model.
 * @retval value The operation-specific I3C read value.
 * @pre Arguments satisfy the ranges documented for I3C read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_i3c_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  return internal_i3c_reg_read(addr - (uint64_t)k_i3c_base);
}

/**
 * @brief MMIO write inside the I3C/IIC_B window.
 * @details MMIO write inside the i3c/iic_b window; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for I3C write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_i3c_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  internal_i3c_reg_write(addr - (uint64_t)k_i3c_base, (uint32_t)value);
}

/**
 * @brief Clear the I3C channel + GT911 state and (re)populate the bus.
 * @details Clear the i3c channel + gt911 state and (re)populate the bus; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for I3C reset. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_i3c_reset(void)
{
  s_i3c   = (i3c_state_t){};
  s_gt911 = (gt911_state_t){};
  priv_board_i2c_imu_fuel_reset(); /* lay the IMU + fuel-gauge register files (CLI-set SOC) */
  /* Populate the modelled I2C bus: the EK-RA8D2 carrier's GT911 touch
   * controller answers at its default 7-bit address on I3C/IIC_B channel 0, so
   * the firmware's real ra8_touch -> ra8_i3c_transfer -> GT911 path returns data
   * instead of needing a function-level touch stub. The LSM6DSO IMU answers at
   * 0x6B so imu_lsm6dso_demo's WHO_AM_I probe + sample reads succeed too. */
  for (uint32_t i = 0U; i < (uint32_t)k_i3c_dev_max; i++) {
    s_i2c_dev[i] = (i2c_device_t){};
  }
  priv_i2c_device_register((uint8_t)k_gt911_addr_7b,
                           internal_gt911_write,
                           internal_gt911_read,
                           internal_gt911_stop,
                           &s_gt911);
  /* LSM6DSO IMU at 0x6B + MAX17048-class fuel gauge at 0x36 (battery SOC +
   * charge direction) register themselves from the devices TU. */
  priv_board_i2c_imu_fuel_register();
}

/**
 * @brief Print the GT911 touch line when the firmware drained any contact.
 * @details Print the gt911 touch line when the firmware drained any contact; this step is contained within the board periph I2C model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for I3C report. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph I2C model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_i3c_report(void)
{
  if (s_gt911.reported > 0U) {
    (void)priv_emu_io_errf("  I3C/I2C GT911 : %u touch frame(s) drained via ra8_touch -> I3C\n",
                           s_gt911.reported);
  }
  if (priv_board_i2c_imu_reads() > 0U) {
    (void)priv_emu_io_errf("  I3C/I2C LSM6DSO: %u register read(s) answered (WHO_AM_I + samples)\n",
                           priv_board_i2c_imu_reads());
  }
  if (s_i3c.periph_mode) {
    /* Target mode: report how many controller write+read round-trips the
     * firmware's IIC_B peripheral accepted, and whether every byte it echoed
     * back matched the byte the synthetic controller wrote. */
    const bool echo_ok = (s_i3c.periph_xfers > 0U) && !s_i3c.periph_echo_bad;
    (void)priv_emu_io_errf(
      "  I3C/I2C target: addr=0x%02X %u peripheral write+read xfer(s) accepted,"
      " echo=%s\n",
      (unsigned)s_i3c.periph_addr_7b,
      (unsigned)s_i3c.periph_xfers,
      echo_ok ? "Y" : "N");
  }
}

/** @brief This block's descriptor (static lifetime; the core keeps the pointer). */
static const board_periph_block_t s_k_i3c_block = {
  .base   = (uint64_t)k_i3c_base,
  .span   = (uint64_t)k_i3c_span,
  .order  = (uint32_t)k_block_order_i2c,
  .read   = internal_i3c_read,
  .write  = internal_i3c_write,
  .tick   = nullptr,
  .reset  = internal_i3c_reset,
  .report = internal_i3c_report,
  .name   = "I3C/I2C+GT911",
};

/** @brief Self-register the I3C/I2C block before main runs (decentralized). */
[[gnu::constructor]] RA8_INTERNAL static void internal_board_periph_i2c_register(void)
{
  board_periph_register_block(&s_k_i3c_block);
}
