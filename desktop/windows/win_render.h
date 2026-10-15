/*
 * win_render.h — the real-time render feed (drift resampler + alignment).
 *
 * Single-TU module, part of pipewire_sync_win.cpp. Included AFTER the align
 * module. Owns fill_render(), which fills a device's WASAPI render buffer:
 * silence during alignment, the depth-driven drift resampler, cross-fades on
 * jumps, and the AIMD ring-depth controller.
 */
#pragma once


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
