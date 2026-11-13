#include "net.h"

#include <windows.h>
#include <shlwapi.h>

#include <sstream>

// ---------------------------------------------------------------------------
// PhoneConnection
// ---------------------------------------------------------------------------
bool PhoneConnection::Connect(const std::string& host, uint16_t port, Callbacks cb, std::string* error) {
    Disconnect();
    m_cb = std::move(cb);

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        if (error) *error = "Adresse invalide : " + host;
        return false;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, res->ai_addr, int(res->ai_addrlen));
    freeaddrinfo(res);

    fd_set wr, ex;
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(s, &wr);
    FD_SET(s, &ex);
    timeval tv{3, 0};
    int r = select(0, nullptr, &wr, &ex, &tv);
    if (r <= 0 || FD_ISSET(s, &ex)) {
        closesocket(s);
        if (error) *error = r == 0 ? "Délai dépassé : le téléphone ne répond pas sur " + host
                                   : "Connexion refusée par " + host + " (application PhoneCam lancée ?)";
        return false;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    int rcvbuf = 8 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

    m_sock = s;
    m_stop = false;
    bytesReceived = 0;
    framesReceived = 0;
    m_thread = std::thread(&PhoneConnection::RecvLoop, this);
    return true;
}

void PhoneConnection::Disconnect() {
    m_stop = true;
    if (m_sock != INVALID_SOCKET) {
        shutdown(m_sock, SD_BOTH);
        closesocket(m_sock);
    }
    if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id()) m_thread.join();
    m_sock = INVALID_SOCKET;
}

bool PhoneConnection::Send(char type, const std::string& payload) {
    std::lock_guard<std::mutex> lk(m_sendLock);
    if (m_sock == INVALID_SOCKET) return false;
    std::string msg;
    msg.reserve(5 + payload.size());
    msg.push_back(type);
    uint32_t len = htonl(uint32_t(payload.size()));
    msg.append(reinterpret_cast<const char*>(&len), 4);
    msg += payload;
    return send(m_sock, msg.data(), int(msg.size()), 0) == int(msg.size());
}

bool PhoneConnection::RecvAll(void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    while (len > 0) {
        int n = recv(m_sock, p, int(len > 1 << 20 ? 1 << 20 : len), 0);
        if (n <= 0) return false;
        p += n;
        len -= size_t(n);
        bytesReceived += uint64_t(n);
    }
    return true;
}

void PhoneConnection::RecvLoop() {
    std::vector<uint8_t> payload;
    std::string reason = "Connexion fermée par le téléphone";
    for (;;) {
        uint8_t head[5];
        if (!RecvAll(head, 5)) break;
        uint32_t len = (uint32_t(head[1]) << 24) | (uint32_t(head[2]) << 16) | (uint32_t(head[3]) << 8) | head[4];
        if (len > 64u * 1024 * 1024) {
            reason = "Message trop long : flux corrompu";
            break;
        }
        payload.resize(len);
        if (len && !RecvAll(payload.data(), len)) break;
        switch (char(head[0])) {
        case 'H':
            if (m_cb.onHello) m_cb.onHello(std::string(payload.begin(), payload.end()));
            break;
        case 'F':
            if (m_cb.onFormat) m_cb.onFormat(std::string(payload.begin(), payload.end()));
            break;
        case 'C':
            if (m_cb.onCodecConfig) m_cb.onCodecConfig(payload);
            break;
        case 'V':
            if (len >= 9) {
                bool key = payload[0] & 1;
                int64_t pts = 0;
                for (int i = 0; i < 8; ++i) pts = (pts << 8) | payload[1 + i];
                framesReceived++;
                if (m_cb.onFrame) m_cb.onFrame(payload.data() + 9, len - 9, key, pts);
            }
            break;
        case 'E':
            if (m_cb.onPhoneError) m_cb.onPhoneError(std::string(payload.begin(), payload.end()));
            break;
        default:
            break;
        }
    }
    if (!m_stop && m_cb.onClosed) m_cb.onClosed(reason);
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------
void Discovery::Start() {
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL reuse = TRUE;
    setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(PHONECAM_BEACON_PORT);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_sock, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return;
    }
    DWORD timeout = 500;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    m_stop = false;
    m_thread = std::thread(&Discovery::Loop, this);
}

