/*
 * sync_wifi.h - umbrella header for the shared sync engine.
 *
 * The implementation is split by concern so each piece stays readable and
 * testable on its own. Include this one file to get everything:
 *
 *   wire.h    the on-wire contract: constants + SyncWifiPacket. THE single
 *             definition every target compiles (Android JNI, desktop tools).
 *   ring.h    the client's stereo sample ring (float read position).
 *   drift.h   the drift PI controller. Pure math, no I/O.
 *   jitter.h  the AIMD adaptive-buffer policy. Pure policy, no I/O.
 *
 * Role model (v1):
 *   MASTER (your phone): captures/generates 48k stereo, plays it LOCALLY
 *     to its own BT bud (master is the clock, no warp needed), AND
 *     broadcasts it over UDP with an absolute frame counter (master_pts).
 *   CLIENT (friend's phone): receives UDP, holds its own Ring at the
 *     target depth where level = master_pts - consumed, and warps its
 *     local playout with the SAME drift PI controller as pipewire_sync.
 *
 * Transport: UDP port 48200, 10ms packets (480 frames @48k). ~200KB/s per
 * client. No deps beyond libc.
 */
#ifndef SYNC_WIFI_H
#define SYNC_WIFI_H

#include "wire.h"
#include "ring.h"
#include "drift.h"
#include "jitter.h"

#endif /* SYNC_WIFI_H */