/*
 * win_health.h — status line + the periodic health check.
 *
 * Single-TU module, part of pipewire_sync_win.cpp. Included AFTER the type
 * and poll modules. Owns print_status() (the on-screen per-device line) and
 * health_tick(), which watches for stalls/underruns and logs reconnects.
 */
#pragma once


static void print_status(uint64_t now)
{
    double cap_rate = -1.0;
    if (app.prev_tick_ms != 0 && now > app.prev_tick_ms)
        cap_rate = 1000.0 * (double)(app.master_written - app.prev_master) /
                   (double)(now - app.prev_tick_ms);
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
    app.prev_master  = app.master_written;
    app.prev_tick_ms = now;
    double ref_total_ms = 0.0;
    for (DeviceOut *d0 : app.devices)
        if (!d0->gone) { ref_total_ms = 1000.0 * (double)d0->ring.level() / RATE
                                        + d0->dev_latency_ms; break; }
    size_t i = 0;
    for (DeviceOut *dev : app.devices)
    {
        if (!dev->gone)
        {
            double depth_ms = 1000.0 * (double)dev->ring.level() / RATE;
            double emit_ms  = depth_ms + dev->dev_latency_ms;
            dev->last_latency_ms   = emit_ms;
            dev->last_drift_frames =
                (double)dev->ring.level() - dev->target_level;
            fprintf(stdout,
                    "  dev %zu%s %-26s latency=%7.1f ms  offset=%+7.2f ms  "
                    "trim=%+5.0f ms  buf=%zu  underruns=%llu  jumps=%llu\n",
                    i, (i == (size_t)g_selected) ? "*" : " ",
                    dev->name.c_str(),
                    emit_ms,
                    emit_ms - ref_total_ms,
                    dev->extra_delay_ms,
                    dev->ring.level(),
                    (unsigned long long)dev->underruns,
                    (unsigned long long)dev->hard_syncs);
        }
        i++;
    }
    fflush(stdout);
}
static void health_tick(uint64_t now)
{
    if (app.switched_default)
    {
        std::wstring cur;
        std::string  cur_name;
        if (get_default_id(cur, cur_name) && cur != app.src_id)
        {
            if (win_set_default(app.src_id))
                fprintf(stdout,
                        "[timer] default output drifted - re-asserted \"%s\"\n",
                        app.src_name.c_str());
        }
    }
    if (!app.cap)
    {
        fprintf(stderr, "[timer] capture not running - restarting\n");
        fflush(stderr);
        start_capture();
    }
    poll_devices(true);
    for (DeviceOut *dev : app.devices)
    {
        if (dev->failed && !dev->gone && dev->endpoint)
        {
            if (start_playback(dev))
            {
                dev->failed = false;
                fprintf(stdout, "[timer] stream restarted for %s\n",
                        dev->name.c_str());
                fflush(stdout);
                unmute_device(dev);
            }
        }
    }
    {
        bool any_stream = false;
        for (DeviceOut *dev : app.devices)
            if (!dev->gone && !dev->failed && dev->client && dev->started)
                any_stream = true;
        if (any_stream)             source_silence();
        else if (g_srcvol.silenced) source_restore();
    }
    if (app.aligning)
    {
        app.align_ticks++;
        bool measured_all = !app.devices.empty();
        for (DeviceOut *dev : app.devices)
            if (!dev->gone && !dev->failed && dev->meas_count == 0)
                measured_all = false;
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
    if (!app.aligning)
        print_status(now);
    else
        fflush(stdout);
}
static BOOL WINAPI on_console_ctrl(DWORD type)
{
    InterlockedExchange(&g_stop, 1);
    if (g_stop_evt) SetEvent(g_stop_evt);
    if ((type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
         type == CTRL_SHUTDOWN_EVENT) && g_clean_evt)
        WaitForSingleObject(g_clean_evt, 4000);
    return TRUE;
}
