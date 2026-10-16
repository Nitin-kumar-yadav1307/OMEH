/*
 * win_source.h — source volume control, silencing, and tone-test tooling.
 *
 * Single-TU module, part of pipewire_sync_win.cpp. Included AFTER the type
 * module. Owns the source-endpoint volume save/restore, source_silence(), the
 * 440 Hz tone-test synth (used to detect a silent capture path), and the
 * endpoint-detail dump.
 */
#pragma once


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
/* How source_silence() keeps the room quiet. MUTE is preferred; VOL0 is the
   fallback for drivers where muting the endpoint also kills the loopback tap
   (self-test returns 2 in that case). Both are restored by source_restore(). */
enum SilenceMode { SILENCE_MUTE = 0, SILENCE_VOL0 = 1 };
static SilenceMode g_silence_mode = SILENCE_MUTE;
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
    if (g_silence_mode == SILENCE_VOL0)
    {
        // Driver kills loopback on mute (self-test == 2): scalar 0 keeps the
        // tap alive on most drivers while the room stays quiet. Keep mute
        // OFF so the self-test result stays valid for this session.
        if (SUCCEEDED(g_srcvol.vol->SetMute(FALSE, nullptr)) &&
            SUCCEEDED(g_srcvol.vol->SetMasterVolumeLevelScalar(0.0f, nullptr)))
        {
            g_srcvol.silenced = true;
            fprintf(stdout, "[source] volume 0 on \"%s\" — no speaker audio, "
                            "loopback capture continues\n", app.src_name.c_str());
            fflush(stdout);
        }
        return;
    }
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
    // VOL0 fallback probe: scalar 0 with mute OFF. Some drivers gate the
    // loopback tap on mute but NOT on the volume scalar, so this is the
    // last quiet option before giving up and staying audible.
    g_srcvol.vol->SetMasterVolumeLevelScalar(0.0f, nullptr);
    double d = tone_test_run(450);
    // Put the user's level back (not 1.0): the probes already saved the
    // original, and staying-audible mode must leave the mixer untouched.
    g_srcvol.vol->SetMasterVolumeLevelScalar(g_srcvol.prev_vol, nullptr);
    tone_test_close();
    fprintf(stdout, "[source] mute self-test: our tone came back at %.4f "
                    "unmuted / %.4f muted / %.4f vol0\n", a, b, d);
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
        g_silence_mode = SILENCE_MUTE;
        return 1;
    }
    // Mute kills the tap (b ~= 0). Try scalar-0: quiet iff d survives at a
    // usable fraction of the unmuted level AND the tap was alive at all.
    if (d > 0.25 * a)
    {
        fprintf(stdout, "[source] muting this device kills the loopback tap, "
                        "but volume-0 does not ->\n"
                        "         source volume will sit at 0 while we stream, "
                        "so the room stays quiet\n");
        fflush(stdout);
        g_silence_mode = SILENCE_VOL0;
        return 2;
    }
    fprintf(stdout, "[source] muting this device WOULD kill the loopback "
                    "capture on this system\n"
                    "         -> staying audible: the speakers keep playing, "
                    "Windows cannot\n"
                    "            give us a silent source here. The tone you "
                    "just heard was this test.\n");
    fflush(stdout);
    // Probes left the endpoint unmuted; hand the user's original mute back
    // since nothing will be applied on top of it.
    g_srcvol.vol->SetMute(g_srcvol.prev_mute, nullptr);
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
