#include "install.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <fstream>
#include <vector>

#include "../common/shared_frame.h"
#include "net.h"

using Microsoft::WRL::ComPtr;

namespace installer {

static const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PhoneCam";
static const wchar_t* kFirewallRule = L"PhoneCam";
constexpr int kDllResource = 2;

std::wstring InstallDir() {
    wchar_t* pf = nullptr;
    std::wstring dir = L"C:\\Program Files";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &pf))) dir = pf;
    CoTaskMemFree(pf);
    return dir + L"\\PhoneCam";
}
std::wstring InstalledExe() { return InstallDir() + L"\\PhoneCam.exe"; }
static std::wstring InstalledDll() { return InstallDir() + L"\\PhoneCamSource.dll"; }

std::wstring SelfPath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

static bool EmbeddedDll(const void** data, DWORD* size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(kDllResource), RT_RCDATA);
    if (!r) return false;
    HGLOBAL h = LoadResource(nullptr, r);
    *data = h ? LockResource(h) : nullptr;
    *size = SizeofResource(nullptr, r);
    return *data && *size;
}

static std::vector<char> ReadFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(f), {});
}

static bool SamePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

bool Needed() {
    const void* data;
    DWORD size;
    if (!EmbeddedDll(&data, &size)) return false;
    auto dll = ReadFile(InstalledDll());
    if (dll.size() != size || memcmp(dll.data(), data, size) != 0) return true;

    std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + PHONECAM_CLSID_STRING + L"\\InprocServer32";
    wchar_t reg[MAX_PATH] = {};
    DWORD cb = sizeof(reg);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, reg, &cb) != ERROR_SUCCESS ||
        !SamePath(reg, InstalledDll()))
        return true;

    if (!SamePath(SelfPath(), InstalledExe()) && ReadFile(SelfPath()) != ReadFile(InstalledExe())) return true;
    return false;
}

int RunElevated(const wchar_t* argument) {
    std::wstring self = SelfPath();
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self.c_str();
    sei.lpParameters = argument;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return -1; // refusé par l'utilisateur
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return int(code);
}

// Le service Frame Server garde la DLL chargée : on l'arrête pour pouvoir la remplacer.
static void StopFrameServer() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    for (const wchar_t* name : {L"FrameServerMonitor", L"FrameServer"}) {
        SC_HANDLE s = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (!s) continue;
        SERVICE_STATUS st{};
        ControlService(s, SERVICE_CONTROL_STOP, &st);
        for (int i = 0; i < 50 && QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(100);
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
}

static bool CallDllExport(const std::wstring& dll, const char* name) {
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return false;
    auto fn = reinterpret_cast<HRESULT(STDAPICALLTYPE*)()>(GetProcAddress(m, name));
    HRESULT hr = fn ? fn() : E_FAIL;
    FreeLibrary(m);
    return SUCCEEDED(hr);
}

static bool WriteFileBytes(const std::wstring& path, const void* data, DWORD size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        // Encore chargée quelque part : on la renomme, l'ancienne copie partira au redémarrage.
        std::wstring old = path + L".old";
        DeleteFileW(old.c_str());
        if (!MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
        MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
    }
    DWORD written = 0;
    BOOL ok = ::WriteFile(f, data, size, &written, nullptr);
    CloseHandle(f);
    return ok && written == size;
}

// C:\ProgramData\PhoneCam : l'application (utilisateur) y écrit le format, le service le lit.
static void PrepareDataDir() {
    CreateDirectoryW(PHONECAM_FORMAT_DIR, nullptr);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;BU)(A;OICI;FRFX;;;LS)", SDDL_REVISION_1, &sd,
            nullptr)) {
        SetFileSecurityW(PHONECAM_FORMAT_DIR, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, sd);
        LocalFree(sd);
    }
}

static std::wstring ShortcutPath() {
    wchar_t* p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0, nullptr, &p))) s = std::wstring(p) + L"\\PhoneCam.lnk";
    CoTaskMemFree(p);
    return s;
}

