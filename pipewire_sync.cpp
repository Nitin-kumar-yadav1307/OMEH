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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <map>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#define RATE          48000
#define CHANNELS      2
#define FRAME_BYTES   (CHANNELS * sizeof(int16_t))

/* Ring capacity in frames (power of two) */
#define RING_BITS     15
#define RING_SIZE     (1u << RING_BITS)
#define RING_MASK     (RING_SIZE - 1)

/* Steady-state ring depth per device (ms) is derived from the measured
   device latency during the alignment phase; see ALIGN_BASE_MS below. */

/*
 * Drift controller tuning.
 *
 * We regulate the ring BUFFER DEPTH (level), not an absolute sample
 * position. Because the capture writes one frame into every ring for
 * every captured frame, level(i) = master_written - consumed(i), so
 * holding every device's level at its target keeps them all at the
 * same position relative to the master — which is exactly "in sync".
 *
 *   ratio = 1 + DRIFT_KP * smoothed(level - target)
 *
 * Two things matter here:
 *
 *  - The ring depth jitters by one graph quantum (256 frames = 5.3 ms)
 *    because the stream consumes in whole quanta, and the Bluetooth
 *    transport delivers in bursts, so the raw depth swings by ~20 ms.
 *    With both earbuds in the same graph that swing is common mode
 *    (sync is preserved) but it must not be fed into the resampler or
 *    it becomes audible pitch wobble. Averaging it over DRIFT_TAU
 *    seconds leaves only the genuine, slow clock drift.
 *  - The loop time constant is 1 / (DRIFT_KP * RATE) seconds, which
 *    must stay far above the transport delay (~buffer + codec) or the
 *    loop oscillates. 40 s is comfortably stable, and real clock drift
 *    (tens of ppm) is still corrected long before it is audible.
 */
#define DRIFT_TAU       10.0    /* s, level-error averaging              */
#define DRIFT_LOOP_TAU  40.0    /* s, closed-loop time constant          */
#define DRIFT_KP        (1.0 / ((double)RATE * DRIFT_LOOP_TAU))
#define DRIFT_MAX_PPM   500.0   /* clamp on resampler ratio              */

/* If a device is further out than this (~50 ms), jump instead of warp */
#define HARD_SYNC_FRAMES 2400.0

/*
 * Latency alignment tuning.
 *
 *   ALIGN_BASE_MS : minimum steady state buffer depth per device.
 *   ALIGN_MAX_MS  : hard cap on the alignment delay we will add.
 *   MEASURE_TICKS : how many 1-second ticks we probe the devices
 *                   before freezing the alignment offsets.
 */
#define ALIGN_BASE_MS   40.0
#define ALIGN_MAX_MS    350.0

/*
 * Self-calibrating inter-device trim.
 *
 * What the ear hears is the TOTAL latency of a device:
 *
 *     total = ring depth + device delay (codec + transport)
 *
 * The device delay a Bluetooth earbud reports is quantised (here one
 * graph quantum, 5.3 ms), so the offsets derived from a single probe
 * can be a quantum off - which is exactly the small residual echo.
 * So the timer keeps nudging each device's target depth until the
 * TOTALS match. Driving the totals equal is what truly aligns the
 * arrival time at the ears; the ring depths then intentionally differ
 * by the device-delay difference.
 */
#define ALIGN_TRIM_GAIN 0.15    /* per second, fraction of the mismatch */
#define ALIGN_SMOOTH    0.3     /* smoothing of the per-tick measurement */
#define MEASURE_TICKS   2

/* our own stream marker, so we never re-route ourselves */
#define OUR_APP_NAME    "pipewire_sync"

/* ------------------------------------------------------------------ */
/* Stereo interleaved ring buffer (single producer, single consumer,   */
/* both on the same main loop -> plain indices are fine)               */
/* ------------------------------------------------------------------ */
struct Ring
{
    int16_t data[RING_SIZE * CHANNELS] = {};
    size_t  read  = 0;   /* frame index */
    size_t  write = 0;   /* frame index */

    size_t level() const { return write - read; }

    /* drop all but the newest `frames` (used to start at low latency) */
    void flush_to(size_t frames)
    {
        if (frames > level()) frames = level();
        read = write - frames;
    }

    void write_frames(const int16_t *src, size_t n)
    {
        /* keep the NEWEST audio if the chunk itself is too large */
        if (n > RING_SIZE)
        {
            src += (n - RING_SIZE) * CHANNELS;
            n = RING_SIZE;
        }

        /* keep the NEWEST audio when we would overflow */
        if (level() + n > RING_SIZE)
            read = write + n - RING_SIZE;

        for (size_t i = 0; i < n; i++)
        {
            size_t w = (write + i) & RING_MASK;
            memcpy(&data[w * CHANNELS], &src[i * CHANNELS], FRAME_BYTES);
        }
        write += n;
    }

