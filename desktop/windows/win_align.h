/*
 * win_align.h — per-device delay/trim math + playback stream setup.
 *
 * Single-TU module, part of pipewire_sync_win.cpp. Included AFTER the type
 * and pipeline modules. Owns default_target_level(), apply_delay_shift() and
 * friends (latency trim that survives restarts), device teardown/fail
 * helpers, and start_playback().
 */
#pragma once


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
    // Any stream ever alive: health_tick may un-silence on total dropout,
    // but never before the first stream (else boot leaks speaker audio).
    app.had_stream = true;
    return true;
}
