/*
 * sync_ui.h — opt-in "--ui" machine mode: pipe commands in, status out.
 *
 * Single-TU module, part of pipewire_sync.cpp. Included LAST, AFTER
 * sync_health.h, because it emits status from the health tick and executes
 * commands that touch the resolved DeviceOut/App state.
 *
 * How the pipe works (see desktop/ui_protocol.h for the wire format):
 *
 *   stdout -> GUI : on every 1 Hz health tick (and once at startup) we print
 *                   a machine block: a "#S ..." summary line plus one
 *                   "#D ..." line per device. Human log lines never start
 *                   with '#', so the GUI splits data from log by prefix alone.
 *   stdin  <- GUI : we watch fd 0 with pw_loop_add_io on the MAIN PipeWire
 *                   loop. Commands are therefore read and applied on the main
 *                   loop — the same thread that runs finish_alignment() and
 *                   on_health_timer() — never on the real-time audio thread.
 *                   Reading is non-blocking; partial lines are buffered.
 *
 * Default (no --ui): ui_mode stays false, nothing here runs, behaviour is
 * unchanged. This module only *adds* an optional channel; it does not move
 * any existing log or touch the render path.
 *
 * Convergence note for `trim`: we update extra_delay_ms and shift
 * target_level by the same delta. The existing drift controller then walks
 * the ring to the new target (and the existing hard-sync path jumps at once
 * if the delta exceeds ~50 ms). No ring indices are mutated from this
 * thread, so there is no race with the render callback.
 */
#pragma once

#include "../ui_protocol.h"
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

/* ------------------------------------------------------------------ */
/* Status emission (main loop, once per health tick + once at startup)  */
/* ------------------------------------------------------------------ */
static void ui_emit_status()
{
    UiSummary s;
    s.aligning = app.aligning ? 1 : 0;
    s.devices  = (unsigned)app.devices.size();
    s.ref_ms   = app.ref_latency_ms;
    s.master   = app.master_written;

    std::string summary = ui_format_summary(s);
    fprintf(stdout, "%s\n", summary.c_str());

    /* Reference "total emit" latency uses device 0, matching the human log. */
    double ref_total_ms = 0.0;
    if (!app.devices.empty())
        ref_total_ms = 1000.0 * (double)app.devices[0]->ring.level() / RATE
                     + app.devices[0]->dev_latency_ms;

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
        d.name      = dev->name;
        d.muted     = dev->muted;
        d.conn      = dev->conn;
        d.vol       = dev->vol;

        std::string line = ui_format_device(d);
        fprintf(stdout, "%s\n", line.c_str());
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Command effects                                                     */
/*                                                                     */
/* Both helpers run on the main loop. They never touch ring read/write  */
/* pointers (owned by the RT callback) — only target_level, which the    */
/* drift controller reads and which finish_alignment() already writes    */
/* from this same thread.                                                */
/* ------------------------------------------------------------------ */
static void ui_apply_trim(DeviceOut *dev, double ms)
{
    if (!dev) return;
    if (ms < -ALIGN_MAX_MS) ms = -ALIGN_MAX_MS;
    if (ms >  ALIGN_MAX_MS) ms =  ALIGN_MAX_MS;

    double old = dev->extra_delay_ms;
    if (ms == old) return;

    dev->extra_delay_ms = ms;
    dev->manual_trim    = true;
    /* shift the steady-state target by the trim delta so the drift
       controller walks the ring to the new depth */
    dev->target_level += (ms - old) * (double)RATE / 1000.0;
    if (dev->target_level < 0.0) dev->target_level = 0.0;

    /* remember across reconnects (registry re-applies trim_by_name) */
    if (!dev->target_name.empty())
        app.trim_by_name[dev->target_name] = ms;
}

/* pactl uses the PulseAudio index space, not the PipeWire node id, so we
   address the sink by its node NAME — exactly what unmute_sink() does. */
static void ui_apply_mute(DeviceOut *dev, bool on)
{
    if (!dev || dev->target_name.empty()) return;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "pactl set-sink-mute '%s' %d 2>/dev/null",
             dev->target_name.c_str(), on ? 1 : 0);
    int r = system(cmd); (void)r;
    dev->muted = on;
}

/* pactl uses the PulseAudio index space, not the PipeWire node id, so we
   address the sink by its node NAME — exactly what unmute_sink() does. */

