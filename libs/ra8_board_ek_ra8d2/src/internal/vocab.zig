//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared vocabulary for the EK-RA8D2 board layer: the error codes, the
//! levels, the clock ids and the packed port/pin encoding every other file
//! here speaks. Values mirror the C headers they came from, named rather than
//! repeated at each use site.

/// `ra8_err_t` values this layer returns or forwards.
pub const Err = struct {
    pub const ok: u32 = 0;
    pub const invalid_arg: u32 = 0x103;
    pub const invalid_size: u32 = 0x105;
    pub const not_found: u32 = 0x106;
    pub const not_initialized: u32 = 0x10F;
    pub const not_supported: u32 = 0x107;
    pub const null_ptr: u32 = 0x504;
    pub const hw_timeout: u32 = 0x203;
    pub const hw_init_failed: u32 = 0x201;
    pub const gpio_conflict: u32 = 0x205;
    pub const nack: u32 = 0x407;
};

/// `ra8_level_t`.
pub const Level = struct {
    pub const low: u32 = 0;
    pub const high: u32 = 1;
};

/// `ra8_clock_id_t` members this layer reads.
pub const ClockId = struct {
    pub const cpuclk0: u32 = 0;
    pub const pclka: u32 = 3;
    pub const pclkd: u32 = 6;
};

/// `ra8_psel_t` members this layer routes.
pub const Psel = struct {
    pub const sci_async: u32 = 0x04;
    pub const usb_fs: u32 = 0x13;
    /// 11000b: ESWM (RGMII). HUM 20.6.
    pub const ether_rgmii: u32 = 0x18;
    /// 00010b: GPT0 (low channels).
    pub const gpt0: u32 = 0x02;
    /// 01111b: camera engine unit.
    pub const ceu: u32 = 0x0F;
    /// 11011b: PDM-IF (PDMCLKn / PDMDATn). HUM 20.6.
    pub const pdm: u32 = 0x1B;
    /// 00111b: IIC / I3C controller-peripheral. HUM 20.6.
    pub const iic: u32 = 0x07;
    /// 10010b: SSIE I2S audio. HUM 20.6.
    pub const ssie: u32 = 0x12;
};

/// `ra8_pfs_dscr_t` drive strengths this layer sets.
pub const Dscr = struct {
    pub const middle: u8 = 1;
    /// 10b: high-speed high drive.
    pub const high_speed_high: u8 = 2;
};

/// `ra8_gpt_*` members this layer selects. All of them are the zero member of
/// their enum, which is why each is named rather than defaulted.
pub const Gpt = struct {
    /// Saw-wave PWM, up-count.
    pub const mode_saw_pwm: u8 = 0;
    /// PCLKD / 1.
    pub const ps_div_1: u8 = 0;
    /// GTIOCnA, the compare register A path.
    pub const pin_a: u8 = 0;
    /// Output is high while duty < count.
    pub const pol_active_high: u8 = 0;
    /// OnDFLT = 0, stop level low.
    pub const stop_low: u8 = 0;
    /// A POEG fault does not affect the pin.
    pub const disable_none: u8 = 0;
};

/// `ra8_mstp_t` members this layer releases. Register index in the high byte,
/// bit number in the low byte.
pub const I3c = struct {
    /// Legacy I2C-compatibility controller.
    pub const mode_i2c: u8 = 1;
};

/// The three board facts about the GT911, plus the driver's own caps.
pub const Touch = struct {
    /// IIC_B / I3C channel 0 carries the GT911.
    pub const i3c_channel: u8 = 0;
    /// GT911 default 7-bit target address.
    pub const target_7b: u8 = 0x5D;
    /// Fast-mode I2C rate for the touch bus.
    pub const bus_hz: u32 = 400_000;
    /// Hard cap, matching GT911 capacity.
    pub const max_points: u8 = 5;
    /// Sentinel for "no IRQ pin attached".
    pub const irq_pin_unset: u8 = 32;
};

/// The SPH0690 microphones as the board wires them, plus what the PDM-IF is
/// asked for.
pub const Pdm = struct {
    /// Decimated SPH0690 sample rate.
    pub const sample_rate_hz: u32 = 16_000;
    /// PDM-IF signed PCM payload width.
    pub const valid_bits: u8 = 20;
    /// Channel 2 carries the EK-RA8D2 MEMS mics.
    pub const channel: u8 = 2;
    /// MIC1 has SELECT tied low, so it clocks out on the rising edge.
    pub const mic1: u8 = 0;
    /// MIC2 has SELECT tied high, so it clocks out on the falling edge.
    pub const mic2: u8 = 1;
    pub const mic_count: u8 = 2;
    /// INPSEL: rising edge of channel n.
    pub const edge_rising: u8 = 0;
    /// INPSEL: falling edge of channel n-1.
    pub const edge_falling: u8 = 1;
};

