# Observed FPGA Behavior — Dante PCIe Card (PCI 0x1a39:0x0004)

All data in this document was captured by reading hardware registers and
standard Linux interfaces on a running device. No proprietary source code
was consulted. Observations were made using debugfs register reads, /proc
filesystem, and standard ALSA tools.

## Device Under Test

- PCI Vendor: 0x1a39, Device: 0x0004
- PCI slot: 1b:00.0
- Card: Digigram LX-DANTE (discontinued)
- Firmware version register (BAR0+0x70): 0x20010010
- Dante sample rate configured via Dante Controller: 48000 Hz

## 1. Firmware Version Register (BAR0 + 0x70)

Reading BAR0 offset 0x70 returns a 32-bit value encoding the sample rate and firmware version.

Observed value: `0x20010010`

| Bits | Value | Meaning |
|------|-------|---------|
| 31-28 | 0x2 | Sample rate index |
| 27-0 | 0x0010010 | Firmware version identifier |

### Sample Rate Index Mapping

Determined by changing the sample rate in Dante Controller and reading the register:

| Index | Sample Rate (Hz) | Max Channels |
|-------|-----------------|--------------|
| 0 | 192000 | 64 |
| 1 | 96000 | 128 |
| 2 | 48000 | 128 |
| 3 | (invalid) | - |
| 4 | 176400 | 64 |
| 5 | 88200 | 128 |
| 6 | 44100 | 128 |

Max channel count was determined by opening the ALSA device at each sample rate and observing the maximum channels accepted by hw_params.

## 2. Sample Counter (BAR0 + 0x74 / 0x78)

A 64-bit free-running counter at BAR0 offsets 0x74 (low) and 0x78 (high).

Measured tick rate: **3000 ticks/second** at 48000 Hz sample rate.

This corresponds to one tick per audio chunk (48000 / 16 frames per chunk = 3000).

The counter does not reset when the driver is loaded/unloaded. It appears to run continuously while the FPGA is powered.

## 3. Event Flags (BAR0 + 0x10)

During active streaming with both playback and capture enabled, the VDMA event register consistently reads `0x0000000c`, indicating bits 2 and 3 are set.

| Bit | Observed State | Purpose |
|-----|---------------|---------|
| 0 | 0 (toggles) | Chunk clock from FPGA. Asserted by FPGA once per chunk, acknowledged by sequencer. Not visible in polled reads due to speed. |
| 1 | 0 (toggles) | Period marker. Set by sequencer, cleared to trigger interrupt. Not visible in polled reads. |
| 2 | 1 (while TX active) | Outbound (playback) enable. Set by host driver when playback stream starts. Cleared when playback stops. |
| 3 | 1 (while RX active) | Inbound (capture) enable. Set by host driver when capture stream starts. Cleared when capture stops. |

Event set: write to BAR0 + 0x08
Event clear: write to BAR0 + 0x0C

## 4. WAIT Instruction Operand

The VDMA sequencer WAIT instruction observed in the running microcode uses operand `0x001001`.

Using the WAIT_EVENT encoding from vdma_seqcode.h:
```
VDMA_WAIT_EVENT(EVENT_EN, EVENT_STATE)
  bits 23-12 = EVENT_EN = 0x001
  bits 11-0  = EVENT_STATE = 0x001
```

This means: wait until event bit 0 (chunk clock) matches state 1 (asserted), with enable mask selecting only bit 0.

## 5. Transfer Control Words

Observed in descriptor RAM during active streaming:

| Word | Value | Interpretation |
|------|-------|---------------|
| Outbound (host→FPGA) | 0x00012040 | Using VDMA_LOAD_XFER_CTL encoding: R=_IM(0), bits include direction + length. Bit 17 set (direction), bit 16 set, bits 5:0 = 0x40 (64 bytes). |
| Inbound (FPGA→host) | 0x00010040 | Same but bit 17 clear (opposite direction). Bit 16 set, bits 5:0 = 0x40 (64 bytes). |

Note: These values are loaded into VDMA_XFER_CTL via the VDMA_LOAD_XFER_CTL instruction. The R field selects _IM (immediate/indexed mode). The transfer length of 0x40 = 64 bytes = one chunk.

## 6. Interrupt Path

The interrupt fires through external interrupt 1 on the GN4124 bridge.

Observed register values:

