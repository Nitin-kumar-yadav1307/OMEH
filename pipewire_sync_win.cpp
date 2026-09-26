/*
 * pipewire_sync_win.cpp
 *
 * Windows port of pipewire_sync.cpp — same job: send the SAME audio to
 * EVERY connected Bluetooth earbud while measuring and correcting the
 * latency difference and the clock drift between the devices.
 *
 * Zero third-party software. Everything below is the inbox Windows SDK
 * (WASAPI / MMDevice API / COM) — build once, run, Ctrl+C to stop.
 *
 * Concept mapping from the Linux version:
 *
 *   Linux (PipeWire / pactl)        Windows (WASAPI)
 *   ----------------------------- -----------------------------------
 *   virtual null sink sync_master   the machine's real default output
 *   monitor of sync_master          WASAPI loopback capture of it
 *   registry: bluez Audio/Sink      EnumAudioEndpoints + BTHENUM check
 *   pw_stream per bud (pinned)      one IAudioClient per bud (shared)
 *   pw_stream_get_time_n delay      IAudioClock: submitted - position
 *   wpctl/pactl set-default         inbox-but-undocumented IPolicyConfig
 *   pactl move-sink-input safety    not needed: WASAPI streams cannot
 *                                   be relocated behind our back
 *
 * Read these three Windows differences before using it:
 *
 *  1. Windows has NO built-in silent virtual device, so we loopback-
 *     capture the real default output — which KEEPS PLAYING the sound
 *     (usually the Speakers). Turn the speaker volume down if you do
 *     not want to hear it twice. CAUTION: on Windows 10 muting the
 *     source can mute the loopback capture itself (the tap sits after
 *     the mute on some builds); Windows 11 is not affected. Safe
 *     recipe: leave the source UNMUTED at low volume.
 *
 *  2. If the default output IS one of the earbuds, that bud would get
 *     the audio twice (directly and from us = echo), so we switch the
 *     default to a non-Bluetooth device first — via the undocumented
 *     but inbox IPolicyConfig COM interface (the exact equivalent of
 *     the Linux version pointing the default at sync_master) — and
 *     restore the original default on exit.
 *
 *  3. Windows does not expose Bluetooth codec-internal latency to
 *     applications, so the automatic alignment covers only what
 *     WASAPI reports (engine + driver queue). Any residual offset
 *     you can hear can be trimmed per device on the command line,
 *     exactly like the Linux build:
 *
 *         pipewire_sync_win.exe 0:0 1:12
 *
 *     -> device #0: +0 ms, device #1: +12 ms extra delay.
 *
 * Build (only Windows SDK libraries, no third-party anything):
 *   MSVC — x64 Native Tools Command Prompt for VS:
 *     cl /O2 /EHsc pipewire_sync_win.cpp ole32.lib oleaut32.lib uuid.lib winmm.lib
 *   MinGW-w64:
 *     g++ -O2 -std=c++17 -o pipewire_sync_win.exe pipewire_sync_win.cpp
 *         -lole32 -loleaut32 -luuid -lwinmm
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>          /* timeBeginPeriod — keeps Sleep() honest */
#include <mmreg.h>            /* WAVEFORMATEX / EXTENSIBLE             */
#include <mmdeviceapi.h>      /* IMMDevice* enumerating                */
#include <audioclient.h>      /* WASAPI IAudioClient and friends       */
#include <endpointvolume.h>   /* IAudioEndpointVolume (unmute)         */
#include <propsys.h>          /* IPropertyStore (device properties)    */
#include <propkey.h>          /* PROPERTYKEY                           */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <set>

/* ------------------------------------------------------------------ *
 * Explicit interface GUIDs (transcribed from the Windows SDK headers) *
 * so the file also builds with compilers that lack __uuidof (g++).    *
 * ------------------------------------------------------------------ */
