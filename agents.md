# PCGKolect — Agent Development Guide

## 📋 Project Overview

**PCGKolect** is an open-source firmware project for collecting high-quality heart sound (PCG — phonocardiogram) recordings using a custom electronic stethoscope platform built around an **ESP32-S3**.

The firmware captures audio from a **PCM1803A stereo ADC** (fed by a piezoelectric sensor + custom analog front-end), converts it to USB Audio Class (UAC 1.0) format, and streams it to a host PC over native USB OTG. A secondary **RV1103** co-processor communicates via UART for additional processing/storage tasks.

> **Goal**: Provide an affordable, reproducible platform for heart sound dataset collection, e-stethoscope research, biomedical signal processing, and ML/AI applications.

---

## 🏗️ System Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        PCGKolect Platform                           │
│                                                                     │
│  ┌──────────────┐    I2S (RX)    ┌──────────────┐                  │
│  │ Piezoelectric │ ──────────────→│  PCM1803A    │                  │
│  │   Sensor      │   GPIO 1/2/4/5 │  ADC (Master)│                  │
│  └──────────────┘                  │  48 kHz,     │                  │
│                                   │  32-bit,     │                  │
│                                   │  Stereo      │                  │
│                                   └──────┬───────┘                  │
│                                          │                          │
│                              ┌───────────▼───────────┐              │
│                              │     ESP32-S3          │              │
│                              │     (240 MHz)         │              │
│                              │                       │              │
│                              │  ┌─────────────────┐  │              │
│                              │  │ I2S RX Channel  │  │              │
│                              │  │ DMA → 32-bit    │  │              │
│                              │  │ int32_t samples │  │              │
│                              │  └────────┬────────┘  │              │
│                              │           │           │              │
│                                 ┌────────▼────────┐                 │
│                                 │ Audio Abstract. │                 │
│                                 │  drop R channel │                 │
│                                 │  decimate rate  │                 │
│                                 │   24/16/8-bit   │                 │
│                                 └────────┬────────┘                 │
│                                    ┌─────┴─────┐                    │
│                                ┌───────┐   ┌───────┐                │
│                                │  USB  │   │  UART │                │
│                                │48k/16b│   │ 8k/16b│                │
│                                └───┬───┘   └───┬───┘                │
│                                    │           │                    │
│                              └─────┬───────────┬─────┘              │
│                                    │           │                    │
│                               ┌────▼────┐ ┌────▼────┐               │
│                               │ USB OTG │ │   UART  │               │
│                               │ (19/20) │ │ (17/18) │               │
│                               │ Host PC │ │  RV1103 │               │
│                               └─────────┘ └─────────┘               │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 🔌 Pin Assignments

All pins are documented in **`PINS.md`** and must be updated whenever pins change.

### Actively Used Pins

| Pin | GPIO | Function | Direction | Notes |
|-----|------|----------|-----------|-------|
| `mclk` | **1** | Master Clock | ESP32→PCM1803A | 512×48kHz = 24.576 MHz |
| `bclk` | **2** | Bit Clock (SCK) | ESP32→PCM1803A | 48kHz × 32bit × 2ch = 3.072 MHz |
| `ws` | **4** | Word Select (LRCK) | ESP32→PCM1803A | 48 kHz |
| `din` | **5** | Data In | PCM1803A→ESP32 | 32-bit stereo audio data |
| `dout` | — | Data Out | — | Not used (`I2S_GPIO_UNUSED`) |
| `USB_DP` | **19** | USB Data+ | — | Native USB OTG (handled by ESP-IDF) |
| `USB_DM` | **20** | USB Data- | — | Native USB OTG (handled by ESP-IDF) |
| `UART_TX` | **17** | UART TX | ESP32→RV1103 | Port 1, 460800 baud, 8N1 |
| `UART_RX` | **18** | UART RX | RV1103→ESP32 | Port 1, 460800 baud, 8N1 |
| `UART_TX_DEBUG` | **43** | TX (debug) | — | Default console UART |
| `UART_RX_DEBUG` | **44** | RX (debug) | — | Default console UART |

