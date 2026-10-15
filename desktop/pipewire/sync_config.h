/*
 * sync_config.h — build configuration: system includes + tuning constants.
 *
 * Single-TU module, part of pipewire_sync.cpp. Included FIRST, before the
 * other sync_*.h modules, so every later module sees the std/pipewire
 * headers and the RATE/CHANNELS/ring/drift tuning values below.
 */
#pragma once


#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <map>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#define RATE          48000
#define CHANNELS      2
#define FRAME_BYTES   (CHANNELS * sizeof(int16_t))

/* Ring capacity in frames (power of two) */
#define RING_BITS     15
#define RING_SIZE     (1u << RING_BITS)
#define RING_MASK     (RING_SIZE - 1)

/* Steady-state ring depth per device (ms) is derived from the measured
   device latency during the alignment phase; see ALIGN_BASE_MS below. */

/*
 * Drift controller tuning.
 *
 * We regulate the ring BUFFER DEPTH (level), not an absolute sample
 * position. Because the capture writes one frame into every ring for
 * every captured frame, level(i) = master_written - consumed(i), so
 * holding every device's level at its target keeps them all at the
 * same position relative to the master — which is exactly "in sync".
 *
 *   ratio = 1 + DRIFT_KP * smoothed(level - target)
 *
 * Two things matter here:
 *
 *  - The ring depth jitters by one graph quantum (256 frames = 5.3 ms)
 *    because the stream consumes in whole quanta, and the Bluetooth
 *    transport delivers in bursts, so the raw depth swings by ~20 ms.
 *    With both earbuds in the same graph that swing is common mode
 *    (sync is preserved) but it must not be fed into the resampler or
 *    it becomes audible pitch wobble. Averaging it over DRIFT_TAU
 *    seconds leaves only the genuine, slow clock drift.
 *  - The loop time constant is 1 / (DRIFT_KP * RATE) seconds, which
 *    must stay far above the transport delay (~buffer + codec) or the
 *    loop oscillates. 40 s is comfortably stable, and real clock drift
 *    (tens of ppm) is still corrected long before it is audible.
 */
#define DRIFT_TAU       10.0    /* s, level-error averaging              */
#define DRIFT_LOOP_TAU  40.0    /* s, closed-loop time constant          */
#define DRIFT_KP        (1.0 / ((double)RATE * DRIFT_LOOP_TAU))
#define DRIFT_MAX_PPM   500.0   /* clamp on resampler ratio              */

/* If a device is further out than this (~50 ms), jump instead of warp */
#define HARD_SYNC_FRAMES 2400.0

/*
 * Latency alignment tuning.
 *
 *   ALIGN_BASE_MS : minimum steady state buffer depth per device.
 *   ALIGN_MAX_MS  : hard cap on the alignment delay we will add.
 *   MEASURE_TICKS : how many 1-second ticks we probe the devices
 *                   before freezing the alignment offsets.
 */
#define ALIGN_BASE_MS   40.0
#define ALIGN_MAX_MS    350.0

/*
 * Self-calibrating inter-device trim.
 *
 * What the ear hears is the TOTAL latency of a device:
 *
 *     total = ring depth + device delay (codec + transport)
 *
 * The device delay a Bluetooth earbud reports is quantised (here one
 * graph quantum, 5.3 ms), so the offsets derived from a single probe
 * can be a quantum off - which is exactly the small residual echo.
 * So the timer keeps nudging each device's target depth until the
 * TOTALS match. Driving the totals equal is what truly aligns the
 * arrival time at the ears; the ring depths then intentionally differ
 * by the device-delay difference.
 */
#define ALIGN_TRIM_GAIN 0.15    /* per second, fraction of the mismatch */
#define ALIGN_SMOOTH    0.3     /* smoothing of the per-tick measurement */
#define MEASURE_TICKS   2

/* our own stream marker, so we never re-route ourselves */
#define OUR_APP_NAME    "pipewire_sync"
