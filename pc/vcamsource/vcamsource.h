#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <ks.h>
#include <dmksctrl.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "../common/shared_frame.h"

using Microsoft::WRL::ComPtr;

extern std::atomic<long> g_objectCount;

// {28F54685-06FD-11D2-B27A-00A0C9223196}
static const IID IID_PhoneCam_IKsControl = {0x28f54685, 0x06fd, 0x11d2, {0xb2, 0x7a, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}};
// PINNAME_VIDEO_CAPTURE {FB6C4281-0353-11d1-905F-0000C0CC16BA}
static const GUID PhoneCam_PINNAME_VIDEO_CAPTURE = {0xfb6c4281, 0x0353, 0x11d1, {0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba}};

void Log(const wchar_t* fmt, ...);

// ---------------------------------------------------------------------------
// Lecture des images publiées par PhoneCam.exe
// ---------------------------------------------------------------------------
class FrameReader {
public:
    ~FrameReader() { Close(); }
    bool Open(uint32_t width, uint32_t height, uint32_t fps);
    void Close();
    // Attend une image plus récente que lastCounter, au plus jusqu'à deadline (QueryPerformanceCounter).
    // Renvoie true si une nouvelle image est arrivée.
    bool WaitNew(LONG64 lastCounter, LONGLONG deadline);
    // Appelle fn(srcNV12) sur la dernière image si elle a moins d'une seconde. Renvoie false sinon.
    template <class F> bool WithLatest(LONG64* counter, F&& fn);
    LONG64 Counter() const { return m_header ? m_header->frameCounter : 0; }
    // Le téléphone envoie des images en ce moment.
    bool Live() const { return m_header && GetTickCount64() - (ULONGLONG)m_header->lastWriteTick < 1000; }
    // Signale à l'application qu'une application regarde la caméra.
    void MarkRead() {
        if (m_header) InterlockedExchange64(&m_header->lastReadTick, LONG64(GetTickCount64()));
    }
    uint32_t Width() const { return m_header ? m_header->width : 0; }
    uint32_t Height() const { return m_header ? m_header->height : 0; }

private:
    HANDLE m_mapping = nullptr;
    HANDLE m_event = nullptr;
    PhoneCamHeader* m_header = nullptr;
};

template <class F> bool FrameReader::WithLatest(LONG64* counter, F&& fn) {
    if (!m_header) return false;
    if (GetTickCount64() - (ULONGLONG)m_header->lastWriteTick > 1000) return false;
    // L'écrivain n'écrit jamais dans le tampon marqué « reading » ni dans « latest ».
    // On marque le tampon, puis on vérifie qu'il est toujours le plus récent : sinon l'écrivain
    // a pu le choisir comme cible juste avant, et on recommence.
    LONG idx = -1;
    for (int attempt = 0; attempt < 8; ++attempt) {
        idx = m_header->latest;
        if (idx < 0 || idx >= PHONECAM_BUFFERS) return false;
        InterlockedExchange(&m_header->reading, idx);
        if (InterlockedCompareExchange(&m_header->latest, idx, idx) == idx) break;
        idx = -1;
    }
    if (idx < 0) {
        InterlockedExchange(&m_header->reading, -1);
        return false;
    }
    *counter = m_header->frameCounter;
    fn(PhoneCamFrame(m_header, idx));
    InterlockedExchange(&m_header->reading, -1);
    return true;
}

// ---------------------------------------------------------------------------
// Format annoncé + conversions
// ---------------------------------------------------------------------------
struct SourceFormat {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 30;
    static SourceFormat Load();
};

// Copie/redimensionne une image NV12 (src w×h, pas = w) vers NV12 ou RGB32 (dst dw×dh, pas dstPitch).
void ConvertFrame(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, LONG dstPitch, uint32_t dw, uint32_t dh,
                  bool rgb32);
void FillPlaceholder(uint8_t* dst, LONG dstPitch, uint32_t dw, uint32_t dh, bool rgb32, uint64_t tick);

