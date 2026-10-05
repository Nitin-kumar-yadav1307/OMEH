/*
 * sync_master.cpp - WiFi sync prototype MASTER.
 *
 * Generates a 48kHz stereo test tone (440Hz L / 660Hz R + 1s beep marker),
 * plays it LOCALLY (master is the clock, no warp: writes to master.wav),
 * AND broadcasts it over UDP with an absolute frame counter.
 *
 * This mirrors the final app where the master plays to its OWN BT bud
 * while broadcasting to friend phones.
 *
 * Build: g++ -O2 -std=c++17 -o sync_master sync_master.cpp
 * Run:   ./sync_master [seconds]   (default 30, broadcast 127.0.0.1:48200)
 */
#include "sync_wifi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
    int seconds = argc > 1 ? atoi(argv[1]) : 30;
    if (seconds <= 0) seconds = 30;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(SYNC_WIFI_PORT);
    /* Unicast to loopback by default (no multi-interface duplicates).
     * Pass "bcast" as argv[2] to use 255.255.255.255 for real WiFi tests. */
    if (argc > 2 && strcmp(argv[2],"bcast")==0)
        dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    else
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    FILE* wav = fopen("master.wav","wb");
    if (!wav) { perror("master.wav"); return 1; }
    uint32_t total_frames = (uint32_t)seconds * SYNC_WIFI_RATE;
    write_wav_header(wav, total_frames);

    SyncWifiPacket pkt{};
    pkt.magic = SYNC_WIFI_MAGIC;
    uint32_t seq = 0;
    uint64_t master_pts = 0;
    double phaseL = 0.0, phaseR = 0.0;

    /* Pace packets at 10ms intervals using CLOCK_MONOTONIC. */
    struct timespec t0{}; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t packets = total_frames / SYNC_WIFI_FRAMES_PER_PACKET;

    for (uint32_t p = 0; p < packets; p++) {
        for (int i = 0; i < SYNC_WIFI_FRAMES_PER_PACKET; i++) {
            uint64_t f = master_pts + i;
            double t = (double)f / SYNC_WIFI_RATE;
            /* 440Hz left, 660Hz right; 1s beep at 880Hz both every 5s. */
            double beep = (fmod(t,5.0) < 1.0) ? 0.5 : 0.0;
            double l = 0.4*sin(2*M_PI*440*t) + beep*0.4*sin(2*M_PI*880*t);
            double r = 0.4*sin(2*M_PI*660*t) + beep*0.4*sin(2*M_PI*880*t);
            pkt.pcm[i*2+0] = (int16_t)(l*30000);
            pkt.pcm[i*2+1] = (int16_t)(r*30000);
            (void)phaseL; (void)phaseR;
        }
        pkt.seq = seq++;
        pkt.master_pts = master_pts;
        ssize_t n = sendto(sock, &pkt, sizeof(pkt), 0,
                           (struct sockaddr*)&dst, sizeof(dst));
        if (n != (ssize_t)sizeof(pkt)) { perror("sendto"); break; }
        fwrite(pkt.pcm, sizeof(int16_t),
               SYNC_WIFI_FRAMES_PER_PACKET*SYNC_WIFI_CHANNELS, wav);

        master_pts += SYNC_WIFI_FRAMES_PER_PACKET;

        /* Sleep until next 10ms slot. */
        struct timespec now{}; clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_us = (now.tv_sec-t0.tv_sec)*1000000L
                        + (now.tv_nsec-t0.tv_nsec)/1000L;
        long target_us = (long)(p+1)*10000L;
        if (target_us > elapsed_us) {
            struct timespec sl{};
            sl.tv_sec = 0;
            sl.tv_nsec = (target_us-elapsed_us)*1000L;
            nanosleep(&sl, nullptr);
        }
        if ((p % 100) == 0)
            fprintf(stderr,"[master] pkt=%u pts=%llu\r",
                    pkt.seq,(unsigned long long)pkt.master_pts);
    }
    fprintf(stderr,"\n[master] done: %u packets, master.wav written\n", seq);
    fclose(wav);
    close(sock);
    return 0;
}
