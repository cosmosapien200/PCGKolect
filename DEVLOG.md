# Development Log

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
