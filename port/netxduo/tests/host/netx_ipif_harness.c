/**
 * @file netx_ipif_harness.c
 * @brief The RA8FW-1065 host harness over ra8_ipif on the vendored NetX Duo +
 *        ThreadX Linux port.
 *
 * @details
 * Re-derived from the RA8FW-360 record (2026-10-26): fourteen checks over the
 * shared IP bring-up facade, run on the host against the vendored Linux port
 * of both stacks. The build graph compiles it (tests/zig_build_graph/
 * netxduo_host.zig), with the header_patch-generated tx_port.h shadowing the
 * vendored header (ULONG widened to unsigned long on x86_64).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "tx_api.h"
#include "nx_api.h"
#include "nxd_dhcp_client.h"
#include "ra8_ipif.h"
extern unsigned long _nx_ip_created_count;

static int g_pass = 0;
static int g_fail = 0;

static void check(int idx, int cond, const char *what)
{
  if (cond) {
    g_pass++;
    printf("PASS %2d: %s\n", idx, what);
  } else {
    g_fail++;
    printf("FAIL %2d: %s\n", idx, what);
    fflush(stdout);
  }
}
#define CHK(idx, cond, what) do { if (cond) check(idx, 1, what); else { check(idx, 0, what " [detail]"); diag(); } } while (0)
static void dn(const char* tag, ra8_ipif_t* ipif)
{
  tx_thread_sleep(10);
  ra8_err_t e = ra8_ipif_down(ipif);
  printf("  down %s -> 0x%x, ip_created_count=%lu\n", tag, (unsigned)e, _nx_ip_created_count);
}
static void diag(void);

/* The fake no-op link driver: every request completes synchronously. */
static void fake_driver(NX_IP_DRIVER *req)
{
  (void)req;
  req->nx_ip_driver_status = NX_SUCCESS;
}

__attribute__((aligned(4))) static uint8_t pool_mem[40960];
__attribute__((aligned(8))) static uint8_t ip_stack[32768];
__attribute__((aligned(4))) static uint8_t arp_cache[1040];
__attribute__((aligned(8))) static uint8_t app_stack[131072];
static NX_DHCP dhcp_block;

static ra8_ipif_cfg_t make_cfg(void)
{
  ra8_ipif_cfg_t c;
  memset(&c, 0, sizeof c);
  c.name = "harness";
  c.driver = fake_driver;
  c.pool_mem = pool_mem;
  c.pool_bytes = sizeof pool_mem;
  c.pkt_payload = 1568U;
  c.ip_stack = ip_stack;
  c.ip_stack_bytes = sizeof ip_stack;
  c.ip_prio = 31U;
  c.arp_cache = arp_cache;
  c.arp_bytes = sizeof arp_cache;
  c.enable_tcp = false;
  c.disable_udp = false;
  c.dhcp_wait_ms = 2000U;
  return c;
}

