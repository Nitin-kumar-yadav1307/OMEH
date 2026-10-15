/*
 * win_types.h — core data structures and global state.
 *
 * Single-TU module, part of pipewire_sync_win.cpp. Included AFTER win_config.h.
 * Owns Ring (per-device sample buffer), F32Resampler, DeviceOut (one Bluetooth
 * endpoint + its WASAPI stream), App (global state), the `app` instance, the
 * stop/event globals, and a few forward declarations.
 */
#pragma once


struct Ring
{
    int16_t data[RING_SIZE * CHANNELS] = {};
    size_t  read  = 0;
    size_t  write = 0;
    size_t level() const { return write - read; }
    void flush_to(size_t frames)
    {
        if (frames > level()) frames = level();
        read = write - frames;
    }
    void write_frames(const int16_t *src, size_t n)
    {
        if (n > RING_SIZE)
        {
            src += (n - RING_SIZE) * CHANNELS;
            n = RING_SIZE;
        }
        if (level() + n > RING_SIZE)
            read = write + n - RING_SIZE;
        for (size_t i = 0; i < n; i++)
        {
            size_t w = (write + i) & RING_MASK;
            memcpy(&data[w * CHANNELS], &src[i * CHANNELS], FRAME_BYTES);
        }
        write += n;
    }
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
struct F32Resampler
{
    std::vector<float> buf;
    double pos  = 0.0;
    double step = 1.0;
    void reset(double s) { buf.clear(); pos = 0.0; step = s; }
    void push(const float *s, size_t n) { buf.insert(buf.end(), s, s + n * 2); }
    int pop(float *dst, int want)
    {
        int frames = (int)(buf.size() / 2);
        int made = 0;
        while (made < want)
        {
            int i = (int)pos;
            if (i + 1 >= frames) break;
            double f = pos - (double)i;
            dst[made * 2 + 0] = (float)(buf[i * 2 + 0] +
                    (buf[(i + 1) * 2 + 0] - buf[i * 2 + 0]) * f);
            dst[made * 2 + 1] = (float)(buf[i * 2 + 1] +
                    (buf[(i + 1) * 2 + 1] - buf[i * 2 + 1]) * f);
            pos += step;
            made++;
        }
        int drop = (int)pos;
        int max_drop = frames > 0 ? frames - 1 : 0;
        if (drop > max_drop) drop = max_drop;
        if (drop > 0)
        {
            buf.erase(buf.begin(), buf.begin() + (size_t)drop * 2);
            pos -= (double)drop;
        }
        return made;
    }
};
struct DeviceOut
{
    std::string  name;
    std::string  id;
    std::wstring id_w;
    IMMDevice          *endpoint = nullptr;
    IAudioClient       *client   = nullptr;
    IAudioRenderClient *render   = nullptr;
    WAVEFORMATEX       *mix_fmt  = nullptr;
    UINT32  buffer_frames = 0;
    REFERENCE_TIME period = 0;
    double  mix_rate = 0.0;
    bool    use_mix  = false;
    F32Resampler out_rs;
    double       mix_push_acc = 0.0;
    std::vector<int16_t> scratch;
    std::vector<float>   out_f32;
    Ring   ring;
    double phase   = 0.0;
    bool   started = false;
    double   ratio   = 1.0;
    uint64_t written = 0;
    double   extra_delay_ms = 0.0;
    bool     manual_trim    = false;
    bool     measuring      = true;
    double   meas_ema       = 0.0;
    double   meas_min_ms    = 0.0;
    double   meas_max_ms    = 0.0;
    uint64_t meas_count     = 0;
    uint64_t meas_frames    = 0;
    double   meas_q_sum     = 0.0;
    uint64_t meas_q_count   = 0;
    double   dev_latency_ms = 0.0;
    double   target_level   = 0.0;
    double   avg_quantum    = 0.0;
    double   avg_level      = -1.0;
    double   dev_frames_s   = -1.0;
    double   total_frames   = -1.0;
    int64_t  master_off = 0;
    uint64_t click_from = 0;
    uint64_t hard_syncs = 0;
    int      xfade_left = 0;
    int16_t  xfade_last[CHANNELS] = {0, 0};
    uint64_t underruns         = 0;
    double   last_latency_ms   = 0.0;
    double   last_drift_frames = 0.0;
    bool     audible_logged    = false;
    bool     gone   = false;
    bool     failed = false;
};
struct App
{
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice        *src_endpoint = nullptr;
    std::wstring      src_id;
    std::string       src_name;
    IAudioClient     *cap_client = nullptr;
    IAudioCaptureClient *cap = nullptr;
    WAVEFORMATEX     *cap_mix = nullptr;
    double            cap_rate = 0.0;
    F32Resampler     cap_rs;
    uint64_t master_written = 0;
    std::vector<DeviceOut *> devices;
    std::wstring prev_default_id;
    std::string  prev_default_name;
    bool   switched_default = false;
    bool   prev_saved       = false;
    bool     aligning    = true;
    uint64_t align_ticks = 0;
    double   ref_latency_ms = 0.0;
    std::map<std::string, double> trim_by_id;
    uint64_t capture_callbacks = 0;
    uint64_t capture_empties   = 0;
    bool     idle_gap          = false;
    uint64_t prev_master       = 0;
    uint64_t prev_tick_ms      = 0;
    uint64_t last_health_ms    = 0;
};
static App app;
static volatile LONG g_stop     = 0;
static HANDLE g_stop_evt        = nullptr;
static HANDLE g_clean_evt       = nullptr;
static uint64_t g_synth_upto_ms = 0;
static int      g_selected   = 0;
static uint64_t g_click_from = 0;
static void print_usage();
static void trim_save();
static double default_target_level(const DeviceOut *dev);
