/*
 * wire.h - the wire contract. THE shared definition: every target (Android
 * JNI bridge, desktop test master/client) compiles this, so changing a
 * constant here changes the protocol for all of them at once.
 *
 * Wire format (all little-endian, packed):
 *   magic(4)=0x53594E43 "SYNC" | seq(4) | master_pts(8) | pcm[480*2]s16
 * Transport: UDP port 48200, 10ms packets (480 frames @48k). ~200KB/s per
 * client. No deps beyond libc.
 */
#ifndef SYNC_WIFI_WIRE_H
#define SYNC_WIFI_WIRE_H

#include <cstdint>
#include <cstddef>

#define SYNC_WIFI_RATE 48000
#define SYNC_WIFI_CHANNELS 2
#define SYNC_WIFI_FRAMES_PER_PACKET 480
#define SYNC_WIFI_PORT 48200
#define SYNC_WIFI_MAGIC 0x53594E43u /* "SYNC" */
#define SYNC_WIFI_PCM_BYTES (SYNC_WIFI_FRAMES_PER_PACKET*SYNC_WIFI_CHANNELS*(int)sizeof(int16_t))
#define SYNC_WIFI_PACKET_BYTES (4+4+8+SYNC_WIFI_PCM_BYTES)

#pragma pack(push,1)
struct SyncWifiPacket {
    uint32_t magic;   /* SYNC_WIFI_MAGIC */
    uint32_t seq;     /* increments by 1 per packet */
    uint64_t master_pts; /* absolute frame counter of FIRST frame in pcm */
    int16_t  pcm[SYNC_WIFI_FRAMES_PER_PACKET*SYNC_WIFI_CHANNELS];
};
#pragma pack(pop)

/* Locks the on-wire size. If a struct edit breaks this, old and new peers
 * silently mis-parse each other - fail the build instead of shipping noise. */
static_assert(sizeof(SyncWifiPacket) == SYNC_WIFI_PACKET_BYTES,
              "wire layout changed - bump SYNC_WIFI_MAGIC and update PROTOCOL.md");

#endif /* SYNC_WIFI_WIRE_H */