    /* read one frame with linear interpolation at fractional position.
       Indices are clamped to the valid [read, write-1] window so a
       fractional phase can never read unwritten/stale samples. */
    void read_frame(double pos, int16_t out[CHANNELS])
    {
        double lo = (double)read;
        double hi = write ? (double)write - 1.0 : 0.0;
        if (pos < lo) pos = lo;
        if (pos > hi) pos = hi;

        size_t i0 = (size_t)pos;
        double f  = pos - (double)i0;
        size_t i1 = i0 + 1;

        for (int c = 0; c < CHANNELS; c++)
        {
            double a = data[(i0 & RING_MASK) * CHANNELS + c];
            double b = data[(i1 & RING_MASK) * CHANNELS + c];
            out[c] = (int16_t)lrint(a + (b - a) * f);
        }
    }

    void advance(size_t n) { read += n; }
};

/* ------------------------------------------------------------------ */
/* One output earbud                                                   */
/* ------------------------------------------------------------------ */
struct DeviceOut
{
    std::string name;
    std::string target_name;   /* node.name — WirePlumber pins by name */
    std::string node_id_str;
    uint32_t    node_id = 0;

    struct pw_stream *stream = nullptr;

    /* unique node.name of our own stream ("sync-out-N") — used to
       find it again in the pulse graph and put it back on the
       right device if something moves it */
    std::string node_name;

    Ring  ring;
    double phase      = 0.0;   /* fractional read position in ring  */
    bool   started    = false; /* alignment buffer filled?          */

    /* drift controller state */
    double ratio      = 1.0;
    uint64_t written  = 0;     /* frames handed to the device       */
    double extra_delay_ms = 0.0;  /* manual trim from argv          */
    bool   manual_trim    = false; /* argv trim disables auto-align  */

    /* latency measurement / alignment */
    bool     measuring      = true;  /* still probing own latency   */
    double   meas_sum       = 0.0;   /* sum of latency probes (ms)  */
    uint64_t meas_count     = 0;     /* number of probes            */
    uint64_t meas_frames    = 0;     /* frames since last probe     */
    double   meas_q_sum     = 0.0;   /* sum of graph quanta (frames) */
    uint64_t meas_q_count   = 0;
    double   dev_latency_ms = 0.0;   /* measured device latency     */
    double   target_level   = 0.0;   /* steady buffer depth (frames)*/
    double   avg_quantum    = 0.0;   /* graph quantum (frames)      */
    double   avg_level      = -1.0;  /* smoothed ring depth         */
    double   dev_frames_s   = -1.0;  /* smoothed device delay (frames)*/
    double   total_frames   = -1.0;  /* smoothed total latency (frames)*/

    /* diagnostics */
    uint64_t underruns    = 0;
    int      route_warned = 0;

    uint64_t quantum       = 0;   /* frames requested in last callback */
    int      last_primes   = 0;   /* stream primes, for reconnect log  */

    /* last reported values */
    double last_latency_ms   = 0.0;
    double last_drift_frames = 0.0;

    /* logged once when this device becomes audible */
    bool   audible_logged = false;

    /* registry removal flag */
    bool   gone = false;
};

/* ------------------------------------------------------------------ */
/* Global app state                                                    */
/* ------------------------------------------------------------------ */
struct App
{
    struct pw_main_loop *loop = nullptr;
    struct pw_core      *core = nullptr;
    struct pw_registry  *registry = nullptr;

    struct pw_stream *capture = nullptr;

    uint64_t master_written = 0;   /* frames captured from monitor */

    std::vector<DeviceOut *> devices;

    /* virtual "Sync Master" sink that apps play into silently */
    uint32_t virtual_sink_id = 0;
    std::string virtual_sink_name;      /* actual node name after WP rename */
    std::string prev_default_sink;      /* restored on exit */
    int null_module_index = 0;          /* pulse module to unload on exit */

    /* latency alignment phase */
    bool     aligning        = true;    /* still probing device latencies */
    uint64_t align_ticks     = 0;       /* 1-second ticks spent probing    */
    double   ref_latency_ms  = 0.0;     /* reference (largest) latency     */

    /* user-supplied per-device trims that survive reconnects */
    std::map<std::string, double> trim_by_name;

