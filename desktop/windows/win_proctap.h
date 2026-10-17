/*
 * win_proctap.h - per-process loopback tap: the probe that may give this
 * machine a quiet room after all.
 *
 * Single-TU module, part of pipewire_sync_win.cpp. Included AFTER win_source.h
 * (reuses the tone-test + source-volume helpers) and BEFORE win_pipeline.h
 * (it swaps the streams drain_capture() reads; the pipeline does not care
 * which tap filled app.cap).
 *
 * Why this exists: on some drivers - the Realtek HDA path is the observed
 * case - the system loopback tap (AUDCLNT_STREAMFLAGS_LOOPBACK) sits AFTER
 * endpoint mute/volume, so muting the speakers kills the captured stream and
 * source_silence_self_test() returns 0 ("stay audible": room noisy, buds OK).
 * Windows 10 build 20438+ offers a SECOND, differently-placed tap:
 * ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, ...)
 * with AUDIOCLIENT_ACTIVATION_PARAMS, filtered per process tree and not
 * tied to one endpoint. Whether that tap survives the mute is driver
 * specific and appears in no documentation - so we measure it exactly like
 * the system test: render the 440 Hz probe tone through an INCLUDE-ourselves
 * tap, read unmuted/muted/vol0 levels, and only if one survives does the
 * engine switch production capture to an EXCLUDE-ourselves tap and mute the
 * room for real. Every other outcome keeps the old behaviour byte-for-byte.
 *
 * Needs <audioclientactivationparams.h> (modern SDK) + mmdevapi.lib
 * (ActivateAudioInterfaceAsync, linked by the build files). Without the
 * header this module compiles to "unsupported" stubs, so older toolchains
 * still build.
 */
#pragma once


#if defined(__has_include) && __has_include(<audioclientactivationparams.h>)
#define SYNCWIN_PROCTAP 1
#include <audioclientactivationparams.h>
#else
#define SYNCWIN_PROCTAP 0
#endif

/* Fallbacks for an SDK that ships the params header but not these macros.
   If the path literal were ever wrong the activation simply fails and the
   probe falls back to the system tap - reported, never fatal. */
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif
#ifndef VT_BLOB
#define VT_BLOB 65
#endif

static bool g_proctap_allowed = true;   /* --no-process-tap clears this */
static bool g_proc_tap_active  = false; /* production runs on EXCLUDE tap */
enum PtMode { PT_INCLUDE = 0, PT_EXCLUDE = 1 };

#if SYNCWIN_PROCTAP
/* Close the capture streams but KEEP app.src_endpoint: mute control and the
   tone renderer still need it while the tap is being swapped. */
static void pt_close_streams()
{
    if (app.cap_client) app.cap_client->Stop();
    if (app.cap)       { app.cap->Release();        app.cap = nullptr; }
    if (app.cap_client){ app.cap_client->Release(); app.cap_client = nullptr; }
    if (app.cap_mix)   { CoTaskMemFree(app.cap_mix); app.cap_mix = nullptr; }
    app.cap_rate = 0.0;
}

/* Completion object for ActivateAudioInterfaceAsync. The API calls back on
   an MTA worker; startup is the only user, so instead of the Microsoft
   sample's MediaFoundation work-queue we hand it an event and block the
   (also MTA) startup thread on it - same apartment, so no deadlock. The
   callback may still arrive while the client mix format is being fetched,
   so it must outlive its local-scope user; the event serialises them.
   Refcounted because Windows may hold a reference after the callback: the
   caller releases the async operation BEFORE its own reference, and never
   frees a handler whose callback has not run (the timeout path leaks it on
   purpose for exactly that reason). */
struct PtHandler : public IActivateAudioInterfaceCompletionHandler
{
    LONG         m_ref    = 1;
    HANDLE       m_done   = nullptr;  /* manual-reset event, caller-owned */
    HRESULT      m_hr     = E_FAIL;
    IAudioClient *m_client = nullptr;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
        {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_ref);
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = (ULONG)InterlockedDecrement(&m_ref);
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation *op) override
    {
        HRESULT hr = E_UNEXPECTED, hrRes = E_UNEXPECTED;
        IUnknown *unk = nullptr;
        if (SUCCEEDED(hr = op->GetActivateResult(&hrRes, &unk)))
            hr = hrRes;
        if (SUCCEEDED(hr) && unk)
            hr = unk->QueryInterface(__uuidof(IAudioClient), (void **)&m_client);
        if (unk) unk->Release();
        if (FAILED(hr) && m_client) { m_client->Release(); m_client = nullptr; }
        m_hr = hr;
        if (m_done) SetEvent(m_done);
        return SUCCEEDED(hr) ? S_OK : hr;
    }
};

