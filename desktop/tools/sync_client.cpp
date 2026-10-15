/*
 * sync_client.cpp - WiFi sync prototype CLIENT (part 1: headers + main).
 * Part 2 appends the render loop. See sync_wifi.h for protocol/math.
 *
 * Build: g++ -O2 -std=c++17 -o sync_client sync_client.cpp -lm
 * Run:   ./sync_client [seconds]   (listens 0.0.0.0:48200)
 */
#include "sync_wifi.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

static void write_wav_header(FILE* f, uint32_t frames) {
    uint32_t data_bytes = frames * SYNC_WIFI_CHANNELS * sizeof(int16_t);
    uint32_t riff = 36 + data_bytes;
    fwrite("RIFF",1,4,f);
    fwrite(&riff,4,1,f);
    fwrite("WAVEfmt ",1,8,f);
    uint32_t fmt_len=16; fwrite(&fmt_len,4,1,f);
    uint16_t audio_fmt=1, ch=SYNC_WIFI_CHANNELS; fwrite(&audio_fmt,2,1,f);
    fwrite(&ch,2,1,f);
    uint32_t rate=SYNC_WIFI_RATE; fwrite(&rate,4,1,f);
    uint32_t br=SYNC_WIFI_RATE*SYNC_WIFI_CHANNELS*sizeof(int16_t);
    fwrite(&br,4,1,f);
    uint16_t ba=SYNC_WIFI_CHANNELS*sizeof(int16_t); fwrite(&ba,2,1,f);
    uint16_t bps=16; fwrite(&bps,2,1,f);
    fwrite("data",1,4,f);
    fwrite(&data_bytes,4,1,f);
}

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 35;
    if (seconds <= 0) seconds = 35;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(SYNC_WIFI_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock,(struct sockaddr*)&bind_addr,sizeof(bind_addr))<0){
        perror("bind"); return 1;
    }
    int fl = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, fl | O_NONBLOCK);

    FILE* wav = fopen("client.wav","wb");
    if (!wav) { perror("client.wav"); return 1; }
    uint32_t total_frames = (uint32_t)seconds * SYNC_WIFI_RATE;
    write_wav_header(wav, total_frames);

    SyncWifiRing ring;
    SyncWifiDrift drift;
    drift.target_frames = SYNC_WIFI_ALIGN_BASE_MS*SYNC_WIFI_RATE/1000.0;

    /* Simulated friend-phone clock: +80ppm fast. Each render quantum of
     * N nominal frames consumes N*1.00008 frames of stream time; the drift
     * loop must pull ratio down to ~0.99992 to compensate. */
    const double CLOCK_PPM = 80.0;
    const double clock_rate = 1.0 + CLOCK_PPM/1e6;

    uint64_t consumed = 0;
    bool got_first = false;
    uint32_t last_seq = 0;
    uint64_t rendered = 0;
    uint32_t lost = 0;
    uint64_t recv_pkts = 0;   /* accepted (unique) packets */
    uint64_t dup_pkts = 0;    /* duplicates skipped */
    uint64_t total_adv = 0;   /* cumulative frames consumed from ring */

    const double dt = (double)SYNC_WIFI_FRAMES_PER_PACKET/SYNC_WIFI_RATE;
    std::vector<char> rbuf(sizeof(SyncWifiPacket));
    uint32_t tick = 0;
    struct timespec r0{}; clock_gettime(CLOCK_MONOTONIC, &r0);
    uint32_t quantum = 0;
    bool clock_started = false; /* anchor r0 at prime, not at process start */

    while (rendered < total_frames) {
        /* Drain all pending packets (non-blocking). */
        for (;;) {
            ssize_t n = recv(sock, rbuf.data(), rbuf.size(), 0);
            if (n < 0) { if (errno==EAGAIN||errno==EWOULDBLOCK) break;
                         perror("recv"); break; }
            if (n != (ssize_t)sizeof(SyncWifiPacket)) continue;
            SyncWifiPacket* p = (SyncWifiPacket*)rbuf.data();
            if (p->magic != SYNC_WIFI_MAGIC) continue;
            if (!got_first) {
                got_first = true;
                last_seq = p->seq;
            }
            if (p->seq <= last_seq) { dup_pkts++; continue; } /* dup: skip */
            if (p->seq != last_seq+1) {
                lost += p->seq-(last_seq+1);
            }
            last_seq = p->seq;
            recv_pkts++;
            ring.write_frames(p->pcm, SYNC_WIFI_FRAMES_PER_PACKET);
        }
        if (!got_first) { usleep(2000); continue; }

        size_t level = ring.level();
        if (!drift.primed) {
            /* Prime once a full target of audio has arrived: drop to
             * target depth, phase = read pointer. */
            if (level < (size_t)drift.target_frames) { usleep(2000); continue; }
            ring.flush_to((size_t)drift.target_frames);
            drift.phase = (double)ring.read;
            drift.smooth_err = 0.0;
            drift.primed = true;
            consumed = ring.read;
            clock_gettime(CLOCK_MONOTONIC, &r0); /* realtime starts NOW */
            clock_started = true;
            quantum = 0;
            fprintf(stderr,"[client] primed level=%zu target=%.0f\n",
                    ring.level(), drift.target_frames);
        }

        if (drift.need_jump(ring.level())) {
            /* >50ms out: hard jump (same HARD_SYNC rule as desktop). */
            ring.flush_to((size_t)drift.target_frames);
            drift.phase = (double)ring.read;
            drift.smooth_err = 0.0;
            consumed = ring.read;
            fprintf(stderr,"[client] JUMP level=%zu\n", ring.level());
        }

        double ratio = drift.update(ring.level(), dt);
        /* Render one 10ms quantum, warped by ratio, consumed at the
         * simulated fast clock: stream advances ratio*clock_rate. */
        double step = ratio * clock_rate;
        for (int i = 0; i < SYNC_WIFI_FRAMES_PER_PACKET
                        && rendered < total_frames; i++) {
            int16_t out[SYNC_WIFI_CHANNELS];
            ring.read_frame(drift.phase, out);
            fwrite(out, sizeof(int16_t), SYNC_WIFI_CHANNELS, wav);
            drift.phase += step;
            rendered++;
        }
        uint64_t new_read = (uint64_t)drift.phase;
        if (new_read > ring.read) {
            uint64_t adv = new_read - ring.read;
            total_adv += adv;
            size_t avail = ring.level();
            if (adv > avail) {
                /* True underrun (master jitter/laptop scheduling): consume
                 * what's buffered and park phase AT write. The old code
                 * parked at write-1, which leaves phase behind read when
                 * level<480 and re-triggers every quantum: read freezes
                 * forever while write keeps growing -> level explodes. */
                ring.advance(avail);
                drift.phase = (double)ring.write;
                consumed = ring.read;
            } else {
                ring.advance((size_t)adv);
                consumed = ring.read;
            }
        }
        (void)consumed;

        if ((tick++ % 100) == 0) {
            double ppm = (ratio-1.0)*1e6;
            fprintf(stderr,"[client] t=%.1fs level=%zu target=%.0f "
                    "ratio=%.6f (%+.0fppm) lost=%u recv=%llu dup=%llu "
                    "W=%llu R=%llu rendered=%llu\n",
                    (double)rendered/SYNC_WIFI_RATE, ring.level(),
                    drift.target_frames, ratio, ppm, lost,
                    (unsigned long long)recv_pkts,
                    (unsigned long long)dup_pkts,
                    (unsigned long long)ring.write,
                    (unsigned long long)ring.read,
                    (unsigned long long)rendered);
        }
        /* Pace render at realtime: sleep ONLY the alignment delta.
         * (See NOTE above: no blind usleep here.) */
        /* Hard-align to the monotonic clock so scheduling jitter in
         * fwrite/recv cannot accumulate: quantum N must end at t0+N*10ms.
         * r0 is anchored at prime; skip alignment until primed.
         * NOTE: there is deliberately NO blind sleep here besides the
         * alignment delta below. An unconditional usleep(10000) PLUS work
         * time makes every quantum ~10.4ms, so the client renders ~96
         * quanta/s while the master sends 100/s: the ring floods at
         * ~480 frames/s no matter what the drift loop does (its 500ppm
         * clamp cannot fight a 4% deficit). The alignment sleep alone
         * keeps quanta at exactly 10ms. */
        if (clock_started) {
            quantum++;
            struct timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_us = (now.tv_sec-r0.tv_sec)*1000000L
                            + (now.tv_nsec-r0.tv_nsec)/1000L;
            long target_us = (long)quantum*10000L;
            if (target_us > elapsed_us) usleep((useconds_t)(target_us-elapsed_us));
        }
    }
    double ppm = (drift.ratio-1.0)*1e6;
    fprintf(stderr,"[client] done: rendered=%llu final_level=%zu "
            "final_ratio=%.6f (%+.1fppm, expect ~-80ppm) lost=%u "
            "recv=%llu dup=%llu W=%llu R=%llu total_adv=%llu\n",
            (unsigned long long)rendered, ring.level(),
            drift.ratio, ppm, lost,
            (unsigned long long)recv_pkts, (unsigned long long)dup_pkts,
            (unsigned long long)ring.write, (unsigned long long)ring.read,
            (unsigned long long)total_adv);
    fclose(wav);
    close(sock);
    return 0;
}
