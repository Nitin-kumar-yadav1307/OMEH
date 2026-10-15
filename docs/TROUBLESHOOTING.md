# Troubleshooting

## P2P pairing fails

### The client never shows an invitation, and no error appears

The most common cause is a **band mismatch**. A Wi-Fi Direct group operates on
exactly one channel, and a 2.4 GHz-only device cannot hear a 5 GHz group
owner's beacons at all. The invitation silently never completes.

Check the band both ends report (these log lines are what the app writes to
its on-screen log panel):

| Where | Log line |
|-------|----------|
| Master | `P2P: group operating at NNNN MHz (5 GHz)` |
| Master | `P2P: master STA on 'X' at NNNN MHz (5 GHz)` |
| Client | `Client: visible P2P group 'DIRECT-...' at NNNN MHz (5 GHz)` |

If the group is on 5 GHz and the client is 2.4 GHz-only, they cannot pair.

**Workaround:** there is no public API to force the group's band
(`createGroup()` takes no configuration). The practical lever is the master's
own Wi-Fi connection: on a single-radio phone the group owner must share a
channel with the station link, so connecting the master to the router's
**2.4 GHz** network tends to pull the group onto 2.4 GHz. Disconnecting the
master entirely has the *opposite* effect on many modern handsets, which then
pick 5 GHz freely.

**Alternative:** reverse the roles. A 2.4 GHz-only phone hosting the group has
no choice but to use 2.4 GHz, and a dual-band client can join it.

### The client finds the peer but `connect failed: 0/3/4/6`

- **Reason 3 (`BUSY`)** — a group already exists on one of the devices, often
  left over from a previous session. A device already in a group cannot accept
  a new invitation. Remove the group on both ends and retry.
- **Reason 0 (`P2P_UNSUPPORTED`)** — Wi-Fi is toggled off, or the device does
  not implement P2P despite the hardware claim.
- **Stale peer records** — if a peer shows as `FAILED` or `UNAVAILABLE` in the
  discovered list, that record is stale and will reject `connect()` locally.
  Rescan before connecting.

### The QR scan succeeds but nothing happens

The group SSID and the peer device name are *different things*. The group's
SSID is `DIRECT-xx-<model>`, which some manufacturers fill with spaces
(`DIRECT-7w-Galaxy F17 5G`). Joining by SSID is unreliable, so the client
matches on the master's P2P **device name** and joins by MAC address. If the
master's device name has been changed to something the client cannot find, or
the peer list has not refreshed, the match fails. Rescan.

## Audio

### The client plays silence

- Confirm the master is actually playing audio.
- **Netflix and other DRM content capture as silence** — this is a platform
  restriction, not a bug. Use YouTube, a browser, or a local music player.
- Check the client status line: `level` should be near the target (≈1920),
  `ratio` near `1.000000`, `lost` near `0`.

### The audio choppes / many underruns

The jitter buffer is starving. This usually means the link is lossy — move the
phones closer, or reduce contention on the channel. The buffer adapts upward
on its own (+15 ms per underrun) but starts from 40 ms, so the first few
glitches can precede the adaptation.

### Capture fails on a budget device

Some lower-end SoCs do not implement multi-channel `AudioPlaybackCapture` and
fail on a stereo configuration. The master falls back to **mono capture**
automatically; watch the log for the fallback message.

## Build

- `SDK location not found` → create `android/local.properties` with
  `sdk.dir=/path/to/Android/Sdk` (gitignored).
- `NDK not found` → re-run
  `sdkmanager "ndk;26.1.10909125" "cmake;3.22.1"`.
- `JAVA_HOME` errors → install JDK 17 and start a fresh shell.
- The Android build needs the header at `protocol/sync_wifi.h`. If you moved
  it, update the include path in
  `android/app/src/main/cpp/CMakeLists.txt`.
- PipeWire targets are skipped when `libpipewire-0.3-dev` is absent — that is
  expected, not an error.

## Diagnosing a crash

Unhandled exceptions are caught globally, appended to the app's
`filesDir/last_crash.txt`, and shown full-screen by a viewer that runs in its
own `:crash` process so it survives the crash it is displaying. Tap
**LAST CRASH** in the app, then **COPY ERROR**.

The on-screen log panel (`AppLog`) is a ring buffer mirrored to logcat under
the tag `SyncWifi`, so it is readable on the device without `adb`.