/* Swap the capture streams for a per-process tap. On failure the capture is
   left fully closed; every caller then reopens the system tap. */
static bool proc_tap_open(int mode, DWORD pid, const char *what)
{
    pt_close_streams();
    g_proc_tap_active = false;
    AUDIOCLIENT_ACTIVATION_PARAMS ap = {};
    ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.ProcessLoopbackMode =
        (mode == PT_INCLUDE) ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                             : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    ap.ProcessLoopbackParams.TargetProcessId = pid;
    PROPVARIANT pv = {};
    pv.vt             = VT_BLOB;
    pv.blob.cbSize    = sizeof(ap);
    pv.blob.pBlobData = (BYTE *)&ap;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return false;
    PtHandler *h = new PtHandler();
    h->m_done = done;
    IActivateAudioInterfaceAsyncOperation *op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
        &pv, h, &op);
    IAudioClient *client = nullptr;
    bool ok = false, timed_out = false;
    if (SUCCEEDED(hr))
    {
        if (WaitForSingleObject(done, 5000) != WAIT_OBJECT_0)
            timed_out = true;  /* callback may still arrive: leak h + event */
        else if (SUCCEEDED(h->m_hr))
        {
            client = h->m_client;
            h->m_client = nullptr;
            ok = true;
        }
        else hr = h->m_hr;
    }
    if (op) op->Release();     /* must precede the handler's Release() */
    if (timed_out)
        fprintf(stderr, "[capture] process-loopback activation timed out\n");
    else
    {
        h->Release();
        CloseHandle(done);
    }
    if (!ok)
    {
        fprintf(stderr, "[capture] process-loopback unavailable (%s): 0x%08lx\n",
                what, (unsigned long)hr);
        return false;
    }
    /* Same initialisation as the system loopback (win_capture.h): shared
       mode, LOOPBACK flag, the engine's own mix format - the pipeline and
       the tone tooling then treat this tap exactly like the old one. */
    WAVEFORMATEX *mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || !mix)
    {
        fprintf(stderr, "[capture] process-loopback GetMixFormat failed\n");
        client->Release();
        return false;
    }
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_LOOPBACK,
                            0, 0, mix, nullptr);
    if (FAILED(hr))
    {
        fprintf(stderr, "[capture] process-loopback Initialize failed: 0x%08lx\n",
                (unsigned long)hr);
        CoTaskMemFree(mix);
        client->Release();
        return false;
    }
    IAudioCaptureClient *cap = nullptr;
    if (FAILED(client->GetService(IID_IAudioCaptureClient_, (void **)&cap)) || !cap)
    {
        fprintf(stderr, "[capture] process-loopback GetService failed\n");
        CoTaskMemFree(mix);
        client->Release();
        return false;
    }
    client->Start();
    app.cap_client = client;
    app.cap        = cap;
    app.cap_mix    = mix;
    app.cap_rate   = mix->nSamplesPerSec;
    app.cap_rs.reset((double)app.cap_rate / (double)RATE);
    g_synth_upto_ms = 0;
    app.idle_gap    = false;
    g_proc_tap_active = (mode == PT_EXCLUDE);
    fprintf(stdout, "[main] per-process loopback capture started (%s, %.0f Hz)\n",
            what, app.cap_rate);
    fflush(stdout);
    return true;
}
#else /* !SYNCWIN_PROCTAP - older SDK: feature out, build in */
static bool proc_tap_open(int, DWORD, const char *) { return false; }
#endif /* SYNCWIN_PROCTAP */

/* Health-watchdog hook: start_capture() calls this first, so a restart of a
   session running on the EXCLUDE tap comes back as an EXCLUDE tap.
   Dropping to the system loopback while the source is muted would silence
   the buds - the one outcome worse than a noisy room. Returning false means
   "not my mode" (or "tap lost", after which g_proc_tap_active is cleared)
   and the normal system-tap path proceeds. */
static bool proc_tap_restart()
{
    if (!g_proc_tap_active) return false;
    stop_capture();
    if (!app.src_id.empty())
    {
        IMMDevice *d = nullptr;
        if (SUCCEEDED(app.enumerator->GetDevice(app.src_id.c_str(), &d)) && d)
            app.src_endpoint = d;
    }
    if (proc_tap_open(PT_EXCLUDE, GetCurrentProcessId(), "exclude self"))
        return true;
    g_proc_tap_active = false;
    fprintf(stderr,
            "[capture] per-process tap lost - falling back to system loopback\n");
    return false;
}

