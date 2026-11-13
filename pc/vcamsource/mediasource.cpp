#include "vcamsource.h"

MediaSource::MediaSource() { g_objectCount++; }

MediaSource::~MediaSource() {
    if (m_stream) m_stream->Release();
    g_objectCount--;
}

ULONG MediaSource::Release() {
    LONG r = InterlockedDecrement(&m_ref);
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP MediaSource::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaSource) ||
        riid == __uuidof(IMFMediaSourceEx)) {
        *ppv = static_cast<IMFMediaSourceEx*>(this);
    } else if (riid == __uuidof(IMFGetService)) {
        *ppv = static_cast<IMFGetService*>(this);
    } else if (riid == IID_PhoneCam_IKsControl) {
        *ppv = static_cast<IKsControl*>(this);
    } else if (riid == __uuidof(IMFSampleAllocatorControl)) {
        *ppv = static_cast<IMFSampleAllocatorControl*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

HRESULT MediaSource::Initialize() {
    SourceFormat fmt = SourceFormat::Load();
    Log(L"MediaSource::Initialize %ux%u@%u", fmt.width, fmt.height, fmt.fps);

    HRESULT hr = MFCreateAttributes(&m_attributes, 4);
    if (FAILED(hr)) return hr;
    hr = MFCreateEventQueue(&m_queue);
    if (FAILED(hr)) return hr;

    m_stream = new (std::nothrow) MediaStream();
    if (!m_stream) return E_OUTOFMEMORY;
    hr = m_stream->Initialize(this, fmt);
    if (FAILED(hr)) return hr;

    ComPtr<IMFStreamDescriptor> sd;
    hr = m_stream->GetStreamDescriptor(&sd);
    if (FAILED(hr)) return hr;
    IMFStreamDescriptor* sds[] = {sd.Get()};
    hr = MFCreatePresentationDescriptor(1, sds, &m_pd);
    if (FAILED(hr)) return hr;
    return m_pd->SelectStream(0);
}

// --- IMFMediaEventGenerator ---------------------------------------------------
STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    return m_queue->BeginGetEvent(cb, state);
}
STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    return m_queue->EndGetEvent(r, e);
}
STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** e) {
    ComPtr<IMFMediaEventQueue> q;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        if (m_shutdown) return MF_E_SHUTDOWN;
        q = m_queue;
    }
    return q->GetEvent(flags, e);
}
STDMETHODIMP MediaSource::QueueEvent(MediaEventType t, REFGUID g, HRESULT hr, const PROPVARIANT* v) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    return m_queue->QueueEventParamVar(t, g, hr, v);
}

// --- IMFMediaSource -----------------------------------------------------------
STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** pd) {
    if (!pd) return E_POINTER;
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    return m_pd->Clone(pd);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* c) {
    if (!c) return E_POINTER;
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    *c = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::Pause() { return MF_E_INVALID_STATE_TRANSITION; }

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* start) {
    if (!pd || !start) return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;

    DWORD count = 0;
    HRESULT hr = pd->GetStreamDescriptorCount(&count);
    if (FAILED(hr)) return hr;
    for (DWORD i = 0; i < count; ++i) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        hr = pd->GetStreamDescriptorByIndex(i, &selected, &sd);
        if (FAILED(hr)) return hr;
        DWORD id = 0;
        sd->GetStreamIdentifier(&id);
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;

        const bool wasActive = m_stream->IsActive();
        if (selected) {
            ComPtr<IMFMediaTypeHandler> handler;
            ComPtr<IMFMediaType> type;
            hr = sd->GetMediaTypeHandler(&handler);
            if (SUCCEEDED(hr)) hr = handler->GetCurrentMediaType(&type);
            if (FAILED(hr)) return hr;
            hr = m_stream->Start(type.Get());
            if (FAILED(hr)) return hr;
            m_queue->QueueEventParamUnk(wasActive ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK,
                                        static_cast<IMFMediaStream2*>(m_stream));
            m_stream->QueueEvent(MEStreamStarted, GUID_NULL, S_OK, start);
        } else if (wasActive) {
            m_stream->Stop();
            m_stream->QueueEvent(MEStreamStopped, GUID_NULL, S_OK, nullptr);
        }
    }
    return m_queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, start);
}

STDMETHODIMP MediaSource::Stop() {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (m_stream->IsActive()) {
        m_stream->Stop();
        m_stream->QueueEvent(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }
    return m_queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

STDMETHODIMP MediaSource::Shutdown() {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    m_shutdown = true;
    Log(L"MediaSource::Shutdown");
    if (m_stream) m_stream->Shutdown();
    if (m_queue) m_queue->Shutdown();
    return S_OK;
}

// --- IMFMediaSourceEx ---------------------------------------------------------
STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** a) {
    if (!a) return E_POINTER;
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    return m_attributes.CopyTo(a);
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD id, IMFAttributes** a) {
    if (!a) return E_POINTER;
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    *a = m_stream->Attributes();
    (*a)->AddRef();
    return S_OK;
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown*) { return S_OK; }

// --- IMFGetService ------------------------------------------------------------
STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* ppv) {
    if (ppv) *ppv = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

// --- IKsControl ---------------------------------------------------------------
STDMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// --- IMFSampleAllocatorControl ------------------------------------------------
STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD streamId, IUnknown* allocator) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (streamId != 0) return MF_E_INVALIDSTREAMNUMBER;
    ComPtr<IMFVideoSampleAllocatorEx> a;
    if (allocator) allocator->QueryInterface(IID_PPV_ARGS(&a));
    m_stream->SetAllocator(a.Get());
    return S_OK;
}

STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) {
    if (!inputStreamId || !usage) return E_POINTER;
    if (streamId != 0) return MF_E_INVALIDSTREAMNUMBER;
    *inputStreamId = streamId;
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}
