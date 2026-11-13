#include "vcam.h"

#include <cstdio>

#include "../common/shared_frame.h"

bool VirtualCamera::SourceInstalled() {
    std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + PHONECAM_CLSID_STRING + L"\\InprocServer32";
    wchar_t path[MAX_PATH] = {};
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS)
        return false;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

bool VirtualCamera::Create(std::wstring* error) {
    Remove();
    BOOL supported = FALSE;
    if (FAILED(MFIsVirtualCameraTypeSupported(MFVirtualCameraType_SoftwareCameraSource, &supported)) || !supported) {
        *error = L"Les caméras virtuelles exigent Windows 11.";
        return false;
    }
    if (!SourceInstalled()) {
        *error = L"PhoneCamSource.dll n'est pas installée : lancez install.ps1 en administrateur.";
        return false;
    }
    HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                                       MFVirtualCameraAccess_CurrentUser, PHONECAM_FRIENDLY_NAME,
                                       PHONECAM_CLSID_STRING, nullptr, 0, &m_cam);
    if (SUCCEEDED(hr)) hr = m_cam->Start(nullptr);
    if (!LoadFormat(&m_w, &m_h, &m_fps)) {
        m_w = 1920;
        m_h = 1080;
        m_fps = 30;
    }
    if (FAILED(hr)) {
        wchar_t buf[128];
        swprintf_s(buf, L"Création de la caméra virtuelle impossible (0x%08X).", unsigned(hr));
        *error = buf;
        m_cam.Reset();
        return false;
    }
    return true;
}

void VirtualCamera::Remove() {
    if (m_cam) {
        m_cam->Stop();
        m_cam->Remove();
        m_cam->Shutdown();
        m_cam.Reset();
    }
}

bool VirtualCamera::SaveFormat(uint32_t width, uint32_t height, uint32_t fps) {
    CreateDirectoryW(PHONECAM_FORMAT_DIR, nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, PHONECAM_FORMAT_FILE, L"w") != 0 || !f) return false;
    fprintf(f, "%u %u %u\n", width, height, fps);
    fclose(f);
    return true;
}

bool VirtualCamera::LoadFormat(uint32_t* width, uint32_t* height, uint32_t* fps) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, PHONECAM_FORMAT_FILE, L"r") != 0 || !f) return false;
    unsigned w, h, r;
    bool ok = fscanf_s(f, "%u %u %u", &w, &h, &r) == 3;
    fclose(f);
    if (ok) {
        *width = w;
        *height = h;
        *fps = r;
    }
    return ok;
}