### UART Communication with RV1103

- **Port**: `UART_NUM_1`
- **Baud Rate**: 460800
- **Config**: 8 data bits, no parity, 1 stop bit, no flow control
- **Source Clock**: `UART_SCLK_DEFAULT`
- **Direction**: Full-duplex (TX on GPIO 17, RX on GPIO 18)
- **Purpose**: Bi-directional communication with RV1103 co-processor (firmware running on separate SoC)
- **Current State**: ESP32 sends "Hello World" every 1 second; RV1103 protocol not yet defined

### Unused / Available Pins

| GPIO | Function / Notes |
|------|-----------------|
| 0 | ⚠️ Boot mode select (pull-up required on boot) |
| 3 | SPI Flash CS (internal, not user-accessible) |
| 6 | SPI Flash MOSI (internal) |
| 7 | SPI Flash MISO (internal) |
| 8 | SPI CLK (internal) |
| 9–18 | SDMMC pins (unused) |
| **21** | **I2C SDA** — available |
| **22** | **I2C SCL** — available |
| 33–42 | General purpose (34–39 input-only) |
| 45–46 | General purpose |

> **⚠️ GPIO 0** must be pulled HIGH during normal boot. Avoid using it unless you handle boot strap circuitry.

---

## 📦 Software Stack

| Component | Version / Detail |
|-----------|-----------------|
| **MCU** | ESP32-S3 (Xtensa dual-core, 240 MHz) |
| **Framework** | ESP-IDF v6.2.0 |
| **Flash** | 2 MB, DIO mode, 80 MHz |
| **Xtal** | 40 MHz |
| **PSRAM** | Not enabled |
| **USB** | Native OTG, FS (full speed) |
| **USB Audio Component** | `espressif/usb_device_uac` (UAC 1.0) |
| **USB VID/PID** | `0x303A` / `0x8000` (Espressif) |
| **USB Product** | "ESP_USB_AUDIO" |
| **Console** | UART0 (GPIO 43/44) + USB Serial JTAG (secondary) |
| **RTOS** | FreeRTOS (preemptive, SMP disabled) |

---

## 📂 Codebase Structure

```
pcg-fw/
├── CMakeLists.txt              # Project-level CMake (includes ESP-IDF project.cmake)
├── sdkconfig                   # ESP-IDF menuconfig output (auto-generated)
└── main/
    ├── CMakeLists.txt          # Component CMake (lists SRCS, PRIV_REQUIRES)
    ├── idf_component.yml       # Component dependencies (usb_device_uac)
    ├── main.c                  # Entry point — app_main()
    ├── pcm1803.c / pcm1803.h   # I2S RX driver for PCM1803A ADC
    ├── audio.c / audio.h       # Audio abstraction: L-channel, resample, bit depth
    ├── usb_audio.c / usb_audio.h  # USB UAC device wrapper (consumer of audio)
    ├── uart_audio.c / uart_audio.h # 8 kHz/16-bit PCG stream to RV1103 (consumer)
    ├── uart_com.c / uart_com.h   # UART communication with RV1103
    ├── pcg-fw.c                # Placeholder / alternate entry (empty)
    └── main.old.c              # Historical monolithic version (reference)
```

---

## 🔧 Core Modules

### 1. PCM1803A I2S Driver (`pcm1803.c`)

**Role**: Configures ESP32-S3 I2S peripheral as a **slave receiver** to read 32-bit stereo audio from the PCM1803A ADC.

**Key Configuration**:
```c
// Channel: I2S_NUM_AUTO, ROLE_SLAVE
// DMA: 480 frames × 2 descriptors = 2 × 480 × 2ch × 4bytes = 7680 bytes total
// Sample Rate: 48,000 Hz
// MCLK Multiple: I2S_MCLK_MULTIPLE_512  (→ 24.576 MHz for PCM1803A 512× fS mode)
// Slot: 32-bit wide, Stereo, Philips format
// GPIO: MCLK=1, BCLK=2, WS=4, DIN=5, DOUT=UNUSED
```