static void CreateShortcut(const std::wstring& exe) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return;
    link->SetPath(exe.c_str());
    link->SetWorkingDirectory(InstallDir().c_str());
    link->SetDescription(L"Utiliser la caméra du téléphone comme webcam");
    ComPtr<IPersistFile> file;
    if (SUCCEEDED(link.As(&file))) file->Save(ShortcutPath().c_str(), TRUE);
}

static void SetString(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
}

static void RegisterUninstall(const std::wstring& exe) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    SetString(k, L"DisplayName", L"PhoneCam");
    SetString(k, L"DisplayVersion", L"1.0");
    SetString(k, L"Publisher", L"JustPita");
    SetString(k, L"DisplayIcon", exe);
    SetString(k, L"InstallLocation", InstallDir());
    SetString(k, L"UninstallString", L"\"" + exe + L"\" --uninstall");
    DWORD one = 1;
    RegSetValueExW(k, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegCloseKey(k);
}

static void FirewallRule(bool add, const std::wstring& exe) {
    std::string out;
    DWORD code;
    RunHidden(std::wstring(L"netsh advfirewall firewall delete rule name=\"") + kFirewallRule + L"\"", &out, &code);
    if (add) {
        RunHidden(std::wstring(L"netsh advfirewall firewall add rule name=\"") + kFirewallRule +
                      L"\" dir=in action=allow protocol=UDP localport=38501 profile=private,domain program=\"" + exe + L"\"",
                  &out, &code);
    }
}

static void CloseOtherInstances() {
    // Une instance non élevée attend peut-être notre fin : on ne ferme que celles du dossier installé.
    // WM_CLOSE ne fait que ranger la fenêtre dans la zone de notification : message dédié.
    const UINT quitMsg = RegisterWindowMessageW(L"PhoneCam.Quit");
    HWND w = nullptr;
    std::vector<HANDLE> procs;
    while ((w = FindWindowExW(nullptr, w, L"PhoneCamWnd", nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId()) continue;
        if (HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid)) procs.push_back(h);
        PostMessageW(w, quitMsg, 0, 0);
    }
    for (HANDLE h : procs) {
        if (WaitForSingleObject(h, 10000) == WAIT_TIMEOUT) TerminateProcess(h, 1);
        CloseHandle(h);
    }
}

int Install() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const void* data;
    DWORD size;
    if (!EmbeddedDll(&data, &size)) return 10;

    const std::wstring dir = InstallDir(), exe = InstalledExe(), dll = InstalledDll(), self = SelfPath();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    CloseOtherInstances();

    if (!SamePath(self, exe) && !CopyFileW(self.c_str(), exe.c_str(), FALSE)) return 11;

    StopFrameServer();
    if (!WriteFileBytes(dll, data, size)) return 12;
    if (!CallDllExport(dll, "DllRegisterServer")) return 13;

    PrepareDataDir();
    FirewallRule(true, exe);
    CreateShortcut(exe);
    RegisterUninstall(exe);
    CoUninitialize();
    return 0;
}

int Uninstall() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (MessageBoxW(nullptr, L"Désinstaller PhoneCam et sa caméra virtuelle ?", L"PhoneCam",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 1;
    const std::wstring dir = InstallDir(), dll = InstalledDll();
    CloseOtherInstances();
    CallDllExport(dll, "DllUnregisterServer");
    StopFrameServer();
    FirewallRule(false, L"");
    DeleteFileW(ShortcutPath().c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kUninstallKey);
    std::wstring data = PHONECAM_FORMAT_DIR;
    DeleteFileW(PHONECAM_FORMAT_FILE);
    RemoveDirectoryW(data.c_str());

    // Le dossier contient l'exe en cours d'exécution : suppression différée par un cmd caché.
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + dir + L"\"";
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    MessageBoxW(nullptr, L"PhoneCam a été désinstallé.", L"PhoneCam", MB_OK | MB_ICONINFORMATION);
    CoUninitialize();
    return 0;
}

}  // namespace installer
