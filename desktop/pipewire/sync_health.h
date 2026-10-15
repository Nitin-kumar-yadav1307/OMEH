/*
 * sync_health.h — latency alignment + the 1 Hz health/status tick.
 *
 * Single-TU module, part of pipewire_sync.cpp. Included AFTER the type,
 * stream, and registry modules. Owns default_target_level(), finish_alignment()
 * (end of the probe phase — pick the reference latency, set per-device trims),
 * and on_health_timer() which logs drift/level and drives alignment.
 */
#pragma once


/* ------------------------------------------------------------------ */
/* Latency alignment                                                   */
/*                                                                     */
/*  While `aligning` is true every device is fed silence and we read    */
/*  how much delay its own chain (Bluetooth codec + buffers) reports.   */
/*  The slowest device becomes the reference; every other device gets   */
/*  an extra buffer of (ref - own) so they all emit the same sample at  */
/*  the same moment.                                                    */
/* ------------------------------------------------------------------ */
static double default_target_level(const DeviceOut *dev)
{
    double ms = ALIGN_BASE_MS + dev->extra_delay_ms;
    if (ms < 0.0)          ms = 0.0;
    if (ms > ALIGN_MAX_MS) ms = ALIGN_MAX_MS;
    return ms * (double)RATE / 1000.0;
}

static void finish_alignment()
{
    /* Reference = SLOWEST device, compared in exact frame counts.
       Reported ms is too coarse for like-for-like buds: averaged over
       two ticks every device reports ~180-190 ms, so we compare the
       raw per-tick probe totals (t.delay sums), which still differ. */
    app.ref_latency_ms = 0.0;
    for (DeviceOut *dev : app.devices)
        if (dev->dev_latency_ms > app.ref_latency_ms)
            app.ref_latency_ms = dev->dev_latency_ms;

    for (DeviceOut *dev : app.devices)
    {
        double align_ms = app.ref_latency_ms - dev->dev_latency_ms;
        if (align_ms < 0.0)          align_ms = 0.0;
        if (align_ms > ALIGN_MAX_MS) align_ms = ALIGN_MAX_MS;

        /*
         * A manual trim from the command line is authoritative: it
         * replaces the automatically measured alignment, so the user
         * can null out any residual offset deterministically.
         */
        if (dev->manual_trim)
            align_ms = 0.0;

        double ms = ALIGN_BASE_MS + align_ms + dev->extra_delay_ms;
        if (ms < 0.0)          ms = 0.0;
        if (ms > ALIGN_MAX_MS) ms = ALIGN_MAX_MS;

        double target_frames = ms * (double)RATE / 1000.0;

        /*
         * No snapping here.
         *
         * The drift controller converges onto target_level whatever it
         * is, so the ring depths of two devices end up differing by
         * exactly the difference of their targets. Snapping each target
         * to a whole graph quantum (5.3 ms) would quantise that
         * difference to a multiple of 5.3 ms and inject up to one
         * quantum of inter-device offset — an audible flam.
         */
        dev->avg_quantum = dev->meas_q_count
            ? dev->meas_q_sum / (double)dev->meas_q_count : 0.0;

        dev->target_level = target_frames;
        dev->measuring    = false;
        dev->started      = false;   /* re-fill buffer, then start */
        dev->ratio        = 1.0;
        dev->avg_level    = -1.0;

        fprintf(stdout,
                "[align] %-28s measured=%6.1f ms  added=%5.1f ms  "
                "buffer=%5.1f ms (quantum %.0f fr)\n",
                dev->name.c_str(), dev->dev_latency_ms,
                align_ms + dev->extra_delay_ms,
                1000.0 * dev->target_level / RATE, dev->avg_quantum);
    }
    fprintf(stdout, "[align] reference latency %.1f ms - "
                    "alignment locked, drift correction active\n",
            app.ref_latency_ms);
    fflush(stdout);

    app.aligning = false;
}

