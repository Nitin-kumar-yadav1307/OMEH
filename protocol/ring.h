/*
 * ring.h - the client's stereo sample ring.
 *
 * Single producer (network thread feeds) + single consumer (render pulls),
 * same process. The float `phase` read position lets the drift controller
 * resample by an arbitrary ratio without a separate resampler stage.
 */
#ifndef SYNC_WIFI_RING_H
#define SYNC_WIFI_RING_H

#include <cstddef>
#include <cstring>
#include <cmath>
#include "wire.h"

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

#endif /* SYNC_WIFI_RING_H */