**API**:
| Function | Description |
|----------|-------------|
| `pcm1803_init()` | Initializes I2S RX channel, returns `ESP_OK` on success |
| `pcm1803_get_rx_handle()` | Returns the I2S channel handle (`i2s_chan_handle_t`) |
| `pcm1803_read(buf, size, &bytes_read)` | Blocking read from I2S (uses `portMAX_DELAY`) |

**Important**: The ESP-IDF I2S driver provides internal DMA buffering, so `pcm1803_read()` blocks until a full DMA block is available. The `audio` module adds a *software* ring buffer on top so several consumers at different rates can share the stream (see below).

### 2. USB Audio UAC Device (`usb_audio.c`)

**Role**: Wraps the `espressif/usb_device_uac` component to stream microphone audio to the host PC. It is a thin consumer of the `audio` module — it no longer touches the I2S peripheral directly.

**Key Configuration** (from sdkconfig):
```
UAC_MIC_CHANNEL_NUM     = 1
UAC_SAMPLE_RATE         = 48000 Hz
UAC_BIT_RESOLUTION      = 16 bits
UAC_BYTES_PER_SAMPLE    = 2
UAC_MIC_INTERVAL_MS     = 10 ms
```

**Data Pipeline in `usb_uac_device_input_cb`**:
1. Called by TinyUSB task when host requests audio data
2. Opens a **48 kHz / 16-bit mono** stream via `audio_setup()` during `usb_audio_init()`
3. Calls `audio_read()` for `len` bytes; the `audio` module handles L-channel selection, resampling and bit-depth conversion
4. Pads with silence if the read ever underruns, so the host never sees a short packet

**Callbacks Required by UAC Component**:
| Callback | Purpose |
|----------|---------|
| `usb_uac_device_input_cb` | Host requests audio data (MIC) — **implemented** |
| `usb_uac_device_output_cb` | Host sends audio data (SPEAKER) — **stub (no-op)** |
| `usb_uac_device_set_mute_cb` | Host sets mute — **stub** |
| `usb_uac_device_set_volume_cb` | Host sets volume — **stub** |

**API**:
| Function | Description |
|----------|-------------|
| `usb_audio_init()` | Opens the 48 kHz/16-bit stream and initializes the UAC device |
| `usb_audio_get_callback_count()` | Returns USB input callback count (debugging) |
| `usb_audio_reset_callback_count()` | Resets callback counter |

### 3. Audio Abstraction Layer (`audio.c`)

**Role**: Sits between the PCM1803A driver and all consumers (USB audio, future UART audio). It is the single owner of the I2S read path and is responsible for:
1. **Dropping the right channel** — only the left channel is kept
2. **Resampling** the 48 kHz source down to a consumer-requested rate (box-average decimation)
3. **Bit-depth conversion** — 24-bit PCM → 24 / 16 / 8-bit signed samples

A single **producer task** reads the PCM1803A, converts each 32-bit slot to canonical 24-bit (`raw >> 8`) and writes the left channel into a 16384-sample ring buffer. Each `audio_stream_t` consumer has its own read cursor + decimation factor, so multiple rates can share one hardware stream (e.g. USB at 48 kHz/16-bit while UART reads 8 kHz/16-bit).

**API**:
| Function | Description |
|----------|-------------|
| `audio_init()` | Idempotent global init: `pcm1803_init()` + ring + producer task |
| `audio_setup(cfg)` | Opens a stream at `cfg.sample_rate` / `cfg.bits`; returns `audio_stream_t` (NULL on error) |
| `audio_read(s, buf, size, &bytes_read)` | Blocking read of mono samples in the stream's format |
| `audio_stream_close(s)` | Releases a stream |

