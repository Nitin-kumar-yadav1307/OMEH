/*
 * diag_props.cpp — diagnostic helper for pipewire_sync_win.cpp
 *
 * Dumps EVERY property Windows exposes on every active audio output
 * endpoint. We use it to find which property identifies Bluetooth
 * devices (the BTH check in pipewire_sync_win.cpp failed on this PC:
 * the buds were listed as outputs but not classified as Bluetooth).
 *
 * Build (MSYS2, same style as before):
 *   /ucrt64/bin/g++ -O2 -o diag_props.exe diag_props.cpp -lole32
 * Run:
 *   ./diag_props.exe > diag_out.txt
 *
 * Share diag_out.txt — the fmtid/pid + value of each property tells
 * us exactly which key to use in is_bluetooth().
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <propkey.h>
#include <cstdio>
#include <string>

/* explicit GUIDs (mingw g++ friendly, same values as in the main program) */
static const GUID CLSID_MMDeviceEnumerator_ = {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const GUID IID_IMMDeviceEnumerator_   = {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};

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

/* GUID -> "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" (narrow, printf-safe) */
static void fmt_guid(const GUID &g, char *out, size_t cap)
{
    snprintf(out, cap,
             "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             (unsigned long)g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
             g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

int main()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        printf("CoInitializeEx failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    IMMDeviceEnumerator *enm = nullptr;
    hr = CoCreateInstance(CLSID_MMDeviceEnumerator_, nullptr, CLSCTX_ALL,
                          IID_IMMDeviceEnumerator_, (void **)&enm);
    if (FAILED(hr) || !enm)
    {
        printf("CoCreateInstance failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    IMMDeviceCollection *col = nullptr;
    enm->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    if (!col)
    {
        printf("EnumAudioEndpoints failed\n");
        return 1;
    }

    UINT count = 0;
    col->GetCount(&count);
    printf("active render endpoints: %u\n\n", count);

    for (UINT i = 0; i < count; i++)
    {
        IMMDevice *d = nullptr;
        if (FAILED(col->Item(i, &d)) || !d) continue;

        LPWSTR idw = nullptr;
        d->GetId(&idw);
        printf("=== output %u ===\n", i);
        printf("endpoint id : %s\n", idw ? narrow(idw).c_str() : "?");
        if (idw) CoTaskMemFree(idw);

        IPropertyStore *ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(0 /* STGM_READ */, &ps)) && ps)
        {
            DWORD n = 0;
            ps->GetCount(&n);
            printf("properties  : %lu\n", (unsigned long)n);
            for (DWORD k = 0; k < n; k++)
            {
                PROPERTYKEY key;
                if (FAILED(ps->GetAt(k, &key))) continue;

                char guidstr[64] = {};
                fmt_guid(key.fmtid, guidstr, sizeof(guidstr));

                PROPVARIANT pv;
                memset(&pv, 0, sizeof(pv));
                if (FAILED(ps->GetValue(key, &pv))) continue;

                printf("  {%s} pid=%-4u vt=%-3u  ",
                       guidstr, (unsigned)key.pid, (unsigned)pv.vt);

                switch (pv.vt)
                {
                case VT_LPWSTR:
                    printf("LPWSTR  \"%s\"\n",
                           pv.pwszVal ? narrow(pv.pwszVal).c_str() : "");
                    if (pv.pwszVal) CoTaskMemFree(pv.pwszVal);
                    break;
                case VT_LPSTR:
                    printf("LPSTR   \"%s\"\n", pv.pszVal ? pv.pszVal : "");
                    if (pv.pszVal) CoTaskMemFree(pv.pszVal);
                    break;
                case VT_UI4:  printf("UI4     %lu\n", pv.ulVal);  break;
                case VT_I4:   printf("I4      %ld\n", pv.lVal);   break;
                case VT_BOOL: printf("BOOL    %s\n", pv.boolVal ? "true" : "false"); break;
                case VT_CLSID:
                {
                    char g2[64] = {};
                    if (pv.puuid) fmt_guid(*pv.puuid, g2, sizeof(g2));
                    printf("GUID    %s\n", g2);
                    if (pv.puuid) CoTaskMemFree(pv.puuid);
                    break;
                }
                default:
                    printf("(value not printed)\n");
                    break;
                }
            }
            ps->Release();
        }
        d->Release();
        printf("\n");
    }

    col->Release();
    enm->Release();
    CoUninitialize();
    return 0;
}
