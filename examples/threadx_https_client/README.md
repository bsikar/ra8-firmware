# threadx_https_client

ThreadX + NetX Duo + Mbed TLS HTTPS client demo for EK-RA8D2.

The board:

1. Brings up the CGC, SCI8 (115200 8N1, J-Link OB CDC bridge),
   the RMII pins on Port 7 / Port 4, and the RSIP-E50D engine.
2. Boots ThreadX with a single worker thread that drives NetX Duo
   (static IPv4 192.168.1.42 / 24, gateway 192.168.1.1) and dials
   `93.184.216.34:443` -- the legacy `www.example.com` IP.
3. Runs an Mbed TLS handshake over the NetX TCP socket. Mbed TLS's
   AES + SHA-256 primitives are routed to the RSIP-E50D engine via
   the `port/mbedtls/` shims; CTR_DRBG is seeded from the RSIP TRNG.
4. Pins the peer leaf certificate by SHA-256 (compile-time constant
   in `main.c`); a mismatch aborts the request.
5. Sends `GET / HTTP/1.1\r\nHost: www.example.com\r\n\r\n` and
   dumps the first 1 KiB of the response body to SCI8.

## Build / flash

```
make threadx_https_client                              # from repo root
make -C examples/threadx_https_client flash            # JLinkExe load
```

The Makefile pins `RA_USE_THREADX=ON`, `RA_USE_NETXDUO=ON` and
`RA_USE_MBEDTLS=ON`; the top-level `cmake -B build` skips this app
unless those three options are enabled, so the bare-metal default
configuration keeps building cleanly.

## Test recipe

1. Plug an Ethernet cable from the EK-RA8D2 J64 RMII connector to a
   workstation. Bridge or NAT the workstation interface so the board
   can reach `93.184.216.34:443`. A typical Linux setup is:

   ```
   sudo ip link set eth1 master br0
   sudo ip addr add 192.168.1.1/24 dev br0
   sudo iptables -t nat -A POSTROUTING -s 192.168.1.0/24 -o wlan0 -j MASQUERADE
   echo 1 | sudo tee /proc/sys/net/ipv4/ip_forward
   ```

   On macOS, share the Wi-Fi adapter via System Settings -> Sharing
   -> Internet Sharing.

2. Open a serial terminal at 115200 8N1 on the J-Link OB CDC port:

   ```
   picocom -b 115200 /dev/cu.usbmodem0001234567891
   ```

3. Capture a fresh certificate fingerprint and update
   `k_demo_cert_pin_sha256` in `main.c`:

   ```
   echo | openssl s_client -connect www.example.com:443 \
       -servername www.example.com 2>/dev/null \
     | openssl x509 -outform der | openssl dgst -sha256
   ```

   Paste the 32-byte digest into the array (or define
   `DEMO_DISABLE_CERT_PIN` while iterating to skip the pin check).

4. `make threadx_https_client && make -C examples/threadx_https_client flash`.

5. Reset the board. Within ~3 seconds you should see:

   ```
   [https] booting ThreadX + NetX Duo + Mbed TLS...
   [https] bringing NetX Duo up...
   [https] connecting to www.example.com:443
   [https] handshake OK, cert pin matches, dumping body
   <!doctype html>...
   ```

   followed by the first 1 KiB of the HTML body.

## Notes

- The DNS path is intentionally static -- this demo focuses on the
  TLS / cert-pin / hardware-crypto integration. A future sweep can
  drop in NetX DNS once the `nx_dns_*` plumbing lands.
- Cert pinning is per-leaf. If `www.example.com` rotates its
  certificate (typical: ~3 months), update `k_demo_cert_pin_sha256`
  and re-flash. The placeholder ships as 32 zero bytes so an
  un-customised flash will deliberately refuse to send the request.
- The RSIP engine performs all AES (CBC / CTR / GCM single-shot)
  rounds; SHA-256 (record MAC + transcript hash) is also routed.
  RSA / ECDH stay on Mbed TLS bignum until the asymmetric path is
  wired through `ra_rsip_rsa_*` / `ra_rsip_ecdh_*`.
