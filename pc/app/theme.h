#pragma once
// Thème clair / sombre qui suit le réglage de Windows (« Mode d'application par défaut »).
#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

struct Theme {
    bool dark = false;
    COLORREF bg, surface, border, text, sub, accent, ok, warn;
    HBRUSH bgBrush = nullptr, surfaceBrush = nullptr;

    static bool SystemDark() {
        DWORD v = 1, cb = sizeof(v);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
        return v == 0;
    }

    void Load() {
        dark = SystemDark();
        if (dark) {
            bg = RGB(32, 32, 32);
            surface = RGB(45, 45, 45);
            border = RGB(70, 70, 70);
            text = RGB(255, 255, 255);
            sub = RGB(190, 190, 190);
            accent = RGB(76, 194, 255);
            ok = RGB(108, 203, 95);
            warn = RGB(252, 185, 0);
        } else {
            bg = RGB(243, 243, 243);
            surface = RGB(255, 255, 255);
            border = RGB(220, 220, 220);
            text = RGB(26, 26, 26);
            sub = RGB(96, 96, 96);
            accent = RGB(0, 95, 184);
            ok = RGB(15, 123, 15);
            warn = RGB(157, 93, 0);
        }
        if (bgBrush) DeleteObject(bgBrush);
        if (surfaceBrush) DeleteObject(surfaceBrush);
        bgBrush = CreateSolidBrush(bg);
        surfaceBrush = CreateSolidBrush(surface);
        AllowDarkMenus(dark);
    }

    // Barre de titre sombre (Windows 11).
    void ApplyWindow(HWND wnd) const {
        BOOL d = dark;
        DwmSetWindowAttribute(wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &d, sizeof(d));
        AllowDarkForWindow(wnd, dark);
    }

    enum Kind { Button, Combo, Edit, Check };
    void ApplyControl(HWND h, Kind k) const {
        AllowDarkForWindow(h, dark);
        if (k == Check && dark) {
            SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
        } else if (dark) {
            SetWindowTheme(h, k == Button ? L"DarkMode_Explorer" : L"DarkMode_CFD", nullptr);
        } else {
            SetWindowTheme(h, L"Explorer", nullptr);
        }
        SendMessageW(h, WM_THEMECHANGED, 0, 0);
    }

private:
    // Fonctions non documentées d'uxtheme (utilisées par l'Explorateur, le Bloc-notes, Notepad++…)
    // pour les menus et listes déroulantes sombres. Sans effet si elles sont absentes.
    static HMODULE UxTheme() {
        static HMODULE m = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m;
    }
    static void AllowDarkMenus(bool dark) {
        using SetPreferredAppMode = int(WINAPI*)(int);
        using Refresh = void(WINAPI*)();
        if (!UxTheme()) return;
        auto set = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(UxTheme(), MAKEINTRESOURCEA(135)));
        auto refresh = reinterpret_cast<Refresh>(GetProcAddress(UxTheme(), MAKEINTRESOURCEA(104)));
        if (set) set(dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
        if (refresh) refresh();
    }
    static void AllowDarkForWindow(HWND h, bool dark) {
        using Allow = BOOL(WINAPI*)(HWND, BOOL);
        if (!UxTheme()) return;
        auto allow = reinterpret_cast<Allow>(GetProcAddress(UxTheme(), MAKEINTRESOURCEA(133)));
        if (allow) allow(h, dark);
    }
};
