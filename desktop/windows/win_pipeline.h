/*
 * win_pipeline.h — packet decode + the per-device render feed.
 *
 * Single-TU module, part of pipewire_sync_win.cpp. Included AFTER the type
 * module. Owns packet_to_f32() (WASAPI buffer -> float), write_to_rings(),
 * and drain_capture() which pulls captured frames and fans them out to every
 * device ring.
 */
#pragma once


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
