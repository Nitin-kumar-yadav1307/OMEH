/*
 * win_ui.h — opt-in "--ui" machine mode: pipe commands in, status out.
 *
 * Single-TU module, part of pipewire_sync_win.cpp. Included LAST, AFTER
 * win_health.h, because it emits status from the health tick and executes
 * commands that touch the resolved DeviceOut/App state.
 *
 * Wire format: desktop/ui_protocol.h. stdout -> GUI carries '#'-prefixed
 * machine lines ("#S" summary + one "#D" per device); human logs never start
 * with '#', so the GUI splits data from log by prefix alone. stdin <- GUI
 * carries one command per line ("ping", "trim <i> <ms>", "mute <i> <0|1>",
 * "volume <i> <pct>", "connect <i> <0|1>", "select <i>", "click", "quit").
 *
 * Polling model: the Windows engine has no fd event loop — main_loop() spins
 * on Sleep(POLL_MS) — so win_ui_poll() is called from the loop each
 * iteration and uses PeekNamedPipe to drain stdin WITHOUT blocking. Command
 * effects therefore run on the main loop thread, the same thread that runs
 * health_tick() and poll_keyboard(), never on a WASAPI callback.
 *
 * Default (no --ui): g_ui_mode stays false, nothing here runs. Keyboard
 * control keeps working; --ui only ADDS the pipe channel.
 *
 * Trim mapping: absolute "trim <i> <ms>" sets extra_delay_ms and recomputes
 * target_level via default_target_level(), exactly like the keyboard path's
 * apply_delay_shift() with remember=true (persists to trim_by_id +
 * trim_save()). Gone/failed devices are skipped in status but keep their
 * dense index, so GUI indices stay stable across reconnects.
 */
#pragma once

#include "../ui_protocol.h"

/* g_ui_mode + win_ui_poll/emit() are declared in win_types.h (defined here). */

