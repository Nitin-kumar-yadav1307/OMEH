/*
 * sync_playback.h — the real-time PLAYBACK (render) path.
 *
 * Single-TU module, part of pipewire_sync.cpp. Included AFTER sync_config.h
 * and sync_types.h. Owns on_play_process(), the PW_STREAM_FLAG_RT_PROCESS
 * callback that runs on the audio thread: underrun handling, the drift
 * resampler, latency-alignment silence, and the AIMD ring-depth controller.
 * Also parse_delay_args(), the argv "index:ms" trims.
 */
#pragma once


/* per-device extra delay from argv: "index:ms" */
static std::map<int, double> parse_delay_args(int argc, char *argv[])
{
    std::map<int, double> m;
    for (int i = 1; i < argc; i++)
    {
        const char *colon = strchr(argv[i], ':');
        if (!colon) continue;
        m[atoi(argv[i])] = atof(colon + 1);
    }
    return m;
}

/* ------------------------------------------------------------------ */
/* Playback process callback (real-time)                               */
/* ------------------------------------------------------------------ */
static void on_play_process(void *userdata)
{
    DeviceOut *dev = static_cast<DeviceOut *>(userdata);

    struct pw_buffer *b = pw_stream_dequeue_buffer(dev->stream);
    if (b == nullptr)
        return;

    struct spa_data *d = &b->buffer->datas[0];
    if (d->data == nullptr)
    {
        pw_stream_queue_buffer(dev->stream, b);
        return;
    }

    uint32_t max_frames = d->maxsize / FRAME_BYTES;

    /* Prefer the number of frames the graph asked for. */
    uint32_t frames = b->requested ? b->requested : max_frames;
    if (frames > max_frames)
        frames = max_frames;

    int16_t *out = static_cast<int16_t *>(d->data);

    d->chunk->offset = 0;
    d->chunk->stride = FRAME_BYTES;
    d->chunk->size   = frames * FRAME_BYTES;

    /*
     * Phase 1 — latency probing.
     *
     * We feed the device silence and watch how much data its own
     * chain (Bluetooth codec + buffers) reports as still queued.
     * That delay is what we compensate later, so that all earbuds
     * emit the same sample at the same moment.
     */
    if (dev->measuring)
    {
        memset(out, 0, frames * FRAME_BYTES);

        if (frames)
        {
            dev->meas_q_sum += frames;
            dev->meas_q_count++;
        }

        dev->meas_frames += frames;
        if (dev->meas_frames >= RATE / 4)          /* 4 probes / second */
        {
            dev->meas_frames -= RATE / 4;

            struct pw_time t;
            pw_stream_get_time_n(dev->stream, &t, sizeof(t));

            /* t.rate is ticks per second -> ms = delay / ticks_per_sec */
            double ticks_per_sec = t.rate.num
                ? (double)t.rate.denom / (double)t.rate.num : 0.0;

            if (ticks_per_sec > 0.0 && t.delay > 0)
            {
                dev->meas_sum += 1000.0 * (double)t.delay / ticks_per_sec;
                dev->meas_count++;
                dev->dev_latency_ms = dev->meas_sum / (double)dev->meas_count;
            }
        }

        pw_stream_queue_buffer(dev->stream, b);
        return;
    }

    /* Wait until the alignment buffer is full so all earbuds start
       at exactly the same capture position. */
    if (!dev->started)
    {
        if ((double)dev->ring.level() < dev->target_level)
        {
            memset(out, 0, frames * FRAME_BYTES);
            pw_stream_queue_buffer(dev->stream, b);
            return;
        }
        /*
         * Trim the backlog down to exactly the target depth. The ring
         * may hold a lot of audio (it filled while we measured), and
         * starting from a full ring would add hundreds of ms of
         * latency and make the drift error meaningless.
         */
        dev->ring.flush_to((size_t)dev->target_level);

        dev->started     = true;
        dev->phase       = (double)dev->ring.read;
        dev->written     = 0;
        dev->ratio       = 1.0;

        /* If the capture is already running, audio goes out NOW.
           Log it, in the timer context the device shows up as RUNNING. */
        dev->route_warned = 0;
    }

    if (dev->ring.level() == 0)
    {
        /* underrun — keep the graph alive with silence */
        memset(out, 0, frames * FRAME_BYTES);
        dev->underruns++;
        pw_stream_queue_buffer(dev->stream, b);
        return;
    }

    /* First REAL (non-silence) buffer handed to this device. */
    if (!dev->audible_logged)
    {
        dev->audible_logged = true;
        fprintf(stderr, "[audio] %-28s first audio handed to device "
                "(ring=%zu target=%.0f master=%llu)\n",
                dev->name.c_str(), dev->ring.level(), dev->target_level,
                (unsigned long long)app.master_written);
        fflush(stderr);
    }

    /*
     * Drift correction — regulate the smoothed ring depth.
     *
     * level = master_written - frames_this_device_consumed, so if the
     * level sits at target on every device, every device has consumed
     * exactly the same number of frames -> all earbuds emit the same
     * sample at the same moment.
     *
     * The raw depth jumps by whole graph quanta, so we work on its
     * exponentially smoothed value: a filtered error keeps the loop
     * from turning that jitter into an audible pumping/limit cycle.
     */
    double raw_level = (double)dev->ring.level();

    if (dev->avg_level < 0.0)
        dev->avg_level = raw_level;
    else
        dev->avg_level += (raw_level - dev->avg_level) *
                          (1.0 - exp(-1.0 / (DRIFT_TAU * (double)RATE)));

    double lerr = dev->avg_level - dev->target_level;
    dev->last_drift_frames = lerr;

    /* way out of sync (e.g. after a reconnect) -> jump, don't warp */
    if (fabs(raw_level - dev->target_level) > HARD_SYNC_FRAMES)
    {
        dev->ring.flush_to((size_t)dev->target_level);
        dev->phase     = (double)dev->ring.read;
        dev->avg_level = (double)dev->ring.level();
        dev->ratio     = 1.0;
        lerr           = 0.0;
    }

    dev->ratio = 1.0 + DRIFT_KP * lerr;
    double max_ratio = 1.0 + DRIFT_MAX_PPM * 1e-6;
    double min_ratio = 1.0 - DRIFT_MAX_PPM * 1e-6;
    if (dev->ratio > max_ratio) dev->ratio = max_ratio;
    if (dev->ratio < min_ratio) dev->ratio = min_ratio;

    for (uint32_t i = 0; i < frames; i++)
    {
        int16_t s[CHANNELS];
        dev->ring.read_frame(dev->phase, s);

        out[i * CHANNELS + 0] = s[0];
        out[i * CHANNELS + 1] = s[1];

        dev->phase += dev->ratio;
        dev->written++;
    }
    dev->quantum = frames;

    /*
     * Advance the ring past what we actually consumed. The phase can
     * be at most (write + ratio) here, so this is clamped to the
     * available level: never consume what was never written, and
     * never read the same samples twice.
     */
    double lo = (double)dev->ring.read;
    double hi = (dev->ring.write ? (double)dev->ring.write - 1.0 : 0.0)
                + dev->ratio;
    if (dev->phase < lo) dev->phase = lo;
    if (dev->phase > hi) dev->phase = hi;

    dev->ring.advance((size_t)(dev->phase - (double)dev->ring.read));
    dev->phase = (double)dev->ring.read;

    pw_stream_queue_buffer(dev->stream, b);
}

/* ------------------------------------------------------------------ */
/* Capture process callback: copy monitor audio to every device ring   */
/* ------------------------------------------------------------------ */