/* Production switch used by main() when the include-tap probe won: replace
   the probe tap with EXCLUDE-ourselves (everyone's audio but our own, so the
   engine's own renders - tone, clicks, the buds' copy - never re-enter the
   rings). Never leaves the session without a tap. */
static bool proc_tap_use_excluded()
{
    if (proc_tap_open(PT_EXCLUDE, GetCurrentProcessId(), "exclude self"))
        return true;
    start_capture();
    return false;
}

/* The experiment itself. Same contract as source_silence_self_test():
   1 = mute survives, 2 = volume-0 survives, -1 = not available/inconclusive.
   Every non-success path leaves the SYSTEM capture reopened and the endpoint
   at the user's exact previous state - the state we entered in - so main()'s
   fall-through keeps the old behaviour byte-for-byte. */
static int proc_tap_self_test()
{
    if (!g_proctap_allowed) return -1;
    g_capture_restart = proc_tap_restart;
#if !SYNCWIN_PROCTAP
    fprintf(stdout, "[source] per-process tap not built in (old Windows SDK)"
                    " -> staying audible (unchanged behaviour)\n");
    fflush(stdout);
    return -1;
#else
    fprintf(stdout, "[source] system tap dies under mute - probing the"
                    " per-process tap...\n");
    fflush(stdout);
    if (!tone_test_open()) { tone_test_close(); return -1; }
    source_vol_save();
    if (!g_srcvol.vol) { tone_test_close(); return -1; }
    if (!proc_tap_open(PT_INCLUDE, GetCurrentProcessId(), "include self"))
    {
        start_capture();   /* back on the system tap, exactly as before */
        tone_test_close();
        fprintf(stdout, "[source] per-process tap unavailable on this Windows"
                        " -> staying audible (unchanged behaviour)\n");
        fflush(stdout);
        return -1;
    }
    /* The same three probes as source_silence_self_test(), now reading the
       include tap. Start unmuted at a probe-safe level so "a" can exist at
       all (the user's saved volume may legitimately be 0). */
    float probe_vol = g_srcvol.prev_vol < 0.05f ? 0.5f : g_srcvol.prev_vol;
    g_srcvol.vol->SetMute(FALSE, nullptr);
    g_srcvol.vol->SetMasterVolumeLevelScalar(probe_vol, nullptr);
    Sleep(150);
    double a = tone_test_run(450);
    g_srcvol.vol->SetMute(TRUE, nullptr);
    Sleep(150);
    double b = tone_test_run(450);
    g_srcvol.vol->SetMute(FALSE, nullptr);
    g_srcvol.vol->SetMasterVolumeLevelScalar(0.0f, nullptr);
    double d = tone_test_run(450);
    /* Hand the user's exact state back: source_silence() applies on top of
       it when this probe wins, and the old stay-audible path needs it
       untouched. */
    g_srcvol.vol->SetMasterVolumeLevelScalar(g_srcvol.prev_vol, nullptr);
    g_srcvol.vol->SetMute(g_srcvol.prev_mute, nullptr);
    tone_test_close();
    fprintf(stdout, "[source] process-tap self-test: our tone came back at "
                    "%.4f unmuted / %.4f muted / %.4f vol0\n", a, b, d);
    fflush(stdout);
    if (a >= 0.005 && b > 0.25 * a)
    {
        fprintf(stdout, "[source] the per-process tap ignores this device's "
                        "mute ->\n"
                        "         switching capture to it; the source will be "
                        "muted while we stream:\n"
                        "         room quiet, buds keep the audio\n");
        fflush(stdout);
        g_silence_mode = SILENCE_MUTE;
        return 1;
    }
    if (a >= 0.005 && d > 0.25 * a)
    {
        fprintf(stdout, "[source] the per-process tap survives volume-0 "
                        "(mute would kill it) ->\n"
                        "         switching capture to it; source volume sits "
                        "at 0 while we stream\n");
        fflush(stdout);
        g_silence_mode = SILENCE_VOL0;
        return 2;
    }
    if (a < 0.005)
        fprintf(stdout, "[source] our own tone never reached the per-process "
                        "tap (inconclusive)\n");
    else
        fprintf(stdout, "[source] the per-process tap ALSO dies when this "
                        "device is muted\n");
    fprintf(stdout, "         -> staying audible (unchanged behaviour)\n");
    fflush(stdout);
    pt_close_streams();
    start_capture();   /* reopen the system tap */
    return -1;
#endif
}