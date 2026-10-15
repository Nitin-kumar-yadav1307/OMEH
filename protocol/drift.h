/*
 * drift.h - the drift PI controller.
 *
 * Regulates buffer DEPTH, not position: measure (level - target), smooth it,
 * feed it through one gain, clamp the result to +/- MAX_PPM. Math reused
 * verbatim from pipewire_sync.cpp / pipewire_sync_win.cpp.
 *
 *   ratio = 1 + DRIFT_KP * smoothed(level - target)
 *
 * This is pure math - no I/O, no clock, no allocation - which is what makes
 * it testable on the desktop without a phone (see sync_selftest.cpp).
 */
#ifndef SYNC_WIFI_DRIFT_H
#define SYNC_WIFI_DRIFT_H

#include <cstddef>
#include "wire.h"

/* Drift controller tuning: identical semantics to pipewire_sync.cpp */
#define SYNC_WIFI_DRIFT_TAU 10.0        /* s, level-error averaging */
#define SYNC_WIFI_DRIFT_LOOP_TAU 40.0   /* s, closed-loop time constant */
#define SYNC_WIFI_DRIFT_KP (1.0/((double)SYNC_WIFI_RATE*SYNC_WIFI_DRIFT_LOOP_TAU))
#define SYNC_WIFI_DRIFT_MAX_PPM 500.0   /* clamp on resampler ratio */
#define SYNC_WIFI_HARD_SYNC_FRAMES 9600.0 /* ~200ms: jump only on massive backlog */
#define SYNC_WIFI_ALIGN_BASE_MS 150.0
#define SYNC_WIFI_ALIGN_MAX_MS 350.0

/* Identical PI loop to pipewire_sync: regulate depth, not position. */
struct SyncWifiDrift {
    double target_frames = SYNC_WIFI_ALIGN_BASE_MS*SYNC_WIFI_RATE/1000.0;
    double smooth_err = 0.0;
    double ratio = 1.0;
    bool   primed = false;
    double phase = 0.0; /* fractional render position, absolute frame domain */

    double alpha(double dt){ return dt/(SYNC_WIFI_DRIFT_TAU+dt); }

    double update(size_t level,double dt){
        double err=(double)level-target_frames;
        if(!primed){ smooth_err=err; primed=true; }
        else smooth_err += (err-smooth_err)*alpha(dt);
        ratio = 1.0 + SYNC_WIFI_DRIFT_KP*smooth_err;
        double mx = SYNC_WIFI_DRIFT_MAX_PPM/1e6;
        if(ratio>1.0+mx)ratio=1.0+mx;
        if(ratio<1.0-mx)ratio=1.0-mx;
        return ratio;
    }
    bool need_jump(size_t level){
        double e=(double)level-target_frames;
        if(e<0)e=-e;
        return e>SYNC_WIFI_HARD_SYNC_FRAMES;
    }
};

#endif /* SYNC_WIFI_DRIFT_H */