// native-lib.cpp - JNI bridge: proven sync_wifi.h engine for Android.
//
// The CLIENT data path stays entirely in C++ for realtime safety:
//   Java: feedPacket(bytes) -> ring.write_frames()   (network thread)
//   AAudio callback (C++) -> drift.update() + ring.read_frame() -> bud
// Only status (level/target/ratio) crosses back to Kotlin for the UI.
//
// MASTER packet building (seq/master_pts/PCM interleave) is also here so
// both ends share one definition of the wire format from sync_wifi.h.
#include <jni.h>
#include <cstdint>
#include <cstring>
#include <android/log.h>

#include "sync_wifi.h"

#define LOG_TAG "SyncWifiNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// ---- Client engine (one instance per process) ----
static SyncWifiRing  g_ring;
static SyncWifiDrift g_drift;
static uint32_t      g_lastSeq = 0;
static bool          g_gotFirst = false;
static uint32_t      g_lost = 0;
static uint32_t      g_dup = 0;

/* Adaptive jitter buffer state (see sync_wifi.h). The buffer size in ms is
 * owned here, not fixed at compile time: it is derived from the link's actual
 * behaviour, so a clean link self-tunes down to low latency. */
static double        g_target_ms = SYNC_WIFI_ALIGN_BASE_MS;
static uint32_t      g_stalls = 0;
static double        g_renderSec = 0.0;              /* monotonic render clock */
static double        g_adaptAt = SYNC_WIFI_TARGET_ADAPT_SEC;

