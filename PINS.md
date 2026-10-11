# ESP32 GPIO Pin Assignments

This project targets the **ESP32-S3** and is used as a PCG (phonocardiogram / heart sound) collection platform with a PCM1803A audio ADC and USB audio streaming.

---

## Actively Used Pins

### I2S Pins (PCM1803A Audio ADC — RX/Input)

Defined in: `pcg-fw/main/pcm1803.c`

| Pin Name | ESP32 GPIO | Function |
|----------|-----------|----------|
| `mclk`   | **GPIO 1**  | Master Clock |
| `bclk`   | **GPIO 2**  | Bit Clock (SCK) |
| `ws`     | **GPIO 4**  | Word Select (LRCK) |
| `din`    | **GPIO 5**  | Data In (audio data from ADC) |
| `dout`   | `I2S_GPIO_UNUSED` | Data Out — not used |

### USB Pins (ESP32-S3 Native USB OTG)

These are handled automatically by the ESP-IDF USB stack — no explicit pin configuration in code.

| Pin Name | ESP32 GPIO | Function |
|----------|-----------|----------|
| `USB_DP` | **GPIO 19** | USB Data+ |
| `USB_DM` | **GPIO 20** | USB Data- |

---

## Default / Boot Pins (Not Explicitly Configured)

| Pin Name | ESP32 GPIO | Function |
|----------|-----------|----------|
| `TX`     | **GPIO 43** | UART debug transmit (default) |
| `RX`     | **GPIO 44** | UART debug receive (default) |
| `SPI CS` | **GPIO 3**  | SPI Flash chip select (default) |
| `SPI CLK`| **GPIO 5**  | ⚠️ **Conflict** — shared with I2S DIN |
| `SPI MOSI`| **GPIO 6** | SPI Flash MOSI (default) |
| `SPI MISO`| **GPIO 7** | SPI Flash MISO (default) |

> **Note:** GPIO 5 is shared between I2S DIN and SPI CLK. This is fine because the SPI flash is accessed internally by the ESP-IDF boot ROM and is not exposed as a user-accessible SPI bus.

---

## Unused / Available Pins

The following ESP32-S3 GPIOs are **not used** and available for future features:

```
GPIO 0*   ⚠️ Boot mode select (must be pulled up on boot)
GPIO 3    SPI CS (default, not user-accessible)
GPIO 6    SPI MOSI (default, not user-accessible)
GPIO 7    SPI MISO (default, not user-accessible)
GPIO 8    SPI CLK (default, not user-accessible)
GPIO 9    SDMMC CMD (not used)
GPIO 10   SDMMC CLK (not used)
GPIO 11   SDMMC D0 (not used)
GPIO 12   SDMMC D1 (not used)
GPIO 13   SDMMC D2 (not used)
GPIO 14   SDMMC D3 (not used)
GPIO 15   SDMMC D4 (not used)
GPIO 16   SDMMC D5 (not used)
GPIO 17   SDMMC D6 (not used)
GPIO 18   SDMMC D7 (not used)
GPIO 21   I2C SDA (available)
GPIO 22   I2C SCL (available)
GPIO 33   General purpose (available)
GPIO 34   General purpose (input only) (available)
GPIO 35   General purpose (input only) (available)
GPIO 36   General purpose (input only) (available)
GPIO 37   General purpose (input only) (available)
GPIO 38   General purpose (input only) (available)
GPIO 39   General purpose (input only) (available)
GPIO 40   General purpose (available)
GPIO 41   General purpose (available)
GPIO 42   General purpose (available)
GPIO 45   General purpose (available)
GPIO 46   General purpose (available)
```

\* **GPIO 0** is the boot mode pin — it must be pulled HIGH during normal boot. Avoid using it for other purposes unless you handle the boot strap circuitry.

---

## Pin Usage Diagram

```
┌─────────────────────────────────────────────────┐
│                  ESP32-S3                        │
│                                                  │
│  GPIO 1  ─── MCLK ──→ PCM1803A                  │
│  GPIO 2  ─── BCLK ──→ PCM1803A                  │
│  GPIO 4  ─── WS   ──→ PCM1803A                  │
│  GPIO 5  ─── DIN  ──→ PCM1803A                  │
│                                                  │
│  GPIO 19 ─── USB_DP ──→ USB Connector           │
│  GPIO 20 ─── USB_DM ──→ USB Connector           │
│                                                  │
│  GPIO 43 ─── TX (UART debug)                    │
│  GPIO 44 ─── RX (UART debug)                    │
└─────────────────────────────────────────────────┘
```
