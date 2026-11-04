# ESP32_AdBlocker

ESP32_AdBlocker acts as a DNS Sinkhole (like [Pi-Hole](https://pi-hole.net/)) by returning 0.0.0.0 for any domain names in its blocklist, else uses an external DNS server to resolve IP addresses. This prevents content being retrieved from or sent to blocked domains. A web server is provided to control the service and monitor its operation.

Fork of [s60sc/ESP32_AdBlocker](https://github.com/s60sc/ESP32_AdBlocker) adding **ESP32-C3 support** (flash-encryption-aware) and a few stability fixes.

## Supported boards

| Board | Blocklist storage | Capacity | Notes |
|---|---|---|---|
| ESP32-S3 (8MB PSRAM) | PSRAM arena, domain names | ~250k domains | Full feature set, fastest lookups |
| ESP32 (4MB PSRAM) | PSRAM arena, domain names | ~100k domains | May truncate very large lists |
| ESP32-C3 (4MB flash, no PSRAM) | Sorted 64-bit domain hashes in flash | ~110k domains | See [ESP32-C3 differences](#esp32-c3-no-psram) |

## Installation

### Build from source

Requires [arduino-cli](https://arduino.github.io/arduino-cli/) with the esp32 core ≥ 3.1.1:

```sh
arduino-cli core install esp32:esp32

tools/build.sh c3      # ESP32-C3  -> build/c3/
tools/build.sh s3      # ESP32-S3  -> build/s3/
tools/build.sh esp32   # ESP32     -> build/esp32/
```

Or use the Arduino IDE with these board settings:

* ESP32-S3 — `8M with spiffs (...)`, PSRAM enabled
* ESP32 — `Minimal SPIFFS (...)`, PSRAM enabled
* ESP32-C3 — `No OTA (2MB APP/2MB SPIFFS)`, USB CDC On Boot enabled

### Flash

```sh
tools/flash.sh /dev/cu.usbmodemXXXX           # normal boards
tools/flash.sh /dev/cu.usbmodemXXXX --encrypt # modules with flash encryption enabled
```

Useful extra modes: `--usb` (reset into download mode over USB, no BOOT/RST buttons needed), `--manual` (board already in download mode), `--check` (print chip/flash info), `--verify` (read-back compare).

### Modules with flash encryption enabled

Some modules ship with `SPI_BOOT_CRYPT_CNT` burned in (flash encryption permanently enabled, key not readable). On those, a plaintext flash boots into `invalid header: 0x...`. Symptoms and recovery:

1. `tools/flash.sh PORT --check` connects but the device never boots.
2. Erase is refused with `Active security features detected ...` — this is expected, do **not** use `--force` on an erase.
3. Flash with `tools/flash.sh PORT --encrypt`: the flasher stub encrypts each block with the chip's own eFuse key during the write, so no key file is needed.

## First-time setup

1. Power up. The device starts AP mode with SSID `ESP32_AdBlocker_<mac>`.
2. Connect to it and open `http://192.168.4.1`, enter your WiFi SSID and password, and press **Connect**. The device reboots, downloads its web pages, and reboots again.
3. Open the page again (via the AP, or the device's LAN IP), then **Edit Config → Network**:
   * Set a **static IP address** — every client will use this as their DNS server, so it must not change. Reserve the same address in the router's DHCP table (or pick one outside the pool).
   * Set gateway, subnet, and the upstream DNS servers (e.g. `1.1.1.1` / `1.0.0.1`). Do **not** point these at the router if the router forwards DNS to this device — that is a loop.
4. **Save** (reboots). The default blocklist (StevenBlack hosts) downloads and is processed. First load takes several minutes; watch the progress on the AdBlocker tab.
5. Point the network at it: set the router's DHCP-advertised DNS to the device's static IP. Disable IPv6 DNS on the router (the sinkhole answers IPv4 only) and turn off *Secure DNS / DoH* in browsers.
6. Verify: `nslookup doubleclick.net <device-ip>` should answer `0.0.0.0`.

Locked ISP router (can't change DHCP DNS)? Set the DNS server manually on each device instead — it persists per network.

## Operation

The web page shows:

* **Allowed / Blocked domains** — counters since restart
* **Current URL for blocklist file** — the source in use
* **Enter new URL for blocklist or domain** — with **Reload**, **AddDomain**, **DelDomain**, **CheckDomain** buttons
* **Stop Blocklist Load** — abort a running download (keeps the previous list)
* **Clear custom blocklist** — drop all manually added/removed domains
* **Enable AdBlocker** — master on/off

To use it as your DNS server, enter its IPv4 address in place of the current DNS server IPs in your router / devices. The blocklist is re-downloaded daily (default 04:00, see `alarmHour`).

## ESP32-C3 (no PSRAM)

The C3 has no PSRAM and about 400KB of SRAM, so the blocklist cannot be held in RAM the way the ESP32 and ESP32-S3 builds do. On the C3 the blocklist is stored in the flash data partition as a sorted array of 64-bit FNV-1a domain hashes and every DNS query binary searches that file. See `blocklistC3.cpp`.

Differences from the PSRAM build:

* Only hashes are stored, not the domain names. **Add Domain**, **Del Domain** and **Check Domain** all still work; the *showBL* debug listing prints hashes.
* `useSnap` and `minMemory` have no effect — the hash file is already the persistent copy, so a reboot serves the previous list immediately.
* **OTA Upload is not available**: the 4MB flash is split into a 2MB application and a 1.875MB blocklist partition, so there is no second application slot. Update over USB instead.
* The first load is slow (several minutes): the list is downloaded over TLS, hashed, then sorted in flash in several passes. Progress is shown on the web page as `Sort 12%` and so on.

## LED status

An onboard LED can signal status. Configure it via **Settings** (pin, active high/low, simple vs WS2812, brightness).

| State | Simple LED |
|---|---|
| AP mode | slow blink |
| Active | steady |
| Offline (serving cached list) | very slow blink |
| Failure | fast blink |
| Downloading hosts | medium blink |

Set the pin to 0 to disable the LED.

## Logging

Log messages are on the web page tab **Show Log**. The **Verbose** button reveals per-query logging. On C3 with USB-CDC-On-Boot, the log is also on the native USB serial at 115200.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `invalid header: 0x...` at boot | module has flash encryption enabled, image written plaintext | reflash with `tools/flash.sh PORT --encrypt` |
| `Active security features detected` when erasing | flash encryption / secure boot eFuses set | expected; write with `--encrypt`, never force an erase |
| `Failed to connect ... No serial data received` | not in download mode | use `--usb`, or hold BOOT while plugging in |
| No `ESP32_AdBlocker_...` AP after flashing | storage/partition or boot failure | watch serial at 115200; reflash with the matching partition scheme |
| `Startup Failure: Crash loop detected` after repeated resets | RTC flag from interrupted setup | fixed in this fork; a full power cycle also clears it |

## Credits

Based on [s60sc/ESP32_AdBlocker](https://github.com/s60sc/ESP32_AdBlocker), which credits [esphole](https://github.com/rubfi/esphole) for the original idea.
