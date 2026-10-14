# PCGKolect
(test firmware)
*Collecting heartbeats, one sample at a time.* ❤️🩺

PCGKolect is an open-source firmware project for collecting high-quality heart sound recordings using a custom electronic stethoscope platform.

The system is built around an **ESP32-S3** and a **PCM1803A audio ADC**, with a custom analog front-end designed for heart sound acquisition. A **piezoelectric sensor** is used as the primary sensing element to capture acoustic vibrations from the chest wall, which are then amplified, digitized, and streamed to a host computer for recording and analysis.

The primary goal of PCGKolect is to provide an affordable and reproducible platform for:

* Heart sound dataset collection
* Electronic stethoscope research
* Biomedical signal processing
* Machine learning and AI applications involving PCG signals

---

## Current Development Status

🚧 **Early Development Phase**

The hardware, firmware, and data collection pipeline are currently under active development and experimentation.

![PCGKolect Development Prototype](docs/media/dev_setup1.jpg)

---

## What the firmware does

```
PCM1803A ─I²S 48 kHz─► ESP32-S3 ─┬─ USB Audio (UAC, 48 kHz)               → PC recording
                                  ├─ UART1 4 Mbaud (GPIO17/18) ◄─────────► RV1103 (quality model, recordings)
                                  ├─ BLE GATT control service            ◄─► PCGKolect phone app
                                  └─ Wi-Fi station on the phone hotspot  ─► HTTP file transfer to the app
```

- **Audio**: one I²S producer, a shared ring buffer (PSRAM) and consumers for USB (48 kHz) and
  the RV1103 (8 kHz, 100 ms CRC-framed packets).
- **Link to the RV1103** (`link.c`): the audio frames plus typed message frames
  (request/response/event/file chunk) on the same duplex UART, negotiated up to 4 Mbaud with
  automatic fallback. Audio/link tasks and the UART ISR run on core 1, Wi-Fi/BLE on core 0.
- **BLE** (`ble.c`): bonded, encrypted control service for the app (commands, live telemetry,
  hotspot provisioning); commands are forwarded to the RV1103, which owns recording state.
- **Wi-Fi + HTTP** (`wifi_sta.c`, `http_api.c`): joins the phone's hotspot and serves recordings
  from the RV1103 with a per-session bearer token, Range resume and SHA-256 headers.
- **Debug console** on UART0 (`console.c`, driven by `tools/esp_console.py`) and a desk BLE client
  (`tools/ble_client.py`).

Protocols: [PCGKolect_app/docs/PROTOCOL.md](https://github.com/cosmosapien200/PCGKolect_app/blob/main/docs/PROTOCOL.md).
Related repos: [PCGKolect_rk](https://github.com/cosmosapien200/PCGKolect_rk) (RV1103),
[PCGKolect_model](https://github.com/cosmosapien200/PCGKolect_model) (quality model),
[PCGKolect_app](https://github.com/cosmosapien200/PCGKolect_app) (phone app).

---

## Hardware Overview

* ESP32-S3 microcontroller
* PCM1803A stereo audio ADC
* Custom analog signal conditioning front-end
* Piezoelectric sensor for heart sound acquisition
* USB audio streaming interface

---

## Building

### Requirements

* ESP-IDF
* ESP32-S3 toolchain
* Python

### Build

```bash
idf.py build
```

### Flash

```bash
idf.py flash
```

### Monitor

```bash
idf.py monitor
```

---

## License

MIT License