static const GUID IID_IAudioClient_         = {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const GUID IID_IAudioRenderClient_   = {0xf294acfc,0x3146,0x4483,{0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2}};
static const GUID IID_IAudioCaptureClient_  = {0xc8adbd64,0xe71e,0x48a0,{0xa4,0xde,0x18,0x5c,0x39,0x5c,0xd3,0x17}};
static const GUID IID_IAudioClock_          = {0xcd63314f,0x3fba,0x4a1b,{0x81,0x2c,0xef,0x96,0x35,0x87,0x28,0xe7}};
static const GUID IID_IAudioEndpointVolume_ = {0x5cdf2c82,0x841e,0x4546,{0x97,0x22,0x0c,0xf7,0x40,0x78,0x22,0x9a}};
static const GUID IID_IMMDeviceEnumerator_  = {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID CLSID_MMDeviceEnumerator_ = {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};

/* PROPERTYKEYs used for device properties (propkey/devpkey values) */
static const PROPERTYKEY PK_FriendlyName = {
    {0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14};
/* DEVPKEY_Device_InstanceId — a Bluetooth endpoint's instance id starts
   with "BTHENUM\" / "BTH\"; that string is our Bluetooth detector. */
static const PROPERTYKEY PK_InstanceId = {
    {0x78c34fc8,0x104a,0x4aca,{0x9e,0xa4,0x52,0x4d,0x52,0x99,0x6e,0x57}}, 256};

/* some mingw builds omit the stream-flag constants (SDK defines them) */
#ifndef AUDCLNT_STREAMFLAGS_LOOPBACK
#define AUDCLNT_STREAMFLAGS_LOOPBACK 0x00020000
#endif

/* ================================================================== *
 * Tuning constants — identical numbers to the Linux build so both     *
 * platforms behave the same way.                                      *
 * ================================================================== */
#define RATE            48000
#define CHANNELS        2
#define FRAME_BYTES     (CHANNELS * sizeof(int16_t))

/* ring capacity in frames (power of two -> wrap with a mask) */
#define RING_BITS       15
#define RING_SIZE       (1u << RING_BITS)
#define RING_MASK       (RING_SIZE - 1)

/*
 * Drift controller. We regulate the ring BUFFER DEPTH (level), not an
 * absolute sample position: capture writes one frame into every ring
 * per captured frame, so level(i) = master_written - consumed(i) —
 * holding every device's level at its target keeps every earbud at
 * the same position of the stream, which IS "in sync".
 *
 *   ratio = 1 + DRIFT_KP * smoothed(level - target)
 *
 * The level jitters by whole engine quanta, so it is exponentially
 * averaged over DRIFT_TAU before entering the loop (otherwise the
 * jitter becomes audible pitch wobble). The closed-loop time constant
 * 1/(DRIFT_KP*RATE) = DRIFT_LOOP_TAU seconds must stay far above the
 * transport delay or the loop oscillates.
 */
#define DRIFT_TAU       10.0    /* s, level-error averaging              */
#define DRIFT_LOOP_TAU  40.0    /* s, closed-loop time constant          */
#define DRIFT_KP        (1.0 / ((double)RATE * DRIFT_LOOP_TAU))
#define DRIFT_MAX_PPM   500.0   /* clamp on resampler ratio              */

/* if a device is further out than this (~50 ms), jump, do not warp */
#define HARD_SYNC_FRAMES 2400.0

/*
 * Latency alignment:
 *   ALIGN_BASE_MS : minimum steady state buffer depth per device
 *   ALIGN_MAX_MS  : hard cap on the alignment delay we add
 *   MEASURE_TICKS : 1-second ticks we probe before freezing offsets
 */
#define ALIGN_BASE_MS   40.0
#define ALIGN_MAX_MS    350.0
#define ALIGN_SMOOTH    0.3     /* smoothing of per-tick measurements    */
#define MEASURE_TICKS   2

#define POLL_MS         5       /* main loop period (needs timeBeginPeriod(1)) */
#define HEALTH_MS       1000    /* 1 s health / alignment / status tick  */

/* our own marker, so logs read like the Linux build */
#define OUR_APP_NAME    "pipewire_sync"

/* ------------------------------------------------------------------ *
 * Stereo interleaved ring buffer — direct port of the Linux Ring.     *
 * Everything runs on ONE thread (the poll loop), so plain size_t      *
 * indices are safe. write_frames keeps the NEWEST audio on overflow.  *
 * ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ *
 * Stateful linear resampler between two float32 stereo streams.       *
 * Used twice:                                                         *
 *    capture: device mix rate -> RATE   (step = mix_rate / 48000)     *
 *    render : RATE -> device mix rate   (step = 48000 / mix_rate)     *
 * The interpolation maths is the same as Ring::read_frame.            *
 * ------------------------------------------------------------------ */
struct F32Resampler
{
    std::vector<float> buf;   /* interleaved history                   */
    double pos  = 0.0;        /* fractional read cursor                */
    double step = 1.0;        /* source frames per output frame        */

    void reset(double s) { buf.clear(); pos = 0.0; step = s; }
    void push(const float *s, size_t n) { buf.insert(buf.end(), s, s + n * 2); }

    /* produce up to `want` frames; returns how many were produced
       (may be less when the input has not arrived yet) */
    int pop(float *dst, int want)
    {
        int frames = (int)(buf.size() / 2);
        int made = 0;
        while (made < want)
        {
            int i = (int)pos;
            if (i + 1 >= frames) break;   /* need i+1 inside the buffer */
            double f = pos - (double)i;
            dst[made * 2 + 0] = (float)(buf[i * 2 + 0] +
                    (buf[(i + 1) * 2 + 0] - buf[i * 2 + 0]) * f);
            dst[made * 2 + 1] = (float)(buf[i * 2 + 1] +
                    (buf[(i + 1) * 2 + 1] - buf[i * 2 + 1]) * f);
            pos += step;
            made++;
        }
        /* forget consumed history, keep one sample of look-back */
        int drop = (int)pos;
        if (drop > 0)
        {
            buf.erase(buf.begin(), buf.begin() + (size_t)drop * 2);
            pos -= (double)drop;
        }
        return made;
    }
};

/* ------------------------------------------------------------------ *
 * One output earbud                                                    *
 * ------------------------------------------------------------------ */
struct DeviceOut
{
    std::string  name;        /* friendly name, e.g. "Headphones (Buds)" */
    std::string  id;          /* IMMDevice::GetId (narrowed)             */
    std::wstring id_w;        /* same, wide (for lookups)                */

    IMMDevice          *endpoint = nullptr;  /* AddRef'd by us           */
    IAudioClient       *client   = nullptr;
    IAudioRenderClient *render   = nullptr;
    IAudioClock        *clock    = nullptr;
    WAVEFORMATEX       *mix_fmt  = nullptr;  /* only on format fallback  */
    UINT32  buffer_frames = 0;               /* engine buffer, frames    */
    REFERENCE_TIME period = 0;               /* engine period, 100 ns    */
    double  mix_rate = 0.0;
    bool    use_mix  = false;                /* need software convert    */
    F32Resampler out_rs;                     /* 48k -> mix rate          */
    double       mix_push_acc = 0.0;   /* fractional push budget (frames) */
    std::vector<int16_t> scratch;            /* our-domain temp buffer   */
    std::vector<float>   out_f32;            /* device-domain temp       */

    Ring   ring;
    double phase   = 0.0;   /* fractional read position in ring          */
    bool   started = false; /* alignment buffer filled?                 */

    /* drift controller state */
    double   ratio   = 1.0;
    uint64_t written = 0;   /* frames handed to the device              */
    double   extra_delay_ms = 0.0;  /* manual trim from argv             */
    bool     manual_trim    = false;

    /* latency measurement / alignment (fields mirror the Linux struct) */
    bool     measuring      = true;  /* still probing own latency        */
    double   meas_sum       = 0.0;   /* sum of latency probes (ms)       */
    uint64_t meas_count     = 0;     /* number of probes                 */
    uint64_t meas_frames    = 0;     /* frames since last probe          */
    double   meas_q_sum     = 0.0;   /* sum of fill sizes (frames)       */
    uint64_t meas_q_count   = 0;
    double   dev_latency_ms = 0.0;   /* measured device latency (frozen) */
    double   target_level   = 0.0;   /* steady buffer depth (frames)     */
    double   avg_quantum    = 0.0;   /* average fill size (frames)       */
    double   avg_level      = -1.0;  /* smoothed ring depth              */
    double   dev_frames_s   = -1.0;  /* smoothed device delay (frames)   */
    double   total_frames   = -1.0;  /* smoothed total latency (frames)  */

    uint64_t submitted = 0;  /* device-domain frames handed to engine    */

    /* diagnostics */
    uint64_t underruns         = 0;
    double   last_latency_ms   = 0.0;
    double   last_drift_frames = 0.0;
    bool     audible_logged    = false;
    bool     gone   = false;  /* endpoint disappeared                   */
    bool     failed = false;  /* stream error — health tick restarts it */
};

/* ------------------------------------------------------------------ *
 * Global app state                                                     *
 * ------------------------------------------------------------------ */
struct App
{
    IMMDeviceEnumerator *enumerator = nullptr;

    /* capture: WASAPI loopback of the source (non-Bluetooth) output   */
    IMMDevice        *src_endpoint = nullptr;
    std::wstring      src_id;
    std::string       src_name;
    IAudioClient     *cap_client = nullptr;
    IAudioCaptureClient *cap = nullptr;
    WAVEFORMATEX     *cap_mix = nullptr;
    double            cap_rate = 0.0;
    F32Resampler     cap_rs;

    uint64_t master_written = 0;   /* frames captured (48k domain)      */
    std::vector<DeviceOut *> devices;

    std::wstring prev_default_id;  /* restored on exit if we changed it */
    std::string  prev_default_name;
    bool   switched_default = false;
    bool   prev_saved       = false;

    /* latency alignment phase */
    bool     aligning    = true;
    uint64_t align_ticks = 0;
    double   ref_latency_ms = 0.0;

    /* user-supplied per-device trims that survive reconnects */
    std::map<std::string, double> trim_by_id;

    /* diagnostics */
    uint64_t capture_callbacks = 0;
    uint64_t capture_empties   = 0;
    bool     idle_gap          = false;
    uint64_t prev_master       = 0;
    uint64_t prev_tick_ms      = 0;
    uint64_t last_health_ms    = 0;
};

static App app;
static volatile LONG g_stop     = 0;   /* set by Ctrl+C handler          */
static HANDLE g_stop_evt        = nullptr;
static HANDLE g_clean_evt       = nullptr;
static uint64_t g_last_synth_ms = 0;   /* silence synthesis clock        */

/* defined below start_playback(), which needs it for late-joining buds */
static double default_target_level(const DeviceOut *dev);

/* ------------------------------------------------------------------ *
 * Small helpers                                                        *
 * ------------------------------------------------------------------ */
static std::string narrow(const wchar_t *w)
{
    std::string s;
    if (!w) return s;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1)
    {
        s.resize((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
        s.resize((size_t)n - 1);   /* drop the NUL */
    }
    return s;
}

static bool prop_string(IMMDevice *d, const PROPERTYKEY &pk, std::string &out)
{
    IPropertyStore *ps = nullptr;
    if (!d || FAILED(d->OpenPropertyStore(0 /* STGM_READ */, &ps)) || !ps)
        return false;

    PROPVARIANT pv;
    memset(&pv, 0, sizeof(pv));
    bool ok = false;
    if (SUCCEEDED(ps->GetValue(pk, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
    {
        out = narrow(pv.pwszVal);
        ok = true;
    }
    /* the VT_LPWSTR payload is CoTaskMemAlloc'ed — this is exactly what
       PropVariantClear() would free; done manually to avoid a header */
    if (pv.vt == VT_LPWSTR && pv.pwszVal)
        CoTaskMemFree(pv.pwszVal);
    ps->Release();
    return ok;
}

static bool is_bluetooth(IMMDevice *d, std::string *instance_out)
{
    std::string inst;
    if (!prop_string(d, PK_InstanceId, inst))
        return false;
    if (instance_out) *instance_out = inst;
    /* BT endpoint instance ids start with "BTHENUM\" or "BTH\" */
    return inst.size() >= 3 &&
           tolower((unsigned char)inst[0]) == 'b' &&
           tolower((unsigned char)inst[1]) == 't' &&
           tolower((unsigned char)inst[2]) == 'h';
}

static std::wstring id_of(IMMDevice *d)
{
    std::wstring r;
    LPWSTR s = nullptr;
    if (d && SUCCEEDED(d->GetId(&s)) && s)
    {
        r = s;
        CoTaskMemFree(s);
    }
    return r;
}

/* per-device extra delay from argv: "index:ms" — same syntax as Linux */
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

/* ------------------------------------------------------------------ *
 * Default-device switching — inbox but UNDOCUMENTED COM interface.    *
 * (Same role as the Linux version's `wpctl set-default sync_master`.) *
 * IPolicyConfig has shipped in Windows since Vista; the SDK does not  *
 * publish it, so we declare the vtable ourselves. SetDefaultEndpoint  *
 * sits at the same slot in both known IIDs; we try them in turn.      *
 * Only used when the default output has to move off an earbud.        *
 * ------------------------------------------------------------------ */
struct IPolicyCfg : public IUnknown
{
    virtual HRESULT __stdcall GetMixFormat(void *, void **) = 0;
    virtual HRESULT __stdcall GetDeviceFormat(void *, int, void **) = 0;
    virtual HRESULT __stdcall ResetDeviceFormat(void *) = 0;
    virtual HRESULT __stdcall SetDeviceFormat(void *, void *, void *) = 0;
    virtual HRESULT __stdcall GetProcessingPeriod(void *, int, int64_t *) = 0;
    virtual HRESULT __stdcall SetProcessingPeriod(void *, int64_t *) = 0;
    virtual HRESULT __stdcall GetShareMode(void *, void *) = 0;
    virtual HRESULT __stdcall SetShareMode(void *, void *) = 0;
    virtual HRESULT __stdcall GetPropertyValue(void *, const PROPERTYKEY &,
                                               PROPVARIANT *) = 0;
    virtual HRESULT __stdcall SetPropertyValue(void *, const PROPERTYKEY &,
                                               PROPVARIANT *) = 0;
    virtual HRESULT __stdcall SetDefaultEndpoint(const wchar_t *, int) = 0;
    virtual HRESULT __stdcall SetEndpointVisibility(const wchar_t *, int) = 0;
};

static const GUID CLSID_PolicyCfgClient = {0x870af99c,0x171d,0x4f9e,{0xaf,0x0d,0xe6,0x3d,0xf4,0x0c,0x2b,0xc9}};
static const GUID IID_PolicyCfg_Vista   = {0xf8679f50,0x850a,0x41cf,{0x9c,0x72,0x43,0x0f,0x29,0x02,0x90,0xc8}};
static const GUID IID_PolicyCfg_Win7    = {0x568b9108,0x44bf,0x40b4,{0x90,0x06,0x86,0xaf,0xe5,0xb5,0xa6,0x20}};

static bool win_set_default(const std::wstring &devid)
{
    IPolicyCfg *p = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_PolicyCfgClient, nullptr, CLSCTX_ALL,
                                  IID_PolicyCfg_Vista, (void **)&p);
    if (FAILED(hr) || !p)
        hr = CoCreateInstance(CLSID_PolicyCfgClient, nullptr, CLSCTX_ALL,
                              IID_PolicyCfg_Win7, (void **)&p);
    if (FAILED(hr) || !p)
        return false;

    bool ok = true;
    for (int role = 0; role <= 2; role++)   /* console, multimedia, comms */
        if (FAILED(p->SetDefaultEndpoint(devid.c_str(), role)))
            ok = false;
    p->Release();
    return ok;
}

static bool get_default_id(std::wstring &id, std::string &name)
{
    IMMDevice *d = nullptr;
    if (!app.enumerator ||
        FAILED(app.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &d)) ||
        !d)
        return false;
    id = id_of(d);
    prop_string(d, PK_FriendlyName, name);
    d->Release();
    return !id.empty();
}

/* list every active output once, at startup (educational, like the
   Linux build's registry log) */
static void print_all_outputs()
{
    IMMDeviceCollection *col = nullptr;
    if (FAILED(app.enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                                  &col)) || !col)
        return;
    UINT n = 0;
    col->GetCount(&n);
    if (n == 0)
        fprintf(stdout, "[registry] (no active output devices visible)\n");
    for (UINT i = 0; i < n; i++)
    {
        IMMDevice *d = nullptr;
        if (FAILED(col->Item(i, &d)) || !d) continue;
        std::string nm;
        prop_string(d, PK_FriendlyName, nm);
        bool bt = is_bluetooth(d, nullptr);
        fprintf(stdout, "[registry] output: \"%s\"%s\n",
                nm.c_str(), bt ? "   [Bluetooth]" : "");
        d->Release();
    }
    col->Release();
}

/*
 * Choose the device we loopback-capture from:
 *   - the current default, if it is NOT a Bluetooth earbud (the common
 *     case: Speakers / HDMI / jack);
 *   - otherwise the first active non-Bluetooth output — switching the
 *     default to it through IPolicyConfig so no bud gets the audio
 *     twice (directly + from us = echo). The original default is
 *     restored on exit.
 */
static bool pick_source()
{
    std::wstring def_id;
    std::string  def_name;
    bool have_def = get_default_id(def_id, def_name);

    /* is the default a Bluetooth device? */
    bool def_is_bt = false;
    if (have_def)
    {
        IMMDevice *d = nullptr;
        if (SUCCEEDED(app.enumerator->GetDevice(def_id.c_str(), &d)) && d)
        {
            def_is_bt = is_bluetooth(d, nullptr);
            d->Release();
        }
    }

    if (have_def && !def_is_bt)
    {
        app.src_id   = def_id;
        app.src_name = def_name;
        if (!app.prev_saved)
        {
            app.prev_default_id   = def_id;
            app.prev_default_name = def_name;
            app.prev_saved        = true;
        }
        fprintf(stdout, "[main] capture source = default output \"%s\"\n",
                app.src_name.c_str());
        return true;
    }

    /* default is a bud (or missing): find any non-Bluetooth output */
    std::wstring src;
    std::string  src_name;
    IMMDeviceCollection *col = nullptr;
    if (SUCCEEDED(app.enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                                     &col)) && col)
    {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n && src.empty(); i++)
        {
            IMMDevice *d = nullptr;
            if (FAILED(col->Item(i, &d)) || !d) continue;
            if (!is_bluetooth(d, nullptr))
            {
                src = id_of(d);
                prop_string(d, PK_FriendlyName, src_name);
            }
            d->Release();
        }
        col->Release();
    }

    if (src.empty())
    {
        fprintf(stderr,
            "No non-Bluetooth output found to capture from.\n"
            "  Windows has no built-in silent virtual device, so we need\n"
            "  Speakers/HDMI/jack enabled as the audio source.\n");
        return false;
    }

    if (!app.prev_saved && have_def)
    {
        app.prev_default_id   = def_id;
        app.prev_default_name = def_name;
        app.prev_saved        = true;
    }

    if (!win_set_default(src))
    {
        fprintf(stderr,
            "Could not switch the default output to \"%s\"\n"
            "  (IPolicyConfig refused — set it manually in\n"
            "   Settings > Sound, then run this program again.\n"
            "   Otherwise the default earbud would play audio twice.)\n",
            src_name.c_str());
        return false;
    }
    app.switched_default = true;
    app.src_id   = src;
    app.src_name = src_name;
    fprintf(stdout, "[main] default output -> \"%s\" (was \"%s\")\n",
            src_name.c_str(), def_name.c_str());
    fprintf(stdout, "[main] capture source = \"%s\"\n", src_name.c_str());
    return true;
}

/* ------------------------------------------------------------------ *
 * Capture: WASAPI loopback of the source device                        *
 * (the Windows equivalent of reading sync_master's monitor)            *
 * ------------------------------------------------------------------ */
static void stop_capture()
{
    if (app.cap_client) app.cap_client->Stop();
    if (app.cap)       { app.cap->Release();       app.cap = nullptr; }
    if (app.cap_client){ app.cap_client->Release(); app.cap_client = nullptr; }
    if (app.cap_mix)   { CoTaskMemFree(app.cap_mix); app.cap_mix = nullptr; }
    if (app.src_endpoint) { app.src_endpoint->Release(); app.src_endpoint = nullptr; }
    app.cap_rate = 0.0;
}

static bool start_capture()
{
    stop_capture();   /* idempotent */

    IMMDevice *d = nullptr;
    if (app.src_id.empty() ||
        FAILED(app.enumerator->GetDevice(app.src_id.c_str(), &d)) || !d)
    {
        /* source device disappeared (HDMI unplugged / BT switcheroo):
           re-pick a source; pick_source may move the default again */
        if (!pick_source())
            return false;
        if (FAILED(app.enumerator->GetDevice(app.src_id.c_str(), &d)) || !d)
            return false;
    }
    app.src_endpoint = d;

    HRESULT hr = d->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr,
                             (void **)&app.cap_client);
    if (FAILED(hr) || !app.cap_client)
    {
        fprintf(stderr, "[capture] Activate failed: 0x%08lx\n", (unsigned long)hr);
        stop_capture();
        return false;
    }

    /* loopback must use the device's mix format — we convert ourselves */
    if (FAILED(app.cap_client->GetMixFormat(&app.cap_mix)) || !app.cap_mix)
    {
        fprintf(stderr, "[capture] GetMixFormat failed\n");
        stop_capture();
        return false;
    }
    app.cap_rate = app.cap_mix->nSamplesPerSec;

    hr = app.cap_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    0, 0, app.cap_mix, nullptr);
    if (FAILED(hr))
    {
        fprintf(stderr, "[capture] Initialize(LOOPBACK) failed: 0x%08lx\n",
                (unsigned long)hr);
        stop_capture();
        return false;
    }

    app.cap_client->GetService(IID_IAudioCaptureClient_, (void **)&app.cap);
    if (!app.cap)
    {
        fprintf(stderr, "[capture] GetService(IAudioCaptureClient) failed\n");
        stop_capture();
        return false;
    }

    app.cap_rs.reset((double)app.cap_rate / (double)RATE);
    app.cap_client->Start();
    g_last_synth_ms = 0;
    app.idle_gap = false;

    fprintf(stdout, "[main] loopback capture started on \"%s\" (%.0f Hz)\n",
            app.src_name.c_str(), app.cap_rate);
    return true;
}

