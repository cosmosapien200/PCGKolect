# UART PCG Protocol (ESP32 → RV1103)

Wire protocol for streaming mono PCG (phonocardiogram) audio from the ESP32-S3
sender (`pcg-fw/main/uart_audio.c`) to the RV1103 (Luckfox Pico) receiver
(`PcgStream`).

---

## 1. Link

| Parameter | Value |
|-----------|-------|
| ESP32 port | `UART_NUM_1` (TX GPIO17, RX GPIO18) |
| RV1103 port | `/dev/ttyS3` (UART3) |
| Baud rate | **460800** |
| Frame | 8 data bits, no parity, 1 stop bit (8N1) |
| Flow control | None |
| Mode | Raw (non-canonical, no echo) |

UART carries 10 bits/byte, so 460800 baud ≈ **46,080 B/s**. The 8 kHz/16-bit
payload is **16,000 B/s** (≈ 35 % utilisation). 115200 would be insufficient.

---

## 2. Audio format

| Property | Value |
|----------|-------|
| Sample rate | 8,000 Hz |
| Resolution | 16-bit signed (`int16`) |
| Channels | 1 (mono) |
| Endianness | Little-endian |
| Frame rate | ~10 frames/s (100 ms) |

---

## 3. Packet format

```
 0        2        4      5                 5+LEN      5+LEN+2
┌────────┬────────┬──────┬──────────────────┬─────────┐
│  A5 5A │  LEN   │ SEQ  │ PCM (int16 LE)   │ CRC16 LE│
│  2 B   │ 2 B LE │ 1 B  │ LEN bytes        │  2 B    │
└────────┴────────┴──────┴──────────────────┴─────────┘
```

| Field | Offset | Size | Description |
|-------|--------|------|-------------|
| `SYNC` | 0 | 2 | Constant `0xA5 0x5A`; resync anchor |
| `LEN` | 2 | 2 | Payload bytes, LE. Even, non-zero, ≤ 2000 |
| `SEQ` | 4 | 1 | Frame counter, wraps at 256; gap ⇒ dropped frame |
| `PCM` | 5 | `LEN` | `LEN/2` little-endian `int16` samples |
| `CRC16` | 5 + `LEN` | 2 | CRC16-CCITT-FALSE over `LEN + SEQ + PCM`, LE |

**Standard 100 ms frame:** 800 samples, `LEN = 1600`, total **1607 bytes**,
~10 frames/s. Sample rate = 800 × 10 = 8000 Hz.

### CRC16-CCITT-FALSE

| Property | Value |
|----------|-------|
| Polynomial | `0x1021` |
| Init | `0xFFFF` |
| Reflect in/out | No / No |
| XOR out | `0x0000` |
| Coverage | bytes `LEN(lo) … last PCM byte` (`3 + LEN` bytes) |
| Stored | 2 bytes little-endian |

```c
uint16_t crc16_ccitt_false(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
```

---

## 4. Test vectors

| Case | Expected |
|------|----------|
| `crc("123456789")` | `0x29B1` |
| Small frame `LEN=4, SEQ=1, [1,2]` | `A5 5A 04 00 01 01 00 02 00 8F 8A` |
| Silence frame `LEN=1600, SEQ=0` CRC | `0x5E22` |

The firmware self-test in `uart_audio.c` checks the first two at boot
(`UART_AUDIO: protocol self-test: PASS`).

---

## 5. ESP32 sender (`uart_audio.c`)

- Opens an `{8000, AUDIO_BITS_16}` stream from the `audio` abstraction
  (see `agents.md`).
- A task blocks in `audio_read()` for exactly 800 samples (100 ms), builds a
  1607-byte frame, and pushes it with `uart_com_write()` (raw
  `uart_write_bytes()`, no flush).
- `UART_COM_TX_BUFFER` (4096 B) holds a whole frame so the write returns
  immediately and the driver drains the wire in the background; otherwise the
  ~35 ms wire time would stretch the 100 ms cadence and overrun the audio ring.
- `SEQ` increments per frame and wraps at 256.
- Runtime stats: `frames_sent`, `read_errors`, `write_errors`
  (`uart_audio_get_stats()`), logged once per second by `main.c`.

## 6. RV1103 receiver (`PcgStream`)

- Background reader thread parses frames into a ring buffer (~4 s).
- `read()` blocks until `count` samples are buffered or `timeout_ms` elapses.
- Bad `LEN` or CRC ⇒ resync by scanning for the next `A5 5A`.
- Stats: `frames_ok`, `crc_errors`, `bytes_read`.
- **Sync/baud gotchas** (fixed and verified end-to-end on 2026-10-11, receiver
  report in the `PCGKolect_rk` repo `docs/uart_receiver_fix_report.md`):
  - the parser must consume **both** sync bytes (`0xA5` then `0x5A`);
  - `cfsetispeed`/`cfsetospeed` must be given `B460800`, **not** the number
    `460800` (passing the raw number silently selects the wrong speed).
