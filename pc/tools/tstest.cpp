// tstest : lit le flux réseau « OBS » (MPEG-TS sur HTTP) avec le lecteur intégré de Windows
// et vérifie qu'il se décode. Usage : tstest http://IP:38510/ [images]
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdio>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"Usage : tstest http://IP:38510/ [images]\n");
        return 1;
    }
    const int frames = argc > 2 ? _wtoi(argv[2]) : 90;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 2);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    attr->SetUINT32(MF_LOW_LATENCY, TRUE);
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(argv[1], attr.Get(), &reader);
    if (FAILED(hr)) {
        wprintf(L"Ouverture impossible : 0x%08X\n", unsigned(hr));
        return 2;
    }
    const DWORD video = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFMediaType> native;
    if (SUCCEEDED(reader->GetNativeMediaType(video, 0, &native))) {
        GUID sub{};
        UINT32 w = 0, h = 0;
        native->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
        wprintf(L"Flux : %s %ux%u\n", sub == MFVideoFormat_HEVC ? L"HEVC" : sub == MFVideoFormat_H264 ? L"H.264" : L"?", w, h);
    }
    // Demande du NV12 : force le décodage, pas seulement le démultiplexage.
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    hr = reader->SetCurrentMediaType(video, nullptr, out.Get());
    if (FAILED(hr)) wprintf(L"Pas de décodeur pour ce flux (0x%08X)\n", unsigned(hr));

    ULONGLONG start = GetTickCount64();
    LONGLONG first = -1, last = 0;
    int got = 0;
    while (got < frames) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        hr = reader->ReadSample(video, 0, nullptr, &flags, &ts, &s);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            wprintf(L"Lecture interrompue : 0x%08X flags=0x%X\n", unsigned(hr), flags);
            break;
        }
        if (!s) continue;
        if (first < 0) first = ts;
        last = ts;
        ++got;
    }
    double secs = (GetTickCount64() - start) / 1000.0;
    wprintf(L"%d images décodées en %.2f s ; horodatages sur %.2f s\n", got, secs, (last - first) / 1e7);
    MFShutdown();
    return got == frames ? 0 : 3;
}