/* one WASAPI packet -> float32 stereo (first two channels) */
static bool packet_to_f32(const BYTE *src, UINT32 frames,
                          const WAVEFORMATEX *fmt, std::vector<float> &out)
{
    WORD tag  = fmt->wFormatTag;
    WORD bits = fmt->wBitsPerSample;
    WORD ch   = fmt->nChannels;
    bool is_float = (tag == WAVE_FORMAT_IEEE_FLOAT);

    if (tag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *)fmt;
        is_float = (e->SubFormat.Data1 == 3);   /* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT */
        tag = is_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    }

    if (!ch) return false;
    if (!(is_float && bits == 32) &&
        !(tag == WAVE_FORMAT_PCM && bits == 16))
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            fprintf(stderr,
                "[capture] unsupported mix format (tag=%u bits=%u) — "
                "only float32 and int16 are handled\n", tag, bits);
        }
        return false;
    }

    out.resize((size_t)frames * 2);
    if (is_float)
    {
        const float *s = (const float *)src;
        for (UINT32 f = 0; f < frames; f++)
        {
            float L = s[f * ch];
            float R = (ch > 1) ? s[f * ch + 1] : L;
            out[f * 2 + 0] = L;
            out[f * 2 + 1] = R;
        }
    }
    else
    {
        const int16_t *s = (const int16_t *)src;
        for (UINT32 f = 0; f < frames; f++)
        {
            float L = (float)s[f * ch] * (1.0f / 32768.0f);
            float R = (ch > 1) ? (float)s[f * ch + 1] * (1.0f / 32768.0f) : L;
            out[f * 2 + 0] = L;
            out[f * 2 + 1] = R;
        }
    }
    return true;
}