void Discovery::Stop() {
    m_stop = true;
    if (m_sock != INVALID_SOCKET) closesocket(m_sock);
    if (m_thread.joinable()) m_thread.join();
    m_sock = INVALID_SOCKET;
}

void Discovery::Loop() {
    char buf[512];
    while (!m_stop) {
        sockaddr_in from{};
        int fl = sizeof(from);
        int n = recvfrom(m_sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        std::string text(buf);
        if (text.rfind("PHONECAM ", 0) != 0) continue;
        Phone p;
        p.port = PHONECAM_PORT;
        auto namePos = text.find("name=");
        auto portPos = text.find(" port=");
        if (namePos != std::string::npos) {
            p.name = text.substr(namePos + 5, portPos == std::string::npos ? std::string::npos : portPos - namePos - 5);
        }
        if (portPos != std::string::npos) p.port = uint16_t(atoi(text.c_str() + portPos + 6));
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        p.address = ip;
        p.lastSeen = GetTickCount64();
        std::lock_guard<std::mutex> lk(m_lock);
        m_phones[p.address] = p;
    }
}

std::vector<Discovery::Phone> Discovery::List() {
    std::vector<Phone> out;
    std::lock_guard<std::mutex> lk(m_lock);
    uint64_t now = GetTickCount64();
    for (auto& [k, p] : m_phones)
        if (now - p.lastSeen < 5000) out.push_back(p);
    return out;
}

// ---------------------------------------------------------------------------
// adb
// ---------------------------------------------------------------------------
std::wstring FindAdb() {
    wchar_t path[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, path, nullptr)) return path;
    const wchar_t* candidates[] = {L"%LOCALAPPDATA%\\Android\\Sdk\\platform-tools\\adb.exe",
                                   L"%ANDROID_HOME%\\platform-tools\\adb.exe",
                                   L"%USERPROFILE%\\Documents\\platform-tools\\adb.exe"};
    for (auto c : candidates) {
        ExpandEnvironmentStringsW(c, path, MAX_PATH);
        if (PathFileExistsW(path)) return path;
    }
    return L"";
}

bool RunHidden(const std::wstring& cmdline, std::string* output, DWORD* exitCode) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmdline;
    BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }
    char buf[1024];
    DWORD n;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) output->append(buf, n);
    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return true;
}

bool AdbEnsureApp() {
    std::wstring adb = FindAdb();
    if (adb.empty()) return false;
    std::string out;
    DWORD code = 1;
    RunHidden(L"\"" + adb + L"\" shell pidof fr.justpita.phonecam", &out, &code);
    if (code == 0 && !out.empty()) return false; // déjà lancée
    out.clear();
    RunHidden(L"\"" + adb + L"\" shell am start -n fr.justpita.phonecam/.MainActivity", &out, &code);
    return code == 0;
}

bool AdbForward(std::string* message) {
    std::wstring adb = FindAdb();
    if (adb.empty()) {
        *message = "adb introuvable : installez les platform-tools Android ou ajoutez adb au PATH.";
        return false;
    }
    std::string out;
    DWORD code = 1;
    if (!RunHidden(L"\"" + adb + L"\" devices", &out, &code)) {
        *message = "Impossible de lancer adb.";
        return false;
    }
    std::istringstream lines(out);
    std::string line;
    int devices = 0, unauthorized = 0;
    while (std::getline(lines, line)) {
        if (line.find("\tdevice") != std::string::npos) devices++;
        if (line.find("\tunauthorized") != std::string::npos) unauthorized++;
    }
    if (devices == 0) {
        *message = unauthorized ? "Téléphone non autorisé : acceptez la demande de débogage USB sur l'écran du téléphone."
                                : "Aucun téléphone en USB (câble branché et débogage USB activé ?).";
        return false;
    }
    if (devices > 1) {
        *message = "Plusieurs appareils adb : débranchez les autres.";
        return false;
    }
    out.clear();
    std::wstring cmd = L"\"" + adb + L"\" forward tcp:" + std::to_wstring(PHONECAM_PORT) + L" tcp:" +
                       std::to_wstring(PHONECAM_PORT);
    if (!RunHidden(cmd, &out, &code) || code != 0) {
        *message = "adb forward a échoué : " + out;
        return false;
    }
    return true;
}
