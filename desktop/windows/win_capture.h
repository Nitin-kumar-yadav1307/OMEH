/*
 * win_capture.h — loopback capture stream lifecycle.
 *
 * Single-TU module, part of pipewire_sync_win.cpp. Included AFTER the type
 * module. Owns stop_capture() and start_capture(), which set up the WASAPI
 * loopback (AUDCLNT_STREAMFLAGS_LOOPBACK) capture of the default output.
 */
#pragma once


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
