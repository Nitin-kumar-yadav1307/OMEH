/*
 * pipewire_sync.cpp
 *
 * Sends the same audio to ALL connected Bluetooth earbuds,
 * while measuring and correcting latency differences and
 * clock drift between the devices.
 *
 * Audio source:
 *   The MONITOR of the default output sink. This means every
 *   sound the machine plays (browser, media player, etc.) is
 *   captured automatically without touching the applications.
 *
 * How it works:
 *   1. A registry scan discovers every Bluetooth sink node
 *      (media.class = "Audio/Sink", bluez device api).
 *   2. One capture stream records the default sink monitor.
 *      Captured frames are copied into a private ring buffer
 *      for every discovered output device.
 *   3. One playback stream per earbud is created, pinned by
 *      node name so it can never land on the "Sync Master"
 *      sink (that would silence the earbuds AND loop the audio
 *      back into our own capture = echo).
 *   4. LATENCY ALIGNMENT: for the first ~2 s every earbud is
 *      fed silence while we read how much data its own
 *      (Bluetooth codec + buffer) chain reports as queued
 *      (pw_stream_get_time_n). The device with the largest
 *      latency is the reference; every other device gets an
 *      extra buffer delay so all of them emit the same sample
 *      at the same moment.
 *   5. CLOCK DRIFT: for each earbud we hold its private ring
 *      buffer at a target depth. Since the capture writes one
 *      frame per captured frame into every ring, the ring depth
 *      equals (master frames - frames this device consumed), so
 *      keeping every ring at its target keeps every earbud at the
 *      same position in the stream. The depth error drives a
 *      per-device adaptive resampler (linear interpolation, ratio
 *      clamped around 1.0 +/- 500 ppm) so the earbuds never drift
 *      apart. A depth error larger than 50 ms is corrected by a
 *      jump instead of a warp (e.g. after a reconnect).
 *
 * Optional per-device extra latency trim (ms), added on top of
 * the automatically measured alignment, e.g.:
 *   ./pipewire_sync 0:0 1:12
 *   -> device #0 gets no extra delay, device #1 gets 12 ms
 *      (useful to fine tune a device whose reported latency is
 *      not the whole truth, e.g. codec-internal buffering).
 *
 * Build:
 *   g++ -O2 -o pipewire_sync pipewire_sync.cpp \
 *       $(pkg-config --cflags --libs libpipewire-0.3)
 */

/*
 * This translation unit is split by concern into single-TU modules below.
 * They are ordinary headers included IN ORDER into this one .cpp, so the
 * compiled program is exactly the old single file - but each area is now
 * its own place to read, edit and debug. Do not reorder the includes:
 * later modules use the types, globals and forward decls from earlier ones.
 *
 *   sync_config.h    includes + tuning constants
 *   sync_types.h     Ring / DeviceOut / App + globals
 *   sync_playback.h  real-time render path (drift resampler, AIMD)
 *   sync_streams.h   real-time capture path + stream setup
 *   sync_pulse.h     pactl routing / virtual sink
 *   sync_registry.h  Bluetooth sink discovery + single-instance guard
 *   sync_health.h    latency alignment + 1 Hz health tick
 *   sync_ui.h        optional "--ui" machine channel for the desktop GUI
 *
 * The original "Build:" command above still works - quoted includes are
 * resolved next to this file, and CMake compiles just this one .cpp.
 */

