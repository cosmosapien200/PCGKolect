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
│                              │  ┌────────▼────────┐  │              │
│                              │  │ 32→16-bit      │  │              │
│                              │  │  Downconversion │  │              │
│                              │  │ (>> 16, MSBs)   │  │              │
│                              │  └────────┬────────┘  │              │
│                              │           │           │              │
│                              │  ┌────────▼────────┐  │              │
│                              │  │  USB Audio     │  │              │
│                              │  │  UAC 1.0 Mic   │  │              │
│                              │  │  (48 kHz, 16b) │  │              │
│                              │  └────────┬────────┘  │              │
│                              │           │           │              │
│                              │  ┌────────▼────────┐  │              │
│                              │  │  UART COM      │  │              │
│                              │  │  Port 1        │  │              │
│                              │  │  115200 baud   │  │              │
│                              │  └────────┬────────┘  │              │
│                              └─────┬─────┴─────┬─────┘              │
│                                    │           │                     │
│                        ┌───────────▼──┐   ┌────▼────────┐           │
│                        │  USB OTG     │   │  UART TX/RX │           │
│                        │  (GPIO 19/20)│   │  (GPIO 17/18)│           │
│                        │  → Host PC   │   │  → RV1103   │           │
│                        └──────────────┘   └─────────────┘           │
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
| `UART_TX` | **17** | UART TX | ESP32→RV1103 | Port 1, 115200 baud, 8N1 |
| `UART_RX` | **18** | UART RX | RV1103→ESP32 | Port 1, 115200 baud, 8N1 |
| `UART_TX_DEBUG` | **43** | TX (debug) | — | Default console UART |
| `UART_RX_DEBUG` | **44** | RX (debug) | — | Default console UART |

### UART Communication with RV1103

- **Port**: `UART_NUM_1`
- **Baud Rate**: 115200
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
    ├── usb_audio.c / usb_audio.h  # USB UAC device wrapper
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

**Important**: The ESP-IDF I2S driver uses internal DMA buffering — no software ring buffer is needed (discovered during development).

### 2. USB Audio UAC Device (`usb_audio.c`)

**Role**: Wraps the `espressif/usb_device_uac` component to stream microphone audio to the host PC.

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
2. Reads 960 bytes (256 × int32 samples) from I2S via `pcm1803_read()`
3. Converts 32-bit → 16-bit by right-shifting 16 bits (`>> 16`, taking MSBs)
4. Handles stereo by reading pairs of samples (left channel only currently)
5. Fills USB buffer with int16_t samples; zeros on read failure

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
| `usb_audio_init()` | Initializes UAC device with callbacks |
| `usb_audio_get_callback_count()` | Returns USB input callback count (debugging) |
| `usb_audio_reset_callback_count()` | Resets callback counter |

### 3. UART Communication (`uart_com.c`)

**Role**: Bi-directional serial link to the RV1103 co-processor.

**Configuration**:
```c
UART_NUM_1, TX=GPIO17, RX=GPIO18, 115200 baud, 8N1
```

**API**:
| Function | Description |
|----------|-------------|
| `uart_com_init()` | Installs UART driver, configures pins |
| `uart_com_send(data, len)` | Sends raw bytes, flushes TX buffer |
| `uart_com_send_string(str)` | Sends null-terminated string |

**RV1103 Protocol**: Not yet defined. The RV1103 firmware is a separate project connected via this UART link.

### 4. Main Application (`main.c`)

```c
void app_main(void) {
    pcm1803_init();     // I2S RX → PCM1803A
    usb_audio_init();   // USB UAC MIC device
    uart_com_init();    // UART → RV1103

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uart_com_send_string("Hello World\r\n");
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
3. **No software ring buffer needed**: ESP-IDF I2S DMA buffering is sufficient for this use case.

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
| RV1103 protocol definition | 📋 Planned | UART communication protocol not yet specified |
| Data logging / storage | 📋 Planned | Possibly via RV1103 or future SD card |
| BLE/Wi-Fi telemetry | 📋 Planned | Currently disabled in sdkconfig |
| PSRAM support | 📋 Planned | Not enabled in current sdkconfig |
| I2C peripherals | 📋 Planned | GPIO 21/22 available (e.g., sensors, EEPROM) |
| TDM audio mode | 📋 Planned | ESP32-S3 supports TDM on I2S0/I2S1 |

---

## ⚠️ Known Constraints & Gotchas

1. **No PSRAM**: Heap allocations must fit in internal SRAM (~256 KB).
2. **GPIO 5 conflict**: Shared between I2S DIN and SPI CLK, but SPI flash is accessed internally by boot ROM — no user conflict.
3. **USB Audio bit depth**: PCM1803A outputs 32-bit slots with 24-bit valid data. Currently downconverted to 16-bit for USB. If 24-bit USB audio is needed, update both `usb_audio.c` and sdkconfig (`CONFIG_UAC_BIT_RESOLUTION_24_IN_32`).
4. **Mono vs Stereo**: I2S is configured as stereo, but USB UAC is configured as 1-channel MIC. The `usb_uac_device_input_cb` currently reads stereo pairs but only uses the left channel.
5. **printf in callback**: The `usb_uac_device_input_cb` still contains a `printf()` for debugging — should be removed or replaced with `ESP_LOGD` in production (and guarded by log level).
6. **No error handling in main loop**: `app_main()` has no recovery logic if I2S or USB init fails.
7. **UART buffer sizes**: Both RX and TX buffers are 0 (default 256 bytes). May need increasing for larger payloads to/from RV1103.
8. **ESP-IDF v6.x UART API**: `uart_port_handle_t`, `uart_get_handle()`, and `uart_flush_output()` were removed. All UART functions now take `uart_port_t` directly. See the API Migration Notes section above.

---

## 🧪 Testing Checklist

- [ ] Audio streams to host PC as USB microphone (verify with Audacity or similar)
- [ ] Heart sounds captured with sufficient SNR after front-end tuning
- [ ] No audio dropouts or glitches during extended recording
- [ ] UART communication with RV1103 sends/receives correctly
- [ ] No WDT resets or memory leaks during extended operation
- [ ] Boot time and startup sequence are reliable

---

*Last updated: 2026-10-05*
