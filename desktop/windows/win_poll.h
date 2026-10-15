/*
 * win_poll.h — alignment finish, device polling, and interactive controls.
 *
 * Single-TU module, part of pipewire_sync_win.cpp. Included AFTER the type
 * and align modules. Owns finish_alignment(), poll_devices() (the 1 Hz device
 * scan/reconnect), the persistent trim file load/save, the click trigger, and
 * poll_keyboard() for the interactive hotkeys.
 */
#pragma once


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
