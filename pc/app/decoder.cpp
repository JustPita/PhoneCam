#include "decoder.h"

#include <mferror.h>

static const GUID& SubtypeFor(const std::string& codec) {
    return codec == "hevc" ? MFVideoFormat_HEVC : MFVideoFormat_H264;
}

static ComPtr<IMFTransform> CreateDecoderMft(const std::string& codec) {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, SubtypeFor(codec)};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    // MFT synchrones uniquement : le décodeur Microsoft fait du DXVA dès qu'on lui donne un device D3D11.
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                           MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, nullptr,
                           &acts, &count);
    ComPtr<IMFTransform> mft;
    if (SUCCEEDED(hr)) {
        for (UINT32 i = 0; i < count && !mft; ++i) {
            if (FAILED(acts[i]->ActivateObject(IID_PPV_ARGS(&mft)))) mft.Reset();
        }
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
    }
    return mft;
}

bool VideoDecoder::IsSupported(const std::string& codec) { return CreateDecoderMft(codec) != nullptr; }

void VideoDecoder::Close() {
    if (m_mft) {
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    }
    m_mft.Reset();
    m_manager.Reset();
    m_device.Reset();
    m_hardware = false;
}

bool VideoDecoder::Open(const std::string& codec, uint32_t width, uint32_t height, std::string* error) {
    Close();
    m_codec = codec;
    m_width = width;
    m_height = height;
    m_codedW = width;
    m_codedH = height;

    m_mft = CreateDecoderMft(codec);
    if (!m_mft) {
        *error = codec == "hevc" ? "Pas de décodeur HEVC : installez « Extensions vidéo HEVC » depuis le Microsoft Store, ou choisissez H.264."
                                 : "Pas de décodeur H.264 sur ce PC.";
        return false;
    }

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(m_mft->GetAttributes(&attrs))) {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        UINT32 d3dAware = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware)) && d3dAware) {
            D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                           ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                ComPtr<ID3D10Multithread> mt;
                if (SUCCEEDED(m_device.As(&mt))) mt->SetMultithreadProtected(TRUE);
                hr = MFCreateDXGIDeviceManager(&m_resetToken, &m_manager);
                if (SUCCEEDED(hr)) hr = m_manager->ResetDevice(m_device.Get(), m_resetToken);
                if (SUCCEEDED(hr))
                    hr = m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_manager.Get()));
                m_hardware = SUCCEEDED(hr);
            }
            if (!m_hardware) {
                m_manager.Reset();
                m_device.Reset();
            }
        }
    }
    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, SubtypeFor(codec));
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width, height);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    HRESULT hr = m_mft->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) {
        char buf[160];
        sprintf_s(buf, "Le décodeur refuse %ux%u (0x%08X).", width, height, unsigned(hr));
        *error = buf;
        Close();
        return false;
    }
    if (!SelectOutputType()) {
        *error = "Le décodeur ne sait pas produire du NV12.";
        Close();
        return false;
    }
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool VideoDecoder::SelectOutputType() {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(m_mft->GetOutputAvailableType(0, i, &t))) return false;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub != MFVideoFormat_NV12) continue;
        if (FAILED(m_mft->SetOutputType(0, t.Get(), 0))) return false;
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h)) && w && h) {
            m_codedW = w;
            m_codedH = h;
        }
        MFT_OUTPUT_STREAM_INFO info{};
        m_mft->GetOutputStreamInfo(0, &info);
        m_providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        m_outSize = info.cbSize;
        return true;
    }
}

bool VideoDecoder::Decode(const uint8_t* data, size_t size, int64_t ptsUs, bool key,
                          const std::function<void(const DecodedFrame&)>& onFrame) {
    if (!m_mft) return false;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(DWORD(size), &buf))) return false;
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    memcpy(p, data, size);
    buf->Unlock();
    buf->SetCurrentLength(DWORD(size));
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(ptsUs * 10);
    if (key) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);

    HRESULT hr = m_mft->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!Drain(onFrame)) return false;
        hr = m_mft->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return false;
    return Drain(onFrame);
}

bool VideoDecoder::Drain(const std::function<void(const DecodedFrame&)>& onFrame) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        ComPtr<IMFSample> own;
        if (!m_providesSamples) {
            ComPtr<IMFMediaBuffer> b;
            DWORD sz = m_outSize ? m_outSize : m_codedW * m_codedH * 3 / 2;
            if (FAILED(MFCreateMemoryBuffer(sz, &b))) return false;
            MFCreateSample(&own);
            own->AddBuffer(b.Get());
            ob.pSample = own.Get();
        }
        DWORD status = 0;
        HRESULT hr = m_mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!SelectOutputType()) return false;
            continue;
        }
        if (FAILED(hr)) {
            if (m_providesSamples && ob.pSample) ob.pSample->Release();
            return false;
        }
        ComPtr<IMFSample> out;
        if (m_providesSamples)
            out.Attach(ob.pSample); // le décodeur nous donne sa référence
        else
            out = own;
        if (!out) continue;

        ComPtr<IMFMediaBuffer> mb;
        if (FAILED(out->GetBufferByIndex(0, &mb))) continue;
        ComPtr<IMF2DBuffer> b2;
        const uint32_t w = (std::min)(m_width, m_codedW), h = (std::min)(m_height, m_codedH);
        if (SUCCEEDED(mb.As(&b2))) {
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
                DecodedFrame f{scan0, scan0 + LONG_PTR(pitch) * m_codedH, pitch, w, h};
                onFrame(f);
                b2->Unlock2D();
            }
        } else {
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(mb->Lock(&p, nullptr, &len))) {
                if (len >= m_codedW * m_codedH * 3 / 2) {
                    DecodedFrame f{p, p + size_t(m_codedW) * m_codedH, LONG(m_codedW), w, h};
                    onFrame(f);
                }
                mb->Unlock();
            }
        }
    }
}