static void run_tests(ULONG arg)
{
  (void)arg;
  ra8_ipif_cfg_t cfg;
  ra8_err_t e;
  UINT st;
  ULONG addr, mask, actual;

  /* Checks 1-5: the static path on one instance. */
  static ra8_ipif_t s_ipif;
  memset(&s_ipif, 0, sizeof s_ipif);
  cfg = make_cfg();
  cfg.ip_address = 0xC0A84D2AU;   /* 192.168.77.42, host byte order */
  cfg.ip_netmask = 0xFFFFFF00U;   /* 255.255.255.0 */
  e = ra8_ipif_up(&s_ipif, &cfg);
  check(1, e == k_ra8_ok, "static up ok");
  addr = 0;
  mask = 0;
  st = nx_ip_address_get(&s_ipif.ip, &addr, &mask);
  check(2, st == NX_SUCCESS && addr == 0xC0A84D2AU, "address readback exact");
  check(3, mask == 0xFFFFFF00U, "netmask readback exact");
  actual = 0;
  st = nx_ip_status_check(&s_ipif.ip, NX_IP_ADDRESS_RESOLVED, &actual, 50);
  check(4, st == NX_SUCCESS, "timed status check survives (static, wait>0)");
  e = ra8_ipif_down(&s_ipif);
  check(5, e == k_ra8_ok, "down ok");

  /* Check 6: UDP enabled by default. */
  static ra8_ipif_t u1;
  memset(&u1, 0, sizeof u1);
  cfg = make_cfg();
  e = ra8_ipif_up(&u1, &cfg);
  NX_UDP_SOCKET sock;
  st = nx_udp_socket_create(&u1.ip, &sock, "s1", NX_IP_NORMAL,
                            NX_FRAGMENT_OKAY, 0x80, 5);
  check(6, e == k_ra8_ok && st == NX_SUCCESS, "UDP socket create by default");
  (void)nx_udp_socket_delete(&sock);
  dn("u1", &u1);

  /* Check 7: disable_udp refuses the socket create. */
  static ra8_ipif_t u2;
  memset(&u2, 0, sizeof u2);
  cfg = make_cfg();
  cfg.disable_udp = true;
  e = ra8_ipif_up(&u2, &cfg);
  NX_UDP_SOCKET sock2;
  st = nx_udp_socket_create(&u2.ip, &sock2, "s2", NX_IP_NORMAL,
                            NX_FRAGMENT_OKAY, 0x80, 5);
  if (e == k_ra8_ok && st == NX_NOT_ENABLED) check(7, 1, "disable_udp -> NX_NOT_ENABLED");
  else { printf("detail7: up=%d(0x%x) create=0x%x\n", e, e, st); check(7, 0, "disable_udp -> NX_NOT_ENABLED"); }
  dn("u2", &u2);

  /* Check 8: a static address with no mask is rejected up front. */
  static ra8_ipif_t n1;
  memset(&n1, 0, sizeof n1);
  cfg = make_cfg();
  cfg.ip_address = 0xC0A84D2AU;
  cfg.ip_netmask = 0;
  e = ra8_ipif_up(&n1, &cfg);
  check(8, e == k_ra8_err_invalid_arg, "no-mask rejected (0x103)");
  dn("n1", &n1);

  /* Checks 9-13: the unbound path with a live DHCP client. */
  static ra8_ipif_t d1;
  memset(&d1, 0, sizeof d1);
  cfg = make_cfg();
  e = ra8_ipif_up(&d1, &cfg);
  addr = 0xdeadbeefU;
  mask = 0;
  st = nx_ip_address_get(&d1.ip, &addr, &mask);
  if (e == k_ra8_ok && st == NX_SUCCESS && addr == 0) check(9, 1, "zero-address created unbound");
  else { printf("detail9: up=0x%x gs=0x%x addr=0x%lx\n", e, st, (unsigned long)addr); check(9, 0, "zero-address created unbound"); }
  memset(&dhcp_block, 0, sizeof dhcp_block);
  st = nx_dhcp_create(&dhcp_block, &d1.ip, "dh");
  if (st == NX_SUCCESS) check(10, 1, "DHCP create NX_SUCCESS");
  else { printf("detail10: create=0x%x\n", st); check(10, 0, "DHCP create NX_SUCCESS"); }
  st = nx_dhcp_start(&dhcp_block);
  if (st == NX_SUCCESS) check(11, 1, "DHCP start NX_SUCCESS");
  else { printf("detail11: start=0x%x\n", st); check(11, 0, "DHCP start NX_SUCCESS"); }
  ULONG slept = tx_thread_sleep(300);
  check(12, slept == TX_SUCCESS, "plain 3 s sleep survives with DHCP running");
  actual = 0;
  st = nx_ip_status_check(&d1.ip, NX_IP_ADDRESS_RESOLVED, &actual, 100);
  if (st == NX_NOT_SUCCESSFUL) check(13, 1, "timed status check after sleep returns 0x43 clean");
  else { printf("detail13: status=0x%x\n", st); check(13, 0, "timed status check after sleep returns 0x43 clean"); }
  /* The harness drove DHCP directly, so it must release the client the same
     way the facade does before teardown: the client's UDP socket stays bound
     to the IP otherwise and nx_ip_delete refuses (NX_SOCKETS_BOUND). */
  st = nx_dhcp_delete(&dhcp_block);
  printf("  harness dhcp_delete -> 0x%x\n", st);
  dn("d1", &d1);

  /* Check 14: the facade's DHCP path on a fresh unbound instance times out
     on the dead link and zeroes the lease. */
  static ra8_ipif_t d2;
  memset(&d2, 0, sizeof d2);
  cfg = make_cfg();
  e = ra8_ipif_up(&d2, &cfg);
  ra8_wifi_lease_t lease;
  memset(&lease, 0xAA, sizeof lease);
  e = ra8_ipif_dhcp(&d2, &lease);
  if (e == k_ra8_err_timeout && lease.bound == false && lease.ip == 0 &&
      lease.mask == 0 && lease.gateway == 0 && lease.dhcp_server == 0)
    check(14, 1, "DHCP path times out on the dead link, lease stays zeroed");
  else { printf("detail14: e=0x%x bound=%d ip=0x%x mask=0x%x gw=0x%x srv=0x%x\n", e, (int)lease.bound, lease.ip, lease.mask, lease.gateway, lease.dhcp_server); check(14, 0, "DHCP path times out on the dead link, lease stays zeroed"); }
  dn("d2", &d2);

  printf("%d/%d PASS, %d FAIL\n", g_pass, g_pass + g_fail, g_fail);
  fflush(stdout);
  exit(g_fail == 0 ? 0 : 1);
}

static TX_THREAD app_thread;

void tx_application_define(void *first_unused_memory)
{
  (void)first_unused_memory;
  tx_thread_create(&app_thread, "app", run_tests, 0,
                   app_stack, sizeof app_stack,
                   20, 20, TX_NO_TIME_SLICE, TX_AUTO_START);
}

int main(void)
{
  tx_kernel_enter();
  return 0;
}
