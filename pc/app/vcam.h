#pragma once
#include <windows.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

// Enregistre la caméra « PhoneCam » auprès de Windows 11 (MFCreateVirtualCamera) le temps
// que l'application tourne. La DLL PhoneCamSource doit avoir été installée (install.ps1).
class VirtualCamera {
public:
    ~VirtualCamera() { Remove(); }
    bool Create(std::wstring* error);
    void Remove();
    bool Active() const { return m_cam != nullptr; }
    // Format lu par la source au moment de la création (format.txt à cet instant).
    bool SameFormat(uint32_t w, uint32_t h, uint32_t fps) const { return w == m_w && h == m_h && fps == m_fps; }

    // Format annoncé aux applications qui ouvriront la caméra ensuite.
    static bool SaveFormat(uint32_t width, uint32_t height, uint32_t fps);
    static bool LoadFormat(uint32_t* width, uint32_t* height, uint32_t* fps);
    static bool SourceInstalled();

private:
    Microsoft::WRL::ComPtr<IMFVirtualCamera> m_cam;
    uint32_t m_w = 0, m_h = 0, m_fps = 0;
};
