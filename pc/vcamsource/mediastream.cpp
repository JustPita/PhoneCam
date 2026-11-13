#include "vcamsource.h"

#include <algorithm>

static HRESULT CreateVideoType(const GUID& subtype, uint32_t w, uint32_t h, uint32_t fps, IMFMediaType** out) {
    ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;
    const bool rgb = subtype == MFVideoFormat_RGB32;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, rgb ? w * 4 : w);
    t->SetUINT32(MF_MT_SAMPLE_SIZE, rgb ? w * h * 4 : w * h * 3 / 2);
    t->SetUINT32(MF_MT_AVG_BITRATE, UINT32(std::min<uint64_t>(uint64_t(w) * h * fps * (rgb ? 32 : 12), 0xFFFFFFFFull)));
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    *out = t.Detach();
    return S_OK;
}

MediaStream::MediaStream() { g_objectCount++; }

MediaStream::~MediaStream() {
    Shutdown();
    g_objectCount--;
}

ULONG MediaStream::Release() {
    LONG r = InterlockedDecrement(&m_ref);
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP MediaStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaStream) ||
        riid == __uuidof(IMFMediaStream2)) {
        *ppv = static_cast<IMFMediaStream2*>(this);
    } else if (riid == IID_PhoneCam_IKsControl) {
        *ppv = static_cast<IKsControl*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

HRESULT MediaStream::Initialize(MediaSource* source, const SourceFormat& fmt) {
    m_source = source;
    m_fmt = fmt;
    HRESULT hr = MFCreateAttributes(&m_attributes, 8);
    if (FAILED(hr)) return hr;
    m_attributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PhoneCam_PINNAME_VIDEO_CAPTURE);
    m_attributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    m_attributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    m_attributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);

    // Types proposés : taille native en NV12 d'abord, puis RGB32, puis des tailles réduites
    // au même rapport largeur/hauteur pour les applications qui veulent du 1080p ou 720p.
    std::vector<ComPtr<IMFMediaType>> types;
    auto add = [&](const GUID& sub, uint32_t w, uint32_t h) {
        ComPtr<IMFMediaType> t;
        if (SUCCEEDED(CreateVideoType(sub, w, h, fmt.fps, &t))) types.push_back(t);
    };
    add(MFVideoFormat_NV12, fmt.width, fmt.height);
    for (uint32_t w : {1920u, 1280u, 640u}) {
        if (w >= fmt.width) continue;
        uint32_t h = uint32_t(uint64_t(w) * fmt.height / fmt.width) & ~1u;
        add(MFVideoFormat_NV12, w, h);
    }
    add(MFVideoFormat_RGB32, fmt.width, fmt.height);
    for (uint32_t w : {1920u, 1280u}) {
        if (w >= fmt.width) continue;
        uint32_t h = uint32_t(uint64_t(w) * fmt.height / fmt.width) & ~1u;
        add(MFVideoFormat_RGB32, w, h);
    }

    std::vector<IMFMediaType*> raw;
    for (auto& t : types) raw.push_back(t.Get());
    hr = MFCreateStreamDescriptor(0, DWORD(raw.size()), raw.data(), &m_descriptor);
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaTypeHandler> handler;
    hr = m_descriptor->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;
    hr = handler->SetCurrentMediaType(raw[0]);
    if (FAILED(hr)) return hr;
    hr = m_attributes->CopyAllItems(m_descriptor.Get());
    if (FAILED(hr)) return hr;

    m_reader.Open(fmt.width, fmt.height, fmt.fps);
    return MFCreateEventQueue(&m_queue);
}

