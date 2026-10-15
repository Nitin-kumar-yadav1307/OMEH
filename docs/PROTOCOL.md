# Wire protocol

Defined in exactly one place: [`protocol/`](../protocol/). The wire contract
itself lives in [`wire.h`](../protocol/wire.h) — constants plus the packed
`SyncWifiPacket` struct. [`sync_wifi.h`](../protocol/sync_wifi.h) is an
umbrella that includes `wire.h` alongside the ring/drift/jitter headers, so
every target still compiles one include.

Every target compiles these headers, so this document describes them — it does
not independently specify them. If they disagree, the headers win and this file
is stale. A `static_assert` in `wire.h` locks `sizeof(SyncWifiPacket)` to
`SYNC_WIFI_PACKET_BYTES`, so a layout edit that would silently corrupt the wire
breaks the build instead.

## Transport

| | |
|---|---|
| Protocol | UDP |
| Audio port | **48200** |
| Control port | **48201** — `PING`/`PONG` link check + `SYNC-HELLO`/`SYNC-ACK` |
| Cadence | one packet per **10 ms** (480 frames) |
| Rate | ~200 KB/s per client |

## Audio format

| | |
|---|---|
| Sample rate | 48 000 Hz |
| Channels | 2 (interleaved) |
| Sample format | signed 16-bit little-endian (`s16le`) |
| Frames per packet | 480 |

The master may fall back to **mono capture** on SoCs that do not implement
multi-channel `AudioPlaybackCapture`. The wire format is unchanged — a mono
capture is up-mixed to stereo before packing, so the client never has to
branch on channel count.

## Packet layout

Little-endian, packed, 16-byte header followed by PCM:

```
 offset  size  field         notes
 ------  ----  ------------  -------------------------------------------
      0     4  magic         0x53594E43  ("SYNC" as ASCII, LE)
      4     4  seq           increments by 1 per packet
      8     8  master_pts   absolute frame counter of the FIRST sample
     16  1920  pcm           480 frames x 2 ch x 2 bytes = s16le
```

Total packet size: `SYNC_WIFI_PACKET_BYTES` = **1936 bytes** (16-byte header +
1920-byte payload).

That is above a standard 1500-byte MTU, so each datagram is IP-fragmented on
the wire. On a direct Wi-Fi link the kernel reassembles it and this is not a
problem in practice, but it is worth knowing before tuning packet sizes — a
single lost fragment discards the whole packet.

`master_pts` is the clock. The client computes its buffer level as
`master_pts - frames_consumed`, which is what the drift controller drives
toward the target.

## Header fields (as compiled)

```c
SYNC_WIFI_RATE                  48000
SYNC_WIFI_CHANNELS              2
SYNC_WIFI_FRAMES_PER_PACKET     480
SYNC_WIFI_PORT                  48200
SYNC_WIFI_MAGIC                 0x53594E43u   /* "SYNC" */
SYNC_WIFI_PCM_BYTES             1920
SYNC_WIFI_PACKET_BYTES          1936
```

## Sync algorithm

Same drift-PI design as the shared `protocol/drift.h` used by the phone app
and the desktop test pair (the constants differ per target). In the earbud
prototypes it lives in `desktop/pipewire/sync_playback.h` /
`sync_health.h` and `desktop/windows/win_render.h` / `win_align.h` — each
was split out of the old monolithic `.cpp` into single-TU modules that the
driver `.cpp` includes in order:

```
ratio = 1 + DRIFT_KP * smoothed(level - target)
```

clamped to ±`DRIFT_MAX_PPM` (500 ppm). A `HARD_SYNC` resample-jump happens
only past `HARD_SYNC_FRAMES` (~200 ms of backlog), so ordinary jitter is
absorbed by the resampler and never produces an audible discontinuity.

| Constant | Value | Meaning |
|---|---|---|
| `DRIFT_TAU` | 10 s | level-error averaging |
| `DRIFT_LOOP_TAU` | 40 s | closed-loop time constant |
| `DRIFT_MAX_PPM` | 500 | clamp on resampler ratio |
| `HARD_SYNC_FRAMES` | 9600 (~200 ms) | jump only on massive backlog |

## Adaptive jitter buffer

The client's buffer target is the only latency term we actually control, and a
fixed value is wrong in one direction: too small breaks audio on a real link,
too large adds audible lag. So the client adapts it **AIMD**:

| Event | Action |
|---|---|
| Underrun (ring emptied) | grow by **+15 ms** immediately |
| 2 s clean (no underrun) | shrink by **−10 ms** |

Bounded to **[40, 250] ms**. On a stable link it walks down to the floor and
stays there; move the phones apart and it climbs just enough to survive the
extra jitter.

## Compatibility

The magic number is the only versioning. A receiver that does not see
`0x53594E43` drops the packet. There is currently no explicit version field —
any change to the header layout must change `SYNC_WIFI_MAGIC`, otherwise old
and new peers will mis-parse each other's packets and produce noise.