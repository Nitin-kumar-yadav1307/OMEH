#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <propkey.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <conio.h>
#include <string>
#include <vector>
#include <map>
#include <set>
static const GUID IID_IAudioClient_         = {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const GUID IID_IAudioRenderClient_   = {0xf294acfc,0x3146,0x4483,{0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2}};
static const GUID IID_IAudioCaptureClient_  = {0xc8adbd64,0xe71e,0x48a0,{0xa4,0xde,0x18,0x5c,0x39,0x5c,0xd3,0x17}};
static const GUID IID_IAudioEndpointVolume_ = {0x5cdf2c82,0x841e,0x4546,{0x97,0x22,0x0c,0xf7,0x40,0x78,0x22,0x9a}};
static const GUID IID_IMMDeviceEnumerator_  = {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID CLSID_MMDeviceEnumerator_ = {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const PROPERTYKEY PK_FriendlyName = {
    {0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14};
static const PROPERTYKEY PK_InstanceId = {
    {0x78c34fc8,0x104a,0x4aca,{0x9e,0xa4,0x52,0x4d,0x52,0x99,0x6e,0x57}}, 256};
static const PROPERTYKEY PK_EnumeratorName = {
    {0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 24};
static const PROPERTYKEY PK_DeviceInterface = {
    {0xb3f8fa53,0x0004,0x438e,{0x90,0x03,0x51,0xa4,0x6e,0x13,0x9b,0xfc}}, 2};
#ifndef AUDCLNT_STREAMFLAGS_LOOPBACK
#define AUDCLNT_STREAMFLAGS_LOOPBACK 0x00020000
#endif
#define RATE            48000
#define CHANNELS        2
#define FRAME_BYTES     (CHANNELS * sizeof(int16_t))
#define RING_BITS       15
#define RING_SIZE       (1u << RING_BITS)
#define RING_MASK       (RING_SIZE - 1)
#define DRIFT_TAU       10.0
#define DRIFT_LOOP_TAU  40.0
#define DRIFT_KP        (1.0 / ((double)RATE * DRIFT_LOOP_TAU))
#define DRIFT_MAX_PPM   1500.0
#define HARD_SYNC_FRAMES 2400.0
#define ALIGN_BASE_MS   10.0
#define ALIGN_MAX_MS    350.0
#define ALIGN_SMOOTH    0.3
#define ALIGN_NOISE_MS  3.0
#define MEASURE_TICKS   2
#define POLL_MS         2
#define HEALTH_MS       1000
#define XFADE_FRAMES    144
#define IDLE_GAP_MS     40
#define CLICK_BURSTS    4
#define CLICK_PERIOD    (RATE * 35 / 100)
#define CLICK_LEN       (RATE / 50)
#define CLICK_LEAD      (RATE / 10)
#define OUR_APP_NAME    "pipewire_sync"
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
static std::string narrow(const wchar_t *w)
{
    std::string s;
    if (!w) return s;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1)
    {
        s.resize((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
        s.resize((size_t)n - 1);
    }
    return s;
}
static bool prop_string(IMMDevice *d, const PROPERTYKEY &pk, std::string &out)
{
    IPropertyStore *ps = nullptr;
    if (!d || FAILED(d->OpenPropertyStore(0 , &ps)) || !ps)
        return false;
    PROPVARIANT pv;
    memset(&pv, 0, sizeof(pv));
    bool ok = false;
    if (SUCCEEDED(ps->GetValue(pk, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
    {
        out = narrow(pv.pwszVal);
        ok = true;
    }
    if (pv.vt == VT_LPWSTR && pv.pwszVal)
        CoTaskMemFree(pv.pwszVal);
    ps->Release();
    return ok;
}
static bool starts_bth(const std::string &s)
{
    return s.size() >= 3 &&
           tolower((unsigned char)s[0]) == 'b' &&
           tolower((unsigned char)s[1]) == 't' &&
           tolower((unsigned char)s[2]) == 'h';
}
static bool is_bluetooth(IMMDevice *d, std::string *instance_out)
{
    std::string s;
    if (prop_string(d, PK_InstanceId, s) && starts_bth(s))
    {
        if (instance_out) *instance_out = s;
        return true;
    }
    if (prop_string(d, PK_EnumeratorName, s) && starts_bth(s))
    {
        if (instance_out) *instance_out = s;
        return true;
    }
    if (prop_string(d, PK_DeviceInterface, s))
    {
        std::string low;
        low.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++)
            low.push_back((char)tolower((unsigned char)s[i]));
        if (low.find("bth") != std::string::npos)
        {
            if (instance_out) *instance_out = s;
            return true;
        }
    }
    return false;
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
    for (int role = 0; role <= 2; role++)
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
        std::string nm, why, note;
        prop_string(d, PK_FriendlyName, nm);
        bool bt = is_bluetooth(d, &why);
        if (!why.empty()) note = "   (" + why + ")";
        fprintf(stdout, "[registry] output: \"%s\"%s%s\n",
                nm.c_str(), bt ? "   [Bluetooth]" : "", note.c_str());
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
    stop_capture();
    IMMDevice *d = nullptr;
    if (app.src_id.empty() ||
        FAILED(app.enumerator->GetDevice(app.src_id.c_str(), &d)) || !d)
    {
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
    g_synth_upto_ms = 0;
    app.idle_gap = false;
    fprintf(stdout, "[main] loopback capture started on \"%s\" (%.0f Hz)\n",
            app.src_name.c_str(), app.cap_rate);
    return true;
}
struct SrcVol
{
    IAudioEndpointVolume *vol = nullptr;
    bool  have     = false;
    BOOL  prev_mute = FALSE;
    float prev_vol  = 1.0f;
    bool  silenced  = false;
};
static SrcVol g_srcvol;
static bool   g_silent_source = true;
static int    g_run_seconds   = 0;
static IAudioClient       *g_tt_client = nullptr;
static IAudioRenderClient *g_tt_render = nullptr;
static UINT32              g_tt_buf    = 0;
static double              g_tt_phase  = 0.0;
static const double        TT_HZ       = 440.0;
static const double        TT_AMP      = 0.10;
static bool fmt_is_float(const WAVEFORMATEX *f)
{
    if (!f) return false;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        return ((const WAVEFORMATEXTENSIBLE *)f)->SubFormat.Data1 == 3;
    return false;
}
static bool source_vol_open()
{
    if (g_srcvol.vol) return true;
    if (!app.src_endpoint) return false;
    if (FAILED(app.src_endpoint->Activate(IID_IAudioEndpointVolume_, CLSCTX_ALL,
                                          nullptr, (void **)&g_srcvol.vol)) ||
        !g_srcvol.vol)
    {
        g_srcvol.vol = nullptr;
        return false;
    }
    return true;
}
static void source_vol_save()
{
    if (!source_vol_open() || g_srcvol.have) return;
    g_srcvol.vol->GetMute(&g_srcvol.prev_mute);
    g_srcvol.vol->GetMasterVolumeLevelScalar(&g_srcvol.prev_vol);
    g_srcvol.have = true;
}
static void source_silence()
{
    if (!g_silent_source) return;
    source_vol_save();
    if (!g_srcvol.vol || g_srcvol.silenced) return;
    if (SUCCEEDED(g_srcvol.vol->SetMute(TRUE, nullptr)))
    {
        g_srcvol.silenced = true;
        fprintf(stdout, "[source] muted \"%s\" — no speaker audio, "
                        "loopback capture continues\n", app.src_name.c_str());
        fflush(stdout);
    }
}
static void source_restore()
{
    if (!g_srcvol.vol) return;
    if (g_srcvol.have)
    {
        g_srcvol.vol->SetMute(g_srcvol.prev_mute, nullptr);
        g_srcvol.vol->SetMasterVolumeLevelScalar(g_srcvol.prev_vol, nullptr);
        fprintf(stdout, "[source] \"%s\" mute/volume restored (%.2f, mute=%d)\n",
                app.src_name.c_str(), g_srcvol.prev_vol,
                (int)g_srcvol.prev_mute);
        fflush(stdout);
    }
    g_srcvol.vol->Release();
    g_srcvol.vol      = nullptr;
    g_srcvol.have     = false;
    g_srcvol.silenced = false;
}
static void tone_test_close()
{
    if (g_tt_client) g_tt_client->Stop();
    if (g_tt_render) { g_tt_render->Release(); g_tt_render = nullptr; }
    if (g_tt_client) { g_tt_client->Release(); g_tt_client = nullptr; }
    g_tt_buf = 0;
}
static bool tone_test_open()
{
    if (g_tt_render) return true;
    if (!app.src_endpoint || !app.cap_mix) return false;
    if (FAILED(app.src_endpoint->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr,
                                          (void **)&g_tt_client)) || !g_tt_client)
        return false;
    if (FAILED(g_tt_client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0,
                                       app.cap_mix, nullptr)))
    {
        tone_test_close();
        return false;
    }
    g_tt_client->GetBufferSize(&g_tt_buf);
    g_tt_client->GetService(IID_IAudioRenderClient_, (void **)&g_tt_render);
    if (!g_tt_render) { tone_test_close(); return false; }
    g_tt_client->Start();
    return true;
}
static void tone_test_feed()
{
    if (!g_tt_render || !g_tt_client || !app.cap_mix) return;
    UINT32 pad = 0;
    if (FAILED(g_tt_client->GetCurrentPadding(&pad))) return;
    UINT32 avail = g_tt_buf > pad ? g_tt_buf - pad : 0;
    if (!avail) return;
    BYTE *dst = nullptr;
    if (FAILED(g_tt_render->GetBuffer(avail, &dst)) || !dst) return;
    WORD   ch  = app.cap_mix->nChannels ? app.cap_mix->nChannels : 1;
    double fs  = (double)app.cap_mix->nSamplesPerSec;
    bool   f32 = fmt_is_float(app.cap_mix);
    for (UINT32 f = 0; f < avail; f++)
    {
        float s = (float)(sin(g_tt_phase * 6.283185307179586) * TT_AMP);
        g_tt_phase += TT_HZ / fs;
        if (g_tt_phase >= 1.0) g_tt_phase -= 1.0;
        for (WORD c = 0; c < ch; c++)
        {
            if (f32) ((float *)dst)[(size_t)f * ch + c] = s;
            else     ((int16_t *)dst)[(size_t)f * ch + c] =
                         (int16_t)(s * 32767.0f);
        }
    }
    g_tt_render->ReleaseBuffer(avail, 0);
}
static double tone_test_run(int ms)
{
    if (!app.cap || !app.cap_mix) return 0.0;
    double s1 = 0.0, s2 = 0.0;
    unsigned long long n = 0;
    double fs    = (double)app.cap_mix->nSamplesPerSec;
    double coeff = 2.0 * cos(6.283185307179586 * TT_HZ / fs);
    WORD   ch    = app.cap_mix->nChannels ? app.cap_mix->nChannels : 1;
    bool   f32   = fmt_is_float(app.cap_mix);
    double scale = f32 ? 1.0 : 1.0 / 32768.0;
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < (DWORD)ms)
    {
        tone_test_feed();
        for (;;)
        {
            UINT32 pkt = 0;
            if (FAILED(app.cap->GetNextPacketSize(&pkt)) || pkt == 0) break;
            BYTE *data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(app.cap->GetBuffer(&data, &frames, &flags,
                                          nullptr, nullptr)))
                break;
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data)
            {
                for (UINT32 f = 0; f < frames; f++)
                {
                    double x = f32
                        ? (double)((const float *)data)[(size_t)f * ch]
                        : (double)((const int16_t *)data)[(size_t)f * ch];
                    x *= scale;
                    double s0 = x + coeff * s1 - s2;
                    s2 = s1; s1 = s0;
                    n++;
                }
            }
            app.cap->ReleaseBuffer(frames);
        }
        Sleep(5);
    }
    double mag2 = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    if (mag2 < 0.0) mag2 = 0.0;
    return n ? 2.0 * sqrt(mag2) / (double)n : 0.0;
}
static int source_silence_self_test()
{
    if (!g_silent_source) return 0;
    if (!tone_test_open()) { tone_test_close(); return -1; }
    source_vol_save();
    if (!g_srcvol.vol) { tone_test_close(); return -1; }
    g_srcvol.vol->SetMute(FALSE, nullptr);
    Sleep(150);
    double a = tone_test_run(450);
    g_srcvol.vol->SetMute(TRUE, nullptr);
    Sleep(150);
    double b = tone_test_run(450);
    g_srcvol.vol->SetMute(FALSE, nullptr);
    tone_test_close();
    fprintf(stdout, "[source] mute self-test: our tone came back at %.4f "
                    "unmuted / %.4f muted\n", a, b);
    if (a < 0.005)
    {
        fprintf(stdout, "[source] self-test inconclusive — leaving the "
                        "source MUTED;\n"
                        "         if the buds stay silent, start it with "
                        "--audible-source\n");
        fflush(stdout);
        return -1;
    }
    if (b > 0.25 * a)
    {
        fprintf(stdout, "[source] this device's mute does not affect the "
                        "loopback tap ->\n"
                        "         it will be muted while we stream, so the "
                        "room stays quiet\n");
        fflush(stdout);
        return 1;
    }
    fprintf(stdout, "[source] muting this device WOULD kill the loopback "
                    "capture on this system\n"
                    "         -> staying audible: the speakers keep playing, "
                    "Windows cannot\n"
                    "            give us a silent source here. The tone you "
                    "just heard was this test.\n");
    fflush(stdout);
    return 0;
}
static void print_endpoint_details()
{
    std::wstring def_id;
    std::string  def_name;
    get_default_id(def_id, def_name);
    IMMDeviceCollection *col = nullptr;
    if (FAILED(app.enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                                  &col)) || !col)
        return;
    UINT n = 0;
    col->GetCount(&n);
    fprintf(stdout, "\n[list] %u active output endpoint(s)\n", n);
    for (UINT i = 0; i < n; i++)
    {
        IMMDevice *d = nullptr;
        if (FAILED(col->Item(i, &d)) || !d) continue;
        std::string nm = "(unnamed)", why;
        prop_string(d, PK_FriendlyName, nm);
        bool bt = is_bluetooth(d, &why);
        std::wstring id = id_of(d);
        std::string fmt = "(unknown)";
        IAudioClient *ac = nullptr;
        if (SUCCEEDED(d->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr,
                                  (void **)&ac)) && ac)
        {
            WAVEFORMATEX *mx = nullptr;
            if (SUCCEEDED(ac->GetMixFormat(&mx)) && mx)
            {
                char b[160];
                snprintf(b, sizeof(b), "%.0f Hz / %u ch / %s %u-bit",
                         (double)mx->nSamplesPerSec,
                         (unsigned)mx->nChannels,
                         fmt_is_float(mx) ? "float" : "int",
                         (unsigned)mx->wBitsPerSample);
                fmt = b;
                CoTaskMemFree(mx);
            }
            ac->Release();
        }
        fprintf(stdout, "[list] #%u %-42s %-26s  %s%s%s\n",
                i, nm.c_str(), fmt.c_str(),
                bt ? "[synced to]  " : "[captured]   ",
                bt ? why.c_str() : "non-Bluetooth",
                (!def_id.empty() && id == def_id) ? "   <- default" : "");
        fprintf(stdout, "[list]      id: %s\n", narrow(id.c_str()).c_str());
        d->Release();
    }
    col->Release();
    fprintf(stdout,
            "\n[list] the tool streams the captured audio to every \"synced to\"\n"
            "       endpoint and captures the first non-Bluetooth \"captured\"\n"
            "       one (moving the default there if an earbud is the default).\n\n");
    fflush(stdout);
}
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
        is_float = (e->SubFormat.Data1 == 3);
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
static void write_to_rings(const int16_t *samples, uint32_t frames)
{
    for (DeviceOut *dev : app.devices)
        if (!dev->gone && !dev->measuring)
        {
            dev->master_off = (int64_t)dev->ring.write
                            - (int64_t)app.master_written;
            dev->ring.write_frames(samples, frames);
        }
    app.master_written += frames;
}
static void drain_capture()
{
    if (!app.cap) return;
    static std::vector<float> f32;
    static std::vector<int16_t> pop16;
    static int16_t zeros[2400 * CHANNELS] = {};
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
    uint64_t now = GetTickCount64();
    if (g_synth_upto_ms == 0) g_synth_upto_ms = now;
    if (got_any)
    {
        app.idle_gap     = false;
        g_synth_upto_ms  = now;
    }
    else
    {
        if (!app.idle_gap)
        {
            app.capture_empties++;
            app.idle_gap = true;
        }
        uint64_t covered = now > IDLE_GAP_MS ? now - IDLE_GAP_MS : 0;
        if (covered > g_synth_upto_ms)
        {
            uint64_t elapsed = covered - g_synth_upto_ms;
            if (elapsed > 50) elapsed = 50;
            uint32_t frames = (uint32_t)(elapsed * RATE / 1000);
            if (frames > 2400) frames = 2400;
            if (frames)
                write_to_rings(zeros, frames);
            g_synth_upto_ms = covered;
        }
    }
}
static double default_target_level(const DeviceOut *dev)
{
    double ms = ALIGN_BASE_MS + dev->extra_delay_ms;
    if (ms < 0.0)          ms = 0.0;
    if (ms > ALIGN_MAX_MS) ms = ALIGN_MAX_MS;
    return ms * (double)RATE / 1000.0;
}
static void apply_delay_shift(DeviceOut *dev, double delta_ms, bool remember)
{
    if (!dev || delta_ms == 0.0) return;
    double was = dev->extra_delay_ms;
    double now = was + delta_ms;
    if (now < -ALIGN_MAX_MS) now = -ALIGN_MAX_MS;
    if (now >  ALIGN_MAX_MS) now =  ALIGN_MAX_MS;
    if (now == was) return;
    double old_target = dev->target_level;
    dev->extra_delay_ms = now;
    dev->target_level   = default_target_level(dev);
    if (dev->started && dev->client && !dev->gone)
    {
        double diff = dev->target_level - old_target;
        if (diff > 0.5)
        {
            static int16_t zeros[2048 * CHANNELS] = {};
            size_t n = (size_t)(diff + 0.5);
            while (n)
            {
                size_t k = n > 2048 ? 2048 : n;
                dev->ring.write_frames(zeros, k);
                n -= k;
            }
        }
        else if (diff < -0.5)
        {
            size_t k = (size_t)(-diff + 0.5);
            if (k > dev->ring.level()) k = dev->ring.level();
            dev->ring.advance(k);
            dev->phase = (double)dev->ring.read;
        }
        if (dev->avg_level >= 0.0)
            dev->avg_level += (dev->target_level - old_target);
    }
    if (remember)
    {
        dev->manual_trim      = true;
        app.trim_by_id[dev->id] = now;
        trim_save();
        fprintf(stdout, "[tune] %s trim %+.0f ms -> %+.0f ms  (buffer %.0f ms)\n",
                dev->name.c_str(), was, now,
                1000.0 * dev->target_level / (double)RATE);
    }
    else
    {
        fprintf(stdout, "[align] %s buffer %.0f -> %.0f ms\n",
                dev->name.c_str(),
                1000.0 * old_target / (double)RATE,
                1000.0 * dev->target_level / (double)RATE);
    }
    fflush(stdout);
}
static void shift_device_delay(DeviceOut *dev, double delta_ms)
{
    apply_delay_shift(dev, delta_ms, true);
}
static void retarget_late_device(DeviceOut *dev)
{
    double ref = app.ref_latency_ms > 0.0 ? app.ref_latency_ms : ALIGN_BASE_MS;
    double add = 0.0;
    if (dev->dev_latency_ms > ref + ALIGN_NOISE_MS)
    {
        double delta = dev->dev_latency_ms - ref;
        for (DeviceOut *o : app.devices)
            if (o != dev && !o->gone && !o->failed)
                apply_delay_shift(o, delta, false);
        fprintf(stdout, "[align] %s is %.1f ms slower than the reference"
                        " - shifted the others\n", dev->name.c_str(), delta);
        app.ref_latency_ms = dev->dev_latency_ms;
    }
    else
    {
        add = ref - dev->dev_latency_ms;
        if (add < 0.0)          add = 0.0;
        if (add > ALIGN_MAX_MS) add = ALIGN_MAX_MS;
        if (dev->manual_trim)   add = 0.0;
    }
    double ms = ALIGN_BASE_MS + add + dev->extra_delay_ms;
    if (ms < 0.0)          ms = 0.0;
    if (ms > ALIGN_MAX_MS) ms = ALIGN_MAX_MS;
    dev->target_level = ms * (double)RATE / 1000.0;
    dev->avg_quantum  = dev->meas_q_count
        ? dev->meas_q_sum / (double)dev->meas_q_count : 0.0;
    dev->measuring = false;
    dev->started   = false;
    dev->ratio     = 1.0;
    dev->avg_level = -1.0;
    dev->audible_logged = false;
    fprintf(stdout, "[align] %-26s joined: measured=%6.1f ms  added=%5.1f ms"
                    "  buffer=%5.1f ms\n", dev->name.c_str(),
            dev->dev_latency_ms, add + dev->extra_delay_ms, ms);
    fflush(stdout);
}
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
static void teardown_device(DeviceOut *dev)
{
    if (dev->client)  dev->client->Stop();
    if (dev->render) { dev->render->Release(); dev->render = nullptr; }
    if (dev->client) { dev->client->Release(); dev->client = nullptr; }
    if (dev->mix_fmt){ CoTaskMemFree(dev->mix_fmt); dev->mix_fmt = nullptr; }
    dev->use_mix = false;
    dev->mix_rate = 0.0;
    dev->buffer_frames = 0;
    dev->mix_push_acc = 0.0;
    dev->out_rs.reset(1.0);
}
static double device_delay_ms(DeviceOut *dev)
{
    if (!dev->client)
        return dev->dev_latency_ms;
    UINT32 padding = 0;
    if (FAILED(dev->client->GetCurrentPadding(&padding)))
        return dev->dev_latency_ms;
    double stream_rate = dev->use_mix && dev->mix_rate > 0.0
                       ? dev->mix_rate : (double)RATE;
    return (double)padding * 1000.0 / stream_rate;
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
    teardown_device(dev);
    HRESULT hr = dev->endpoint->Activate(IID_IAudioClient_, CLSCTX_ALL,
                                         nullptr, (void **)&dev->client);
    if (FAILED(hr) || !dev->client)
    {
        fprintf(stderr, "[main] %s: Activate(IAudioClient) failed 0x%08lx\n",
                dev->name.c_str(), (unsigned long)hr);
        return false;
    }
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
    hr = dev->client->Start();
    if (!app.aligning)
    {
        dev->measuring    = true;
        dev->meas_ema     = 0.0;
        dev->meas_min_ms  = 0.0;
        dev->meas_max_ms  = 0.0;
        dev->meas_count   = 0;
        dev->meas_frames  = 0;
        dev->meas_q_sum   = 0.0;
        dev->meas_q_count = 0;
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
        dev->mix_push_acc += (double)avail * (double)RATE / dev->mix_rate;
        need = (UINT32)dev->mix_push_acc;
        dev->mix_push_acc -= (double)need;
        if (need == 0 && avail > 0) need = 1;
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
        size_t scratch_samples = (size_t)need * CHANNELS;
        if (dev->scratch.size() < scratch_samples)
            dev->scratch.resize(scratch_samples);
        out = dev->scratch.data();
    }
    UINT32 frames = need;
    if (dev->measuring)
    {
        memset(out, 0, (size_t)frames * FRAME_BYTES);
        dev->meas_q_sum += frames;
        dev->meas_q_count++;
        dev->meas_frames += frames;
        if (dev->meas_frames >= RATE / 4)
        {
            dev->meas_frames -= RATE / 4;
            double ms = device_delay_ms(dev);
            if (ms > 0.0)
            {
                dev->meas_ema = dev->meas_count
                              ? dev->meas_ema + ALIGN_SMOOTH * (ms - dev->meas_ema)
                              : ms;
                if (!dev->meas_count || ms < dev->meas_min_ms)
                    dev->meas_min_ms = ms;
                if (ms > dev->meas_max_ms)
                    dev->meas_max_ms = ms;
                dev->meas_count++;
                dev->dev_latency_ms = dev->meas_ema;
            }
        }
        if (!app.aligning && dev->meas_count >= MEASURE_TICKS)
            retarget_late_device(dev);
        goto commit;
    }
    if (!dev->started)
    {
        if ((double)dev->ring.level() < dev->target_level)
        {
            memset(out, 0, (size_t)frames * FRAME_BYTES);
            goto commit;
        }
        dev->ring.flush_to((size_t)dev->target_level);
        dev->started = true;
        dev->phase   = (double)dev->ring.read;
        dev->written = 0;
        dev->ratio   = 1.0;
    }
    if (dev->ring.level() == 0)
    {
        memset(out, 0, (size_t)frames * FRAME_BYTES);
        dev->underruns++;
        goto commit;
    }
    if (!dev->audible_logged)
    {
        dev->audible_logged = true;
        fprintf(stderr, "[audio] %-28s first audio handed to device "
                "(ring=%zu target=%.0f master=%llu)\n",
                dev->name.c_str(), dev->ring.level(), dev->target_level,
                (unsigned long long)app.master_written);
        fflush(stderr);
    }
    {
        double raw_level = (double)dev->ring.level();
        double dt_s  = (double)frames / (double)RATE;
        double alpha = 1.0 - exp(-dt_s / DRIFT_TAU);
        if (dev->avg_level < 0.0)
            dev->avg_level = raw_level;
        else
            dev->avg_level += (raw_level - dev->avg_level) * alpha;
        double lerr = dev->avg_level - dev->target_level;
        dev->last_drift_frames = lerr;
        if (fabs(raw_level - dev->target_level) > HARD_SYNC_FRAMES)
        {
            dev->ring.flush_to((size_t)dev->target_level);
            dev->phase      = (double)dev->ring.read;
            dev->avg_level  = (double)dev->ring.level();
            dev->ratio      = 1.0;
            lerr            = 0.0;
            dev->hard_syncs++;
            dev->xfade_left = XFADE_FRAMES;
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
            if (dev->xfade_left > 0)
            {
                double g = 1.0 - (double)dev->xfade_left / (double)XFADE_FRAMES;
                for (int c = 0; c < CHANNELS; c++)
                    s[c] = (int16_t)((double)dev->xfade_last[c] * g +
                                     (double)s[c] * (1.0 - g));
                dev->xfade_left--;
            }
            else
            {
                for (int c = 0; c < CHANNELS; c++)
                    dev->xfade_last[c] = s[c];
            }
            if (g_click_from)
            {
                int64_t pos = (int64_t)dev->phase + dev->master_off;
                int64_t rel = pos - (int64_t)g_click_from;
                if (rel >= 0 && rel < (int64_t)CLICK_BURSTS * CLICK_PERIOD)
                {
                    int64_t off = rel % CLICK_PERIOD;
                    if (off < (int64_t)CLICK_LEN)
                    {
                        double env = 1.0;
                        if (off < 48) env = (double)off / 48.0;
                        if (off > (int64_t)CLICK_LEN - 48)
                            env = (double)((int64_t)CLICK_LEN - off) / 48.0;
                        int16_t clk = (int16_t)(sin(6.283185307179586 *
                                        1000.0 * (double)off / (double)RATE) *
                                        env * 12000.0);
                        s[0] = clk;
                        s[1] = clk;
                    }
                }
            }
            out[i * CHANNELS + 0] = s[0];
            out[i * CHANNELS + 1] = s[1];
            dev->phase += dev->ratio;
            dev->written++;
        }
        double lo = (double)dev->ring.read;
        double hi = (dev->ring.write ? (double)dev->ring.write - 1.0 : 0.0)
                    + dev->ratio;
        if (dev->phase < lo) dev->phase = lo;
        if (dev->phase > hi) dev->phase = hi;
        dev->ring.advance((size_t)(dev->phase - (double)dev->ring.read));
        dev->phase = (double)dev->ring.read;
    }
commit:
    if (dev->use_mix)
    {
        hr = dev->render->GetBuffer(avail, &devbuf);
        if (FAILED(hr) || !devbuf)
        {
            fail_device(dev, FAILED(hr) ? hr : E_POINTER);
            return;
        }
        size_t scratch_frames = frames > avail ? frames : avail;
        if (dev->out_f32.size() < scratch_frames * 2)
            dev->out_f32.resize(scratch_frames * 2);
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
        if (made < avail)
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
static void finish_alignment()
{
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
        if (align_ms < ALIGN_NOISE_MS) align_ms = 0.0;
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
        dev->started      = false;
        dev->ratio        = 1.0;
        dev->avg_level    = -1.0;
        fprintf(stdout,
                "[align] %-28s measured=%6.1f ms (%.0f-%.0f)  added=%5.1f ms  "
                "buffer=%5.1f ms (quantum %.0f fr)\n",
                dev->name.c_str(), dev->dev_latency_ms,
                dev->meas_min_ms, dev->meas_max_ms,
                align_ms + dev->extra_delay_ms,
                1000.0 * dev->target_level / RATE, dev->avg_quantum);
    }
    fprintf(stdout, "[align] reference latency %.1f ms - "
                    "alignment locked, drift correction active\n",
            app.ref_latency_ms);
    fflush(stdout);
    app.aligning = false;
}
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
            found[id] = d;
        }
        else
            d->Release();
    }
    col->Release();
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
            dev->meas_ema = 0.0; dev->meas_min_ms = 0.0; dev->meas_max_ms = 0.0;
            dev->meas_count = 0; dev->meas_frames = 0;
            dev->meas_q_sum = 0.0; dev->meas_q_count = 0;
            dev->avg_level = -1.0; dev->ratio = 1.0;
            dev->audible_logged = false;
            dev->ring = Ring();
            auto it = app.trim_by_id.find(dev->id);
            if (it != app.trim_by_id.end())
            {
                dev->extra_delay_ms = it->second;
                dev->manual_trim    = true;
            }
        }
    }
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
            {
                dev->extra_delay_ms = it->second;
                dev->manual_trim    = true;
            }
            fprintf(stdout, "[registry] found Bluetooth sink: %s\n",
                    dev->name.c_str());
            fflush(stdout);
        }
        else if (!dev->gone)
        {
            kv.second->Release();
            continue;
        }
        else
        {
            fprintf(stdout, "[registry] device back: %s\n", dev->name.c_str());
            fflush(stdout);
            dev->gone = false;
        }
        if (dev->endpoint) dev->endpoint->Release();
        dev->endpoint = kv.second;
        if (!autostart)
            continue;
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
static DeviceOut *selected_device()
{
    if (app.devices.empty()) return nullptr;
    if (g_selected < 0 || g_selected >= (int)app.devices.size())
        g_selected = 0;
    return app.devices[(size_t)g_selected];
}
static std::string trim_file_path()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::string p = narrow(buf);
    size_t slash = p.find_last_of("\\/");
    if (slash != std::string::npos) p.resize(slash + 1);
    return p + "pipewire_sync_win.trims";
}
static void trim_load()
{
    FILE *f = fopen(trim_file_path().c_str(), "rb");
    if (!f) return;
    char line[1024];
    int loaded = 0;
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        std::string id = line;
        double ms = atof(tab + 1);
        if (!id.empty()) { app.trim_by_id[id] = ms; loaded++; }
    }
    fclose(f);
    if (loaded)
        fprintf(stdout, "[main] loaded %d remembered trim(s)\n", loaded);
    fflush(stdout);
}
static void trim_save()
{
    if (app.trim_by_id.empty()) return;
    FILE *f = fopen(trim_file_path().c_str(), "wb");
    if (!f) return;
    fprintf(f, "# pipewire_sync_win per-device trims: device id<TAB>ms\n");
    for (auto &kv : app.trim_by_id)
        fprintf(f, "%s\t%.1f\n", kv.first.c_str(), kv.second);
    fclose(f);
}
static void click_trigger()
{
    g_click_from = app.master_written + CLICK_LEAD;
    if (!g_click_from) g_click_from = 1;
    fprintf(stdout, "\n[tune] click test: %d clicks %d ms apart — trim the "
                    "selected bud\n"
                    "       until both buds give you ONE click\n",
            CLICK_BURSTS, (int)(1000u * CLICK_PERIOD / RATE));
    fflush(stdout);
}
static void poll_keyboard()
{
    while (_kbhit())
    {
        int c = _getch();
        if (c == 0 || c == 224) { _getch(); continue; }
        DeviceOut *dev = nullptr;
        switch (c)
        {
        case '\t':
            if (!app.devices.empty())
            {
                g_selected = (g_selected + 1) % (int)app.devices.size();
                dev = selected_device();
                fprintf(stdout, "\n[tune] selected dev %d: %s (trim %+.0f ms)\n",
                        g_selected, dev ? dev->name.c_str() : "-",
                        dev ? dev->extra_delay_ms : 0.0);
                fflush(stdout);
            }
            break;
        case ']': case '+': shift_device_delay(selected_device(),  +5.0); break;
        case '[': case '-': shift_device_delay(selected_device(),  -5.0); break;
        case '.': case '>': shift_device_delay(selected_device(),  +1.0); break;
        case ',': case '<': shift_device_delay(selected_device(),  -1.0); break;
        case 't': case 'T': click_trigger(); break;
        case 'h': case '?': print_usage(); break;
        case 'm': case 'M':
            dev = selected_device();
            if (dev && dev->endpoint)
            {
                IAudioEndpointVolume *v = nullptr;
                if (SUCCEEDED(dev->endpoint->Activate(
                        IID_IAudioEndpointVolume_, CLSCTX_ALL, nullptr,
                        (void **)&v)) && v)
                {
                    BOOL m = FALSE;
                    v->GetMute(&m);
                    v->SetMute(m ? FALSE : TRUE, nullptr);
                    fprintf(stdout, "\n[tune] %s %s\n", dev->name.c_str(),
                            m ? "unmuted" : "muted");
                    fflush(stdout);
                    v->Release();
                }
            }
            break;
        case 'q': case 'Q': case 3:
            fprintf(stdout, "\n[tune] quit requested\n");
            fflush(stdout);
            InterlockedExchange(&g_stop, 1);
            break;
        default:
            break;
        }
    }
}
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
static void main_loop()
{
    uint64_t t0 = GetTickCount64();
    app.last_health_ms = t0;
    while (!g_stop)
    {
        if (g_run_seconds > 0 &&
            GetTickCount64() - t0 >= (uint64_t)g_run_seconds * 1000ULL)
        {
            fprintf(stdout, "\n[main] --seconds %d reached - stopping\n",
                    g_run_seconds);
            fflush(stdout);
            break;
        }
        drain_capture();
        poll_keyboard();
        if (g_click_from &&
            app.master_written > g_click_from +
                                (uint64_t)CLICK_BURSTS * CLICK_PERIOD + 4800)
            g_click_from = 0;
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
static void cleanup_all()
{
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
    trim_save();
    source_restore();
    tone_test_close();
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
    if (g_clean_evt) SetEvent(g_clean_evt);
    CoUninitialize();
}
struct Flags
{
    std::map<int, double> delays;
    bool list           = false;
    bool help           = false;
    bool audible_source = false;
    int  seconds        = 0;
};
static Flags parse_args(int argc, char *argv[])
{
    Flags f;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--audible-source") { f.audible_source = true; continue; }
        if (a == "--list" || a == "-l") { f.list = true; continue; }
        if (a == "--help" || a == "-h") { f.help = true; continue; }
        if (a == "--seconds" && i + 1 < argc) { f.seconds = atoi(argv[++i]); continue; }
        const char *colon = strchr(argv[i], ':');
        if (!colon) continue;
        f.delays[atoi(argv[i])] = atof(colon + 1);
    }
    return f;
}
static void print_usage()
{
    fprintf(stdout,
        "\nUsage: pipewire_sync_win.exe [device:ms ...] [options]\n\n"
        "Sends the machine's default-output audio to EVERY connected\n"
        "Bluetooth earbud, measuring and correcting the latency and clock\n"
        "difference between them.\n\n"
        "Options:\n"
        "  --audible-source   do not mute the source output (Speakers keep\n"
        "                     playing what the buds get).  Default: the\n"
        "                     source is muted while streaming, after a\n"
        "                     self-test that proves capture survives it.\n"
        "  --list, -l         list the output endpoints and exit\n"
        "  --seconds N        stop cleanly after N seconds (quick test)\n"
        "  -h, --help         this text\n"
        "  <index>:<ms>       per-device extra delay, e.g. 1:25\n\n"
        "Keys while running:\n"
        "  Tab                select the next bud (marked * in the status)\n"
        "  ]  [               selected bud  +5 ms / -5 ms\n"
        "  .  ,               selected bud  +1 ms / -1 ms\n"
        "  t                  alignment click test (4 clicks, both buds)\n"
        "  m                  mute / unmute the selected bud\n"
        "  h  ?               print this key help again\n"
        "  q                  quit (Ctrl+C works too)\n\n"
        "Trims are remembered per device id in pipewire_sync_win.trims next\n"
        "to the exe, so a tuned pair starts correct on the next run.\n\n");
    fflush(stdout);
}
int main(int argc, char *argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    Flags flags = parse_args(argc, argv);
    if (flags.help)
    {
        print_usage();
        return 0;
    }
    g_silent_source = !flags.audible_source;
    g_run_seconds   = flags.seconds;
    int rc = 0;
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
    if (flags.list)
    {
        print_endpoint_details();
        cleanup_all();
        if (mtx) CloseHandle(mtx);
        return 0;
    }
    trim_load();
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
    for (auto &kv : flags.delays)
        if (kv.first >= 0 && kv.first < (int)app.devices.size())
        {
            DeviceOut *dev = app.devices[kv.first];
            dev->extra_delay_ms = kv.second;
            dev->manual_trim    = true;
            app.trim_by_id[dev->id] = kv.second;
            fprintf(stdout, "[main] device %d gets +%.1f ms extra delay\n",
                    kv.first, kv.second);
        }
    if (!pick_source())
    {
        cleanup_all();
        if (mtx) CloseHandle(mtx);
        return 1;
    }
    if (!start_capture())
    {
        cleanup_all();
        if (mtx) CloseHandle(mtx);
        return 1;
    }
    if (g_silent_source && source_silence_self_test() == 0)
        g_silent_source = false;
    if (g_silent_source)
        source_silence();
    if (g_run_seconds > 0)
        fprintf(stdout, "[main] will stop by itself after %d s\n",
                g_run_seconds);
    for (DeviceOut *dev : app.devices)
    {
        if (start_playback(dev))
        {
            fprintf(stdout, "[main] stream started for %s (%s, %.0f Hz)\n",
                dev->name.c_str(),
                dev->use_mix ? "mix format" : "48 kHz PCM",
                dev->use_mix ? dev->mix_rate : (double)RATE);
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
