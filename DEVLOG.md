# Development Log

## 2026-10-11 — End-to-End PCG Link Verified; RV1103 Receiver Fixes

### Milestone

* Verified the ESP32 → RV1103 8 kHz/16-bit PCG link **end to end**: the receiver
  (`PCGKolect_rk` `PcgStream`) now decodes ~10 frames/s with `crc_errors == 0`.
* Fixed two bugs in the RV1103 receiver that had blocked decoding:
  1. **Baud:** `cfsetispeed`/`cfsetospeed` were given the number `460800`
     instead of the termios constant `B460800`, leaving the port at 921600.
  2. **Framing off-by-one:** the parser consumed only the first sync byte
     `0xA5`, misreading the second (`0x5A`) as `LEN` lo, so conforming frames
     could never decode.

### Verification

* Before: `frames=0 crc_err=0 bytes≈23,450 B/s`, `/proc/tty/driver/serial`
  framing errors climbing.
* After: `stty` reports `460800 baud`; `bytes` deltas exactly `21 × 1607`;
  rate `16,025 B/s`; `fe`/`brk` unchanged; 1 CRC error over 308 frames
  (correlated with a UART overrun `oe`, self-recovering).
* No change needed on the ESP32 sender.

### Full report

* `PCGKolect_rk/docs/uart_receiver_fix_report.md`

---

## 2026-10-11 — UART PCG Streaming to RV1103 (`uart_audio`)

### Milestone

* Added `uart_audio.c` / `uart_audio.h`: streams mono **8 kHz / 16-bit** PCG
  audio to the RV1103 over UART1 as framed packets, ~10 frames/s.
* Added `uart_com_write()` (raw `uart_write_bytes()`, no flush) and enlarged the
  UART TX ring buffer to 4096 B.
* Bumped the audio ring buffer from 8192 → 16384 canonical samples (64 KB
  internal SRAM, ~340 ms at 48 kHz) for extra margin under the 8 kHz consumer.
* Documented the shared wire protocol in `docs/uart_pcg_protocol.md`.

---

### Motivation

The RV1103 receiver (`PcgStream`) expects a self-synchronising, CRC-checked
frame stream rather than raw PCM, so the ESP32 sender must emit that exact
format. The sender is a consumer of the `audio` abstraction, opening an 8 kHz /
16-bit stream while USB continues at 48 kHz / 16-bit.

---

### Protocol (sender side)

```
A5 5A | LEN(2, LE) | SEQ(1) | PCM(int16 LE, LEN bytes) | CRC16(2, LE)
LEN = 1600 (800 samples), total 1607 B, ~10 frames/s
CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF) over LEN+SEQ+PCM, stored LE
```

* Sync `0xA5 0x5A`; `SEQ` wraps at 256; PCM is mono little-endian `int16`.
* Bandwidth: 8000 × 2 = 16,000 B/s vs ~46,080 B/s at 460800 (≈35 %).

---

### Implementation notes

* The sender task blocks in `audio_read()` for exactly 800 samples (100 ms),
  builds a 1607-byte frame into a static buffer, and calls `uart_com_write()`.
* **TX buffer sizing matters**: a frame takes ~35 ms on the wire at 460800. With
  a 256-byte TX buffer `uart_write_bytes()` would block ~35 ms, stretching the
  cadence past 100 ms and overrunning the ring. At 4096 B the write queues the
  whole frame and returns immediately; the driver drains it in the background.
* `main.c` no longer sends `"Hello World"` on UART1 — interleaving text would
  corrupt the binary stream. It now logs `uart_audio` stats once per second.
* A boot self-test validates the protocol test vectors (CRC `0x29B1`,
  small-frame `A5 5A 04 00 01 01 00 02 00 8F 8A`) and logs `PASS`.

---

### Verification