    /* diagnostics */
    uint64_t capture_callbacks = 0;
    uint64_t capture_empties   = 0;
    int      capture_route_warned = 0;
    uint64_t prev_master       = 0;   /* master at previous tick */
    uint64_t prev_tick_ns      = 0;   /* CLOCK_MONOTONIC of previous tick */
};

/* defined below start_playback(), which needs it for late-joining buds */
struct DeviceOut;
static double default_target_level(const DeviceOut *dev);

static App app;

static void reroute_clients();   /* forward decl (defined below) */

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
        if (!dev->measuring)
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
static bool create_virtual_sink()
{
    FILE *p = popen(
        "pactl load-module module-null-sink "
        "sink_name=sync_master "
        "sink_properties=device.description='Sync\\ Master' "
        "2>/dev/null",
        "r");
    if (!p)
        return false;

    char line[64] = {};
    bool ok = fgets(line, sizeof(line), p) != nullptr;
    pclose(p);

    if (!ok || atoi(line) <= 0)
        return false;

    app.null_module_index = atoi(line);
    fprintf(stdout, "[main] virtual sink loaded (module %d)\n",
            app.null_module_index);

    /* wait for the registry to report the new node */
    int rounds = 0;
    while (app.virtual_sink_id == 0 && rounds < 100)
    {
        pw_loop_iterate(pw_main_loop_get_loop(app.loop), 100);
        rounds++;
    }

    return app.virtual_sink_id != 0;
}

/* ------------------------------------------------------------------ */
/* Default sink switching (via pipewire-pulse client 'pactl')          */
/* ------------------------------------------------------------------ */
static bool remember_default_sink()
{
    FILE *p = popen("pactl get-default-sink 2>/dev/null", "r");
    if (!p) return false;
    char line[256] = {};
    bool ok = fgets(line, sizeof(line), p) != nullptr;
    pclose(p);
    /* strip newline */
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    if (ok && line[0])
        app.prev_default_sink = line;
    return ok;
}

static void set_default_sink(const char *name)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "pactl set-default-sink '%s' 2>/dev/null", name);
    int r = system(cmd);
    (void)r;
    fprintf(stdout, "[main] default sink -> %s\n", name);
}

/* ------------------------------------------------------------------ */
/* Small helpers around the pulse layer (pactl)                        */
/* ------------------------------------------------------------------ */
static std::string quoted_value(const char *line)
{
    const char *a = strchr(line, '"');
    if (a == nullptr) return std::string();
    const char *b = strchr(a + 1, '"');
    if (b == nullptr) return std::string();
    return std::string(a + 1, (size_t)(b - a - 1));
}

struct SinkInput
{
    int         id         = 0;
    int         sink_index = -1;
    std::string node_name;
    std::string media_name;
    std::string app_name;
};

static std::vector<SinkInput> list_sink_inputs()
{
    std::vector<SinkInput> v;
    FILE *p = popen("pactl list sink-inputs 2>/dev/null", "r");
    if (p == nullptr) return v;

    char line[1024];
    SinkInput cur;
    bool have = false;

    while (fgets(line, sizeof(line), p))
    {
        if (strncmp(line, "Sink Input #", 12) == 0)
        {
            if (have) v.push_back(cur);
            cur    = SinkInput();
            cur.id = atoi(line + 12);
            have   = true;
        }
        else if (!have) continue;
        else if (line[0] == '\t' && strncmp(line + 1, "Sink: ", 6) == 0)
            cur.sink_index = atoi(line + 7);
        else if (strstr(line, "node.name = \""))
            cur.node_name = quoted_value(line);
        else if (strstr(line, "media.name = \""))
            cur.media_name = quoted_value(line);
        else if (strstr(line, "application.name = \""))
            cur.app_name = quoted_value(line);
    }
    if (have) v.push_back(cur);
    pclose(p);
    return v;
}

struct SourceOutput
{
    int         id           = 0;
    int         source_index = -1;
    std::string node_name;
};

static std::vector<SourceOutput> list_source_outputs()
{
    std::vector<SourceOutput> v;
    FILE *p = popen("pactl list source-outputs 2>/dev/null", "r");
    if (p == nullptr) return v;

    char line[1024];
    SourceOutput cur;
    bool have = false;

    while (fgets(line, sizeof(line), p))
    {
        if (strncmp(line, "Source Output #", 15) == 0)
        {
            if (have) v.push_back(cur);
            cur    = SourceOutput();
            cur.id = atoi(line + 15);
            have   = true;
        }
        else if (!have) continue;
        else if (line[0] == '\t' && strncmp(line + 1, "Source: ", 8) == 0)
            cur.source_index = atoi(line + 9);
        else if (strstr(line, "node.name = \""))
            cur.node_name = quoted_value(line);
    }
    if (have) v.push_back(cur);
    pclose(p);
    return v;
}