/* Apply a per-device volume in percent (0-100, clamped). pactl drives the
   sink by node NAME; the GUI sends the value rounded to nearest percent. */
static void ui_apply_volume(DeviceOut *dev, double pct)
{
    if (!dev || dev->target_name.empty()) return;
    if (pct <= 0.0)
    {
        ui_apply_mute(dev, true);      /* 0% => effectively muted */
        return;
    }
    if (pct > 100.0) pct = 100.0;
    dev->vol = pct;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "pactl set-sink-volume '%s' %llu%% 2>/dev/null",
             dev->target_name.c_str(),
             (unsigned long long)(pct * 100.0));
    int r = system(cmd); (void)r;
}

/* ------------------------------------------------------------------ */
/* Command dispatch (main loop)                                        */
static void ui_set_connected(DeviceOut *dev, bool connect);
/* Drop / re-attach one device's playback stream on GUI request. Drop
   destroys the stream and stops feeding the ring, but keeps the device
   entry (name, trims, latency) — re-attach resumes from the CURRENT master
   position after a short re-probe, it does NOT replay the missed audio. */
/* ------------------------------------------------------------------ */
/* Command dispatch (main loop)                                        */
/* ------------------------------------------------------------------ */
static void ui_handle_command(const UiCommand &c)
{
    switch (c.op)
    {
    case UI_CMD_PING:
        fprintf(stdout, "%s\n", ui_format_ack("pong").c_str());
        break;

    case UI_CMD_TRIM:
        if (c.idx >= 0 && c.idx < (int)app.devices.size())
        {
            ui_apply_trim(app.devices[c.idx], c.value);
            fprintf(stdout, "%s\n",
                    ui_format_ack("trim ok").c_str());
        }
        else
            fprintf(stdout, "%s\n", ui_format_ack("trim bad-index").c_str());
        break;
    case UI_CMD_VOLUME:
        if (c.idx >= 0 && c.idx < (int)app.devices.size())
        {
            ui_apply_volume(app.devices[c.idx], c.value);
            fprintf(stdout, "%s\n",
                    ui_format_ack("volume ok").c_str());
        }
        else
            fprintf(stdout, "%s\n",
                    ui_format_ack("volume bad-index").c_str());
        break;

    case UI_CMD_MUTE:
        if (c.idx >= 0 && c.idx < (int)app.devices.size())
        {
            ui_apply_mute(app.devices[c.idx], c.value != 0.0);
            fprintf(stdout, "%s\n", ui_format_ack("mute ok").c_str());
        }
        else
            fprintf(stdout, "%s\n", ui_format_ack("mute bad-index").c_str());
        break;

    case UI_CMD_SELECT:
        /* The Linux engine has no selection cursor (that is a Windows-tune
           concept); ack so the GUI's round-trip stays synchronous. */
        fprintf(stdout, "%s\n", ui_format_ack("select ok").c_str());
        break;

    case UI_CMD_CONNECT:
    {
        int i = c.idx;
        if (i < 0 || i >= (int)app.devices.size())
        {
            fprintf(stdout, "%s\n",
                    ui_format_ack("connect: bad index").c_str());
            break;
        }
        ui_set_connected(app.devices[(size_t)i], c.value != 0.0);
        break;
    }

    case UI_CMD_CLICK:
        /* No click-tone generator exists in the Linux engine yet. Say so
           rather than silently doing nothing. */
        fprintf(stdout, "%s\n",
                ui_format_ack("click unsupported").c_str());
        break;

    case UI_CMD_QUIT:
        fprintf(stdout, "%s\n", ui_format_ack("bye").c_str());
        fflush(stdout);
        pw_main_loop_quit(app.loop);
        break;

    case UI_CMD_NONE:
    default:
        break;   /* ignore unknown/partial lines — never kill the stream */
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* stdin watcher (main loop, non-blocking)                             */
/*                                                                     */
/* Accumulates bytes until a full line, then dispatches. A very long     */
/* line with no newline (a misbehaving writer) is dropped, not allowed   */
/* to grow without bound.                                                */
/* ------------------------------------------------------------------ */
static void ui_on_stdin(void *userdata, int fd, uint32_t mask)
{
    (void)userdata;

    if (mask & (SPA_IO_ERR | SPA_IO_HUP))
    {
        /* Writer closed the pipe (GUI went away) — exit cleanly so the
           engine does not sit in a headless loop. */
        pw_main_loop_quit(app.loop);
        return;
    }
    if (!(mask & SPA_IO_IN))
        return;

    static std::string buf;
    char tmp[512];

    for (;;)
    {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n > 0)
        {
            buf.append(tmp, (size_t)n);
        }
        else if (n == 0)
        {
            pw_main_loop_quit(app.loop);   /* EOF */
            return;
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;                     /* drained */
            if (errno == EINTR)
                continue;
            return;                        /* real read error */
        }

        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos)
        {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty()) continue;
            ui_handle_command(ui_parse_command(line));
        }

        if (buf.size() > 8192)
            buf.clear();                   /* no newline after 8 KB: bail */
    }
}

