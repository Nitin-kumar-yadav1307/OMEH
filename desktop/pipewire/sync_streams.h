/*
 * sync_streams.h — the real-time CAPTURE path + PipeWire stream plumbing.
 *
 * Single-TU module, part of pipewire_sync.cpp. Included AFTER sync_config.h
 * and sync_types.h. Owns on_capture_process() (copies each captured frame
 * into every device's ring), build_format(), and start_capture() /
 * start_playback() which create and connect the PipeWire streams.
 */
#pragma once


static void on_capture_process(void *userdata)
{
    (void)userdata;

    struct pw_buffer *b = pw_stream_dequeue_buffer(app.capture);
    if (b == nullptr)
        return;

    struct spa_data *d = &b->buffer->datas[0];
    if (d->data == nullptr)
    {
        pw_stream_queue_buffer(app.capture, b);
        return;
    }

    uint32_t frames = d->chunk->size / FRAME_BYTES;
    if (frames == 0)
    {
        app.capture_empties++;
        pw_stream_queue_buffer(app.capture, b);
        return;
    }

    const int16_t *samples = reinterpret_cast<const int16_t *>(
        (char *)d->data + d->chunk->offset);

    /*
     * Devices still probing their own latency consume nothing, so
     * nothing is written into their ring: letting it fill up during the
     * measurement would leave a huge backlog to drain afterwards (and a
     * multi-hundred-ms latency spike when playback starts).
     */
    for (DeviceOut *dev : app.devices)
        if (!dev->measuring && !dev->dropped)
            dev->ring.write_frames(samples, frames);

    app.master_written += frames;
    app.capture_callbacks++;

    /* Status reporting happens in the 1-second health timer, so it
       also shows up while nothing is playing. */

    pw_stream_queue_buffer(app.capture, b);
}

/* ------------------------------------------------------------------ */
/* Shared: build S16/2ch/48k format param                              */
/* ------------------------------------------------------------------ */
static const struct spa_pod *build_format(struct spa_pod_builder *b)
{
    struct spa_audio_info_raw info = {};
    info.format   = SPA_AUDIO_FORMAT_S16;
    info.channels = CHANNELS;
    info.rate     = RATE;
    return spa_format_audio_raw_build(b, SPA_PARAM_EnumFormat, &info);
}

/* ------------------------------------------------------------------ */
/* Capture stream (default sink monitor)                               */
/* ------------------------------------------------------------------ */
/* events must outlive the stream: PipeWire keeps a pointer to them */
static const struct pw_stream_events capture_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = on_capture_process,
};

static bool start_capture(uint32_t target_node)
{
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE,     "Music",
        PW_KEY_STREAM_CAPTURE_SINK, "true",   /* record a sink, not mic */
        PW_KEY_STREAM_MONITOR,      "true",   /* force monitor port —
                                                 never fall back to mic  */
        /* CRITICAL: never let WirePlumber relocate this capture.
           Relocation fallback = the earbuds' mic = echo + "mic in
           use" indicator. If the target is gone we recreate here. */
        "node.dont-relocate",  "true",
        nullptr);

    app.capture = pw_stream_new_simple(
        pw_main_loop_get_loop(app.loop),
        "sync-capture",
        props,
        &capture_events,
        nullptr);

    if (!app.capture)
        return false;

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    const struct spa_pod *params[1];
    params[0] = build_format(&b);

    /*
     * Pin by the virtual sink's NODE ID (exact). Name-based targets
     * broke because WirePlumber renames the pulse null-sink node
     * (e.g. node.name shows up as "input.sync_master"), so name
     * resolution silently missed and the graph linked us to a
     * fallback source (one bud's monitor = double audio / echo).
     * The numeric target is honoured when combined with AUTOCONNECT
     * for streams created natively like this one.
     */
    int r = pw_stream_connect(
        app.capture,
        PW_DIRECTION_INPUT,
        target_node ? target_node : PW_ID_ANY,
        (pw_stream_flags)(
            PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS |
            PW_STREAM_FLAG_AUTOCONNECT),
        params, 1);

    return r == 0;
}

/* ------------------------------------------------------------------ */
/* Playback stream per discovered Bluetooth sink                       */
/* ------------------------------------------------------------------ */
/* events must outlive the stream: PipeWire keeps a pointer to them */
static const struct pw_stream_events play_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = on_play_process,
};

static bool start_playback(DeviceOut *dev)
{
    /*
     * A UNIQUE name for our own stream. This is what lets us tell
     * our own streams apart in the PulseAudio graph, so we never
     * accidentally re-route ourselves into the virtual sink —
     * doing that would silence the earbuds AND feed our own output
     * back into our capture (echo / feedback loop).
     */
    static unsigned out_seq = 0;
    dev->node_name = "sync-out-" + std::to_string(++out_seq);

    /*
     * IMPORTANT: pin this stream to the exact earbud by node NAME.
     * WirePlumber ignores numeric-id targets; a name works reliably.
     */
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE,     "Music",
        PW_KEY_NODE_LATENCY,   "480/48000",   /* 10 ms quantum */
        PW_KEY_NODE_NAME,      dev->node_name.c_str(),
        PW_KEY_TARGET_OBJECT,  dev->target_name.c_str(),
        "node.target",         dev->target_name.c_str(),
        /* never relocate to another device (prevents double audio
           when a bud reconnects and WP auto-switches defaults) */
        "node.dont-relocate",  "true",
        /* tag our streams so reroute_clients() never moves them */
        PW_KEY_APP_NAME,       OUR_APP_NAME,
        nullptr);

    dev->stream = pw_stream_new_simple(
        pw_main_loop_get_loop(app.loop),
        dev->name.c_str(),
        props,
        &play_events,
        dev);

    if (!dev->stream)
        return false;

    /*
     * Buds that connect AFTER the measurement phase never probe —
     * give them the default buffer immediately so they start at once.
     */
    if (!app.aligning)
    {
        dev->measuring    = false;
        dev->started      = false;
        dev->target_level = default_target_level(dev);
    }

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    const struct spa_pod *params[1];
    params[0] = build_format(&b);

    int r = pw_stream_connect(
        dev->stream,
        PW_DIRECTION_OUTPUT,
        dev->node_id,                   /* the earbud's own node */
        (pw_stream_flags)(
            PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS |
            PW_STREAM_FLAG_AUTOCONNECT),
        params, 1);

    return r == 0;
}

/* ------------------------------------------------------------------ */
/* Virtual silent "Sync Master" sink: apps play into it, we capture    */
/* its monitor. Created at the PULSE layer so it is guaranteed to be   */
/* visible to pactl/wpctl (native adapter null-sinks are not).         */
/* ------------------------------------------------------------------ */