| Register | Offset | Value | Meaning |
|----------|--------|-------|---------|
| INT_CTRL | BAR4+0x810 | 0x00000002 | Bit 1 set: external interrupt 1 rising edge |
| GPIO_BYPASS | BAR4+0xA00 | 0x00000002 | Bit 1 set: GPIO pin 1 bypasses to external interrupt 1 |
| INT_STAT acknowledge bit | BAR4+0x814 | bit 5 | Write bit 5 to clear the period interrupt |
| INT_CFG routing | BAR4+0x820+4*N | bit 5 | Route bit 5 of INT_STAT to MSI vector N |

The MSI vector index N is determined by reading the MSI data register (PCI config space) after enabling MSI. The low 2 bits of the MSI data value select which INT_CFG slot to use.

## 7. Audio Format

Observed via `arecord --dump-hw-params` and `/proc/asound/card0/pcm0c/sub0/hw_params`:

| Parameter | Value |
|-----------|-------|
| Access | MMAP_NONINTERLEAVED |
| Format | S32_LE |
| Subformat | STD |
| Channels | 1 to max_channels (rate-dependent) |
| Period size | Must be multiple of 16 frames |
| Periods | 2 to 512 |
| Buffer size | period_size * periods |

Audio data is 24-bit aligned to the most significant bits of a 32-bit word. The low 8 bits are zero-padded.

## 8. DMA Buffer Layout

Non-interleaved: each channel occupies a contiguous region.

```
[Channel 0: chunk0 chunk1 ... chunkN]
[Channel 1: chunk0 chunk1 ... chunkN]
...
[Channel 127: chunk0 chunk1 ... chunkN]
```

Each chunk: 16 frames × 4 bytes = 64 bytes.
Channel stride: chunks_per_channel × 64 bytes.
Total buffer: channel_count × channel_stride.

With 128 channels, 64 chunks: 128 × 64 × 64 = 524,288 bytes per direction.

## 9. Observed Descriptor RAM State During Active 128ch Streaming

```
Word 0x00 = 0x00000000  (constant zero)
Word 0x01 = 0x00000001  (constant one)
Word 0x02 = 0xffffffff  (constant negative one)
Word 0x03 = 0x00012040  (outbound transfer control)
Word 0x04 = 0x00010040  (inbound transfer control)
Word 0x05 = 0x00000040  (chunk size = 64 bytes)
Word 0x06 = 0x00000080  (outbound channels = 128)
Word 0x07 = 0x00000080  (inbound channels = 128)
Word 0x08 = 0x00000020  (periods per interrupt = 32)
Word 0x09 = (varies)    (period countdown, runtime)
Word 0x10 = 0x00000040  (outbound chunks per channel = 64)
Word 0x11 = 0x00000040  (inbound chunks per channel = 64)
Word 0x12 = 0x00001000  (outbound channel stride = 4096 bytes)
Word 0x13 = 0x00001000  (inbound channel stride = 4096 bytes)
Word 0x14 = (buffer addr lo)  (outbound DMA buffer physical address)
Word 0x15 = (buffer addr hi)
Word 0x16 = (buffer addr lo)  (inbound DMA buffer physical address)
Word 0x17 = (buffer addr hi)
Word 0x18 = (varies)    (outbound current chunk index, counts down)
Word 0x19 = (varies)    (inbound current chunk index, counts down)
Word 0x1a = (varies)    (outbound current base address lo)
Word 0x1b = (varies)    (outbound current base address hi)
Word 0x1c = (varies)    (inbound current base address lo)
Word 0x1d = (varies)    (inbound current base address hi)
```

## 10. Buffer Wrap Behavior

The chunk index (words 0x18/0x19) counts down from chunks_per_channel toward zero. The base address (words 0x1a-0x1d) advances by chunk_size (64 bytes) each iteration.

When the chunk index reaches zero, the sequencer resets it to chunks_per_channel and reloads the base address from the configured buffer start address (words 0x14-0x17). This wrap check occurs at the beginning of each period cycle before the WAIT instruction.

The driver's ALSA pointer callback reads the current base address (word 0x1a or 0x1c), subtracts the buffer start address, and applies modulo to get the frame position within the ring buffer.

## 11. MSI Routing Quirk

The GN4124 advertises 1 MSI vector but requires the Multiple Message Enable field to be set to 4 vectors (value 2 in bits 6:4 of MSI FLAGS). Without this, interrupts do not route correctly.

After setting this field, read the MSI data register to determine the actual vector offset (low 2 bits). Use this offset as the index into INT_CFG0-7 for routing interrupt sources.

This behavior was determined by observing that the device does not generate interrupts without the MSI control register modification, and functions correctly after it.
