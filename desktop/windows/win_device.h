/*
 * win_device.h — device enumeration, identification, and default-device control.
 *
 * Single-TU module, part of pipewire_sync_win.cpp. Included AFTER win_config.h
 * and win_types.h. Owns wchar/narrow helpers, Bluetooth detection, the default
 * output-device switch (IPolicyCfg), printing all outputs, and pick_source().
 */
#pragma once


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