/* fan-out: one captured frame copy into every non-probing device ring */
static void write_to_rings(const int16_t *samples, uint32_t frames)
{
    for (DeviceOut *dev : app.devices)
        if (!dev->gone && !dev->measuring)
            dev->ring.write_frames(samples, frames);
    app.master_written += frames;
}

/*
 * Drain every packet currently queued on the loopback stream, convert
 * to 48k/int16 and copy into every device ring. (Linux equivalent:
 * on_capture_process.)
 *
 * Windows quirk: when NO application is rendering, the source engine
 * goes idle and the loopback simply stops delivering packets — on
 * Linux the sink monitor keeps ticking silence. If we did nothing,
 * the rings would drain away and the drift loop would crawl back at
 * ±500 ppm (minutes of dead air). So during an idle gap we SYNTHESISE
 * real-time silence into the rings: the ring depth (and therefore the
 * alignment) is held exactly where the controller wants it.
 */
static void drain_capture()
{
    if (!app.cap) return;

    static std::vector<float> f32;
    static std::vector<int16_t> pop16;
    static int16_t zeros[2400 * CHANNELS] = {};   /* up to 50 ms       */

    bool got_any = false;
    for (;;)
    {
        UINT32 pkt = 0;
        if (FAILED(app.cap->GetNextPacketSize(&pkt)) || pkt == 0)
            break;

        BYTE *data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(app.cap->GetBuffer(&data, &frames, &flags,
                                      nullptr, nullptr)))
            break;
        got_any = true;

        if (frames)
        {
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
            {
                /* engine reports "all silence": feed zeros, same as
                   real samples — keeps the ring depth honest */
                f32.assign((size_t)frames * 2, 0.0f);
                app.cap_rs.push(f32.data(), frames);
            }
            else if (data && packet_to_f32(data, frames, app.cap_mix, f32))
            {
                app.cap_rs.push(f32.data(), frames);
            }
        }

        app.cap->ReleaseBuffer(frames);
        app.capture_callbacks++;
    }

    /* ---- convert everything the resampler can give, into rings ---- */
    /* (the resampler buffers across packets, so this runs every call) */
    {
        static std::vector<float> dst;
        dst.resize(2048 * 2);
        for (;;)
        {
            int n = app.cap_rs.pop(dst.data(), 2048);
            if (n <= 0) break;
            pop16.resize((size_t)n * CHANNELS);
            for (int i = 0; i < n * CHANNELS; i++)
            {
                float v = dst[(size_t)i] * 32767.0f;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                pop16[(size_t)i] = (int16_t)v;
            }
            write_to_rings(pop16.data(), (uint32_t)n);
        }
    }

    /* ---- idle handling / silence synthesis (see comment above) ---- */
    uint64_t now = GetTickCount64();
    if (got_any)
    {
        app.idle_gap = false;
        g_last_synth_ms = now;
    }
    else
    {
        if (!app.idle_gap)
        {
            app.capture_empties++;
            app.idle_gap = true;
        }
        if (g_last_synth_ms != 0)
        {
            uint64_t elapsed = now - g_last_synth_ms;
            if (elapsed > 50) elapsed = 50;   /* never burst             */
            uint32_t frames = (uint32_t)(elapsed * RATE / 1000);
            if (frames > 2400) frames = 2400;
            if (frames)
                write_to_rings(zeros, frames);  /* master_written += too */
        }
        g_last_synth_ms = now;
    }
}

