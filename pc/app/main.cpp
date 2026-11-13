// PhoneCam : reçoit le flux H.264/HEVC du téléphone, le décode et l'expose comme webcam Windows 11.
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "decoder.h"
#include "framewriter.h"
#include "install.h"
#include "net.h"
#include "playout.h"
#include "vcam.h"

#pragma comment(linker, "/subsystem:windows")

// ---------------------------------------------------------------------------
// Utilitaires texte
// ---------------------------------------------------------------------------
static std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
static std::string U(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
static std::map<std::string, std::string> ParseKv(const std::string& text) {
    std::map<std::string, std::string> kv;
    std::istringstream in(text);
    std::string tok;
    while (in >> tok) {
        auto i = tok.find('=');
        if (i != std::string::npos) kv[tok.substr(0, i)] = tok.substr(i + 1);
    }
    return kv;
}

// ---------------------------------------------------------------------------
// Capacités du téléphone
// ---------------------------------------------------------------------------
struct CamInfo {
    std::string id, facing;
    int orientation = 0;
};
struct ModeInfo {
    std::string camera, codec;
    int width = 0, height = 0, fps = 0;
    bool highSpeed = false;
};
struct PhoneCaps {
    std::string device;
    std::vector<std::string> codecs;
    std::vector<CamInfo> cams;
    std::vector<ModeInfo> modes;

    static PhoneCaps Parse(const std::string& text) {
        PhoneCaps c;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string kind;
            ls >> kind;
            if (kind == "device") {
                std::getline(ls, c.device);
                c.device.erase(0, c.device.find_first_not_of(' '));
            } else if (kind == "codec") {
                std::string n;
                ls >> n;
                c.codecs.push_back(n);
            } else if (kind == "camera") {
                CamInfo ci;
                ls >> ci.id >> ci.facing >> ci.orientation;
                c.cams.push_back(ci);
            } else if (kind == "mode") {
                ModeInfo m;
                int hs = 0;
                ls >> m.camera >> m.width >> m.height >> m.fps >> hs >> m.codec;
                m.highSpeed = hs != 0;
                if (m.width > 0) c.modes.push_back(m);
            }
        }
        return c;
    }
};

// ---------------------------------------------------------------------------
// État global de l'application
// ---------------------------------------------------------------------------
enum : UINT {
    WM_APP_HELLO = WM_APP + 1,
    WM_APP_FORMAT,
    WM_APP_PHONE_ERROR,
    WM_APP_CLOSED,
    WM_APP_PREVIEW,
    WM_APP_STATUS,
    WM_APP_TRAY,
    WM_APP_SHOW, // envoyé par une 2e instance : réafficher la fenêtre
};
enum : UINT { IDM_OPEN = 200, IDM_CONNECT, IDM_QUIT };
enum : int {
    ID_SOURCE = 100,
    ID_MANUAL,
    ID_CONNECT,
    ID_CAMERA,
    ID_RES,
    ID_FPS,
    ID_CODEC,
    ID_BITRATE,
    ID_MIRROR,
    ID_ROTATE,
    ID_STATUS,
    ID_SMOOTH,
    ID_TIMER_STATS = 1,
    ID_TIMER_DISCOVERY = 2,
};

struct App {
    HWND wnd = nullptr;
    HFONT font = nullptr;
    HWND source, manual, connect, camera, res, fps, codec, bitrate, mirror, rotate, smooth, status;
    float scale = 1.0f;
    RECT previewRect{};

    PhoneConnection conn;
    Discovery discovery;
    VirtualCamera vcam;
    std::wstring vcamError;

    PhoneCaps caps;
    std::set<std::string> pcCodecs;     // décodables sur ce PC
    std::vector<std::string> sourceIds; // parallèle aux entrées du combo « téléphone »

    // Côté réseau (fil de réception) puis fil de lissage (seul à écrire dans la mémoire partagée)
    VideoDecoder decoder;
    Playout playout;
    FrameWriter writer;
    std::string formatText;
    bool waitingKey = true;
    std::atomic<bool> optMirror{false}, optRotate{false};
    std::atomic<uint64_t> framesDecoded{0};
    std::atomic<bool> consumer{false};
    std::atomic<bool> writerReset{false};

    // Régularité du flux (remis à zéro chaque seconde par UpdateStats)
    int streamFps = 30;
    int64_t lastPts = -1;
    LONGLONG lastArrival = 0;
    std::atomic<int> maxArrivalGapUs{0}, lateArrivals{0}, ptsGaps{0}, decodeFailures{0};
    std::atomic<int> maxDecodeUs{0};
    std::string decoderError;

    // Aperçu (BGRA réduit)
    std::mutex previewLock;
    std::vector<uint32_t> preview;
    int previewW = 0, previewH = 0;
    ULONGLONG lastPreview = 0;
    std::atomic<bool> previewPending{false};

    // Zone de notification
    NOTIFYICONDATAW tray{};
    UINT taskbarCreated = 0;
    bool trayHintShown = false;
    bool quitting = false;

    // Statistiques
    uint64_t lastBytes = 0, lastFrames = 0, lastDecoded = 0;
    ULONGLONG lastStatTick = 0;
    std::wstring phoneMessage;
};
static App g;