* Host test (`/tmp/opencode/uart_frame_test.c`): CRC known-answer, small-frame
  byte-for-byte, and standard 1607-byte silence frame (`CRC = 0x5E22`) all pass.
* On-device (ESP32-S3, `/dev/ttyACM0`):
  * `UART_AUDIO: protocol self-test: PASS`
  * `stream open: 8000 Hz, 16-bit, decim=6`
  * `frames` incremented **exactly 10/s** (10 → 140 over 13 s) = 8000 samples/s
  * `read_err=0`, `write_err=0`; no WDT; USB stream unaffected.
* Not yet verified on the wire (UART1 GPIO17/18 is not wired to this host) or by
  the RV1103 receiver end-to-end.

---

## 2026-10-11 — Audio Abstraction Layer

### Milestone

* Introduced `audio.c` / `audio.h`, an abstraction layer between the PCM1803A
  I2S driver and its consumers.
* Moved channel selection, resampling and bit-depth conversion out of
  `usb_audio.c` and into the new layer.

---

### Motivation

Previously `usb_uac_device_input_cb` did all the DSP inline: it read the I2S
stream, dropped the right channel, and shifted 32-bit slots down to 16-bit. Any
new consumer (e.g. a future UART audio stream at a different rate) would have
had to duplicate that logic. A dedicated layer centralizes it.

---

### Architecture

* A single **producer task** owns the I2S read path. It reads one DMA block,
  takes the left channel (`raw >> 8` → canonical 24-bit) and writes it into an
  8192-sample ring buffer.
* Consumers open an `audio_stream_t` with `audio_setup(&cfg)` where `cfg`
  carries a **sample rate** and **bit depth**. Each stream has its own read
  cursor and decimation factor, so multiple rates share one hardware stream.
* Decimation is a **box average** over `48000 / sample_rate` samples (basic
  anti-aliasing) — cheap and adequate for heart sounds (< 1 kHz).
* Bit depth conversion: `s24`, `s24 >> 8`, `s24 >> 16` for 24/16/8-bit.

---

### API

```c
esp_err_t      audio_init(void);                    // idempotent; pcm1803 + ring + task
audio_stream_t audio_setup(const audio_config_t *cfg);
esp_err_t      audio_read(audio_stream_t s, void *buf, size_t size, size_t *bytes_read);
void           audio_stream_close(audio_stream_t s);
```

---

### Changes

| File | Change |
| ---- | ------ |
| `main/audio.c`, `main/audio.h` | **New** abstraction layer (ring buffer, producer task, streams) |
| `main/usb_audio.c` | Reduced to a thin `audio_read()` consumer (48 kHz / 16-bit) |
| `main/main.c` | No longer calls `pcm1803_init()` directly |
| `main/pcm1803.h` | Added `PCM1803_DMA_FRAMES` (480) |
| `main/CMakeLists.txt` | Added `audio.c` |

**Result**: Build clean; USB audio output is bit-identical to the previous
`raw >> 16` path at 48 kHz/16-bit. The `audio` layer is now the single place to
add future consumers such as UART audio (8 kHz / 16-bit).

---

### Next

* `uart_audio` module: read an 8 kHz / 16-bit stream and stream it over UART to
  the RV1103.

---

## 2026-09-04 — UAC Audio Pipeline Bring-Up

### Milestone

* End-to-end audio acquisition and USB streaming operational.
* Stable recording from external audio sources achieved.
* Heart sound acquisition still pending front-end tuning.

---

### Codebase Refactor

**Changes**

* Split monolithic firmware into functionality-based modules.
* Separated audio acquisition, USB streaming, drivers, and application logic.

**Result**

* Easier debugging and maintenance.

---

### Discovery: No Additional Ring Buffer Required

**Finding**

* ESP-IDF I2S driver already uses DMA buffering internally.

**Result**

* Removed need for a software ring buffer.
* Simplified audio pipeline.

---

### Issue #1: Recording Duration Incorrect

**Symptom**

