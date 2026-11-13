#pragma once
// Réglages de l'utilisateur dans HKCU\Software\PhoneCam, et lancement au démarrage de Windows.
#include <windows.h>

#include <string>

namespace settings {

inline const wchar_t* kKey = L"Software\\PhoneCam";
inline const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

inline DWORD GetDword(const wchar_t* name, DWORD def) {
    DWORD v = 0, cb = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS ? v : def;
}
inline void SetDword(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_DWORD, &v, sizeof(v));
}
inline std::wstring GetString(const wchar_t* name) {
    wchar_t buf[512] = {};
    DWORD cb = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_SZ, nullptr, buf, &cb) != ERROR_SUCCESS) return L"";
    return buf;
}
inline void SetString(const wchar_t* name, const std::wstring& v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_SZ, v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t)));
}

// Démarrage avec Windows : PhoneCam se lance caché dans la zone de notification.
inline bool AutoStartEnabled() {
    DWORD cb = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"PhoneCam", RRF_RT_REG_SZ, nullptr, nullptr, &cb) == ERROR_SUCCESS;
}
inline void SetAutoStart(bool on, const std::wstring& exe) {
    if (on) {
        std::wstring cmd = L"\"" + exe + L"\" --tray";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"PhoneCam", REG_SZ, cmd.c_str(),
                        DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"PhoneCam");
    }
    SetDword(L"AutoStart", on ? 1 : 0);
}

}  // namespace settings
