// camtest : ouvre la webcam « PhoneCam » comme le ferait une application et lit quelques images.
// Usage : camtest [nombre d'images]
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;
static const DWORD kVideo = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

int wmain(int argc, wchar_t** argv) {
    int frames = argc > 1 ? _wtoi(argv[1]) : 60;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 1);
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 n = 0;
    MFEnumDeviceSources(a.Get(), &devs, &n);
    ComPtr<IMFActivate> cam;
    wprintf(L"%u caméra(s) :\n", n);
    for (UINT32 i = 0; i < n; ++i) {
        wchar_t* name = nullptr;
        UINT32 len = 0;
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        wprintf(L"  - %s\n", name);
        // Windows ajoute « (Caméra virtuelle Windows) » au nom.
        if (name && std::wstring(name).rfind(L"PhoneCam", 0) == 0) cam = devs[i];
        CoTaskMemFree(name);
    }
    if (!cam) {
        wprintf(L"PhoneCam absente : PhoneCam.exe doit être lancé.\n");
        return 1;
    }

    ComPtr<IMFMediaSource> src;
    HRESULT hr = cam->ActivateObject(IID_PPV_ARGS(&src));
    if (FAILED(hr)) {
        wprintf(L"ActivateObject : 0x%08X\n", unsigned(hr));
        return 2;
    }
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(src.Get(), nullptr, &reader);
    if (FAILED(hr)) {
        wprintf(L"SourceReader : 0x%08X\n", unsigned(hr));
        return 3;
    }
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(reader->GetNativeMediaType(kVideo, i, &t))) break;
        GUID sub;
        UINT32 w, h, num, den;
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
        wprintf(L"  type %u : %s %ux%u @ %u i/s\n", i, sub == MFVideoFormat_NV12 ? L"NV12" : L"RGB32", w, h, num / (den ? den : 1));
    }

    ULONGLONG start = GetTickCount64();
    LONGLONG firstTs = -1, lastTs = 0;
    LARGE_INTEGER freq, prev{}, now;
    QueryPerformanceFrequency(&freq);
    double maxGap = 0, sumGap = 0;
    int late = 0, gaps = 0;
    UINT32 fn = 30, fd = 1;
    {
        ComPtr<IMFMediaType> cur;
        if (SUCCEEDED(reader->GetCurrentMediaType(kVideo, &cur))) MFGetAttributeRatio(cur.Get(), MF_MT_FRAME_RATE, &fn, &fd);
    }
    const double periodMs = 1000.0 * fd / (fn ? fn : 30);
    for (int i = 0; i < frames; ++i) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        hr = reader->ReadSample(kVideo, 0, nullptr, &flags, &ts, &s);
        if (FAILED(hr)) {
            wprintf(L"ReadSample : 0x%08X\n", unsigned(hr));
            return 4;
        }
        if (!s) continue;
        QueryPerformanceCounter(&now);
        if (prev.QuadPart && i > 5) { // on ignore le démarrage
            double gap = (now.QuadPart - prev.QuadPart) * 1000.0 / freq.QuadPart;
            maxGap = gap > maxGap ? gap : maxGap;
            sumGap += gap;
            ++gaps;
            if (gap > periodMs * 1.5) ++late;
        }
        prev = now;
        if (firstTs < 0) firstTs = ts;
        lastTs = ts;
        if (i == 0 || i == frames - 1) {
            ComPtr<IMFMediaBuffer> b;
            s->ConvertToContiguousBuffer(&b);
            BYTE* p;
            DWORD len;
            b->Lock(&p, nullptr, &len);
            wprintf(L"  image %d : %u octets, premier pixel Y=%u\n", i, len, p[0]);
            b->Unlock();
        }
    }
    double secs = (GetTickCount64() - start) / 1000.0;
    wprintf(L"%d images en %.2f s = %.1f i/s\n", frames, secs, frames / secs);
    if (gaps)
        wprintf(L"Écart entre images : moyen %.1f ms, max %.1f ms (période %.1f ms) ; %d écarts > 1,5 période\n",
                sumGap / gaps, maxGap, periodMs, late);
    src->Shutdown();
    MFShutdown();
    return 0;
}