/* ------------------------------------------------------------------ *
 * Playback: one IAudioClient per earbud                               *
 * ------------------------------------------------------------------ */
static double default_target_level(const DeviceOut *dev)
{
    double ms = ALIGN_BASE_MS + dev->extra_delay_ms;
    if (ms < 0.0)          ms = 0.0;
    if (ms > ALIGN_MAX_MS) ms = ALIGN_MAX_MS;
    return ms * (double)RATE / 1000.0;
}

/* make sure the bud is unmuted and audible (pactl set-sink-mute 0) */
static void unmute_device(DeviceOut *dev)
{
    IAudioEndpointVolume *v = nullptr;
    if (dev->endpoint &&
        SUCCEEDED(dev->endpoint->Activate(IID_IAudioEndpointVolume_,
                                          CLSCTX_ALL, nullptr,
                                          (void **)&v)) && v)
    {
        v->SetMute(FALSE, nullptr);
        v->Release();
    }
}

/* drop the stream side, keep the endpoint (used on errors/restarts) */
static void teardown_device(DeviceOut *dev)
{
    if (dev->client)  dev->client->Stop();
    if (dev->render) { dev->render->Release(); dev->render = nullptr; }
    if (dev->clock)  { dev->clock->Release();  dev->clock  = nullptr; }
    if (dev->client) { dev->client->Release(); dev->client = nullptr; }
    if (dev->mix_fmt){ CoTaskMemFree(dev->mix_fmt); dev->mix_fmt = nullptr; }
    dev->use_mix = false;
    dev->mix_rate = 0.0;
    dev->buffer_frames = 0;
    dev->submitted = 0;
    dev->mix_push_acc = 0.0;
    dev->out_rs.reset(1.0);
}

/*
 * Device latency probe — the Windows equivalent of pw_stream_get_time_n:
 *
 *     delay = (frames we submitted to the engine) - (position the
 *             engine reports as already played)
 *
 * Both are converted to seconds, so a mix format at 44.1 kHz still
 * measures correctly. This is WASAPI's view of the queue — engine +
 * driver; codec-internal Bluetooth buffering is NOT visible to apps
 * (trim with argv, see header).
 */
static double device_delay_ms(DeviceOut *dev)
{
    if (!dev->clock)
        return dev->dev_latency_ms;
    UINT64 pos = 0, freq = 0;
    if (FAILED(dev->clock->GetPosition(&pos, &freq)) || freq == 0)
        return dev->dev_latency_ms;
    double sub_s = (double)dev->submitted / (double)RATE;
    double pos_s = (double)pos / (double)freq;
    double ms = (sub_s - pos_s) * 1000.0;
    return ms < 0.0 ? 0.0 : ms;
}

static void fail_device(DeviceOut *dev, HRESULT hr)
{
    if (dev->failed) return;
    dev->failed = true;
    fprintf(stderr, "[timer] %s stream error 0x%08lx - will reconnect\n",
            dev->name.c_str(), (unsigned long)hr);
    fflush(stderr);
    teardown_device(dev);
    dev->started = false;
    dev->measuring = app.aligning;
    dev->audible_logged = false;
}

static bool start_playback(DeviceOut *dev)
{
    teardown_device(dev);   /* idempotent — also used for restarts */

    HRESULT hr = dev->endpoint->Activate(IID_IAudioClient_, CLSCTX_ALL,
                                         nullptr, (void **)&dev->client);
    if (FAILED(hr) || !dev->client)
    {
        fprintf(stderr, "[main] %s: Activate(IAudioClient) failed 0x%08lx\n",
                dev->name.c_str(), (unsigned long)hr);
        return false;
    }

    /* prefer our own format: 48 kHz / int16 / stereo */
    WAVEFORMATEX want = {};
    want.wFormatTag      = WAVE_FORMAT_PCM;
    want.nChannels       = CHANNELS;
    want.nSamplesPerSec  = RATE;
    want.wBitsPerSample  = 16;
    want.nBlockAlign     = FRAME_BYTES;
    want.nAvgBytesPerSec = RATE * FRAME_BYTES;

    hr = dev->client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0,
                                 &want, nullptr);
    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
    {
        /* fall back to the device's mix format — we convert in software */
        dev->client->Release();
        dev->client = nullptr;
        hr = dev->endpoint->Activate(IID_IAudioClient_, CLSCTX_ALL,
                                     nullptr, (void **)&dev->client);
        if (SUCCEEDED(hr) && dev->client)
        {
            hr = dev->client->GetMixFormat(&dev->mix_fmt);
            if (SUCCEEDED(hr) && dev->mix_fmt)
            {
                dev->use_mix  = true;
                dev->mix_rate = dev->mix_fmt->nSamplesPerSec;
                dev->out_rs.reset((double)RATE / dev->mix_rate);
                hr = dev->client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                             0, 0, dev->mix_fmt, nullptr);
            }
        }
    }
    if (FAILED(hr) || !dev->client)
    {
        fprintf(stderr, "[main] %s: Initialize failed 0x%08lx\n",
                dev->name.c_str(), (unsigned long)hr);
        teardown_device(dev);
        return false;
    }

    dev->client->GetBufferSize(&dev->buffer_frames);
    dev->client->GetDevicePeriod(&dev->period, nullptr);
    dev->client->GetService(IID_IAudioRenderClient_, (void **)&dev->render);
    dev->client->GetService(IID_IAudioClock_,        (void **)&dev->clock);
    dev->submitted = 0;
    hr = dev->client->Start();

    /*
     * Buds that connect AFTER the measurement phase never probe —
     * give them the default buffer immediately so they start at once
     * (mirrors the Linux start_playback() logic).
     */
    if (!app.aligning)
    {
        dev->measuring    = false;
        dev->started      = false;
        dev->target_level = default_target_level(dev);
    }

    if (FAILED(hr))
    {
        fprintf(stderr, "[main] %s: Start failed 0x%08lx\n",
                dev->name.c_str(), (unsigned long)hr);
        teardown_device(dev);
        return false;
    }
    return true;
}