/* ------------------------------------------------------------------ */
/* Status emission (main loop, once per health tick + once at startup) */
/* ------------------------------------------------------------------ */
static void win_ui_emit_status()
{
    UiSummary s;
    s.aligning = app.aligning ? 1 : 0;
    s.devices  = (unsigned)app.devices.size();
    s.ref_ms   = app.ref_latency_ms;
    s.master   = app.master_written;

    std::string summary = ui_format_summary(s);
    fprintf(stdout, "%s\n", summary.c_str());

    /* Reference "total emit" latency uses the first live device, matching
       the human print_status() line. */
    double ref_total_ms = 0.0;
    for (DeviceOut *d0 : app.devices)
        if (!d0->gone) { ref_total_ms = 1000.0 * (double)d0->ring.level() / RATE
                                          + d0->dev_latency_ms; break; }

    for (size_t i = 0; i < app.devices.size(); i++)
    {
        const DeviceOut *dev = app.devices[i];
        double depth_ms = 1000.0 * (double)dev->ring.level() / RATE;
        double emit_ms  = depth_ms + dev->dev_latency_ms;

        UiDeviceStatus d;
        d.idx       = (int)i;
        d.lat       = emit_ms;
        d.off       = emit_ms - ref_total_ms;
        d.ratio     = dev->ratio;
        d.buf       = (unsigned long long)dev->ring.level();
        d.underruns = (unsigned long long)dev->underruns;
        d.trim      = dev->extra_delay_ms;
        d.meas      = dev->measuring;
        d.muted = dev->muted;
        d.conn  = true;               /* Windows plays all sinks; never detached */
        d.vol   = dev->vol;           /* persisted from the last volume command */
        d.name      = dev->name;

        std::string line = ui_format_device(d);
        fprintf(stdout, "%s\n", line.c_str());
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Command effects (main loop thread)                                  */
/* ------------------------------------------------------------------ */
static void win_ui_set_mute(DeviceOut *dev, bool on)
{
    if (!dev) return;
    // Echo the requested state even when the endpoint is gone, so the GUI
    // checkbox tracks intent and the next status tick reflects it.
    dev->muted = on;
    if (!dev->endpoint) return;
    IAudioEndpointVolume *v = nullptr;
    if (SUCCEEDED(dev->endpoint->Activate(IID_IAudioEndpointVolume_,
                                           CLSCTX_ALL, nullptr, (void **)&v)) && v)
    {
        v->SetMute(on ? TRUE : FALSE, nullptr);
        v->Release();
    }
}

static void win_ui_set_volume(DeviceOut *dev, double pct)
{
    if (!dev) return;
    // Match the Linux ui_apply_volume(): 0% mutes (and echoes muted=1),
    // positive values unmute and drive the scalar. dev->vol always echoes
    // the last GUI value so the status line stays truthful.
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    dev->vol = pct;
    if (pct <= 0.0)
    {
        win_ui_set_mute(dev, true);
        return;
    }
    win_ui_set_mute(dev, false);
    if (!dev->endpoint) return;
    IAudioEndpointVolume *v = nullptr;
    if (SUCCEEDED(dev->endpoint->Activate(IID_IAudioEndpointVolume_,
                                           CLSCTX_ALL, nullptr,
                                           (void **)&v)) && v)
    {
        /* WASAPI device-vector scalars run 0.0-1.0. */
        double s = dev->vol / 100.0;
        v->SetMasterVolumeLevelScalar((float)s, nullptr);
        v->Release();
    }
}

static void win_ui_handle_command(const UiCommand &c)
{
    switch (c.op)
    {
    case UI_CMD_PING:
        fprintf(stdout, "%s\n", ui_format_ack("pong").c_str());
        break;

    case UI_CMD_TRIM:
        if (c.idx >= 0 && c.idx < (int)app.devices.size())
        {
            DeviceOut *dev = app.devices[(size_t)c.idx];
            /* Absolute trim: reuse the keyboard path's delta machinery so
               ring fill/drain, avg_level fixup and trim persistence behave
               identically to pressing ] [ . , */
            double clamped = c.value;
            if (clamped < -ALIGN_MAX_MS) clamped = -ALIGN_MAX_MS;
            if (clamped >  ALIGN_MAX_MS) clamped =  ALIGN_MAX_MS;
            apply_delay_shift(dev, clamped - dev->extra_delay_ms, true);
            fprintf(stdout, "%s\n", ui_format_ack("trim ok").c_str());
        }
        else
            fprintf(stdout, "%s\n", ui_format_ack("trim bad-index").c_str());
        break;

    case UI_CMD_MUTE:
        if (c.idx >= 0 && c.idx < (int)app.devices.size())
        {
            win_ui_set_mute(app.devices[(size_t)c.idx], c.value != 0.0);
            fprintf(stdout, "%s\n", ui_format_ack("mute ok").c_str());
        }
        else
            fprintf(stdout, "%s\n", ui_format_ack("mute bad-index").c_str());
        break;

    case UI_CMD_VOLUME:
        if (c.idx >= 0 && c.idx < (int)app.devices.size())
        {
            DeviceOut *dev = app.devices[(size_t)c.idx];
            // win_ui_set_volume() clamps, echoes vol/muted, and drives the
            // WASAPI scalar, so the next status tick reflects the GUI value.
            win_ui_set_volume(dev, c.value);
            fprintf(stdout, "%s\n", ui_format_ack("volume ok").c_str());
        }
        else
            fprintf(stdout, "%s\n", ui_format_ack("volume bad-index").c_str());
        break;

    case UI_CMD_CONNECT:
        /* Windows always plays on every Bluetooth sink; a GUI "reconnect"
           is therefore a no-op that just confirms state, unlike the
           Linux engine which drops/re-attaches the stream. */
        if (c.idx >= 0 && c.idx < (int)app.devices.size())
        {
            DeviceOut *dev = app.devices[(size_t)c.idx];
            dev->muted = false;       /* GUI reconnect implies audible */
            if (dev->endpoint)
            {
                IAudioEndpointVolume *v = nullptr;
                if (SUCCEEDED(dev->endpoint->Activate(
                        IID_IAudioEndpointVolume_,
                        CLSCTX_ALL, nullptr, (void **)&v)) && v)
                {
                    v->SetMute(FALSE, nullptr);
                    v->Release();
                }
            }
            fprintf(stdout, "%s\n", ui_format_ack("connect ok").c_str());
        }
        else
            fprintf(stdout, "%s\n", ui_format_ack("connect bad-index").c_str());
        break;

    case UI_CMD_SELECT:
        /* Unlike the Tab key this does not print a [tune] line (that would
           pollute the GUI log); it just moves the shared cursor so the next
           keyboard trim acts where the GUI points. */
        if (c.idx >= 0 && c.idx < (int)app.devices.size())
        {
            g_selected = c.idx;
            fprintf(stdout, "%s\n", ui_format_ack("select ok").c_str());
        }
        else
            fprintf(stdout, "%s\n", ui_format_ack("select bad-index").c_str());
        break;

    case UI_CMD_CLICK:
        click_trigger();   /* prints its own [tune] line into the GUI log */
        fprintf(stdout, "%s\n", ui_format_ack("click ok").c_str());
        break;

    case UI_CMD_QUIT:
        fprintf(stdout, "%s\n", ui_format_ack("bye").c_str());
        fflush(stdout);
        InterlockedExchange(&g_stop, 1);
        if (g_stop_evt) SetEvent(g_stop_evt);
        break;

    case UI_CMD_NONE:
    default:
        break;   /* ignore unknown/partial lines — never kill the stream */
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* stdin poll (called once per main_loop() iteration when --ui)        */
/*                                                                     */
/* PeekNamedPipe reports queued bytes on the stdin pipe WITHOUT consuming
 * them, so an idle GUI costs one cheap syscall per 2 ms tick and a live one
 * is drained line by line. A console stdin (interactive run) is not a pipe:
 * PeekNamedPipe fails there and we simply do nothing, leaving _kbhit()
 * keyboard control untouched.                                          */
/* ------------------------------------------------------------------ */
static void win_ui_poll()
{
    if (!g_ui_mode) return;
    static std::string buf;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
    for (;;)
    {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr))
            return;   /* not a pipe (console) or broken: leave keyboard alone */
        if (avail == 0)
            return;   /* drained */
        char tmp[512];
        DWORD chunk = avail > sizeof(tmp) ? sizeof(tmp) : avail;
        DWORD got = 0;
        if (!ReadFile(h, tmp, chunk, &got, nullptr) || got == 0)
            return;
        buf.append(tmp, got);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos)
        {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty()) continue;
            win_ui_handle_command(ui_parse_command(line));
        }
        if (buf.size() > 8192)
            buf.clear();   /* no newline after 8 KB: drop, don't grow */
    }
}