/// GPIO drive levels and input pull selection, as `ra8_port_constants.h`
/// numbers them.
pub const Io = struct {
    pub const level_low: u32 = 0;
    pub const level_high: u32 = 1;
    pub const pull_up: u32 = 1;
};

/// The DA7212 CODEC link over SSIE0, as the board wires it.
pub const Audio = struct {
    pub const ssie_channel: u8 = 0;
    pub const channels_mono: u8 = 1;
    pub const channels_stereo: u8 = 2;
    /// Two int16 samples pack into one 32-bit SSIE FIFO word.
    pub const samples_per_word: u32 = 2;
};

/// The U15 PI4IOE5V6408 that overrides the SW4 configuration switches.
/// UM Section 5.5.3.
pub const IoExpander = struct {
    pub const addr_7b: u8 = 0x43;
    pub const iic_channel: u8 = 1;
    pub const bus_hz: u32 = 100_000;
    /// PCLKB = PLL1P/16 post-CGC.
    pub const pclkb_hz: u32 = 62_500_000;

    /// Register map, from Renesas's `board_cfg_switch.c` for the sister
    /// EK-RA8T2, which wires U15 identically.
    pub const reg_devid: u8 = 0x01;
    pub const reg_iodir: u8 = 0x03;
    pub const reg_output: u8 = 0x05;
    pub const reg_hiz: u8 = 0x07;
    pub const reg_pud_sel: u8 = 0x0D;
    pub const reg_input_lvl: u8 = 0x0F;

    /// Output polarity is OFF == bit HIGH, per the FSP reference.
    pub const iodir_all_outputs: u8 = 0xFF;
    pub const output_all_high: u8 = 0xFF;
    pub const hiz_none: u8 = 0x00;
    /// SW4-1 ON, SW4-2 OFF (Pmod1 UART), SW4-3 ON, SW4-4 ON, SW4-5 OFF (I2C).
    pub const output_project_default: u8 = 0xF2;
    pub const output_usbhs_host: u8 = 0x72;
    pub const output_octospi_active: u8 = 0xF8;

    /// Bit-bang recovery: nine SCL clocks flush one stuck byte plus its ACK.
    pub const recover_pulses: u32 = 9;
    /// Busy-loop iterations that come to roughly one SCL half-period.
    pub const recover_spins: u32 = 2000;
};

/// USB-HS bring-up: the MSTP gate, the PHY speed and the J7 role strap.
pub const Usb = struct {
    /// MSTPB12 USBHS.
    pub const mstp_usbhs: u16 = (1 << 8) | 12;
    pub const speed_hs: u32 = 1;
    /// PD07 selects the J7 role: low is Device, high is Host. UM 6.2 p 34.
    pub const role_pin_port: u16 = 13;
    pub const role_pin_index: u16 = 7;
};

pub const Mstp = struct {
    const reg_c: u16 = 2;
    /// MSTPC30 ESWM.
    pub const eswm: u16 = (reg_c << 8) | 30;
};

/// The Ethernet vocabulary: which ETHA and RMAC instance the board wires, and
/// the `ra8_etha_*` / `ra8_rmac_*` enum values its configuration asks for.
pub const Eth = struct {
    /// ETHA1 and RMAC1. UM 6.1.
    pub const etha_port: u8 = 1;
    pub const rmac_port: u8 = 1;

    /// `ra8_etha_opc_t`, EAMC.OPC[1:0].
    pub const opc_reset: u8 = 0;
    pub const opc_disable: u8 = 1;
    pub const opc_config: u8 = 2;
    pub const opc_operation: u8 = 3;

    /// `ra8_rmac_mrafc_t` receive-filter bits. Each composite is the hash bit
    /// OR the perfect-match bit for that address class.
    pub const mrafc_unicast_match: u32 = 0x0000_0001 | 0x0001_0000;
    pub const mrafc_broadcast: u32 = 0x0000_0004 | 0x0004_0000;
    /// BCACE.
    pub const mrafc_bc_accept: u32 = 0x0000_0040;

    /// `ra8_rmac_pis_t`: MII (000b), the internal interface an external RGMII
    /// link presents to the MAC at 10/100.
    pub const pis_mii: u8 = 0;
    /// `ra8_rmac_lsc_t`.
    pub const lsc_100mbit: u8 = 1;
    /// `ra8_rmac_duplex_t`.
    pub const duplex_full: u8 = 1;
    /// 1 MHz MDC, well below the 2.5 MHz ceiling.
    pub const mdc_default_hz: u32 = 1_000_000;
};

/// Packed `ra8_port_pin_t`: port in the high byte, pin in the low byte.
pub const Pin = struct {
    pub fn pack(port_id: u16, pin_index: u16) u16 {
        return (port_id << 8) | pin_index;
    }
};
