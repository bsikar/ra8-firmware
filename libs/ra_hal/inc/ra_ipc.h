/**
 * @file ra_ipc.h
 * @brief Inter-Processor Communication (IPC) HAL driver -- public API
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Bare-metal driver for the RA8D2 IPC mailbox between the Cortex-M85
 * (CPU0) and Cortex-M33 (CPU1). Exposes the full HUM Ch 3 surface:
 *
 * - **Lifecycle**: ``ra_ipc_init`` / ``ra_ipc_deinit`` / ``ra_ipc_reset_fifo``
 *   / ``ra_ipc_set_event_mask`` per FIFO channel.
 * - **Channel pairing convention**: ``ra_ipc_channel_for_send`` /
 *   ``ra_ipc_channel_for_recv`` map the local CPU id (CPU0 / CPU1) to the
 *   right IPC unit so application code does not hard-code channel ids.
 * - **Per-channel attribution check**: ``ra_ipc_can_access`` reads
 *   IPCSAR / IPCPAR before issuing a register write that the SAU /
 *   IDAU would otherwise silently drop.
 * - **Send / receive**: single-shot ``ra_ipc_send_message`` /
 *   ``ra_ipc_recv_message`` plus burst variants
 *   ``ra_ipc_send_burst`` / ``ra_ipc_recv_burst`` with overflow
 *   protection driven by the 4-stage FIFO depth (HUM Ch 3.1 p 204).
 * - **Maskable IRQ events**: ``ra_ipc_send_event`` /
 *   ``ra_ipc_clear_event`` plus per-event-line callback attach via
 *   ``ra_ipc_attach_event_handler``. Each of the eight IRQ lines
 *   per channel can have its own decoded handler.
 * - **NMI surface**: ``ra_ipc_nmi_send`` / ``ra_ipc_nmi_clear`` /
 *   ``ra_ipc_nmi_get_status`` / ``ra_ipc_attach_nmi_handler`` /
 *   ``ra_ipc_dispatch_nmi`` cover IPC0NMI* and IPC1NMI*.
 * - **Hardware semaphores**: ``ra_ipc_sem_try_take`` /
 *   ``ra_ipc_sem_release`` / ``ra_ipc_sem_take_timeout`` /
 *   ``ra_ipc_sem_is_locked`` wrap IPCSEM0..15 with the test-and-set
 *   semantics documented in HUM Ch 3.2.3 p 210.
 * - **Real ISR wiring**: ``ra_ipc_install_isr`` registers the IPC
 *   IRQ event with ``ra_isr_register`` so production builds get a
 *   real interrupt path. Tests can still call ``ra_ipc_dispatch``
 *   directly to drive the dispatch logic.
 * - **Shared-memory ring buffer**: ``ra_ipc_ring_*`` builds a
 *   producer/consumer ring on top of an SRAM region; the IPC FIFO
 *   word carries the producer head index.
 * - **Error recovery**: ``ra_ipc_send_message_retry`` /
 *   ``ra_ipc_recv_message_retry`` clear FERR/RERR and retry up to a
 *   bounded number of times. ``ra_ipc_clear_errors`` clears RERR /
 *   FERR explicitly.
 *
 * Channel numbering (matches ``ra8d2_ipc_regs.h``):
 *
 * | id | direction        | unit | FIFO  |
 * |---:|------------------|------|-------|
 * |  0 | CPU1 -> CPU0     | IPC0 | FIFO00|
 * |  1 | CPU1 -> CPU0     | IPC0 | FIFO01|
 * |  2 | CPU0 -> CPU1     | IPC1 | FIFO10|
 * |  3 | CPU0 -> CPU1     | IPC1 | FIFO11|
 *
 * The two cores typically pair channels: e.g. M85 sends through
 * channel 2 and reads replies on channel 0. The
 * ``ra_ipc_channel_for_*`` helpers encode that convention so user
 * code remains symmetric across both cores.
 *
 * @note
 * This header is a thin umbrella. The declarations are split across
 * ``ra_ipc_types.h`` (typed enums, structs, callback typedefs),
 * ``ra_ipc_xfer.h`` (lifecycle, channel-pair, send / receive, status,
 * attribution), and ``ra_ipc_sync.h`` (semaphores, NMI, interrupt
 * dispatch, ring-buffer). Consumers continue to ``#include "ra_ipc.h"``
 * unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_ipc_regs.h"
#include "ra_err.h"
#include "ra_ipc_sync.h"
#include "ra_ipc_types.h"
#include "ra_ipc_xfer.h"

#ifdef __cplusplus
}
#endif
