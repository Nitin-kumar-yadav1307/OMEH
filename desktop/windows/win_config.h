/*
 * win_config.h — Windows/WASAPI build configuration.
 *
 * Single-TU module, part of pipewire_sync_win.cpp. Included FIRST. Owns
 * WIN32_LEAN_AND_MEAN, the COM/WASAPI headers, the IAudio*/MMDevice GUIDs
 * and PROPERTYKEYs, and the RATE/CHANNELS/ring/drift tuning constants.
 */
#pragma once


#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <propkey.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <conio.h>
#include <string>
#include <vector>
#include <map>
#include <set>
static const GUID IID_IAudioClient_         = {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const GUID IID_IAudioRenderClient_   = {0xf294acfc,0x3146,0x4483,{0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2}};
static const GUID IID_IAudioCaptureClient_  = {0xc8adbd64,0xe71e,0x48a0,{0xa4,0xde,0x18,0x5c,0x39,0x5c,0xd3,0x17}};
static const GUID IID_IAudioEndpointVolume_ = {0x5cdf2c82,0x841e,0x4546,{0x97,0x22,0x0c,0xf7,0x40,0x78,0x22,0x9a}};
static const GUID IID_IMMDeviceEnumerator_  = {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID CLSID_MMDeviceEnumerator_ = {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const PROPERTYKEY PK_FriendlyName = {
    {0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14};
static const PROPERTYKEY PK_InstanceId = {
    {0x78c34fc8,0x104a,0x4aca,{0x9e,0xa4,0x52,0x4d,0x52,0x99,0x6e,0x57}}, 256};
static const PROPERTYKEY PK_EnumeratorName = {
    {0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 24};
static const PROPERTYKEY PK_DeviceInterface = {
    {0xb3f8fa53,0x0004,0x438e,{0x90,0x03,0x51,0xa4,0x6e,0x13,0x9b,0xfc}}, 2};
#ifndef AUDCLNT_STREAMFLAGS_LOOPBACK
#define AUDCLNT_STREAMFLAGS_LOOPBACK 0x00020000
#endif
#define RATE            48000
#define CHANNELS        2
#define FRAME_BYTES     (CHANNELS * sizeof(int16_t))
#define RING_BITS       15
#define RING_SIZE       (1u << RING_BITS)
#define RING_MASK       (RING_SIZE - 1)
#define DRIFT_TAU       10.0
#define DRIFT_LOOP_TAU  40.0
#define DRIFT_KP        (1.0 / ((double)RATE * DRIFT_LOOP_TAU))
#define DRIFT_MAX_PPM   1500.0
#define HARD_SYNC_FRAMES 2400.0
#define ALIGN_BASE_MS   10.0
#define ALIGN_MAX_MS    350.0
#define ALIGN_SMOOTH    0.3
#define ALIGN_NOISE_MS  3.0
#define MEASURE_TICKS   2
#define POLL_MS         2
#define HEALTH_MS       1000
#define XFADE_FRAMES    144
#define IDLE_GAP_MS     40
#define CLICK_BURSTS    4
#define CLICK_PERIOD    (RATE * 35 / 100)
#define CLICK_LEN       (RATE / 50)
#define CLICK_LEAD      (RATE / 10)
#define OUR_APP_NAME    "pipewire_sync"