extern "C" {

// Reset on START (client role).
JNIEXPORT void JNICALL
Java_com_syncwifi_NativeBridge_clientReset(JNIEnv*, jclass) {
    g_ring = SyncWifiRing{};
    g_drift = SyncWifiDrift{};
    g_drift.target_frames = SYNC_WIFI_ALIGN_BASE_MS * SYNC_WIFI_RATE / 1000.0;
    g_lastSeq = 0;
    g_gotFirst = false;
    g_lost = 0;
    g_dup = 0;
    g_target_ms = SYNC_WIFI_ALIGN_BASE_MS;
    g_stalls = 0;
    g_renderSec = 0.0;
    g_adaptAt = SYNC_WIFI_TARGET_ADAPT_SEC;
    LOGI("clientReset target=%.0f", g_drift.target_frames);
}

// Feed one UDP datagram (byte[]) into the ring. Returns level after write.
JNIEXPORT jint JNICALL
Java_com_syncwifi_NativeBridge_clientFeed(JNIEnv* env, jclass,
                                          jbyteArray data, jint len) {
    if (len != SYNC_WIFI_PACKET_BYTES) return -1;
    SyncWifiPacket pkt;
    env->GetByteArrayRegion(data, 0, len, reinterpret_cast<jbyte*>(&pkt));
    if (pkt.magic != SYNC_WIFI_MAGIC) return -2;
    if (!g_gotFirst) {
        g_gotFirst = true;
        g_lastSeq = pkt.seq;
    } else {
        if (pkt.seq <= g_lastSeq) { g_dup++; return (jint)g_ring.level(); } // dup: skip
        if (pkt.seq != g_lastSeq + 1) g_lost += pkt.seq - (g_lastSeq + 1);
        g_lastSeq = pkt.seq;
    }
    g_ring.write_frames(pkt.pcm, SYNC_WIFI_FRAMES_PER_PACKET);
    return (jint)g_ring.level();
}

// AAudio render: pull `nframes` stereo s16 into out (called from C++
// callback in a later step; exposed via JNI now for testability).
// Applies prime / hard-jump / PI warp exactly like sync_client.cpp.
JNIEXPORT jint JNICALL
Java_com_syncwifi_NativeBridge_clientRender(JNIEnv* env, jclass,
                                            jshortArray out, jint nframes) {
    if (!g_gotFirst) return 0;
    if (!g_drift.primed) {
        if (g_ring.level() < (size_t)g_drift.target_frames) return 0;
        g_ring.flush_to((size_t)g_drift.target_frames);
        g_drift.phase = (double)g_ring.read;
        g_drift.smooth_err = 0.0;
        g_drift.primed = true;
    }
    if (g_drift.need_jump(g_ring.level())) {
        g_ring.flush_to((size_t)g_drift.target_frames);
        g_drift.phase = (double)g_ring.read;
        g_drift.smooth_err = 0.0;
    }
    const double dt = (double)nframes / SYNC_WIFI_RATE;
    g_renderSec += dt;

    // Ring empty after priming: true underrun (WiFi jitter burst). Return 0
    // so Kotlin writes silence, paces with a sleep and counts the stall —
    // rendering clamped/held samples here would drone the last frame.
    if (g_ring.level() == 0) {
        // AIMD "increase": this link just proved it needs more buffer, so take
        // it now. Audio continuity outranks latency every single time.
        g_stalls++;
        g_target_ms += SYNC_WIFI_TARGET_GROW_MS;
        if (g_target_ms > SYNC_WIFI_TARGET_CEIL_MS) g_target_ms = SYNC_WIFI_TARGET_CEIL_MS;
        g_drift.target_frames = g_target_ms * SYNC_WIFI_RATE / 1000.0;
        g_adaptAt = g_renderSec + SYNC_WIFI_TARGET_ADAPT_SEC; // unstable: no give-back yet
        return 0;
    }

    // AIMD "decrease": a full window with zero underruns means we are carrying
    // more latency than this link needs. Hand it back a little at a time and
    // stop the moment a stall tells us we went too far.
    if (g_renderSec >= g_adaptAt) {
        g_adaptAt = g_renderSec + SYNC_WIFI_TARGET_ADAPT_SEC;
        if (g_target_ms > SYNC_WIFI_TARGET_FLOOR_MS) {
            g_target_ms -= SYNC_WIFI_TARGET_SHRINK_MS;
            if (g_target_ms < SYNC_WIFI_TARGET_FLOOR_MS) g_target_ms = SYNC_WIFI_TARGET_FLOOR_MS;
            g_drift.target_frames = g_target_ms * SYNC_WIFI_RATE / 1000.0;
        }
    }

    double ratio = g_drift.update(g_ring.level(), dt);
    jshort* dst = env->GetShortArrayElements(out, nullptr);
    for (int i = 0; i < nframes; i++) {
        int16_t s[SYNC_WIFI_CHANNELS];
        g_ring.read_frame(g_drift.phase, s);
        dst[i * 2 + 0] = s[0];
        dst[i * 2 + 1] = s[1];
        g_drift.phase += ratio; // phone clock ~= nominal; AAudio is the clock
    }
    env->ReleaseShortArrayElements(out, dst, 0);
    uint64_t nr = (uint64_t)g_drift.phase;
    if (nr > g_ring.read) {
        uint64_t adv = nr - g_ring.read;
        size_t avail = g_ring.level();
        if (adv > avail) {
            g_ring.advance(avail);
            g_drift.phase = (double)g_ring.write;
        } else {
            g_ring.advance((size_t)adv);
        }
    }
    return nframes;
}

// Status for the UI thread (poll ~2/s, mirrors desktop status line).
// [0]=level(frames) [1]=target(ms, adaptive) [2]=ratio [3]=lost [4]=dup [5]=stalls
JNIEXPORT jdoubleArray JNICALL
Java_com_syncwifi_NativeBridge_clientStatus(JNIEnv* env, jclass) {
    jdoubleArray a = env->NewDoubleArray(6);
    jdouble v[6] = {
        (jdouble)g_ring.level(),
        g_target_ms,
        g_drift.ratio,
        (jdouble)g_lost,
        (jdouble)g_dup,
        (jdouble)g_stalls
    };
    env->SetDoubleArrayRegion(a, 0, 6, v);
    return a;
}

// Master: build one wire packet from PCM (byte[] s16le stereo, 480f).
// seq/master_pts passed in from Kotlin which owns the counters; the
// layout (magic/seq/pts/PCM) is defined once here via sync_wifi.h.
JNIEXPORT jbyteArray JNICALL
Java_com_syncwifi_NativeBridge_masterBuild(JNIEnv* env, jclass,
                                           jint seq, jlong pts,
                                           jshortArray pcm) {
    SyncWifiPacket pkt{};
    pkt.magic = SYNC_WIFI_MAGIC;
    pkt.seq = (uint32_t)seq;
    pkt.master_pts = (uint64_t)pts;
    env->GetShortArrayRegion(pcm, 0,
        SYNC_WIFI_FRAMES_PER_PACKET * SYNC_WIFI_CHANNELS, pkt.pcm);
    jbyteArray out = env->NewByteArray(SYNC_WIFI_PACKET_BYTES);
    env->SetByteArrayRegion(out, 0, SYNC_WIFI_PACKET_BYTES,
                            reinterpret_cast<jbyte*>(&pkt));
    return out;
}

} // extern "C"