/* index -> name map for "sinks" / "sources" (from pactl list short) */
static std::map<int, std::string> list_objects(const char *what)
{
    std::map<int, std::string> m;

    std::string cmd = std::string("pactl list short ") + what + " 2>/dev/null";
    FILE *p = popen(cmd.c_str(), "r");
    if (p == nullptr) return m;

    char line[1024];
    while (fgets(line, sizeof(line), p))
    {
        int  idx   = 0;
        char nm[512] = {};
        if (sscanf(line, "%d\t%511s", &idx, nm) == 2)
            m[idx] = nm;
    }
    pclose(p);
    return m;
}

static std::string get_default_sink_name()
{
    FILE *p = popen("pactl get-default-sink 2>/dev/null", "r");
    if (p == nullptr) return std::string();

    char line[256] = {};
    bool ok = fgets(line, sizeof(line), p) != nullptr;
    pclose(p);
    if (!ok) return std::string();

    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    return std::string(line);
}

/*
 * Make sure an earbud is not muted. NOTE: pactl uses the PulseAudio
 * index space, which is NOT the PipeWire node id — so we pass the
 * sink's node NAME instead.
 */
static void unmute_sink(const DeviceOut *dev)
{
    if (dev->target_name.empty())
        return;

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "pactl set-sink-mute '%s' 0 2>/dev/null",
             dev->target_name.c_str());
    int r = system(cmd); (void)r;
}

/*
 * WirePlumber/Plasma may revert the default sink, and app streams that
 * existed before us do not follow the default change. So every second:
 *
 *   - re-assert sync_master as default sink,
 *   - move every app's sink-input into sync_master,
 *   - move our capture back onto sync_master's monitor if something
 *     relocated it (that relocation is the classic mic/echo bug),
 *
 * Our own streams (node.name "sync-out-*" / "sync-capture") are NEVER
 * moved: putting a playback stream on sync_master would silence the
 * earbuds and loop our own audio back into the capture (= echo).
 */
static bool is_our_stream(const std::string &node_name,
                          const std::string &media_name)
{
    if (node_name.rfind("sync-out-", 0) == 0)  return true;
    if (node_name.rfind("sync-capture", 0) == 0) return true;
    if (node_name.rfind("input.sync_master", 0) == 0) return true;
    (void)media_name;
    return false;
}

static void reroute_clients()
{
    /* 1. keep the default sink */
    std::string cur_default = get_default_sink_name();
    if (!app.virtual_sink_name.empty() && cur_default != app.virtual_sink_name)
        set_default_sink(app.virtual_sink_name.c_str());

    if (app.virtual_sink_name.empty())
        return;

    /* 2. sink-inputs -> sync_master (skip our own output streams) */
    std::map<int, std::string> sinks = list_objects("sinks");
    for (const SinkInput &si : list_sink_inputs())
    {
        if (is_our_stream(si.node_name, si.media_name))
        {
            /*
             * Safety net: our playback stream must NEVER sit on the
             * virtual sink. If it does, put it back on its earbud.
             */
            for (DeviceOut *dev : app.devices)
            {
                if (dev->stream == nullptr) continue;
                if (!si.node_name.empty() &&
                    dev->node_name == si.node_name)
                {
                    auto it = sinks.find(si.sink_index);
                    if (it != sinks.end() && it->second == app.virtual_sink_name)
                    {
                        char cmd[512];
                        snprintf(cmd, sizeof(cmd),
                                 "pactl move-sink-input %d '%s' 2>/dev/null",
                                 si.id, dev->target_name.c_str());
                        int r = system(cmd); (void)r;
                    }
                    break;
                }
            }
            continue;
        }

        auto it = sinks.find(si.sink_index);
        if (it == sinks.end() || it->second != app.virtual_sink_name)
        {
            char cmd[512];
            snprintf(cmd, sizeof(cmd),
                     "pactl move-sink-input %d '%s' 2>/dev/null",
                     si.id, app.virtual_sink_name.c_str());
            int r = system(cmd); (void)r;
        }
    }

    /* 3. our capture must stay on the virtual sink's monitor */
    std::map<int, std::string> sources = list_objects("sources");
    for (const SourceOutput &so : list_source_outputs())
    {
        if (so.node_name != "sync-capture")
            continue;

        /* find sync_master's monitor source index */
        static const char *want = "sync_master.monitor";
        auto sit = sources.find(so.source_index);
        if (sit == sources.end())
            continue;

        if (sit->second.find("sync_master") == std::string::npos)
        {
            for (const auto &kv : sources)
            {
                if (kv.second.find("sync_master") != std::string::npos)
                {
                    char cmd[512];
                    snprintf(cmd, sizeof(cmd),
                             "pactl move-source-output %d '%s' 2>/dev/null",
                             so.id, kv.second.c_str());
                    int r = system(cmd); (void)r;

                    if (!app.capture_route_warned)
                    {
                        app.capture_route_warned = 1;
                        fprintf(stdout,
                                "[timer] capture had been relocated away from "
                                "sync_master - moved it back\n");
                    }
                    break;
                }
            }
        }
        (void)want;
    }
}