**Sample conversion** (canonical `s24 = i2s_raw >> 8`):

| Output | Conversion | Container |
|--------|-----------|-----------|
| 24-bit | `s24` | `int32_t` (sign-extended) |
| 16-bit | `s24 >> 8` | `int16_t` |
| 8-bit  | `s24 >> 16` | `int8_t` |

**Notes**:
- Requested `sample_rate` must divide 48000 evenly; `decim = 48000 / sample_rate`.
- Decimation uses a box average over `decim` canonical samples (basic anti-aliasing).
- Overrun (consumer too slow) is detected via monotonic sample counters; the stream resyncs to the newest data and logs a warning once.
- `AUDIO_RING_SAMPLES = 16384` (~340 ms at 48 kHz, 64 KB internal SRAM).

### 4. UART Audio (`uart_audio.c`)

**Role**: Streams mono 8 kHz / 16-bit PCG audio to the RV1103 over UART1. It is a consumer of the `audio` abstraction and the sender half of the UART PCG protocol.

**Frame format** (~10 frames/s, `docs/uart_pcg_protocol.md`):

```
A5 5A | LEN(2, LE) | SEQ(1) | PCM(int16 LE, LEN bytes) | CRC16(2, LE)
LEN = 1600 (800 samples), total 1607 B
CRC16-CCITT-FALSE (0x1021, init 0xFFFF) over LEN+SEQ+PCM
```

**API**:
| Function | Description |
|----------|-------------|
| `uart_audio_init()` | Opens `{8000, 16-bit}` stream, runs protocol self-test, starts the sender task |
| `uart_audio_get_stats(&st)` | `frames_sent`, `read_errors`, `write_errors` |

**Behaviour**:
- The task blocks in `audio_read()` for exactly 800 samples (100 ms), builds a 1607-byte frame, and sends it via `uart_com_write()`.
- `SEQ` increments per frame and wraps at 256.
- Buffers (`s_pcm[800]`, `s_frame[1607]`) are static, not on the task stack.
- A boot self-test validates the CRC and small-frame test vectors (`protocol self-test: PASS`).

**Timing note**: a 1607-byte frame takes ~35 ms to clock out at 460800. `UART_COM_TX_BUFFER` is 4096 B so `uart_write_bytes()` queues the whole frame and returns immediately; otherwise the task would stretch past 100 ms and the 8 kHz stream would overrun the ring.

### 5. UART Communication (`uart_com.c`)

**Role**: Bi-directional serial link to the RV1103 co-processor.

**Configuration**:
```c
UART_NUM_1, TX=GPIO17, RX=GPIO18, 460800 baud, 8N1
RX buffer = 256 B, TX buffer = 4096 B
```

**API**:
| Function | Description |
|----------|-------------|
| `uart_com_init()` | Installs UART driver, configures pins |
| `uart_com_write(data, len)` | Raw `uart_write_bytes()`, no flush — used for audio frames |
| `uart_com_send(data, len)` | Sends raw bytes then `uart_flush()` (debug/text) |
| `uart_com_send_string(str)` | Sends null-terminated string |

**RV1103 Protocol**: Defined in `docs/uart_pcg_protocol.md` (framed 8 kHz/16-bit PCG). The RV1103 firmware (`PcgStream` / `PcgPreprocessor`) is a separate project consuming this link.

### 6. Main Application (`main.c`)

```c
void app_main(void) {
    usb_audio_init();    // opens 48k/16b stream → brings up pcm1803 via audio_init()
    uart_com_init();     // UART → RV1103
    uart_audio_init();   // opens 8k/16b stream and starts streaming frames

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        // log uart_audio_get_stats()
    }
}
```

---

## 🔑 Verified Parameters (from DEVLOG.md)