/*
 * The engine asks this device for data — port of Linux on_play_process(),
 * with the identical alignment / underrun / drift state machine:
 *
 *   measuring     -> feed silence, probe the device's own queue latency
 *   !started      -> feed silence until the ring holds target_level,
 *                    then flush the backlog to exactly the target and go
 *   ring empty    -> silence + underrun count
 *   otherwise     -> drift controller + resampled read from the ring
 *
 * `avail` is what the DEVICE wants (device domain). `need` is how many
 * of OUR 48k frames that corresponds to (equal unless the device runs
 * on a mix format rate we could not Initialize away from).
 */
static void fill_render(DeviceOut *dev)
{
    if (!dev->client || !dev->render || dev->gone || dev->failed)
        return;

    UINT32 padding = 0;
    HRESULT hr = dev->client->GetCurrentPadding(&padding);
    if (FAILED(hr)) { fail_device(dev, hr); return; }
    UINT32 avail = dev->buffer_frames > padding
                 ? dev->buffer_frames - padding : 0;
    if (avail == 0) return;

    UINT32 need = avail;
    if (dev->use_mix)
    {
        /*
         * Exact source-frame budget for the render resampler: the
         * fractional carry makes sure we neither starve it nor push
         * more than we pop (a fixed "+headroom" would accumulate a
         * few frames per call = ever-growing latency).
         */
        dev->mix_push_acc += (double)avail * (double)RATE / dev->mix_rate;
        need = (UINT32)dev->mix_push_acc;
        dev->mix_push_acc -= (double)need;
        if (need == 0 && avail > 0) need = 1;   /* mix rate > 48k edge */
    }

    BYTE *devbuf = nullptr;
    int16_t *out;
    if (!dev->use_mix)
    {
        hr = dev->render->GetBuffer(avail, &devbuf);
        if (FAILED(hr)) { fail_device(dev, hr); return; }
        out = (int16_t *)devbuf;
    }
    else
    {
        if (dev->scratch.size() < need) dev->scratch.resize(need);
        out = dev->scratch.data();
    }
    UINT32 frames = need;

    /* ---- Phase 1: latency probing (silence in, measure queue) ---- */
    if (dev->measuring)
    {
        memset(out, 0, (size_t)frames * FRAME_BYTES);
        dev->meas_q_sum += frames;
        dev->meas_q_count++;
        dev->meas_frames += frames;
        if (dev->meas_frames >= RATE / 4)          /* 4 probes / second */
        {
            dev->meas_frames -= RATE / 4;
            double ms = device_delay_ms(dev);
            if (ms > 0.0)
            {
                dev->meas_sum += ms;
                dev->meas_count++;
                dev->dev_latency_ms = dev->meas_sum / (double)dev->meas_count;
            }
        }
        goto commit;
    }

    /* ---- Phase 2: wait until the alignment buffer is full ---- */
    if (!dev->started)
    {
        if ((double)dev->ring.level() < dev->target_level)
        {
            memset(out, 0, (size_t)frames * FRAME_BYTES);
            goto commit;
        }
        /*
         * Trim the backlog down to exactly the target depth — the ring
         * may hold a lot of audio (it filled while we measured) and
         * starting from a full ring would add hundreds of ms of latency.
         */
        dev->ring.flush_to((size_t)dev->target_level);
        dev->started = true;
        dev->phase   = (double)dev->ring.read;
        dev->written = 0;
        dev->ratio   = 1.0;
    }

    if (dev->ring.level() == 0)
    {
        /* underrun — keep the device alive with silence */
        memset(out, 0, (size_t)frames * FRAME_BYTES);
        dev->underruns++;
        goto commit;
    }

    /* first REAL (non-silence) buffer handed to this device */
    if (!dev->audible_logged)
    {
        dev->audible_logged = true;
        fprintf(stderr, "[audio] %-28s first audio handed to device "
                "(ring=%zu target=%.0f master=%llu)\n",
                dev->name.c_str(), dev->ring.level(), dev->target_level,
                (unsigned long long)app.master_written);
        fflush(stderr);
    }

    /* ---- Phase 3: drift correction + resampled read --------------- */
    {
        double raw_level = (double)dev->ring.level();

        /* smooth the quantum jitter; only genuine drift enters the loop */
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

        for (UINT32 i = 0; i < frames; i++)
        {
            int16_t s[CHANNELS];
            dev->ring.read_frame(dev->phase, s);
            out[i * CHANNELS + 0] = s[0];
            out[i * CHANNELS + 1] = s[1];
            dev->phase += dev->ratio;
            dev->written++;
        }

        /* advance the ring past what we actually consumed — clamped so
           we never consume unwritten frames nor re-read old ones */
        double lo = (double)dev->ring.read;
        double hi = (dev->ring.write ? (double)dev->ring.write - 1.0 : 0.0)
                    + dev->ratio;
        if (dev->phase < lo) dev->phase = lo;
        if (dev->phase > hi) dev->phase = hi;
        dev->ring.advance((size_t)(dev->phase - (double)dev->ring.read));
        dev->phase = (double)dev->ring.read;
    }

commit:
    /* the engine position advances in DEVICE-domain frames */
    dev->submitted += avail;

    if (dev->use_mix)
    {
        /* our 48k int16 -> device mix format (resample if rates differ) */
        if (dev->out_f32.size() < (size_t)avail * 2)
            dev->out_f32.resize((size_t)avail * 2);
        float *f = dev->out_f32.data();
        for (UINT32 i = 0; i < frames * CHANNELS; i++)
            f[i] = (float)out[i] * (1.0f / 32768.0f);
        dev->out_rs.push(f, frames);

        UINT32 made = 0;
        while (made < avail)
        {
            int n = dev->out_rs.pop(f + (size_t)made * 2, (int)(avail - made));
            if (n <= 0) break;
            made += (UINT32)n;
        }
        if (made < avail)   /* never in practice — pad defensively */
            memset(f + (size_t)made * 2, 0,
                   (size_t)(avail - made) * 2 * sizeof(float));

        WORD bits = dev->mix_fmt->wBitsPerSample;
        WORD ch   = dev->mix_fmt->nChannels;
        bool is_float = (dev->mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (dev->mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            is_float = (((WAVEFORMATEXTENSIBLE *)dev->mix_fmt)->SubFormat.Data1 == 3);

        if (is_float && bits == 32)
        {
            float *dst = (float *)devbuf;
            for (UINT32 fr = 0; fr < avail; fr++)
            {
                dst[fr * ch + 0] = f[fr * 2 + 0];
                if (ch > 1) dst[fr * ch + 1] = f[fr * 2 + 1];
                for (WORD c = 2; c < ch; c++) dst[fr * ch + c] = 0.0f;
            }
        }
        else if (bits == 16)
        {
            int16_t *dst = (int16_t *)devbuf;
            for (UINT32 fr = 0; fr < avail; fr++)
            {
                float L = f[fr * 2 + 0] * 32767.0f;
                float R = f[fr * 2 + 1] * 32767.0f;
                dst[fr * ch + 0] = (int16_t)(L < -32768 ? -32768 :
                                             L > 32767 ? 32767 : L);
                if (ch > 1)
                    dst[fr * ch + 1] = (int16_t)(R < -32768 ? -32768 :
                                                 R > 32767 ? 32767 : R);
                for (WORD c = 2; c < ch; c++) dst[fr * ch + c] = 0;
            }
        }
    }

    hr = dev->render->ReleaseBuffer(avail, 0);
    if (FAILED(hr)) fail_device(dev, hr);
}

/* ------------------------------------------------------------------ *
 * Latency alignment                                                    *
 *   While `aligning`, every device is fed silence and we read how      *
 *   much delay its own chain reports. The slowest device becomes the   *
 *   reference; every other device gets an extra buffer of (ref - own)  *
 *   so they all emit the same sample at the same moment.               *
 * ------------------------------------------------------------------ */
static void finish_alignment()
{
    /* reference = slowest device */
    app.ref_latency_ms = 0.0;
    for (DeviceOut *dev : app.devices)
        if (!dev->gone && dev->dev_latency_ms > app.ref_latency_ms)
            app.ref_latency_ms = dev->dev_latency_ms;
    if (app.ref_latency_ms <= 0.0)
        app.ref_latency_ms = ALIGN_BASE_MS;

    for (DeviceOut *dev : app.devices)
    {
        if (dev->gone) continue;

        double align_ms = app.ref_latency_ms - dev->dev_latency_ms;
        if (align_ms < 0.0)          align_ms = 0.0;
        if (align_ms > ALIGN_MAX_MS) align_ms = ALIGN_MAX_MS;

        /* a manual trim from the command line is authoritative: it
           replaces the automatically measured alignment */
        if (dev->manual_trim)
            align_ms = 0.0;

        double ms = ALIGN_BASE_MS + align_ms + dev->extra_delay_ms;
        if (ms < 0.0)          ms = 0.0;
        if (ms > ALIGN_MAX_MS) ms = ALIGN_MAX_MS;

        double target_frames = ms * (double)RATE / 1000.0;

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

/* ------------------------------------------------------------------ *
 * Device discovery — called once at startup and then every health      *
 * tick, the Windows stand-in for the PipeWire registry events:         *
 *   - new Bluetooth buds get a DeviceOut + playback stream             *
 *   - vanished buds are torn down (gone=true) and picked up again      *
 *     when they reconnect (trim settings survive, keyed by id)         *
 * ------------------------------------------------------------------ */
static void poll_devices(bool autostart)
{
    IMMDeviceCollection *col = nullptr;
    if (FAILED(app.enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                                  &col)) || !col)
        return;
    UINT n = 0;
    col->GetCount(&n);

    std::set<std::wstring> present;
    std::map<std::wstring, IMMDevice *> found;
    for (UINT i = 0; i < n; i++)
    {
        IMMDevice *d = nullptr;
        if (FAILED(col->Item(i, &d)) || !d) continue;
        if (is_bluetooth(d, nullptr))
        {
            std::wstring id = id_of(d);
            present.insert(id);
            found[id] = d;          /* AddRef kept for new devices      */
        }
        else
            d->Release();
    }
    col->Release();

    /* buds that disappeared */
    for (DeviceOut *dev : app.devices)
    {
        if (!dev->gone && !present.count(dev->id_w))
        {
            fprintf(stdout, "[registry] device gone: %s\n", dev->name.c_str());
            fflush(stdout);
            teardown_device(dev);
            if (dev->endpoint) { dev->endpoint->Release(); dev->endpoint = nullptr; }
            dev->gone   = true;
            dev->failed = false;
            dev->started = false;
            dev->measuring = true;
            dev->meas_sum = 0.0; dev->meas_count = 0; dev->meas_frames = 0;
            dev->meas_q_sum = 0.0; dev->meas_q_count = 0;
            dev->avg_level = -1.0; dev->ratio = 1.0;
            dev->audible_logged = false;
            dev->ring = Ring();               /* drop stale backlog      */
            auto it = app.trim_by_id.find(dev->id);
            if (it != app.trim_by_id.end())
                dev->extra_delay_ms = it->second;
        }
    }

    /* new buds and reconnects */
    for (auto &kv : found)
    {
        DeviceOut *dev = nullptr;
        for (DeviceOut *d : app.devices)
            if (d->id_w == kv.first) { dev = d; break; }

        if (!dev)
        {
            dev = new DeviceOut();
            dev->id_w = kv.first;
            dev->id   = narrow(kv.first.c_str());
            prop_string(kv.second, PK_FriendlyName, dev->name);
            app.devices.push_back(dev);
            auto it = app.trim_by_id.find(dev->id);
            if (it != app.trim_by_id.end())
                dev->extra_delay_ms = it->second;
            fprintf(stdout, "[registry] found Bluetooth sink: %s\n",
                    dev->name.c_str());
            fflush(stdout);
        }
        else if (!dev->gone)
        {
            kv.second->Release();   /* already tracking this endpoint   */
            continue;
        }
        else
        {
            fprintf(stdout, "[registry] device back: %s\n", dev->name.c_str());
            fflush(stdout);
            dev->gone = false;
        }

        /* adopt the fresh endpoint pointer */
        if (dev->endpoint) dev->endpoint->Release();
        dev->endpoint = kv.second;

        if (!autostart)
            continue;               /* startup applies trims first      */

        if (start_playback(dev))
        {
            fprintf(stdout, "[main] stream started for %s\n",
                    dev->name.c_str());
            fflush(stdout);
            unmute_device(dev);
        }
        else
            dev->failed = true;
    }
}

/* ------------------------------------------------------------------ *
 * Status report — same table as the Linux on_health_timer              *
 * ------------------------------------------------------------------ */
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

    /*
     * What the ear hears is  ring depth + device delay  (both in ms);
     * device 0 is the reference, so its offset is 0.00 — same
     * presentation as the Linux build.
     */
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
                    "  dev %zu %-28s latency=%7.1f ms  offset=%+7.2f ms  "
                    "ratio=%.6f  buf=%zu  underruns=%llu\n",
                    i, dev->name.c_str(),
                    emit_ms,
                    emit_ms - ref_total_ms,
                    dev->ratio,
                    dev->ring.level(),
                    (unsigned long long)dev->underruns);
        }
        i++;
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ *
 * 1-second health tick: re-asserts the default, keeps the capture      *
 * alive, (re)connects streams, picks up new buds, drives alignment.    *
 * (Windows needs no reroute_clients(): WASAPI streams cannot be        *
 *  relocated behind our back like Pulse sink-inputs can.)              *
 * ------------------------------------------------------------------ */
static void health_tick(uint64_t now)
{
    /* 1. keep the default pointing at our source if we moved it */
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

    /* 2. capture health (source unplugged, engine error, ...) */
    if (!app.cap)
    {
        fprintf(stderr, "[timer] capture not running - restarting\n");
        fflush(stderr);
        start_capture();
    }

    /* 3. device health: pick up new/gone buds, restart dead streams */
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

    /* 4. latency alignment probe (mirror of the Linux timer) */
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
            /* devices never reported anything (timeout) */
            for (DeviceOut *dev : app.devices)
                if (dev->meas_count == 0)
                    dev->dev_latency_ms = ALIGN_BASE_MS;
            finish_alignment();
        }
    }

    /* 5. status (always, even when nothing is playing) */
    if (!app.aligning)
        print_status(now);
    else
        fflush(stdout);
}