/* default sink by node id — pulse names get renamed by WirePlumber */
static void set_default_node(uint32_t node_id)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "wpctl set-default %u 2>/dev/null", node_id);
    int r = system(cmd);
    (void)r;
    fprintf(stdout, "[main] default sink -> sync_master (node %u)\n",
            node_id);
}

/* refuse to run twice — overlapping instances fight over routing */
static bool another_instance_running()
{
    FILE *p = popen("pgrep -x -c pipewire_sync 2>/dev/null", "r");
    if (!p) return false;
    char line[64] = {};
    bool ok = fgets(line, sizeof(line), p) != nullptr;
    pclose(p);
    /* 1 match = ourselves; more than 1 = someone else */
    return ok && atoi(line) > 1;
}

/* ------------------------------------------------------------------ */
/* Registry: discover every Bluetooth sink node                        */
/* ------------------------------------------------------------------ */
static void registry_event_global(void *data, uint32_t id,
                                  uint32_t permissions,
                                  const char *type, uint32_t version,
                                  const struct spa_dict *props)
{
    (void)data; (void)permissions; (void)version;

    if (!props || strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    /* Did our virtual "Sync Master" sink appear? */
    const char *cls_check = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *nm_check  = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (nm_check && strstr(nm_check, "sync_master") &&
        cls_check && strstr(cls_check, "Audio/Sink"))
    {
        app.virtual_sink_id = id;
        /* WirePlumber may rename it (e.g. "input.sync_master") —
           remember the ACTUAL node name for reliable pinning */
        app.virtual_sink_name = nm_check;
        fprintf(stdout, "[registry] virtual sink ready: %s (node %u)\n",
                nm_check, id);
        fflush(stdout);
        return;   /* never treat it as a Bluetooth device */
    }

    const char *media_class =
        spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media_class || strcmp(media_class, "Audio/Sink") != 0)
        return;

    /* Bluetooth only */
    const char *dev_api   = spa_dict_lookup(props, PW_KEY_DEVICE_API);
    const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);

    bool is_bt =
        (dev_api && strstr(dev_api, "bluez5")) ||
        (node_name && strncmp(node_name, "bluez_output", 12) == 0);

    if (!is_bt)
        return;

    /* Skip our own helper nodes just in case */
    if (node_name && strstr(node_name, "sync-"))
        return;

    DeviceOut *dev = new DeviceOut();
    dev->node_id = id;
    dev->node_id_str = std::to_string(id);

    const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    dev->name = (desc && *desc) ? desc :
                (node_name ? node_name : dev->node_id_str);
    dev->target_name = node_name ? node_name : dev->node_id_str;

    app.devices.push_back(dev);

    fprintf(stdout, "[registry] found Bluetooth sink: %s (node %u)\n",
            dev->name.c_str(), id);
    fflush(stdout);
}

static void registry_event_remove(void *data, uint32_t id)
{
    (void)data;

    /* an earbud disappeared: mark the device gone, destroy its stream.
       When it comes back the global handler re-adds it (probing again)
       and any user-supplied trim survives, keyed by node name. */
    for (DeviceOut *dev : app.devices)
    {
        if (dev->node_id == id && !dev->gone)
        {
            fprintf(stdout, "[registry] device gone: %s (node %u)\n",
                    dev->name.c_str(), id);
            fflush(stdout);
            dev->gone = true;
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
            auto it = app.trim_by_name.find(dev->target_name);
            if (it != app.trim_by_name.end())
                dev->extra_delay_ms = it->second;
            break;
        }
    }

    if (id == app.virtual_sink_id)
    {
        app.virtual_sink_id = 0;
        app.virtual_sink_name.clear();
    }
}

static const struct pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global  = registry_event_global,
    .global_remove = registry_event_remove,
};

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

    /* (re)start playback streams */
    for (DeviceOut *dev : app.devices)
    {
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
            if (dev->stream && dev->meas_count == 0)
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
}

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