// --- IMFMediaEventGenerator ---------------------------------------------------
STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    return m_queue->BeginGetEvent(cb, state);
}
STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    return m_queue->EndGetEvent(r, e);
}
STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** e) {
    ComPtr<IMFMediaEventQueue> q;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        if (m_shutdown) return MF_E_SHUTDOWN;
        q = m_queue;
    }
    return q->GetEvent(flags, e); // bloquant : hors verrou
}
STDMETHODIMP MediaStream::QueueEvent(MediaEventType t, REFGUID g, HRESULT hr, const PROPVARIANT* v) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    return m_queue->QueueEventParamVar(t, g, hr, v);
}

// --- IMFMediaStream -----------------------------------------------------------
STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** src) {
    if (!src) return E_POINTER;
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown || !m_source) return MF_E_SHUTDOWN;
    return m_source.CopyTo(src);
}
STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** sd) {
    if (!sd) return E_POINTER;
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    return m_descriptor.CopyTo(sd);
}
STDMETHODIMP MediaStream::RequestSample(IUnknown* token) {
    {
        std::lock_guard<std::mutex> lk(m_lock);
        if (m_shutdown) return MF_E_SHUTDOWN;
        if (m_state != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;
        m_tokens.emplace_back(token);
    }
    m_cv.notify_one();
    return S_OK;
}

// --- IMFMediaStream2 ----------------------------------------------------------
STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    m_state = state;
    if (state != MF_STREAM_STATE_RUNNING) m_tokens.clear();
    return S_OK;
}
STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    *state = m_state;
    return S_OK;
}

// --- IKsControl : aucune propriété prise en charge ----------------------------
STDMETHODIMP MediaStream::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaStream::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaStream::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// --- Démarrage / arrêt --------------------------------------------------------
HRESULT MediaStream::Start(IMFMediaType* type) {
    StopWorker();
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;

    GUID sub{};
    type->GetGUID(MF_MT_SUBTYPE, &sub);
    UINT32 w = 0, h = 0, n = 0, d = 1;
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &n, &d);
    m_outW = w;
    m_outH = h;
    m_outFps = d ? std::max<UINT32>(1, n / d) : 30;
    m_rgb32 = sub == MFVideoFormat_RGB32;
    Log(L"Stream::Start %ux%u@%u %s", w, h, m_outFps, m_rgb32 ? L"RGB32" : L"NV12");

    m_allocatorReady = false;
    if (m_allocator) {
        HRESULT hr = m_allocator->InitializeSampleAllocator(10, type);
        m_allocatorReady = SUCCEEDED(hr);
        if (FAILED(hr)) Log(L"InitializeSampleAllocator: 0x%08X", hr);
    }

    m_state = MF_STREAM_STATE_RUNNING;
    m_workerStop = false;
    m_tokens.clear();
    m_worker = std::thread(&MediaStream::Worker, this);
    return S_OK;
}

void MediaStream::StopWorker() {
    {
        std::lock_guard<std::mutex> lk(m_lock);
        m_workerStop = true;
        m_tokens.clear();
    }
    m_cv.notify_all();
    if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id()) m_worker.join();
}

HRESULT MediaStream::Stop() {
    StopWorker();
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    m_state = MF_STREAM_STATE_STOPPED;
    if (m_allocator && m_allocatorReady) m_allocator->UninitializeSampleAllocator();
    m_allocatorReady = false;
    return S_OK;
}

void MediaStream::Shutdown() {
    StopWorker();
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return;
    m_shutdown = true;
    if (m_queue) m_queue->Shutdown();
    if (m_allocator && m_allocatorReady) m_allocator->UninitializeSampleAllocator();
    m_allocator.Reset();
    m_source.Reset();
    m_reader.Close();
}