/* ------------------------------------------------------------------ *
 * Ctrl+C / console close                                               *
 * ------------------------------------------------------------------ */
static BOOL WINAPI on_console_ctrl(DWORD type)
{
    InterlockedExchange(&g_stop, 1);
    if (g_stop_evt) SetEvent(g_stop_evt);
    /* when the console window itself is closing, Windows kills the
       process right after this handler returns — wait until main()
       has restored the default output */
    if ((type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
         type == CTRL_SHUTDOWN_EVENT) && g_clean_evt)
        WaitForSingleObject(g_clean_evt, 4000);
    return TRUE;
}

/* ------------------------------------------------------------------ *
 * Main loop — the Windows stand-in for pw_main_loop_run():             *
 * poll capture, feed every device, run the health tick, sleep.         *
 * ------------------------------------------------------------------ */
static void main_loop()
{
    app.last_health_ms = GetTickCount64();
    while (!g_stop)
    {
        drain_capture();
        for (DeviceOut *dev : app.devices)
            if (!dev->gone && !dev->failed)
                fill_render(dev);

        uint64_t now = GetTickCount64();
        if (now - app.last_health_ms >= HEALTH_MS)
        {
            app.last_health_ms = now;
            health_tick(now);
        }
        Sleep(POLL_MS);
    }
}

