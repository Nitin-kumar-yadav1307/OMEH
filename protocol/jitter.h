/*
 * jitter.h - adaptive jitter buffer policy (client side).
 *
 * The network buffer is the only latency term we actually control, and it has
 * to cover the WORST WiFi burst on the link - so any fixed value is wrong in
 * one direction: 150 ms adds audible lag, 40 ms breaks the audio on a real
 * phone-to-phone link. Instead the client measures itself (AIMD, as WebRTC's
 * congestion control does):
 *
 *   underrun (ring emptied) -> grow immediately   (fast up, audio first)
 *   2 s with no underrun    -> give a little back (slow down, latency)
 *
 * Bounded so it can never regress into the breaking regime or grow absurd.
 * On a stable link it walks down to the floor and stays there; walk away from
 * the phone and it climbs back just enough to survive the extra jitter.
 *
 * This struct owns the policy and nothing else - no JNI, no ring, no I/O.
 * It writes the resulting depth into a SyncWifiDrift, which is the only thing
 * it touches outside itself.
 */
#ifndef SYNC_WIFI_JITTER_H
#define SYNC_WIFI_JITTER_H

#include <cstdint>
#include "drift.h"

#define SYNC_WIFI_TARGET_FLOOR_MS  40.0
#define SYNC_WIFI_TARGET_CEIL_MS  250.0
#define SYNC_WIFI_TARGET_GROW_MS   15.0  /* added per underrun          */
#define SYNC_WIFI_TARGET_SHRINK_MS 10.0  /* removed per clean window    */
#define SYNC_WIFI_TARGET_ADAPT_SEC  2.0

struct SyncWifiJitter {
    double   target_ms = SYNC_WIFI_ALIGN_BASE_MS;
    uint32_t stalls    = 0;
    double   render_sec = 0.0;             /* monotonic render clock */
    double   adapt_at   = SYNC_WIFI_TARGET_ADAPT_SEC;

    void reset(){
        target_ms  = SYNC_WIFI_ALIGN_BASE_MS;
        stalls     = 0;
        render_sec = 0.0;
        adapt_at   = SYNC_WIFI_TARGET_ADAPT_SEC;
    }

    /* Render clock advances once per callback, BEFORE the underrun check. */
    void advance(double dt){ render_sec += dt; }

    /* True underrun (WiFi jitter burst). Caller must return silence - holding
     * the last frame would drone. Audio continuity outranks latency, so the
     * growth is taken now and the give-back clock is pushed out: a link that
     * just stalled has earned no latency credit. */
    bool on_underrun(SyncWifiDrift& d){
        stalls++;
        target_ms += SYNC_WIFI_TARGET_GROW_MS;
        if (target_ms > SYNC_WIFI_TARGET_CEIL_MS) target_ms = SYNC_WIFI_TARGET_CEIL_MS;
        d.target_frames = target_ms * SYNC_WIFI_RATE / 1000.0;
        adapt_at = render_sec + SYNC_WIFI_TARGET_ADAPT_SEC;
        return true;
    }

    /* A full window with zero underruns means we are carrying more latency
     * than this link needs. Hand it back a little at a time; the next stall
     * stops us. */
    void maybe_shrink(SyncWifiDrift& d){
        if (render_sec < adapt_at) return;
        adapt_at = render_sec + SYNC_WIFI_TARGET_ADAPT_SEC;
        if (target_ms <= SYNC_WIFI_TARGET_FLOOR_MS) return;
        target_ms -= SYNC_WIFI_TARGET_SHRINK_MS;
        if (target_ms < SYNC_WIFI_TARGET_FLOOR_MS) target_ms = SYNC_WIFI_TARGET_FLOOR_MS;
        d.target_frames = target_ms * SYNC_WIFI_RATE / 1000.0;
    }
};

#endif /* SYNC_WIFI_JITTER_H */