#include "sync_config.h"
#include "sync_types.h"
#include "sync_playback.h"
#include "sync_streams.h"
#include "sync_pulse.h"
#include "sync_registry.h"
#include "sync_health.h"
#include "sync_ui.h"
/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (another_instance_running())
    {
        fprintf(stderr,
                "Another pipewire_sync instance is already running.\n"
                "Stop it first (killall pipewire_sync) — two instances "
                "would fight over routing.\n");
        return 1;
    }

    pw_init(&argc, &argv);

    auto delays = parse_delay_args(argc, argv);

    /*
     * --ui: opt-in machine mode for the desktop GUI. Human logs stay exactly
     * as they are; status additionally goes to stdout as '#'-prefixed lines
     * and stdin becomes a command channel. Without the flag nothing changes.
     * ("--ui" has no ':', so parse_delay_args above already ignores it.)
     */
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--ui") == 0)
            ui_mode = true;

    app.loop = pw_main_loop_new(nullptr);
    if (!app.loop)
    {
        fprintf(stderr, "failed to create main loop\n");
        return 1;
    }

    struct pw_context *context = pw_context_new(
        pw_main_loop_get_loop(app.loop), nullptr, 0);
    if (!context)
    {
        fprintf(stderr, "failed to create context\n");
        return 1;
    }

    struct pw_core *core = pw_context_connect(context, nullptr, 0);
    if (!core)
    {
        fprintf(stderr, "failed to connect to PipeWire\n");
        return 1;
    }
    app.core = core;

    app.registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

    static struct spa_hook registry_listener;
    spa_zero(registry_listener);
    pw_registry_add_listener(app.registry, &registry_listener,
                             &registry_events, nullptr);

    /* clean shutdown on Ctrl+C so we can restore the default sink */
    signal(SIGINT,  [](int){ pw_main_loop_quit(app.loop); });
    signal(SIGTERM, [](int){ pw_main_loop_quit(app.loop); });

    /* remember the current default sink so we can restore it later */
    if (remember_default_sink())
        fprintf(stdout, "[main] previous default sink: %s\n",
                app.prev_default_sink.c_str());

    /* create the silent virtual sink that apps will play into */
    if (!create_virtual_sink())
    {
        fprintf(stderr, "failed to create virtual sink\n");
        return 1;
    }

    /*
     * Pump the loop synchronously until the registry has
     * enumerated the Bluetooth devices.
     */
    int rounds = 0;
    while (app.devices.empty() && rounds < 50)
    {
        pw_loop_iterate(pw_main_loop_get_loop(app.loop), 100);
        rounds++;
    }

    if (app.devices.empty())
    {
        fprintf(stderr,
                "No Bluetooth sinks found. Are the earbuds connected?\n"
                "  (check with: pw-cli ls Node | grep -B2 -A4 bluez)\n");
        /* --ui: still emit a devices=0 snapshot so the GUI learns the pipe
           works and there is simply nothing connected, rather than hanging
           on a silent engine that is about to exit. */
        if (ui_mode)
            ui_emit_status();
        return 1;
    }

    /* apply per-device extra delay trims from argv */
    for (auto &kv : delays)
        if (kv.first >= 0 && kv.first < (int)app.devices.size())
        {
            app.devices[kv.first]->extra_delay_ms = kv.second;
            app.devices[kv.first]->manual_trim    = true;
            fprintf(stdout, "[main] device %d gets +%.1f ms extra delay\n",
                    kv.first, kv.second);
        }

    /* create one playback stream per earbud */
    for (size_t i = 0; i < app.devices.size(); i++)
    {
        DeviceOut *dev = app.devices[i];
        if (!start_playback(dev))
        {
            fprintf(stderr, "failed to create stream for %s\n",
                    dev->name.c_str());
            return 1;
        }
        fprintf(stdout, "[main] stream started for %s\n", dev->name.c_str());

        /* make sure the bud is unmuted and audible */
        unmute_sink(dev);
    }

    /* capture the monitor of the virtual sink */
    if (!start_capture(app.virtual_sink_id))
    {
        fprintf(stderr, "failed to start capture\n");
        return 1;
    }

    /*
     * Route all application audio into the silent virtual sink.
     * This also guarantees the earbuds only receive audio from
     * this program (no double playback).
     */
    if (app.virtual_sink_id != 0)
        set_default_node(app.virtual_sink_id);

    /* move already-running apps (browser etc.) into the virtual sink */
    reroute_clients();

    /*
     * 1-second health timer: re-asserts routing, reconnects errored
     * streams, and picks up buds connected after startup.
     */
    struct spa_source *timer = pw_loop_add_timer(
        pw_main_loop_get_loop(app.loop), on_health_timer, nullptr);
    if (timer)
    {
        struct timespec interval;
        interval.tv_sec  = 1;             /* 1 second */
        interval.tv_nsec = 0;
        pw_loop_update_timer(pw_main_loop_get_loop(app.loop),
                             timer, &interval, &interval, false);
    }

    fprintf(stdout,
            "\nSync running: sending default-monitor audio to %zu "
            "Bluetooth device(s). Ctrl+C to stop.\n\n",
            app.devices.size());
    fflush(stdout);

    /* --ui: watch stdin for GUI commands + emit the first status snapshot.
       No-op unless the engine was launched with --ui. */
    ui_setup();

    pw_main_loop_run(app.loop);

    /* restore the previous default sink */
    if (!app.prev_default_sink.empty() &&
        app.prev_default_sink != app.virtual_sink_name)
        set_default_sink(app.prev_default_sink.c_str());

    /* unload the pulse null-sink module (removes the virtual sink) */
    if (app.null_module_index > 0)
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "pactl unload-module %d 2>/dev/null",
                 app.null_module_index);
        int r = system(cmd);
        (void)r;
        fprintf(stdout, "[main] virtual sink removed\n");
    }

    /* cleanup */
    for (DeviceOut *dev : app.devices)
    {
        if (dev->stream) pw_stream_destroy(dev->stream);
        delete dev;
    }
    if (app.capture) pw_stream_destroy(app.capture);
    pw_main_loop_destroy(app.loop);
    pw_deinit();
    return 0;
}