/* ------------------------------------------------------------------ *
 * Cleanup — safe to call on every exit path, even after partial setup  *
 * ------------------------------------------------------------------ */
static void cleanup_all()
{
    /* restore the default output if we changed it (do this FIRST so
       apps start flowing back to their real device immediately) */
    if (app.switched_default && !app.prev_default_id.empty())
    {
        if (win_set_default(app.prev_default_id))
            fprintf(stdout, "[main] default output restored -> \"%s\"\n",
                    app.prev_default_name.c_str());
        else
            fprintf(stdout,
                    "[main] could not restore default output - set it "
                    "manually to \"%s\"\n", app.prev_default_name.c_str());
        fflush(stdout);
        app.switched_default = false;
    }

    for (DeviceOut *dev : app.devices)
    {
        teardown_device(dev);
        if (dev->endpoint) dev->endpoint->Release();
        delete dev;
    }
    app.devices.clear();

    stop_capture();

    if (app.enumerator)
    {
        app.enumerator->Release();
        app.enumerator = nullptr;
    }

    timeEndPeriod(1);
    if (g_clean_evt) SetEvent(g_clean_evt);   /* wake a closing handler */

    CoUninitialize();
}

/* ------------------------------------------------------------------ *
 * main                                                                 *
 * ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    auto delays = parse_delay_args(argc, argv);
    int rc = 0;

    /* refuse to run twice — overlapping instances fight over routing */
    HANDLE mtx = CreateMutexW(nullptr, TRUE,
                              L"Local\\pipewire_sync_singleton");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        fprintf(stderr,
            "Another pipewire_sync instance is already running.\n"
            "Stop it first (close its window or Ctrl+C) — two instances "
            "would fight over routing.\n");
        CloseHandle(mtx);
        return 1;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        fprintf(stderr, "CoInitializeEx failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    /* Sleep(POLL_MS) would otherwise round up to ~15 ms on Windows —
       that starves 10 ms engine periods and causes audible glitches */
    timeBeginPeriod(1);

    g_stop_evt  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_clean_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator_, nullptr, CLSCTX_ALL,
                          IID_IMMDeviceEnumerator_, (void **)&app.enumerator);
    if (FAILED(hr) || !app.enumerator)
    {
        fprintf(stderr, "Cannot create IMMDeviceEnumerator: 0x%08lx\n",
                (unsigned long)hr);
        cleanup_all();
        if (mtx) CloseHandle(mtx);
        return 1;
    }

    fprintf(stdout, "%s (Windows/WASAPI port)\n", OUR_APP_NAME);
    fflush(stdout);
    print_all_outputs();
    fflush(stdout);

    /* discover Bluetooth buds (no streams yet — trims come first) */
    poll_devices(false);
    if (app.devices.empty())
    {
        fprintf(stderr,
            "No Bluetooth sinks found. Are the earbuds connected?\n"
            "  (check in Settings > Bluetooth & devices)\n");
        cleanup_all();
        if (mtx) CloseHandle(mtx);
        return 1;
    }

    /* apply per-device extra delay trims from argv: "index:ms" */
    for (auto &kv : delays)
        if (kv.first >= 0 && kv.first < (int)app.devices.size())
        {
            DeviceOut *dev = app.devices[kv.first];
            dev->extra_delay_ms = kv.second;
            dev->manual_trim    = true;
            app.trim_by_id[dev->id] = kv.second;
            fprintf(stdout, "[main] device %d gets +%.1f ms extra delay\n",
                    kv.first, kv.second);
        }

    /* pick the loopback source (may move the default off a bud) */
    if (!pick_source())
    {
        cleanup_all();
        if (mtx) CloseHandle(mtx);
        return 1;
    }

    /* capture the source's loopback — everything apps play */
    if (!start_capture())
    {
        cleanup_all();
        if (mtx) CloseHandle(mtx);
        return 1;
    }

    /* one playback stream per earbud */
    for (DeviceOut *dev : app.devices)
    {
        if (start_playback(dev))
        {
            fprintf(stdout, "[main] stream started for %s\n",
                    dev->name.c_str());
            unmute_device(dev);
        }
        else
        {
            fprintf(stderr, "failed to create stream for %s\n",
                    dev->name.c_str());
            dev->failed = true;
        }
    }

    fprintf(stdout,
            "\nSync running: sending default-output audio to %zu "
            "Bluetooth device(s). Ctrl+C to stop.\n\n",
            app.devices.size());
    fflush(stdout);

    main_loop();
    cleanup_all();

    if (mtx) CloseHandle(mtx);
    return rc;
}