/* ------------------------------------------------------------------ */
/* 1-second health timer: always runs (independent of audio flow)      */
/*   - re-asserts default sink + client routing                        */
/*   - recreates capture if it died (never falls back to a mic)        */
/*   - reconnects playback streams (bud reconnect / error)             */
/*   - creates streams for buds connected after startup                */
/*   - drives the latency alignment phase and prints status            */
/* ------------------------------------------------------------------ */
static void on_health_timer(void *userdata, uint64_t expirations)
{
    (void)userdata; (void)expirations;

    /* keep the default sink and client routing pinned to us */
    reroute_clients();

    /* recreate the capture stream if it errored out */
    if (app.capture)
    {
        const char *err = nullptr;
        pw_stream_state st = pw_stream_get_state(app.capture, &err);
        if (st == PW_STREAM_STATE_ERROR)
        {
            fprintf(stderr, "[timer] capture error: %s — recreating\n",
                    err ? err : "?");
            pw_stream_destroy(app.capture);
            app.capture = nullptr;
        }
    }
    if (!app.capture && app.virtual_sink_id != 0)
        start_capture(app.virtual_sink_id);

    /* (re)start playback streams — but never a GUI-dropped one: its silence
       is intentional, and restarting it would undo the user's disconnect. */
    for (DeviceOut *dev : app.devices)
    {
        if (dev->dropped)
            continue;
        if (!dev->stream)
        {
            if (start_playback(dev))
            {
                fprintf(stdout, "[timer] stream started for %s\n",
                        dev->name.c_str());
                unmute_sink(dev);
            }
            continue;
        }
        const char *err = nullptr;
        pw_stream_state st = pw_stream_get_state(dev->stream, &err);
        if (st == PW_STREAM_STATE_ERROR)
        {
            fprintf(stderr, "[timer] %s stream error: %s — reconnecting\n",
                    dev->name.c_str(), err ? err : "?");
            pw_stream_destroy(dev->stream);
            dev->stream    = nullptr;
            dev->started   = false;
            dev->measuring = app.aligning;
            start_playback(dev);
        }
    }

    /* ---- latency alignment probe ---- */
    if (app.aligning)
    {
        app.align_ticks++;

        bool measured_all = !app.devices.empty();
        for (DeviceOut *dev : app.devices)
            if (!dev->dropped && dev->stream && dev->meas_count == 0)
                measured_all = false;

        /* also stop if the devices never report anything (timeout) */
        if (measured_all && app.align_ticks >= MEASURE_TICKS)
            finish_alignment();
        else if (app.align_ticks >= MEASURE_TICKS + 5)
        {
            for (DeviceOut *dev : app.devices)
                if (dev->meas_count == 0)
                    dev->dev_latency_ms = ALIGN_BASE_MS;
            finish_alignment();
        }
    }

    /* ---- status report (always, even when nothing is playing) ---- */
    if (!app.aligning)
    {
        /*
         * True per-second rate from CLOCK_MONOTONIC, not from the
         * nominal tick: this keeps honest what the monitor really
         * delivers (usually 48000/s) and turns the first tick into a
         * calibration tick instead of printing a bogus spike.
         */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;

        double cap_rate = -1.0;
        if (app.prev_tick_ns != 0 && now_ns > app.prev_tick_ns)
            cap_rate = 1e9 * (double)(app.master_written - app.prev_master) /
                       (double)(now_ns - app.prev_tick_ns);

        if (cap_rate >= 0.0)
            fprintf(stdout,
                    "\n[status] capture: %llu buffers (%llu empty)  "
                    "master=%llu frames (+%.0f/s)\n",
                    (unsigned long long)app.capture_callbacks,
                    (unsigned long long)app.capture_empties,
                    (unsigned long long)app.master_written,
                    cap_rate);
        else
            fprintf(stdout,
                    "\n[status] capture: %llu buffers (%llu empty)  "
                    "master=%llu frames\n",
                    (unsigned long long)app.capture_callbacks,
                    (unsigned long long)app.capture_empties,
                    (unsigned long long)app.master_written);

        app.prev_master = app.master_written;
        app.prev_tick_ns = now_ns;

        /*
         * Measure each device's TOTAL latency:
         *     total = our ring depth + the device's own delay
         * Both are expressed in frames so the trim below can act on
         * exact sample counts.
         */
        for (size_t i = 0; i < app.devices.size(); i++)
        {
            DeviceOut *dev = app.devices[i];

            double dev_frames = 0.0;
            if (dev->stream)
            {
                struct pw_time t;
                pw_stream_get_time_n(dev->stream, &t, sizeof(t));
                if (t.rate.num)
                {
                    double hz = (double)t.rate.denom / (double)t.rate.num;
                    if (hz > 0.0)
                        dev_frames = (double)t.delay / hz * (double)RATE;
                }
            }

            dev->dev_frames_s = dev->dev_frames_s < 0.0 ? dev_frames
                : dev->dev_frames_s + ALIGN_SMOOTH * (dev_frames - dev->dev_frames_s);

            double total = (double)dev->ring.level() + dev->dev_frames_s;
            dev->total_frames = dev->total_frames < 0.0 ? total
                : dev->total_frames + ALIGN_SMOOTH * (total - dev->total_frames);

            dev->last_latency_ms = 1000.0 * dev->total_frames / RATE;
            dev->last_drift_frames =
                (double)dev->ring.level() - dev->target_level;
        }

        /*
         * NOTE: there is deliberately no continuous "self calibrating"
         * trim here.
         *
         * The device delay a Bluetooth earbud reports is quantised to
         * the graph quantum (5.3 ms) and noisy, so a live loop that
         * keeps moving target_level from those readings walks the
         * targets around and the earbuds drift apart (that is the
         * "perfectly in sync at first, echoing after a while" symptom).
         *
         * The offsets are therefore frozen once, from the averaged
         * probe taken during the alignment phase, where each device is
         * measured many times while silenced. The drift controller then
         * only has to hold each ring at that fixed depth.
         */

        /*
         * Inter-device offset report.
         *
         * What the ear hears is  ring depth + device delay,  so the
         * residual offset between two earbuds is the difference of that
         * sum. The frozen (averaged) device latency is used here - the
         * instantaneous one is quantised (one graph quantum) and jumps
         * around. Device 0 is the reference, so its offset is 0.00, and
         * a difference of about one quantum (5.3 ms here) is the floor
         * this measurement can resolve.
         */
        double ref_total_ms = app.devices.empty() ? 0.0
            : 1000.0 * (double)app.devices[0]->ring.level() / RATE
              + app.devices[0]->dev_latency_ms;

        for (size_t i = 0; i < app.devices.size(); i++)
        {
            DeviceOut *dev = app.devices[i];

            double depth_ms = 1000.0 * (double)dev->ring.level() / RATE;
            double emit_ms  = depth_ms + dev->dev_latency_ms;
            dev->last_latency_ms = emit_ms;
            dev->last_drift_frames =
                (double)dev->ring.level() - dev->target_level;

            fprintf(stdout,
                    "  dev %zu %-28s latency=%7.1f ms  offset=%+7.2f ms  "
                    "ratio=%.6f  buf=%zu  underruns=%llu\n",
                    i, dev->name.c_str(),
                    emit_ms,
                    emit_ms - ref_total_ms,
                    dev->ratio,
                    dev->ring.level(),
                    (unsigned long long)dev->underruns);
        }
        app.prev_master = app.master_written;
    }
    fflush(stdout);

    /* --ui: one machine-readable status block per tick, after the human log */
    if (ui_mode)
        ui_emit_status();
}