// --- Production des images ----------------------------------------------------
void MediaStream::Worker() {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    LONGLONG nextDue = now.QuadPart;
    for (;;) {
        ComPtr<IUnknown> token;
        ComPtr<IMFMediaEventQueue> queue;
        uint32_t fps;
        {
            std::unique_lock<std::mutex> lk(m_lock);
            m_cv.wait(lk, [&] { return m_workerStop || !m_tokens.empty(); });
            if (m_workerStop) return;
            token = m_tokens.front();
            m_tokens.pop_front();
            queue = m_queue;
            fps = m_outFps;
        }
        // Cadence : on attend une nouvelle image du téléphone, au plus jusqu'à l'échéance suivante.
        // L'échéance est cumulée (et non « maintenant + période ») pour que les retards se compensent.
        const LONGLONG period = freq.QuadPart / std::max<uint32_t>(1, fps);
        nextDue += period;
        QueryPerformanceCounter(&now);
        if (nextDue < now.QuadPart - period) nextDue = now.QuadPart; // trop en retard : on repart
        // Flux du téléphone actif : on tolère une image en retard (gigue Wi-Fi) avant de répéter la précédente.
        const LONGLONG deadline = m_reader.Live() ? now.QuadPart + period * 3 : nextDue;
        if (m_reader.WaitNew(m_lastCounter, deadline)) {
            QueryPerformanceCounter(&now);
            nextDue = now.QuadPart; // image réelle : on se cale sur le téléphone
        }
        m_reader.MarkRead();

        ComPtr<IMFSample> sample;
        HRESULT hr = MakeSample(&sample);
        if (SUCCEEDED(hr)) {
            if (token) sample->SetUnknown(MFSampleExtension_Token, token.Get());
            queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
        } else {
            Log(L"MakeSample: 0x%08X", hr);
            queue->QueueEventParamVar(MEError, GUID_NULL, hr, nullptr);
        }
    }
}

HRESULT MediaStream::MakeSample(IMFSample** out) {
    const uint32_t w = m_outW, h = m_outH;
    const bool rgb = m_rgb32;
    ComPtr<IMFSample> sample;
    HRESULT hr;
    if (m_allocator && m_allocatorReady) {
        hr = m_allocator->AllocateSample(&sample);
        if (FAILED(hr)) return hr;
    } else {
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) return hr;
        ComPtr<IMFMediaBuffer> b;
        hr = MFCreate2DMediaBuffer(w, h, rgb ? 21 /* D3DFMT_A8R8G8B8 */ : MFVideoFormat_NV12.Data1, FALSE, &b);
        if (FAILED(hr)) return hr;
        sample->AddBuffer(b.Get());
    }

    ComPtr<IMFMediaBuffer> buffer;
    hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) return hr;

    auto fill = [&](BYTE* scan0, LONG pitch) {
        const uint32_t sw = m_reader.Width(), sh = m_reader.Height();
        bool ok = sw && m_reader.WithLatest(&m_lastCounter, [&](const uint8_t* src) {
            ConvertFrame(src, sw, sh, scan0, pitch, w, h, rgb);
        });
        if (!ok) {
            FillPlaceholder(scan0, pitch, w, h, rgb, GetTickCount64());
            // On mémorise le compteur pour que la prochaine demande attende une période complète.
            m_lastCounter = m_reader.Counter();
        }
    };

    ComPtr<IMF2DBuffer2> b2;
    if (SUCCEEDED(buffer.As(&b2))) {
        BYTE* scan0 = nullptr;
        BYTE* start = nullptr;
        LONG pitch = 0;
        DWORD len = 0;
        hr = b2->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &len);
        if (FAILED(hr)) return hr;
        fill(scan0, pitch);
        b2->Unlock2D();
    } else {
        BYTE* p = nullptr;
        DWORD maxLen = 0;
        hr = buffer->Lock(&p, &maxLen, nullptr);
        if (FAILED(hr)) return hr;
        DWORD need = rgb ? w * h * 4 : w * h * 3 / 2;
        if (maxLen >= need) fill(p, LONG(rgb ? w * 4 : w));
        buffer->Unlock();
        buffer->SetCurrentLength(need);
    }

    sample->SetSampleTime(MFGetSystemTime());
    sample->SetSampleDuration(10'000'000LL / std::max<uint32_t>(1, m_outFps));
    *out = sample.Detach();
    return S_OK;
}