static int S(int v) { return int(v * g.scale + 0.5f); }

static void SetStatus(const std::wstring& text) { SetWindowTextW(g.status, text.c_str()); }

static std::wstring ComboText(HWND combo) {
    int i = int(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (i < 0) return L"";
    int len = int(SendMessageW(combo, CB_GETLBTEXTLEN, i, 0));
    std::wstring s(size_t(len) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(s.data()));
    s.resize(size_t(len));
    return s;
}
static std::wstring WindowText(HWND h) {
    int len = GetWindowTextLengthW(h);
    std::wstring s(size_t(len) + 1, L'\0');
    GetWindowTextW(h, s.data(), len + 1);
    s.resize(size_t(len));
    return s;
}

// ---------------------------------------------------------------------------
// Aperçu : NV12 -> BGRA réduit
// ---------------------------------------------------------------------------
static void UpdatePreview(const DecodedFrame& f) {
    ULONGLONG now = GetTickCount64();
    if (now - g.lastPreview < 15 || g.previewPending) return; // ~60 i/s max
    g.lastPreview = now;
    int maxW = g.previewRect.right - g.previewRect.left, maxH = g.previewRect.bottom - g.previewRect.top;
    if (maxW <= 0 || maxH <= 0) return;
    int pw = maxW, ph = int(int64_t(maxW) * f.height / f.width);
    if (ph > maxH) {
        ph = maxH;
        pw = int(int64_t(maxH) * f.width / f.height);
    }
    const bool mirror = g.optMirror, rot = g.optRotate;
    const bool flipX = mirror != rot, flipY = rot;
    std::lock_guard<std::mutex> lk(g.previewLock);
    g.preview.resize(size_t(pw) * ph);
    g.previewW = pw;
    g.previewH = ph;
    for (int y = 0; y < ph; ++y) {
        uint32_t sy = uint32_t(int64_t(y) * f.height / ph);
        if (flipY) sy = f.height - 1 - sy;
        const uint8_t* yr = f.y + LONG_PTR(f.pitch) * sy;
        const uint8_t* uvr = f.uv + LONG_PTR(f.pitch) * (sy / 2);
        uint32_t* d = &g.preview[size_t(y) * pw];
        for (int x = 0; x < pw; ++x) {
            uint32_t sx = uint32_t(int64_t(x) * f.width / pw);
            if (flipX) sx = f.width - 1 - sx;
            int c = (yr[sx] - 16) * 298, u = uvr[sx & ~1u] - 128, v = uvr[(sx & ~1u) + 1] - 128;
            int r = (c + 459 * v + 128) >> 8, gg = (c - 55 * u - 136 * v + 128) >> 8, b = (c + 541 * u + 128) >> 8;
            auto cl = [](int t) { return uint32_t(t < 0 ? 0 : t > 255 ? 255 : t); };
            d[x] = (cl(r) << 16) | (cl(gg) << 8) | cl(b);
        }
    }
    g.previewPending = true;
    PostMessageW(g.wnd, WM_APP_PREVIEW, 0, 0);
}

// ---------------------------------------------------------------------------
// Callbacks réseau (fil de réception)
// ---------------------------------------------------------------------------
static void AtomicMax(std::atomic<int>& a, int v) {
    int cur = a;
    while (v > cur && !a.compare_exchange_weak(cur, v)) {
    }
}

static void OnFrameData(const uint8_t* data, size_t size, bool key, int64_t pts) {
    const int64_t arrivalUs = Playout::NowUs();
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const int periodUs = 1000000 / (g.streamFps > 0 ? g.streamFps : 30);
    // Écart d'arrivée (réseau, USB/Wi-Fi) et écart d'horodatage (images perdues côté téléphone).
    if (g.lastArrival) {
        int gapUs = int((now.QuadPart - g.lastArrival) * 1000000 / freq.QuadPart);
        AtomicMax(g.maxArrivalGapUs, gapUs);
        if (gapUs > periodUs * 3 / 2) g.lateArrivals++;
    }
    if (g.lastPts >= 0 && pts - g.lastPts > periodUs * 3 / 2) g.ptsGaps++;
    g.lastArrival = now.QuadPart;
    g.lastPts = pts;

    if (!g.decoder.Codec().size()) return;
    if (g.waitingKey && !key) return;
    g.waitingKey = false;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    bool ok = g.decoder.Decode(data, size, pts, key, [&](const DecodedFrame& f) {
        g.framesDecoded++;
        g.playout.Push(f, pts, arrivalUs);
    });
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    AtomicMax(g.maxDecodeUs, int((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart));
    if (!ok) {
        // Image perdue ou corrompue : on repart de la prochaine image clé.
        g.decodeFailures++;
        g.waitingKey = true;
        g.conn.Send('K', "");
    }
}

static void OnFormat(const std::string& text) {
    auto kv = ParseKv(text);
    std::string codec = kv["codec"];
    uint32_t w = uint32_t(atoi(kv["width"].c_str())), h = uint32_t(atoi(kv["height"].c_str()));
    uint32_t fps = uint32_t(atoi(kv["fps"].c_str()));
    std::string err;
    g.decoderError.clear();
    g.streamFps = int(fps);
    g.lastPts = -1;
    g.lastArrival = 0;
    g.playout.Reset();
    if (!g.decoder.Open(codec, w, h, &err)) g.decoderError = err;
    g.waitingKey = true;
    VirtualCamera::SaveFormat(w, h, fps);
    PostMessageW(g.wnd, WM_APP_FORMAT, 0, reinterpret_cast<LPARAM>(new std::string(text)));
    g.conn.Send('K', "");
}

static PhoneConnection::Callbacks MakeCallbacks() {
    PhoneConnection::Callbacks cb;
    cb.onHello = [](const std::string& t) { PostMessageW(g.wnd, WM_APP_HELLO, 0, reinterpret_cast<LPARAM>(new std::string(t))); };
    cb.onFormat = OnFormat;
    cb.onCodecConfig = [](const std::vector<uint8_t>& csd) {
        // Les en-têtes sont aussi répétés devant chaque image clé ; on les passe quand même au décodeur.
        if (!csd.empty()) g.decoder.Decode(csd.data(), csd.size(), 0, true, [](const DecodedFrame&) {});
    };
    cb.onFrame = OnFrameData;
    cb.onPhoneError = [](const std::string& t) {
        PostMessageW(g.wnd, WM_APP_PHONE_ERROR, 0, reinterpret_cast<LPARAM>(new std::string(t)));
    };
    cb.onClosed = [](const std::string& t) { PostMessageW(g.wnd, WM_APP_CLOSED, 0, reinterpret_cast<LPARAM>(new std::string(t))); };
    return cb;
}

// ---------------------------------------------------------------------------
// Listes déroulantes
// ---------------------------------------------------------------------------
static std::wstring FacingName(const std::string& f) {
    return f == "back" ? L"arrière" : f == "front" ? L"avant" : L"externe";
}

static void FillCodecs() {
    SendMessageW(g.codec, CB_RESETCONTENT, 0, 0);
    int sel = -1, i = 0;
    for (auto& c : g.caps.codecs) {
        if (!g.pcCodecs.count(c)) continue;
        SendMessageW(g.codec, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c == "hevc" ? L"HEVC (H.265)" : L"H.264"));
        if (sel < 0 || c == "hevc") sel = i;
        ++i;
    }
    SendMessageW(g.codec, CB_SETCURSEL, sel, 0);
}
static std::string SelectedCodec() { return ComboText(g.codec).rfind(L"HEVC", 0) == 0 ? "hevc" : "avc"; }
static std::string SelectedCamera() {
    int i = int(SendMessageW(g.camera, CB_GETCURSEL, 0, 0));
    return (i >= 0 && size_t(i) < g.caps.cams.size()) ? g.caps.cams[size_t(i)].id : "0";
}
static bool SelectedSize(int* w, int* h) { return swscanf_s(ComboText(g.res).c_str(), L"%d×%d", w, h) == 2; }

// Resélectionne l'entrée qui commence par « previous » si elle existe encore, sinon la première.
static void SelectOrFirst(HWND combo, const std::wstring& previous) {
    LRESULT i = previous.empty() ? CB_ERR
                                 : SendMessageW(combo, CB_FINDSTRING, WPARAM(-1), reinterpret_cast<LPARAM>(previous.c_str()));
    SendMessageW(combo, CB_SETCURSEL, i == CB_ERR ? 0 : i, 0);
}

static void FillFps() {
    int w = 0, h = 0;
    std::wstring previous = ComboText(g.fps);
    if (auto sp = previous.find(L' '); sp != std::wstring::npos) previous.resize(sp + 1); // « 30 »
    SendMessageW(g.fps, CB_RESETCONTENT, 0, 0);
    if (!SelectedSize(&w, &h)) return;
    std::string cam = SelectedCamera(), codec = SelectedCodec();
    // Session normale : toutes les valeurs usuelles jusqu'au maximum. Haute vitesse : valeurs exactes seulement.
    int regularMax = 0;
    std::set<int> values;
    for (auto& m : g.caps.modes) {
        if (m.camera != cam || m.codec != codec || m.width != w || m.height != h) continue;
        if (m.highSpeed)
            values.insert(m.fps);
        else
            regularMax = (std::max)(regularMax, m.fps);
    }
    for (int v : {15, 24, 25, 30, 48, 50, 60, 90, 120, 240})
        if (v <= regularMax) values.insert(v);
    if (regularMax > 0) values.insert(regularMax);
    int i = 0;
    for (auto it = values.rbegin(); it != values.rend(); ++it, ++i) {
        std::wstring label = std::to_wstring(*it) + L" i/s";
        for (auto& m : g.caps.modes)
            if (m.camera == cam && m.codec == codec && m.width == w && m.height == h && m.highSpeed && m.fps == *it)
                label += L" (haute vitesse)";
        SendMessageW(g.fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SelectOrFirst(g.fps, previous); // sinon le plus élevé
}

static void FillResolutions() {
    const std::wstring previous = ComboText(g.res);
    SendMessageW(g.res, CB_RESETCONTENT, 0, 0);
    std::string cam = SelectedCamera(), codec = SelectedCodec();
    std::vector<std::pair<int, int>> sizes;
    for (auto& m : g.caps.modes) {
        if (m.camera != cam || m.codec != codec) continue;
        if (uint64_t(m.width) * m.height > PHONECAM_MAX_PIXELS) continue;
        auto p = std::make_pair(m.width, m.height);
        if (std::find(sizes.begin(), sizes.end(), p) == sizes.end()) sizes.push_back(p);
    }
    std::sort(sizes.begin(), sizes.end(), [](auto& a, auto& b) { return a.first * a.second > b.first * b.second; });
    for (auto& [w, h] : sizes) {
        std::wstring label = std::to_wstring(w) + L"×" + std::to_wstring(h);
        SendMessageW(g.res, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SelectOrFirst(g.res, previous); // sinon la plus grande
    FillFps();
}

static void FillCameras() {
    SendMessageW(g.camera, CB_RESETCONTENT, 0, 0);
    int sel = 0, i = 0;
    for (auto& c : g.caps.cams) {
        std::wstring label = L"Caméra " + W(c.id) + L" (" + FacingName(c.facing) + L")";
        SendMessageW(g.camera, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (c.facing == "back" && sel == 0 && i != 0 && g.caps.cams[0].facing != "back") sel = i;
        ++i;
    }
    SendMessageW(g.camera, CB_SETCURSEL, sel, 0);
}

static void EnableStreamControls(bool on) {
    for (HWND h : {g.camera, g.res, g.fps, g.codec, g.bitrate}) EnableWindow(h, on);
}

static void RefreshSources() {
    auto phones = g.discovery.List();
    std::vector<std::string> ids = {"usb"};
    std::vector<std::wstring> labels = {L"USB (adb)"};
    for (auto& p : phones) {
        ids.push_back(p.address + ":" + std::to_string(p.port));
        labels.push_back(L"Wi-Fi : " + W(p.name) + L" (" + W(p.address) + L")");
    }
    if (ids == g.sourceIds) return;
    std::string current;
    int i = int(SendMessageW(g.source, CB_GETCURSEL, 0, 0));
    if (i >= 0 && size_t(i) < g.sourceIds.size()) current = g.sourceIds[size_t(i)];
    g.sourceIds = ids;
    SendMessageW(g.source, CB_RESETCONTENT, 0, 0);
    int sel = 0;
    for (size_t k = 0; k < ids.size(); ++k) {
        SendMessageW(g.source, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(labels[k].c_str()));
        if (ids[k] == current) sel = int(k);
    }
    // Premier téléphone Wi-Fi vu : sélection automatique si rien n'était choisi.
    if (current.empty() && ids.size() > 1) sel = 1;
    SendMessageW(g.source, CB_SETCURSEL, sel, 0);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static void SendStart() {
    int w = 0, h = 0, fps = 0;
    if (!SelectedSize(&w, &h)) return;
    swscanf_s(ComboText(g.fps).c_str(), L"%d", &fps);
    double mbps = _wtof(WindowText(g.bitrate).c_str());
    std::ostringstream os;
    os << "camera=" << SelectedCamera() << " width=" << w << " height=" << h << " fps=" << fps
       << " codec=" << SelectedCodec() << " bitrate=" << int64_t(mbps * 1e6);
    g.conn.Send('S', os.str());
    SetStatus(L"Démarrage de la capture…");
}

static void Disconnect() {
    g.conn.Disconnect();
    g.decoder.Close();
    g.caps = PhoneCaps{};
    g.formatText.clear();
    SetWindowTextW(g.connect, L"Connecter");
    EnableStreamControls(false);
    EnableWindow(g.source, TRUE);
    EnableWindow(g.manual, TRUE);
}

static void Connect() {
    std::string host;
    uint16_t port = PHONECAM_PORT;
    std::wstring manual = WindowText(g.manual);
    if (!manual.empty()) {
        host = U(manual);
        auto c = host.find(':');
        if (c != std::string::npos) {
            port = uint16_t(atoi(host.c_str() + c + 1));
            host.resize(c);
        }
    } else {
        int i = int(SendMessageW(g.source, CB_GETCURSEL, 0, 0));
        std::string id = (i >= 0 && size_t(i) < g.sourceIds.size()) ? g.sourceIds[size_t(i)] : "usb";
        if (id == "usb") {
            std::string msg;
            SetStatus(L"USB : préparation d'adb…");
            if (!AdbForward(&msg)) {
                SetStatus(W(msg));
                return;
            }
            host = "127.0.0.1";
        } else {
            auto c = id.find(':');
            host = id.substr(0, c);
            port = uint16_t(atoi(id.c_str() + c + 1));
        }
    }
    SetStatus(L"Connexion à " + W(host) + L"…");
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::string err;
    bool ok = g.conn.Connect(host, port, MakeCallbacks(), &err);
    SetCursor(old);
    if (!ok) {
        if (host == "127.0.0.1" && err.find("refusée") != std::string::npos)
            err = "USB : le téléphone ne répond pas. L'application PhoneCam est-elle ouverte sur le téléphone ?";
        SetStatus(W(err));
        return;
    }
    // Lissage par défaut : aucun en USB (pas de gigue), automatique en Wi-Fi.
    const bool usb = host == "127.0.0.1";
    SendMessageW(g.smooth, CB_SETCURSEL, usb ? 0 : 1, 0);
    g.playout.SetDelayMs(usb ? 0 : -1);
    SetWindowTextW(g.connect, L"Déconnecter");
    EnableWindow(g.source, FALSE);
    EnableWindow(g.manual, FALSE);
    SetStatus(L"Connecté, lecture des capacités du téléphone…");
}

static void EnsureVirtualCamera() {
    if (g.vcam.Active()) return;
    std::wstring err;
    if (!g.vcam.Create(&err)) g.vcamError = err;
    else g.vcamError.clear();
}

// ---------------------------------------------------------------------------
// Zone de notification
// ---------------------------------------------------------------------------
static void TrayAdd() {
    g.tray = {};
    g.tray.cbSize = sizeof(g.tray);
    g.tray.hWnd = g.wnd;
    g.tray.uID = 1;
    g.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g.tray.uCallbackMessage = WM_APP_TRAY;
    LoadIconMetric(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), LIM_SMALL, &g.tray.hIcon);
    wcscpy_s(g.tray.szTip, L"PhoneCam");
    Shell_NotifyIconW(NIM_ADD, &g.tray);
    g.tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g.tray);
}

static void TrayTip(const std::wstring& text) {
    std::wstring t = text.substr(0, _countof(g.tray.szTip) - 1);
    if (t == g.tray.szTip) return;
    wcscpy_s(g.tray.szTip, t.c_str());
    g.tray.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g.tray);
}

static void TrayBalloon(const wchar_t* title, const wchar_t* text) {
    g.tray.uFlags = NIF_INFO;
    wcscpy_s(g.tray.szInfoTitle, title);
    wcscpy_s(g.tray.szInfo, text);
    g.tray.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &g.tray);
}

static void ShowMain() {
    ShowWindow(g.wnd, SW_SHOW);
    if (IsIconic(g.wnd)) ShowWindow(g.wnd, SW_RESTORE);
    SetForegroundWindow(g.wnd);
}

static void HideToTray() {
    ShowWindow(g.wnd, SW_HIDE);
    if (!g.trayHintShown) {
        g.trayHintShown = true;
        TrayBalloon(L"PhoneCam continue en arrière-plan",
                    L"La webcam reste disponible. Cliquez sur l'icône pour rouvrir, clic droit pour quitter.");
    }
}

static void TrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"Ouvrir PhoneCam");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_CONNECT, g.conn.Connected() ? L"Déconnecter le téléphone" : L"Connecter le téléphone");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"Quitter");
    SetMenuDefaultItem(m, IDM_OPEN, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g.wnd); // sinon le menu ne se ferme pas en cliquant ailleurs
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g.wnd, nullptr);
    PostMessageW(g.wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void UpdateStats() {
    ULONGLONG now = GetTickCount64();
    double dt = g.lastStatTick ? (now - g.lastStatTick) / 1000.0 : 0;
    uint64_t bytes = g.conn.bytesReceived, frames = g.conn.framesReceived, dec = g.framesDecoded;
    std::wostringstream os;
    if (!g.vcamError.empty()) os << L"⚠ " << g.vcamError << L"\r\n";
    else os << L"Caméra virtuelle « PhoneCam » : " << (g.vcam.Active() ? (g.consumer ? L"utilisée par une application" : L"prête") : L"inactive") << L"\r\n";
    if (g.conn.Connected()) {
        os << L"Téléphone : " << W(g.caps.device.empty() ? "?" : g.caps.device);
        if (!g.formatText.empty()) {
            auto kv = ParseKv(g.formatText);
            os << L" · " << W(kv["width"]) << L"×" << W(kv["height"]) << L" @ " << W(kv["fps"]) << L" i/s "
               << (kv["codec"] == "hevc" ? L"HEVC" : L"H.264") << L" · décodage "
               << (g.decoder.Hardware() ? L"matériel" : L"logiciel");
        }
        os << L"\r\n";
        if (dt > 0) {
            wchar_t buf[200];
            swprintf_s(buf, L"Réception : %.1f Mb/s · %.0f i/s reçues · %.0f i/s décodées",
                       (bytes - g.lastBytes) * 8 / dt / 1e6, (frames - g.lastFrames) / dt, (dec - g.lastDecoded) / dt);
            os << buf;
            swprintf_s(buf, L"\r\nRégularité (1 s) : écart max %.0f ms · %d retards réseau · %d images manquantes (téléphone) · décodage max %.1f ms · %d erreurs",
                       g.maxArrivalGapUs.exchange(0) / 1000.0, g.lateArrivals.exchange(0), g.ptsGaps.exchange(0),
                       g.maxDecodeUs.exchange(0) / 1000.0, g.decodeFailures.exchange(0));
            os << buf;
            if (g.playout.DelayMs() > 0) {
                swprintf_s(buf, L" · lissage %s%d ms : %d en attente, %d sautées", g.playout.Auto() ? L"auto " : L"",
                           g.playout.DelayMs(), g.playout.Queued(), g.playout.dropped.exchange(0));
                os << buf;
            }
        }
        if (!g.decoderError.empty()) os << L"\r\n⚠ " << W(g.decoderError);
        if (!g.phoneMessage.empty()) os << L"\r\n⚠ Téléphone : " << g.phoneMessage;
    } else {
        os << L"Non connecté. Choisissez le téléphone (Wi-Fi détecté ou USB) puis « Connecter ».";
        if (!g.phoneMessage.empty()) os << L"\r\n" << g.phoneMessage;
    }
    SetStatus(os.str());

    std::wstring tip = L"PhoneCam — ";
    if (!g.conn.Connected()) tip += L"non connecté";
    else if (g.formatText.empty()) tip += L"connecté";
    else {
        auto kv = ParseKv(g.formatText);
        tip += W(kv["width"]) + L"×" + W(kv["height"]) + L" @ " + W(kv["fps"]) + L" i/s";
        if (g.consumer) tip += L" · utilisée";
    }
    TrayTip(tip);

    g.lastBytes = bytes;
    g.lastFrames = frames;
    g.lastDecoded = dec;
    g.lastStatTick = now;
}

// ---------------------------------------------------------------------------
// Fenêtre
// ---------------------------------------------------------------------------
static HWND MakeCtl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND h = CreateWindowExW(cls == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text,
                             WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return h;
}

static std::vector<std::pair<HWND, RECT>> g_labels;

static void Layout(HWND wnd) {
    RECT rc;
    GetClientRect(wnd, &rc);
    const int m = S(12), rowH = S(26), gap = S(8);
    int x = m, y = m;
    auto place = [&](HWND h, int w, int height = 0) {
        MoveWindow(h, x, y, w, height ? height : rowH, TRUE);
        x += w + gap;
    };
    // Ligne 1
    place(g_labels[0].first, S(70));
    place(g.source, S(300), S(300));
    place(g_labels[1].first, S(60));
    place(g.manual, S(150));
    place(g.connect, S(110));
    // Ligne 2
    x = m;
    y += rowH + gap;
    place(g.camera, S(170), S(300));
    place(g.res, S(130), S(400));
    place(g.fps, S(170), S(300));
    place(g.codec, S(120), S(200));
    place(g_labels[2].first, S(50));
    place(g.bitrate, S(50));
    // Ligne 3
    x = m;
    y += rowH + gap;
    place(g.mirror, S(110));
    place(g.rotate, S(180));
    place(g_labels[3].first, S(70));
    place(g.smooth, S(190), S(200));
    // Aperçu et état
    y += rowH + gap;
    const int statusH = S(96);
    g.previewRect = {m, y, rc.right - m, rc.bottom - m - statusH - gap};
    MoveWindow(g.status, m, rc.bottom - m - statusH, rc.right - 2 * m, statusH, TRUE);
    InvalidateRect(wnd, nullptr, TRUE);
}

static void Paint(HWND wnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(wnd, &ps);
    RECT r = g.previewRect;
    HBRUSH bg = CreateSolidBrush(RGB(24, 26, 30));
    std::lock_guard<std::mutex> lk(g.previewLock);
    if (g.previewW > 0 && g.conn.Connected()) {
        int ox = r.left + ((r.right - r.left) - g.previewW) / 2, oy = r.top + ((r.bottom - r.top) - g.previewH) / 2;
        // Fond autour de l'image
        RECT bands[4] = {{r.left, r.top, r.right, oy},
                         {r.left, oy + g.previewH, r.right, r.bottom},
                         {r.left, oy, ox, oy + g.previewH},
                         {ox + g.previewW, oy, r.right, oy + g.previewH}};
        for (auto& b : bands) FillRect(dc, &b, bg);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = g.previewW;
        bi.bmiHeader.biHeight = -g.previewH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, ox, oy, DWORD(g.previewW), DWORD(g.previewH), 0, 0, 0, UINT(g.previewH),
                          g.preview.data(), &bi, DIB_RGB_COLORS);
    } else {
        FillRect(dc, &r, bg);
        SetTextColor(dc, RGB(160, 165, 175));
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old = SelectObject(dc, g.font);
        DrawTextW(dc, L"Aperçu", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
    }
    DeleteObject(bg);
    EndPaint(wnd, &ps);
}

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.wnd = wnd;
        g.scale = GetDpiForWindow(wnd) / 96.0f;
        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, GetDpiForWindow(wnd));
        g.font = CreateFontIndirectW(&ncm.lfMessageFont);

        g_labels.push_back({MakeCtl(wnd, L"STATIC", L"Téléphone :", SS_CENTERIMAGE, 0), {}});
        g.source = MakeCtl(wnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_SOURCE);
        g_labels.push_back({MakeCtl(wnd, L"STATIC", L"ou IP :", SS_CENTERIMAGE | SS_RIGHT, 0), {}});
        g.manual = MakeCtl(wnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_MANUAL);
        SendMessageW(g.manual, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"192.168.x.x"));
        g.connect = MakeCtl(wnd, L"BUTTON", L"Connecter", WS_TABSTOP, ID_CONNECT);

        g.camera = MakeCtl(wnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_CAMERA);
        g.res = MakeCtl(wnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_RES);
        g.fps = MakeCtl(wnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_FPS);
        g.codec = MakeCtl(wnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, ID_CODEC);
        g_labels.push_back({MakeCtl(wnd, L"STATIC", L"Mb/s :", SS_CENTERIMAGE | SS_RIGHT, 0), {}});
        g.bitrate = MakeCtl(wnd, L"EDIT", L"0", ES_NUMBER | WS_TABSTOP, ID_BITRATE);

        g.mirror = MakeCtl(wnd, L"BUTTON", L"Miroir", BS_AUTOCHECKBOX | WS_TABSTOP, ID_MIRROR);
        g.rotate = MakeCtl(wnd, L"BUTTON", L"Retourner (180°)", BS_AUTOCHECKBOX | WS_TABSTOP, ID_ROTATE);
        g_labels.push_back({MakeCtl(wnd, L"STATIC", L"Lissage :", SS_CENTERIMAGE | SS_RIGHT, 0), {}});
        g.smooth = MakeCtl(wnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, ID_SMOOTH);
        for (const wchar_t* s : {L"Aucun (latence minimale)", L"Auto (selon la gigue)", L"50 ms", L"100 ms", L"150 ms", L"250 ms"})
            SendMessageW(g.smooth, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        SendMessageW(g.smooth, CB_SETCURSEL, 0, 0);
        g.playout.Start([](const DecodedFrame& f) {
            if (g.writerReset.exchange(false)) g.writer.Close();
            g.consumer = g.writer.Write(f, g.optMirror, g.optRotate);
            UpdatePreview(f);
        });
        g.status = MakeCtl(wnd, L"STATIC", L"", SS_LEFT, ID_STATUS);

        EnableStreamControls(false);
        g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        TrayAdd();
        g.discovery.Start();
        RefreshSources();
        SetTimer(wnd, ID_TIMER_STATS, 1000, nullptr);
        SetTimer(wnd, ID_TIMER_DISCOVERY, 1500, nullptr);
        EnsureVirtualCamera();
        UpdateStats();
        return 0;
    }
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            HideToTray();
            return 0;
        }
        Layout(wnd);
        return 0;
    case WM_CLOSE:
        // La croix range dans la zone de notification : la webcam doit rester disponible.
        if (!g.quitting) {
            HideToTray();
            return 0;
        }
        DestroyWindow(wnd);
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            ShowMain();
            break;
        case WM_CONTEXTMENU:
            TrayMenu();
            break;
        }
        return 0;
    case WM_APP_SHOW:
        ShowMain();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {S(960), S(560)};
        return 0;
    }
    case WM_PAINT:
        Paint(wnd);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_TIMER:
        if (wp == ID_TIMER_STATS) UpdateStats();
        if (wp == ID_TIMER_DISCOVERY && !g.conn.Connected()) RefreshSources();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDM_OPEN) {
            ShowMain();
        } else if (id == IDM_QUIT) {
            g.quitting = true;
            PostMessageW(wnd, WM_CLOSE, 0, 0);
        } else if (id == ID_CONNECT || id == IDM_CONNECT) {
            g.phoneMessage.clear();
            if (g.conn.Connected()) Disconnect();
            else Connect();
            UpdateStats();
        } else if (id == IDOK) {
            // Entrée : valide le débit saisi.
            if (GetFocus() == g.bitrate && g.conn.Connected()) SendStart();
        } else if ((id == ID_CAMERA || id == ID_CODEC) && code == CBN_SELCHANGE) {
            // Chaque changement s'applique tout de suite, en gardant la taille et les i/s si possible.
            FillResolutions();
            SendStart();
        } else if (id == ID_RES && code == CBN_SELCHANGE) {
            FillFps();
            SendStart();
        } else if (id == ID_FPS && code == CBN_SELCHANGE) {
            SendStart();
        } else if (id == ID_BITRATE && code == EN_KILLFOCUS) {
            static std::wstring lastBitrate = L"0";
            std::wstring now = WindowText(g.bitrate);
            if (now != lastBitrate && g.conn.Connected()) SendStart();
            lastBitrate = now;
        } else if (id == ID_MIRROR) {
            g.optMirror = SendMessageW(g.mirror, BM_GETCHECK, 0, 0) == BST_CHECKED;
        } else if (id == ID_ROTATE) {
            g.optRotate = SendMessageW(g.rotate, BM_GETCHECK, 0, 0) == BST_CHECKED;
        } else if (id == ID_SMOOTH && code == CBN_SELCHANGE) {
            static const int delays[] = {0, -1, 50, 100, 150, 250};
            int i = int(SendMessageW(g.smooth, CB_GETCURSEL, 0, 0));
            if (i >= 0 && i < 6) g.playout.SetDelayMs(delays[i]);
        }
        return 0;
    }
    case WM_APP_HELLO: {
        std::unique_ptr<std::string> t(reinterpret_cast<std::string*>(lp));
        g.caps = PhoneCaps::Parse(*t);
        FillCameras();
        FillCodecs();
        FillResolutions();
        EnableStreamControls(true);
        if (g.caps.modes.empty()) {
            SetStatus(L"Le téléphone n'annonce aucun mode vidéo utilisable.");
        } else {
            SendStart(); // démarre directement avec le meilleur mode
        }
        return 0;
    }
    case WM_APP_FORMAT: {
        std::unique_ptr<std::string> t(reinterpret_cast<std::string*>(lp));
        g.formatText = *t;
        g.phoneMessage.clear();
        // Windows garde la source en mémoire : pour annoncer le nouveau format aux applications,
        // on recrée la caméra virtuelle, sauf si une application s'en sert (l'image est alors mise à l'échelle).
        auto kv = ParseKv(*t);
        uint32_t fw = uint32_t(atoi(kv["width"].c_str())), fh = uint32_t(atoi(kv["height"].c_str())),
                 ff = uint32_t(atoi(kv["fps"].c_str()));
        if (g.vcam.Active() && !g.vcam.SameFormat(fw, fh, ff) && !g.consumer) {
            g.writerReset = true; // le fil réseau rouvrira la mémoire partagée de la nouvelle source
            g.vcam.Remove();
        }
        EnsureVirtualCamera();
        UpdateStats();
        return 0;
    }
    case WM_APP_PHONE_ERROR: {
        std::unique_ptr<std::string> t(reinterpret_cast<std::string*>(lp));
        g.phoneMessage = W(*t);
        UpdateStats();
        return 0;
    }
    case WM_APP_CLOSED: {
        std::unique_ptr<std::string> t(reinterpret_cast<std::string*>(lp));
        Disconnect();
        g.phoneMessage = W(*t);
        UpdateStats();
        InvalidateRect(wnd, &g.previewRect, FALSE);
        return 0;
    }
    case WM_APP_PREVIEW:
        g.previewPending = false;
        InvalidateRect(wnd, &g.previewRect, FALSE);
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g.tray);
        if (g.tray.hIcon) DestroyIcon(g.tray.hIcon);
        KillTimer(wnd, ID_TIMER_STATS);
        KillTimer(wnd, ID_TIMER_DISCOVERY);
        g.conn.Disconnect();
        g.playout.Stop();
        g.discovery.Stop();
        g.decoder.Close();
        g.writer.Close();
        g.vcam.Remove();
        PostQuitMessage(0);
        return 0;
    }
    // Explorateur redémarré : l'icône de notification a disparu, on la remet.
    if (msg == g.taskbarCreated && g.taskbarCreated) {
        TrayAdd();
        return 0;
    }
    // Demande de fermeture de l'installeur (mise à jour / désinstallation).
    static const UINT quitMsg = RegisterWindowMessageW(L"PhoneCam.Quit");
    if (msg == quitMsg) {
        g.quitting = true;
        DestroyWindow(wnd);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show) {
    std::wstring args = cmdLine ? cmdLine : L"";
    if (args.find(L"--install") != std::wstring::npos) return installer::Install();
    if (args.find(L"--uninstall") != std::wstring::npos) return installer::Uninstall();

    // Premier lancement (ou nouvelle version) : installation de la caméra virtuelle.
    if (installer::Needed()) {
        int answer = MessageBoxW(nullptr,
                                 L"PhoneCam doit installer sa caméra virtuelle (une seule fois, droits administrateur).\n\n"
                                 L"L'application sera copiée dans Program Files et ajoutée au menu Démarrer.\n\nInstaller maintenant ?",
                                 L"PhoneCam", MB_YESNO | MB_ICONINFORMATION);
        if (answer != IDYES) return 0;
        int code = installer::RunElevated(L"--install");
        if (code != 0) {
            wchar_t msg[200];
            swprintf_s(msg, code < 0 ? L"Installation annulée." : L"L'installation a échoué (code %d).", code);
            MessageBoxW(nullptr, msg, L"PhoneCam", MB_OK | MB_ICONERROR);
            return 1;
        }
        // On continue depuis la copie installée (la règle de pare-feu vise ce chemin).
        if (_wcsicmp(installer::SelfPath().c_str(), installer::InstalledExe().c_str()) != 0) {
            ShellExecuteW(nullptr, L"open", installer::InstalledExe().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
    }

    // Une seule instance (la caméra virtuelle et le port de découverte sont uniques).
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\PhoneCamApp");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Déjà lancée (peut-être cachée dans la zone de notification) : on la réaffiche.
        HWND other = FindWindowW(L"PhoneCamWnd", nullptr);
        if (other) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(other, WM_APP_SHOW, 0, 0);
        }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    g.pcCodecs.insert("avc");
    if (VideoDecoder::IsSupported("hevc")) g.pcCodecs.insert("hevc");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"PhoneCamWnd";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    float scale = GetDpiForSystem() / 96.0f;
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"PhoneCam", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               int(1040 * scale), int(760 * scale), nullptr, nullptr, inst, nullptr);
    ShowWindow(wnd, show);
    UpdateWindow(wnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    WSACleanup();
    MFShutdown();
    CoUninitialize();
    CloseHandle(single);
    return 0;
}