| Parameter | Value |
|-----------|-------|
| ADC | PCM1803A |
| ADC Mode | Master (512× fS) |
| Sample Rate | 48 kHz |
| USB Audio Rate | 48 kHz |
| USB Bit Resolution | 16-bit |
| I2S Data Width | 32-bit |
| DMA Frames | 480 |
| DMA Descriptors | 2 |
| DMA Buffer Size | 7,680 bytes per descriptor |

### Lessons Learned (from DEVLOG.md)

1. **MCLK multiplier matters**: PCM1803A needs 512× fS but ESP-IDF default is 256×. Mismatch causes 24 kHz instead of 48 kHz.
2. **DMA block size > total DMA size**: `i2s_channel_read()` returns one DMA block at a time. Block size must match USB callback expectations (480 frames).
3. **Software ring buffer**: The audio layer adds a fan-out ring (16384 canonical samples, 64 KB internal SRAM) so multiple consumers can read at different rates; I2S DMA alone cannot fan out.

---

## 🛠️ Build & Flash

```bash
cd pcg-fw
idf.py build        # Compile
idf.py flash        # Flash to device
idf.py monitor      # Serial monitor (GPIO 43/44, 115200 baud)
```

**Alternative**: Use USB Serial JTAG as secondary console (enabled in sdkconfig).

---

## 📚 ESP-IDF Documentation References

**Use the offline docs** — they match your exact ESP-IDF version and are faster to read:

> **Local docs path**: `/home/lokesh/.espressif/esp-idf/docs/en/api-reference/`

| Topic | Local Path |
|-------|-----------|
| **I2S (Standard Mode)** | `peripherals/i2s.html` |
| **UART Driver** | `peripherals/uart.html` |
| **GPIO** | `peripherals/gpio.html` |
| **Clock Tree** | `peripherals/clk_tree.html` |
| **USB Device (esp-usb)** | `N/A — hosted only: https://docs.espressif.com/projects/esp-usb/en/latest/esp32/usb_device.html` |
| **FreeRTOS** | `system/freertos.html` |
| **ESP-IDF Kconfig Reference** | `kconfig-reference.html` |

**Quick lookup**: Open the docs directory and navigate to the relevant subsection, e.g.:
```
/home/lokesh/.espressif/esp-idf/docs/en/api-reference/peripherals/uart.html
```

---

## ⚡ ESP-IDF v6.x API Migration Notes

When upgrading from ESP-IDF v5.x to v6.x, watch for these breaking changes that affect this codebase:

### UART Driver (v5.x → v6.x)

| v5.x API | v6.x API | Notes |
|----------|----------|-------|
| `uart_port_handle_t` | ❌ Removed | Use `uart_port_t` (just `UART_NUM_0/1/2`) directly |
| `uart_get_handle(port, &handle)` | ❌ Removed | No longer needed — APIs take `uart_port_t` directly |
| `uart_flush_output(port)` | `uart_flush(port)` | Renamed (alias of `uart_flush_input`) |
| `uart_write_bytes(port, data, len)` | ✅ Same | Still works, but `port` is `uart_port_t` not a handle |

**Pattern in v6.x**: All UART functions take `uart_port_t` (enum: `UART_NUM_0`, `UART_NUM_1`, `UART_NUM_2`) directly. No handle objects.

### General v6.x Pattern

Many ESP-IDF v6.x drivers moved from a **handle-based** API to a **port/number-based** API. When in doubt:
1. Check the local docs at `/home/lokesh/.espressif/esp-idf/docs/en/api-reference/`
2. Check the header files in `/home/lokesh/.espressif/esp-idf/components/esp_driver_*/include/driver/`
3. Look for `*_handle_t` types — if they're gone, the API likely now uses port numbers directly

---

## 📝 PINS.md Maintenance

**`PINS.md`** is the single source of truth for all GPIO assignments. Agents must:

1. **Update `PINS.md`** whenever a new pin is assigned or an existing pin is changed.
2. **Keep sections current**:
   - ✅ **Actively Used Pins** — add/remove pins as hardware/firmware evolves
   - ✅ **Default / Boot Pins** — update if conflicts arise
   - ✅ **Unused / Available Pins** — mark pins as "in use" when allocated
   - ✅ **Pin Usage Diagram** — keep ASCII diagram in sync
3. **Note conflicts** explicitly (e.g., shared pins, boot strapping constraints).
4. **Reference source files** in tables (e.g., "Defined in: `pcm1803.c`").

---

## 🔮 Future / Planned Features

| Feature | Status | Notes |
|---------|--------|-------|
| Heart sound front-end tuning | 🚧 In Progress | Analog gain, filtering, sensor coupling |
| RV1103 protocol definition | ✅ Done | Framed 8 kHz/16-bit PCG — see `docs/uart_pcg_protocol.md` |
| Data logging / storage | 📋 Planned | Possibly via RV1103 or future SD card |
| BLE/Wi-Fi telemetry | 📋 Planned | Currently disabled in sdkconfig |
| PSRAM support | 📋 Planned | Not enabled in current sdkconfig |
| I2C peripherals | 📋 Planned | GPIO 21/22 available (e.g., sensors, EEPROM) |
| TDM audio mode | 📋 Planned | ESP32-S3 supports TDM on I2S0/I2S1 |

---

## ⚠️ Known Constraints & Gotchas

1. **No PSRAM**: Heap allocations must fit in internal SRAM (~256 KB).
2. **GPIO 5 conflict**: Shared between I2S DIN and SPI CLK, but SPI flash is accessed internally by boot ROM — no user conflict.
3. **USB Audio bit depth**: PCM1803A outputs 32-bit slots with 24-bit valid data. The `audio` layer can emit 24/16/8-bit, but USB is currently 16-bit. If 24-bit USB audio is needed, open the stream with `AUDIO_BITS_24` and update sdkconfig (`CONFIG_UAC_BIT_RESOLUTION_24_IN_32`).
4. **Mono vs Stereo**: I2S is configured as stereo; the `audio` layer keeps only the left channel and presents a mono stream to consumers. USB UAC is 1-channel MIC, matching.
5. **Blocking reads**: `audio_read()` blocks until it can fill the requested samples. A stalled producer (I2S not clocked) will block the caller — there is a 100 ms periodic re-check but no hard timeout.
6. **Producer priority**: The `audio_producer` task runs at priority 6 (above the TinyUSB task at 5) so it can keep the ring fed.
7. **No error handling in main loop**: `app_main()` has no recovery logic if audio/USB/UART init fails.
8. **UART buffer sizes**: RX buffer is 256 B, TX buffer is 4096 B. The TX buffer must hold a full `uart_audio` frame (1607 B) so `uart_com_write()` never blocks; reducing it below a frame size will stall the 8 kHz stream and overrun the audio ring.
9. **UART framing vs text**: `uart_audio` emits binary frames continuously, so `main.c` must not interleave text (`uart_com_send_string`) on the same port — it would corrupt the stream. The old "Hello World" loop was removed.
10. **ESP-IDF v6.x UART API**: `uart_port_handle_t`, `uart_get_handle()`, and `uart_flush_output()` were removed. All UART functions now take `uart_port_t` directly. See the API Migration Notes section above.

---

## 🧪 Testing Checklist

- [ ] Audio streams to host PC as USB microphone (verify with Audacity or similar)
- [ ] Heart sounds captured with sufficient SNR after front-end tuning
- [ ] No audio dropouts or glitches during extended recording
- [x] UART PCG stream to RV1103 sends at 8 kHz/16-bit (~10 frames/s, no errors) — verified on-device
- [x] RV1103 `PcgStream` decodes frames end-to-end with correct CRC — verified 2026-10-11
- [ ] No WDT resets or memory leaks during extended operation
- [ ] Boot time and startup sequence are reliable

---

*Last updated: 2026-10-11*
