/*
 * pipewire_sync_win.cpp
 *
 * Windows/WASAPI port: plays the same audio on ALL connected Bluetooth
 * earbuds, measuring and correcting latency + clock drift between them.
 *
 * This translation unit is split by concern into single-TU modules below.
 * They are ordinary headers included IN ORDER into this one .cpp, so the
 * compiled program is exactly the old single file - but each area is now
 * its own place to read, edit and debug. Do not reorder the includes:
 * later modules use the types, globals and forward decls from earlier ones.
 *
 *   win_config.h    WASAPI/COM headers + GUIDs + tuning constants
 *   win_types.h     Ring / F32Resampler / DeviceOut / App + globals
 *   win_device.h    device enumeration + default-device switching
 *   win_capture.h   loopback capture stream lifecycle
 *   win_source.h    source volume, silencing, tone-test tooling
 *   win_pipeline.h  packet decode + per-device render feed
 *   win_align.h     delay/trim math + playback stream setup
 *   win_render.h    real-time render feed (drift resampler + alignment)
 *   win_poll.h      alignment finish, device polling, interactive controls
 *   win_health.h    status line + periodic health check
 *   win_ui.h        optional "--ui" machine channel for the desktop GUI
 *   win_main.h      main loop, cleanup, CLI parsing, console control
 *
 * Build (unchanged - quoted includes resolve next to this file):
 *   g++ -O2 -std=c++17 -Wall pipewire_sync_win.cpp -o pipewire_sync_win.exe \
 *       -lole32 -loleaut32 -luuid -lwinmm
 */

#include "win_config.h"
#include "win_types.h"
#include "win_device.h"
#include "win_capture.h"
#include "win_source.h"
#include "win_pipeline.h"
#include "win_align.h"
#include "win_render.h"
#include "win_poll.h"
#include "win_health.h"
#include "win_main.h"
#include "win_ui.h"

int main(int argc, char *argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    Flags flags = parse_args(argc, argv);
    if (flags.help)
    {
        print_usage();
        return 0;
    }
    g_silent_source = !flags.audible_source;
    g_run_seconds   = flags.seconds;
    g_ui_mode       = flags.ui;
    bool root_console = console_is_root() &&
                       !flags.list && flags.seconds <= 0;
    int rc = 0;
    HANDLE mtx = CreateMutexW(nullptr, TRUE,
                              L"Local\\pipewire_sync_singleton");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        fprintf(stderr,
            "Another pipewire_sync instance is already running.\n"
            "Stop it first (close its window or Ctrl+C) — two instances "
            "would fight over routing.\n");
        CloseHandle(mtx);
        pause_if_root(root_console);
        return 1;
    }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        fprintf(stderr, "CoInitializeEx failed: 0x%08lx\n", (unsigned long)hr);
        pause_if_root(root_console);
        return 1;
    }
    timeBeginPeriod(1);
    g_stop_evt  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_clean_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    hr = CoCreateInstance(CLSID_MMDeviceEnumerator_, nullptr, CLSCTX_ALL,
                          IID_IMMDeviceEnumerator_, (void **)&app.enumerator);
    if (FAILED(hr) || !app.enumerator)
    {
        fprintf(stderr, "Cannot create IMMDeviceEnumerator: 0x%08lx\n",
                (unsigned long)hr);
        cleanup_all();
        if (mtx) CloseHandle(mtx);
        pause_if_root(root_console);
        return 1;
    }
    fprintf(stdout, "%s (Windows/WASAPI port)\n", OUR_APP_NAME);
    fflush(stdout);
    print_all_outputs();
    fflush(stdout);
    if (flags.list)
    {
        print_endpoint_details();
        cleanup_all();
        if (mtx) CloseHandle(mtx);
        return 0;
    }
    trim_load();
    poll_devices(false);
    if (app.devices.empty())
    {
        fprintf(stderr,
            "No Bluetooth sinks found. Are the earbuds connected?\n"
            "  (check in Settings > Bluetooth & devices)\n");
        cleanup_all();
        if (mtx) CloseHandle(mtx);
        pause_if_root(root_console);
        return 1;
    }
    for (auto &kv : flags.delays)
        if (kv.first >= 0 && kv.first < (int)app.devices.size())
        {
            DeviceOut *dev = app.devices[kv.first];
            dev->extra_delay_ms = kv.second;
            dev->manual_trim    = true;
            app.trim_by_id[dev->id] = kv.second;
            fprintf(stdout, "[main] device %d gets +%.1f ms extra delay\n",
                    kv.first, kv.second);
        }
    if (!pick_source())
    {
        cleanup_all();
        if (mtx) CloseHandle(mtx);
        pause_if_root(root_console);
        return 1;
    }
    if (!start_capture())
    {
        cleanup_all();
        if (mtx) CloseHandle(mtx);
        pause_if_root(root_console);
        return 1;
    }
    int silence_probe = g_silent_source ? source_silence_self_test() : 0;
    if (g_silent_source && silence_probe == 0)
        g_silent_source = false;
    // Probe 1 (mute-safe) and 2 (vol0 fallback) both stream quietly; -1 is
    // inconclusive (no tone at all) so still attempt mute — old behaviour.
    // Anything else (0) means no quiet mode works -> stay audible.
    if (g_silent_source && silence_probe != 0)
        source_silence();
    if (g_run_seconds > 0)
        fprintf(stdout, "[main] will stop by itself after %d s\n",
                g_run_seconds);
    for (DeviceOut *dev : app.devices)
    {
        if (start_playback(dev))
        {
            fprintf(stdout, "[main] stream started for %s (%s, %.0f Hz)\n",
                dev->name.c_str(),
                dev->use_mix ? "mix format" : "48 kHz PCM",
                dev->use_mix ? dev->mix_rate : (double)RATE);
            unmute_device(dev);
        }
        else
        {
            fprintf(stderr, "failed to create stream for %s\n",
                    dev->name.c_str());
            dev->failed = true;
        }
    }
    fprintf(stdout,
            "\nSync running: sending default-output audio to %zu "
            "Bluetooth device(s). Ctrl+C to stop.\n\n",
            app.devices.size());
    fflush(stdout);
    main_loop();
    cleanup_all();
    if (mtx) CloseHandle(mtx);
    pause_if_root(root_console);
    return rc;
}
