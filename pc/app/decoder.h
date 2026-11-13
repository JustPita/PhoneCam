#pragma once
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// Image décodée NV12 en mémoire système. y/uv pointent dans un tampon verrouillé, valides pendant l'appel.
struct DecodedFrame {
    const uint8_t* y;
    const uint8_t* uv;
    LONG pitch;
    uint32_t width;  // taille visible
    uint32_t height;
};

// Décodeur H.264 / HEVC Media Foundation, accéléré par DXVA (D3D11) quand c'est possible.
class VideoDecoder {
public:
    ~VideoDecoder() { Close(); }
    bool Open(const std::string& codec, uint32_t width, uint32_t height, std::string* error);
    void Close();
    // Données Annex-B d'une image complète. Appelle onFrame pour chaque image sortie.
    bool Decode(const uint8_t* data, size_t size, int64_t ptsUs, bool key,
                const std::function<void(const DecodedFrame&)>& onFrame);
    bool Hardware() const { return m_hardware; }
    const std::string& Codec() const { return m_codec; }

    // Le décodeur HEVC de Windows exige l'extension « HEVC Video Extensions » du Store.
    static bool IsSupported(const std::string& codec);

private:
    bool SelectOutputType();
    bool Drain(const std::function<void(const DecodedFrame&)>& onFrame);

    ComPtr<ID3D11Device> m_device;
    ComPtr<IMFDXGIDeviceManager> m_manager;
    UINT m_resetToken = 0;
    ComPtr<IMFTransform> m_mft;
    bool m_hardware = false;
    bool m_providesSamples = false;
    DWORD m_outSize = 0;
    std::string m_codec;
    uint32_t m_width = 0, m_height = 0;   // visible (annoncée par le téléphone)
    uint32_t m_codedW = 0, m_codedH = 0;  // taille des surfaces décodées (alignée)
    std::vector<uint8_t> m_copy;          // pour les tampons non 2D
};