/* Drop / re-attach one device's playback stream on GUI request.
   Drop destroys the stream and stops feeding the ring, but keeps the device
   entry (name, trims, latency) — re-attach resumes from the CURRENT master
   position after a short re-probe, it does NOT replay the missed audio.
   Runs on the main loop, never on the RT path. */
static void ui_set_connected(DeviceOut *dev, bool connect)
{
    if (!dev)
        return;

    if (!connect)
    {
        if (dev->dropped)
        {
            fprintf(stdout, "%s\n", ui_format_ack("drop already").c_str());
            fflush(stdout);
            return;
        }
        dev->dropped = true;
        /* Reuse the registry-removal teardown so drop behaves exactly like
           a vanished bud downstream: stream gone, alignment flags reset,
           trims preserved. `gone` stays false — this device is ours to
           re-attach, the registry must NOT try to recreate it. */
        if (dev->stream)
        {
            pw_stream_destroy(dev->stream);
            dev->stream = nullptr;
        }
        dev->started   = false;
        dev->measuring = true;
        dev->meas_sum = 0.0; dev->meas_count = 0; dev->meas_frames = 0;
        dev->meas_q_sum = 0.0; dev->meas_q_count = 0;
        dev->avg_level = -1.0; dev->ratio = 1.0;
        dev->audible_logged = false;
        auto it = app.trim_by_name.find(dev->target_name);
        if (it != app.trim_by_name.end())
            dev->extra_delay_ms = it->second;
        fprintf(stdout, "%s\n", ui_format_ack("drop ok").c_str());
    }
    else
    {
        if (dev->gone)
        {
            /* Physical device gone (powered off) — only the registry can
               bring it back. Say so rather than silently doing nothing. */
            fprintf(stdout, "%s\n",
                    ui_format_ack("attach failed: device gone").c_str());
            fflush(stdout);
            return;
        }
        if (!dev->dropped)
        {
            fprintf(stdout, "%s\n", ui_format_ack("attach already").c_str());
            fflush(stdout);
            return;
        }
        dev->dropped = false;
        /* Fresh probe against the CURRENT master so the re-attached bud
           aligns to where the music is now, not where it was dropped. */
        dev->started   = false;
        dev->measuring = true;
        dev->meas_sum = 0.0; dev->meas_count = 0; dev->meas_frames = 0;
        dev->meas_q_sum = 0.0; dev->meas_q_count = 0;
        dev->avg_level = -1.0; dev->ratio = 1.0;
        dev->audible_logged = false;
        if (start_playback(dev))
        {
            fprintf(stdout, "%s\n", ui_format_ack("attach ok").c_str());
            unmute_sink(dev);
        }
        else
        {
            dev->dropped = true;   /* restore: attach did not happen */
            fprintf(stdout, "%s\n",
                    ui_format_ack("attach failed: stream").c_str());
        }
        dev->conn = connect;
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Setup (called once from main when --ui is present)                  */
/* ------------------------------------------------------------------ */
static void ui_setup()
{
    ui_mode = true;

    /* Make stdin non-blocking so the watcher never parks the main loop. */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0)
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    struct spa_source *src = pw_loop_add_io(
        pw_main_loop_get_loop(app.loop),
        STDIN_FILENO, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP,
        false, ui_on_stdin, nullptr);

    if (!src)
    {
        fprintf(stderr,
                "[ui] warning: cannot watch stdin, status will still be "
                "emitted but commands will be ignored\n");
    }

    /* Emit an immediate snapshot so the GUI shows devices before the first
       1 s tick (and so a no-devices run still proves the pipe works). */
    ui_emit_status();
}