* ~38 s recording appeared as ~20 s.

**Root Cause**

* PCM1803A configured for **512× fS Master Mode**.
* ESP-IDF default MCLK multiplier = **256× fS**.
* ADC was effectively running at **24 kHz** instead of **48 kHz**.

| Parameter             | Value           |
| --------------------- | --------------- |
| Requested Sample Rate | 48 kHz          |
| MCLK Multiplier       | 256×            |
| MCLK Output           | 12.288 MHz      |
| PCM1803A Mode         | Master, 512× fS |
| Actual Sample Rate    | 24 kHz          |

**Fix**

* Corrected MCLK multiplier configuration.

**Result**

* PCM1803A operating at 48 kHz as intended.

---

### Issue #2: Fragmented / Discontinuous Audio

**Symptom**

* Audio appeared broken despite correct clocking.

**Investigation Checklist**

✅ PCM1803A sample rate verified

✅ USB sample rate verified (48 kHz)

✅ I2S format verified

❌ DMA block size mismatch found

**Finding**

Original configuration:

```text id="z9crjv"
dma_desc_num  = 6
dma_frame_num = 240
```

Although total DMA capacity was large:

```text id="b58cf9"
240 frames × 6 blocks
```

each `i2s_channel_read()` call returned at most:

```text id="m4wlfw"
240 frames
```

USB pipeline expected:

```text id="m8rztj"
480 frames
```

**Fix**

```text id="1zhwsm"
dma_desc_num  = 2
dma_frame_num = 480
```

Resulting layout:

```text id="pjlwmq"
480 frames × 2 blocks
```

**Result**

* Continuous audio stream.
* Stable USB recording.

---

### Important Verified Parameters

| Item            | Value            |
| --------------- | ---------------- |
| ADC             | PCM1803A         |
| Mode            | Master (512× fS) |
| Sample Rate     | 48 kHz           |
| USB Audio Rate  | 48 kHz           |
| DMA Frames      | 480              |
| DMA Descriptors | 2                |

---

### Lessons Learned

* Total DMA size is less important than DMA block size.
* `i2s_channel_read()` returns one DMA block at a time.
* Verify clock tree before debugging higher-level audio issues.
* ESP-IDF I2S DMA is often sufficient without an additional ring buffer.

---

## 2026-10-14 — RV1103 link v1: typed frames, 4 Mbaud, control plane

Goal: carry control commands, telemetry and recording-file transfer on the
**existing** duplex UART (GPIO17 TX / GPIO18 RX ↔ RV `/dev/ttyS3`) without
breaking the audio stream. Spec: `PCGKolect_app/docs/PROTOCOL.md`.

### What changed
* `uart_com.c` replaced by `link.c`: one TX mutex so whole frames never
  interleave, an RX parser for RV→ESP frames, request/response with ids,
  retries and timeouts, chunk reads, and a supervisor task.
* New message frame `A5 5C | LEN | TYPE | SEQ | DATA | CRC16` alongside the
  unchanged audio frame `A5 5A`. Types: REQ, RSP, EVT, CHUNK.
* `console.c`: debug console on UART0 (`req <json>`, `baud`, `flood`,
  `rbench`, `stats`), driven from the PC by `tools/esp_console.py`.
* Flash size corrected to 16 MB (`esptool flash-id`), custom partition table
  with a 4 MB factory app for the coming BLE + Wi-Fi stack.

### Baud benchmark (audio streaming, RV NPU running)

| Baud | RV→ESP stream | request/response | errors |
|---|---|---|---|
| 460 800 | 40 KB/s | 32 KB/s | 0 |
| 1 500 000 | 129 KB/s | 79 KB/s | 2 audio frames |
| 3 000 000 | 254 KB/s | 100 KB/s | 0 |
| **4 000 000** | **334 KB/s** | **128 KB/s** | **0 in 18 MB soak** |

