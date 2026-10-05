/*
 * sync_wifi.h - shared protocol + sync engine for WiFi multi-phone sync.
 *
 * Role model (v1):
 *   MASTER (your phone): captures/generates 48k stereo, plays it LOCALLY
 *     to its own BT bud (master is the clock, no warp needed), AND
 *     broadcasts it over UDP with an absolute frame counter (master_pts).
 *   CLIENT (friend's phone): receives UDP, holds its own Ring at the
 *     target depth where level = master_pts - consumed, and warps its
 *     local playout with the SAME drift PI controller as pipewire_sync.
 *
 * Math reused verbatim from pipewire_sync.cpp / pipewire_sync_win.cpp:
 *   ratio = 1 + DRIFT_KP * smoothed(level - target)
 *   clamp +/- DRIFT_MAX_PPM, HARD_SYNC jump beyond HARD_SYNC_FRAMES.
 *
 * Wire format (all little-endian, packed):
 *   magic(4)=0x53594E43 "SYNC" | seq(4) | master_pts(8) | pcm[480*2]s16
 * Transport: UDP broadcast port 48200, 10ms packets (480 frames @48k).
 * ~200KB/s per client. No deps beyond libc.
 */
#ifndef SYNC_WIFI_H
#define SYNC_WIFI_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>

#define SYNC_WIFI_RATE 48000
#define SYNC_WIFI_CHANNELS 2
#define SYNC_WIFI_FRAMES_PER_PACKET 480
#define SYNC_WIFI_PORT 48200
#define SYNC_WIFI_MAGIC 0x53594E43u /* "SYNC" */
#define SYNC_WIFI_PCM_BYTES (SYNC_WIFI_FRAMES_PER_PACKET*SYNC_WIFI_CHANNELS*(int)sizeof(int16_t))
#define SYNC_WIFI_PACKET_BYTES (4+4+8+SYNC_WIFI_PCM_BYTES)

/* Drift controller tuning: identical semantics to pipewire_sync.cpp */
#define SYNC_WIFI_DRIFT_TAU 10.0        /* s, level-error averaging */
#define SYNC_WIFI_DRIFT_LOOP_TAU 40.0   /* s, closed-loop time constant */
#define SYNC_WIFI_DRIFT_KP (1.0/((double)SYNC_WIFI_RATE*SYNC_WIFI_DRIFT_LOOP_TAU))
#define SYNC_WIFI_DRIFT_MAX_PPM 500.0   /* clamp on resampler ratio */
#define SYNC_WIFI_HARD_SYNC_FRAMES 2400.0 /* ~50ms: jump instead of warp */
#define SYNC_WIFI_ALIGN_BASE_MS 40.0
#define SYNC_WIFI_ALIGN_MAX_MS 350.0

#pragma pack(push,1)
struct SyncWifiPacket {
    uint32_t magic;   /* SYNC_WIFI_MAGIC */
    uint32_t seq;     /* increments by 1 per packet */
    uint64_t master_pts; /* absolute frame counter of FIRST frame in pcm */
    int16_t  pcm[SYNC_WIFI_FRAMES_PER_PACKET*SYNC_WIFI_CHANNELS];
};
#pragma pack(pop)

#define SYNC_WIFI_RING_BITS 15
#define SYNC_WIFI_RING_SIZE (1u<<SYNC_WIFI_RING_BITS)
#define SYNC_WIFI_RING_MASK (SYNC_WIFI_RING_SIZE-1)

/* Stereo interleaved ring, int16. Single producer (net) + single consumer
 * (render) on the same thread in the prototype -> plain indices are fine. */
struct SyncWifiRing {
    int16_t data[SYNC_WIFI_RING_SIZE*SYNC_WIFI_CHANNELS]={};
    uint64_t read=0;   /* frame index consumed */
    uint64_t write=0;  /* frame index received */

    size_t level() const { return (size_t)(write-read); }

    void write_frames(const int16_t* src, size_t n) {
        if (n > SYNC_WIFI_RING_SIZE) {
            src += (n-SYNC_WIFI_RING_SIZE)*SYNC_WIFI_CHANNELS;
            n = SYNC_WIFI_RING_SIZE;
        }
        if (level()+n > SYNC_WIFI_RING_SIZE)
            read = write+n-SYNC_WIFI_RING_SIZE; /* keep newest on overflow */
        for (size_t i=0;i<n;i++) {
            size_t w=(size_t)((write+i)&SYNC_WIFI_RING_MASK);
            memcpy(&data[w*SYNC_WIFI_CHANNELS],&src[i*SYNC_WIFI_CHANNELS],
                   SYNC_WIFI_CHANNELS*sizeof(int16_t));
        }
        write+=n;
    }

    void read_frame(double pos,int16_t out[SYNC_WIFI_CHANNELS]) {
        double lo=(double)read;
        double hi=write?(double)write-1.0:0.0;
        if(pos<lo)pos=lo;
        if(pos>hi)pos=hi;
        size_t i0=(size_t)pos;
        double f=pos-(double)i0;
        size_t i1=i0+1;
        for(int c=0;c<SYNC_WIFI_CHANNELS;c++){
            double a=data[(i0&SYNC_WIFI_RING_MASK)*SYNC_WIFI_CHANNELS+c];
            double b=data[(i1&SYNC_WIFI_RING_MASK)*SYNC_WIFI_CHANNELS+c];
            out[c]=(int16_t)lrint(a+(b-a)*f);
        }
    }

    void flush_to(size_t frames){
        if(frames>level())frames=level();
        read=write-frames;
    }
    void advance(size_t n){ read+=n; }
};

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

#endif /* SYNC_WIFI_H */
