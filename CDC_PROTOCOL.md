# DA15 USB CDC Protocol — Electron App Integration Guide

## Connection

The DA15 enumerates as a USB composite device with three interfaces:
- **Audio** (UAC1 stereo speaker, 48kHz/24-bit)
- **DFU Runtime** (firmware update trigger)
- **CDC** (virtual serial port for EQ profile management)

The CDC interface has string descriptor **"DA15 EQ Config"**. Use this to identify the correct serial port.

**USB identifiers:** VID `0x1209`, PID `0xDA15`

No baud rate configuration is needed (it's USB CDC, not a real UART), but most serial libraries require one — any value works (e.g. 115200).

## Binary Frame Protocol

All communication is request/response. The host (Electron app) sends a request, the device always replies.

### Request frame
```
[CMD:1] [LEN:2 LE] [PAYLOAD:LEN bytes] [CRC8:1]
```

### Response frame
```
[CMD|0x80:1] [LEN:2 LE] [STATUS:1] [PAYLOAD:LEN-1 bytes] [CRC8:1]
```

- **CMD**: Command byte. Response echoes it with bit 7 set (`CMD | 0x80`).
- **LEN**: Little-endian uint16. In requests, this is the payload size. In responses, this is `1 + payload_size` (the status byte counts toward the length).
- **CRC8**: Polynomial `0x07` (SMBus), initial value `0x00`. Computed over everything before the CRC byte (header + payload, or header + status + payload).

### CRC8 Implementation

```javascript
function crc8(data) {
  let crc = 0x00;
  for (const byte of data) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) {
      if (crc & 0x80) crc = ((crc << 1) ^ 0x07) & 0xFF;
      else crc = (crc << 1) & 0xFF;
    }
  }
  return crc;
}
```

### Status Codes

| Value | Name | Meaning |
|-------|------|---------|
| `0x00` | OK | Success |
| `0x01` | ERR_INVALID_CMD | Unknown command byte |
| `0x02` | ERR_INVALID_PARAM | Bad ID, wrong payload size, etc. |
| `0x03` | ERR_FLASH | Flash erase/write failed |

## Commands

### 0x01 — GET_DEVICE_INFO

**Request payload:** (none, LEN=0)

**Response payload (6 bytes):**
| Offset | Type | Field |
|--------|------|-------|
| 0 | uint8 | fw_version_major |
| 1 | uint8 | fw_version_minor |
| 2 | uint8 | fw_version_patch |
| 3 | uint8 | max_profiles (10) |
| 4 | uint8 | max_filters_per_profile (10) |
| 5 | uint8 | active_profile_id (0-9, or 0xFF=OFF) |

### 0x02 — GET_PROFILE_LIST

**Request payload:** (none, LEN=0)

**Response payload (variable):**
| Offset | Type | Field |
|--------|------|-------|
| 0 | uint8 | count (number of stored profiles) |
| 1+ | repeated | For each profile: `[id:1] [name:16]` |

Each entry is 17 bytes: 1 byte slot ID (0-9) + 16 bytes null-terminated name. Only non-empty slots are included.

### 0x03 — GET_PROFILE

**Request payload (1 byte):** `[profile_id:1]`

**Response payload (380 bytes):** Raw `eq_profile_t` struct bytes (see data structures below).

Returns `ERR_INVALID_PARAM` if the slot is empty or ID >= 10.

### 0x04 — SET_PROFILE

**Request payload (381 bytes):** `[profile_id:1] [eq_profile_t:380]`

Writes a profile to the specified slot **in RAM only**. Call `SAVE_TO_FLASH` afterward to persist. Returns `ERR_INVALID_PARAM` if ID >= 10.

### 0x05 — DELETE_PROFILE

**Request payload (1 byte):** `[profile_id:1]`

Clears the profile slot **in RAM only**. Call `SAVE_TO_FLASH` to persist. If the deleted profile was active, the device switches to OFF. Returns `ERR_INVALID_PARAM` if ID >= 10.

### 0x06 — SET_ACTIVE

**Request payload (1 byte):** `[profile_id:1]` (0-9 for a profile, `0xFF` for OFF)

Takes effect immediately — the device switches EQ processing to the selected profile (or back to legacy bass/treble if OFF). The active profile is also persisted in the device's settings sector automatically.

### 0x07 — SAVE_TO_FLASH

**Request payload:** (none, LEN=0)

Erases the profile flash sector and writes all current profiles from RAM. Returns `ERR_FLASH` if the operation fails.

## Data Structures (Binary Layout)

### eq_filter_t — 36 bytes

All floats are IEEE 754 single-precision, **little-endian** (ARM native byte order).

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 4 | float | b0 | Biquad coefficient |
| 4 | 4 | float | b1 | Biquad coefficient |
| 8 | 4 | float | b2 | Biquad coefficient |
| 12 | 4 | float | a1 | Biquad coefficient |
| 16 | 4 | float | a2 | Biquad coefficient |
| 20 | 4 | float | freq | Center/corner frequency (Hz) |
| 24 | 4 | float | gain | Gain in dB |
| 28 | 4 | float | q | Q factor |
| 32 | 1 | uint8 | type | Filter type (see below) |
| 33 | 1 | uint8 | enabled | 0=bypass, 1=active |
| 34 | 2 | — | padding | Set to 0x00 |

**Filter types:**
| Value | Type |
|-------|------|
| 0 | OFF (filter slot unused) |
| 1 | Bell (peaking EQ) |
| 2 | Low Shelf |
| 3 | High Shelf |
| 4 | Low Pass |
| 5 | High Pass |

### eq_profile_t — 380 bytes

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 16 | char[16] | name | Null-terminated UTF-8 string (max 15 chars + null) |
| 16 | 1 | uint8 | filter_count | Number of active filters (0-10) |
| 17 | 3 | — | padding | Set to 0x00 |
| 20 | 360 | eq_filter_t[10] | filters | Array of 10 filter slots |

Filters beyond `filter_count` are ignored by the device but should be zeroed.

## Biquad Coefficient Computation

The Electron app must pre-compute biquad coefficients. The device uses **Direct Form II Transposed** processing:

```
y[n] = b0*x[n] + s1
s1   = b1*x[n] - a1*y[n] + s2
s2   = b2*x[n] - a2*y[n]
```

Coefficients must be **normalized** (a0 = 1, divide all by a0). The `a1` and `a2` values stored are the standard denominator coefficients — the firmware applies them with a **minus sign** as shown above.

Use the standard Audio EQ Cookbook formulas (Robert Bristow-Johnson). Sample rate is always **48000 Hz**.

## Typical Workflow

```
1. Open CDC serial port (identify by VID/PID or "DA15 EQ Config" descriptor)
2. GET_DEVICE_INFO → check firmware version, get current state
3. GET_PROFILE_LIST → display stored profiles
4. User creates/edits a profile in the UI
5. SET_PROFILE(id, data) → upload to device RAM
6. SET_ACTIVE(id) → switch to the new profile (immediate audio effect)
7. SAVE_TO_FLASH → persist all profiles
```

For bulk operations (uploading multiple profiles), send SET_PROFILE for each, then a single SAVE_TO_FLASH at the end.

## Example: Sending GET_DEVICE_INFO

```javascript
// Build request frame
const cmd = 0x01;
const len = 0; // no payload
const frame = Buffer.from([cmd, len & 0xFF, (len >> 8) & 0xFF]);
const crcByte = crc8(frame);
const packet = Buffer.concat([frame, Buffer.from([crcByte])]);
// packet = [0x01, 0x00, 0x00, 0x79]

// Send over serial port, then read response:
// [0x81, 0x07, 0x00, 0x00, 0x02, 0x00, 0x00, 0x0A, 0x0A, 0xFF, <crc>]
//  ^CMD|0x80   ^LEN=7       ^OK  ^v2.0.0      ^10   ^10   ^OFF
```

## Example: Uploading a Profile

```javascript
// Build a profile with one bell filter at 1kHz, +3dB, Q=1.4
const name = Buffer.alloc(16); // null-terminated
Buffer.from('My Profile').copy(name);

const filter = Buffer.alloc(36);
// Write biquad coefficients (pre-computed for 1kHz bell, +3dB, Q=1.4, fs=48000)
filter.writeFloatLE(1.0015, 0);   // b0
filter.writeFloatLE(-1.8955, 4);  // b1
filter.writeFloatLE(0.8985, 8);   // b2
filter.writeFloatLE(-1.8955, 12); // a1 (note: firmware negates these)
filter.writeFloatLE(0.9000, 16);  // a2
filter.writeFloatLE(1000.0, 20);  // freq
filter.writeFloatLE(3.0, 24);     // gain
filter.writeFloatLE(1.4, 28);     // q
filter[32] = 1;                   // type = FILTER_BELL
filter[33] = 1;                   // enabled

// Build eq_profile_t (380 bytes)
const profile = Buffer.alloc(380);
name.copy(profile, 0);            // name at offset 0
profile[16] = 1;                  // filter_count = 1
// padding at 17-19 is already 0
filter.copy(profile, 20);         // first filter at offset 20
// remaining 9 filter slots are already zeroed

// Build SET_PROFILE request: [cmd] [len_lo] [len_hi] [id] [profile...] [crc]
const id = 0; // slot 0
const payload = Buffer.concat([Buffer.from([id]), profile]); // 381 bytes
const header = Buffer.from([0x04, payload.length & 0xFF, (payload.length >> 8) & 0xFF]);
const fullFrame = Buffer.concat([header, payload]);
const crc = crc8(fullFrame);
const packet = Buffer.concat([fullFrame, Buffer.from([crc])]);

// Send packet, expect response: [0x84, 0x01, 0x00, 0x00, <crc>]
//                                 ^CMD   ^LEN=1     ^OK

// Then persist:
// SAVE_TO_FLASH: [0x07, 0x00, 0x00, <crc>]
```