RV1103 UART3 clocks are exact at 1.5/3/4 Mbaud and 4 Mbaud is exact on the
ESP32-S3 (80 MHz / 20), so 4 Mbaud is the target. Both sides boot at 460800;
the supervisor negotiates up, and both fall back after silence (ESP or RV
reboot) and renegotiate.

### Bugs found on the way
* **Console starvation:** `uart_flush_input()` waits on the driver RX mutex
  that the higher-priority `link_rx` task re-takes in a loop, so the console
  task hung while holding the TX lock and audio stopped. Removed the flush;
  the parser resynchronises by itself.
* **Response replay after reboot:** the RV caches responses by request id;
  after an ESP reset ids restarted at 1 and old answers were replayed. Ids now
  start from a random base per boot.
* Opening `/dev/ttyACM0` resets the ESP (auto-reset circuit); tests that need
  continuity run in one console session.

---

## 2026-10-14 — BLE control service (CP3)

* NimBLE peripheral (`ble.c`), service `7a1c0001-…` with Info / Command / Response /
  Telemetry / Wi-Fi characteristics; JSON messages fragmented with a 1-byte header
  (FIRST/LAST/index) so they work at any MTU (tested at BlueZ's default 23).
* Command characteristic requires an encrypted link (LE Secure Connections, Just Works,
  bonds persisted in NVS); `delete`, `clear`, `wifi_*` additionally require a bonded link.
* Commands are queued from the GATT callback and handled on `ble_cmd`: `hello` merges ESP
  info (device id from the BT MAC, firmware, link state) with the RV `hello`; other ops are
  forwarded over the link with the app's id restored in the answer.
* Telemetry: RV status events are tagged `"src":"rv","link_up":true` and notified at ≤ 5 Hz;
  with the link down the ESP sends its own `LINK_DEGRADED` status every 2 s.
* PSRAM (2 MB, quad) enabled; NimBLE allocates from it.
* `tools/ble_client.py` (bleak): desk test client — pair, run ops, print telemetry.

Verified from a Linux PC: pair + bond, hello/status/manifest, guarded delete rejected by the
RV, unknown op → UNSUPPORTED, telemetry notifications, writes refused before pairing.

---

## 2026-10-14 — Wi-Fi station + local HTTP API (CP4)

* `wifi_sta.c`: station mode on the phone hotspot. `wifi_set` (bonded BLE only) stores the
  credentials in our own NVS namespace (Wi-Fi driver storage = RAM) so the device rejoins
  after reboot; `wifi_forget` erases them. Reconnect with backoff (2 s → 10 s → 30 s) and
  states off/connecting/connected/failed with reasons (NO_AP_FOUND, AUTH_FAILED, …), notified
  on the Wi-Fi characteristic. Modem sleep is kept (required with BLE coexistence).
* HTTP bearer token: 128-bit random, rotated on every BLE connection, returned only in the
  `wifi_status` answer on a bonded link (never in notifications/logs); compared in constant time.
* `http_api.c` (`esp_http_server`, port 8080, one request at a time, priority below audio):
  `/v1/health`, `/v1/device`, `/v1/stats` (RV stats + ESP heap/RSSI/link counters),
  `/v1/recordings` (pages the RV manifest), `/v1/recordings/{id}/{audio|metadata|sidecars/…}`
  streamed in 4 KB link reads with `Content-Length`, `Range` → 206, `X-Content-SHA256`,
  `POST /v1/transfers/{id}/{ack|delete}`, `POST /v1/recordings/clear`. RV statuses map to
  HTTP 400/404/409/502/503/504.

Verified over BLE: token only in bonded answers and rotated per connection, invalid
credentials rejected, absent network → failed/NO_AP_FOUND with retries, forget → off; audio
link unaffected (10 frames/s, no new gaps). HTTP endpoints still need a network test on a
real hotspot/LAN (no Wi-Fi adapter on the dev PC).
