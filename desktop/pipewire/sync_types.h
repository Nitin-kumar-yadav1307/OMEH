/*
 * sync_types.h — core data structures and global state.
 *
 * Single-TU module, part of pipewire_sync.cpp. Included AFTER sync_config.h.
 * Owns the Ring (per-device sample buffer), DeviceOut (one Bluetooth sink +
 * its stream), App (the global app state), the `app` instance, and the few
 * forward declarations the real-time callbacks rely on.
 */
#pragma once



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

    /* GUI "disconnect" (connect 0): our playback stream is dropped and the
       ring is no longer fed, but the registry entry is KEPT so a later
       connect 1 resumes without a registry round-trip. This is ours alone —
       a physically vanished bud additionally gets `gone` from the registry
       path, which always wins. */
    bool   dropped = false;

    /* registry removal flag */
    bool   gone = false;

    /* GUI-visible state, echoed back as #D lines so the GUI can show the
       real sink state and toggle each control back and forth. These are
       updated by ui_apply_mute / ui_apply_volume / ui_set_connected. */
    bool   muted   = false;   /* sink currently muted            */
    bool   conn    = true;    /* stream attached (false = dropped) */
    double   vol     = 100.0;   /* last known volume percent       */
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

/* ------------------------------------------------------------------ */
/* --ui machine mode (see sync_ui.h)                                   */
/*                                                                     */
/* When enabled the engine writes machine-readable status lines (one    */
/* per tick, each starting '#') to stdout and reads GUI commands from    */
/* stdin, polled on the main PipeWire loop — never on the RT path. The   */
/* default (no --ui) behaviour is completely unchanged.                  */
/* ------------------------------------------------------------------ */
static bool ui_mode = false;
static void ui_emit_status();    /* defined in sync_ui.h */
static void ui_setup();          /* defined in sync_ui.h */