// ---------------------------------------------------------------------------
// Base COM qui délègue IMFAttributes à un objet interne
// ---------------------------------------------------------------------------
template <class Base> class AttributesImpl : public Base {
protected:
    ComPtr<IMFAttributes> m_attr;

public:
    // clang-format off
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return m_attr->GetItem(k, v); }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return m_attr->GetItemType(k, t); }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return m_attr->CompareItem(k, v, r); }
    STDMETHODIMP Compare(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return m_attr->Compare(a, m, r); }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return m_attr->GetUINT32(k, v); }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return m_attr->GetUINT64(k, v); }
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return m_attr->GetDouble(k, v); }
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return m_attr->GetGUID(k, v); }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* l) override { return m_attr->GetStringLength(k, l); }
    STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 n, UINT32* l) override { return m_attr->GetString(k, s, n, l); }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* l) override { return m_attr->GetAllocatedString(k, s, l); }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return m_attr->GetBlobSize(k, n); }
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* r) override { return m_attr->GetBlob(k, b, n, r); }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override { return m_attr->GetAllocatedBlob(k, b, n); }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID i, LPVOID* p) override { return m_attr->GetUnknown(k, i, p); }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return m_attr->SetItem(k, v); }
    STDMETHODIMP DeleteItem(REFGUID k) override { return m_attr->DeleteItem(k); }
    STDMETHODIMP DeleteAllItems() override { return m_attr->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return m_attr->SetUINT32(k, v); }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return m_attr->SetUINT64(k, v); }
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return m_attr->SetDouble(k, v); }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return m_attr->SetGUID(k, v); }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return m_attr->SetString(k, v); }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return m_attr->SetBlob(k, b, n); }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return m_attr->SetUnknown(k, u); }
    STDMETHODIMP LockStore() override { return m_attr->LockStore(); }
    STDMETHODIMP UnlockStore() override { return m_attr->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* n) override { return m_attr->GetCount(n); }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return m_attr->GetItemByIndex(i, k, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes* d) override { return m_attr->CopyAllItems(d); }
    // clang-format on
};

class MediaSource;

// ---------------------------------------------------------------------------
// Flux vidéo unique
// ---------------------------------------------------------------------------
class MediaStream final : public IMFMediaStream2, public IKsControl {
public:
    MediaStream();
    HRESULT Initialize(MediaSource* source, const SourceFormat& fmt);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** e) override;
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID g, HRESULT hr, const PROPVARIANT* v) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** src) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** sd) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override;
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override;
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override;

    HRESULT Start(IMFMediaType* type);
    HRESULT Stop();
    void Shutdown();
    bool IsActive() const { return m_state == MF_STREAM_STATE_RUNNING; }
    IMFAttributes* Attributes() { return m_attributes.Get(); }
    void SetAllocator(IMFVideoSampleAllocatorEx* a) { m_allocator = a; }

private:
    ~MediaStream();
    void Worker();
    HRESULT MakeSample(IMFSample** out);
    void StopWorker();

    LONG m_ref = 1;
    std::mutex m_lock;
    ComPtr<IMFMediaEventQueue> m_queue;
    ComPtr<IMFStreamDescriptor> m_descriptor;
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<IMFMediaSource> m_source; // référence forte, coupée par Shutdown()
    ComPtr<IMFVideoSampleAllocatorEx> m_allocator;
    bool m_allocatorReady = false;
    MF_STREAM_STATE m_state = MF_STREAM_STATE_STOPPED;
    bool m_shutdown = false;

    // Type de sortie en cours
    uint32_t m_outW = 0, m_outH = 0, m_outFps = 30;
    bool m_rgb32 = false;

    SourceFormat m_fmt;
    FrameReader m_reader;
    LONG64 m_lastCounter = -1;

    std::thread m_worker;
    std::condition_variable m_cv;
    std::deque<ComPtr<IUnknown>> m_tokens; // nullptr autorisé
    bool m_workerStop = false;
};

// ---------------------------------------------------------------------------
// Source
// ---------------------------------------------------------------------------
class MediaSource final : public IMFMediaSourceEx,
                          public IMFGetService,
                          public IKsControl,
                          public IMFSampleAllocatorControl {
public:
    MediaSource();
    HRESULT Initialize();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** e) override;
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID g, HRESULT hr, const PROPVARIANT* v) override;

    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override;
    STDMETHODIMP GetCharacteristics(DWORD* c) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* start) override;
    STDMETHODIMP Stop() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** a) override;
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** a) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* ppv) override;

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override;
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override;
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD streamId, IUnknown* allocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) override;

private:
    ~MediaSource();
    HRESULT CheckShutdown() const { return m_shutdown ? MF_E_SHUTDOWN : S_OK; }

    LONG m_ref = 1;
    std::mutex m_lock;
    bool m_shutdown = false;
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<IMFMediaEventQueue> m_queue;
    ComPtr<IMFPresentationDescriptor> m_pd;
    MediaStream* m_stream = nullptr; // une référence détenue
};

// ---------------------------------------------------------------------------
// Objet d'activation créé par CoCreateInstance(CLSID_PhoneCamSource)
// ---------------------------------------------------------------------------
class Activator final : public AttributesImpl<IMFActivate> {
public:
    Activator();
    HRESULT Initialize() { return MFCreateAttributes(&m_attr, 4); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

private:
    ~Activator() { g_objectCount--; }
    LONG m_ref = 1;
    ComPtr<IMFMediaSource> m_source;
};
