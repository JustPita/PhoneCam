#include "obs.h"

#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>

// ---------------------------------------------------------------------------
// TsMuxer
// ---------------------------------------------------------------------------
static constexpr int PID_PAT = 0x0000, PID_PMT = 0x1000, PID_VIDEO = 0x0100;

static uint32_t Crc32Mpeg(const uint8_t* p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= uint32_t(p[i]) << 24;
        for (int b = 0; b < 8; ++b) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

static void AppendCrc(std::vector<uint8_t>& s) {
    uint32_t c = Crc32Mpeg(s.data(), s.size());
    s.push_back(uint8_t(c >> 24));
    s.push_back(uint8_t(c >> 16));
    s.push_back(uint8_t(c >> 8));
    s.push_back(uint8_t(c));
}

bool TsMuxer::SetCodec(const std::string& codec) {
    uint8_t t = codec == "hevc" ? 0x24 : 0x1B;
    if (t == streamType_) return false;
    streamType_ = t;
    version_ = uint8_t((version_ + 1) & 0x1F);
    return true;
}

std::vector<uint8_t> TsMuxer::Pat() const {
    std::vector<uint8_t> s = {0x00, 0xB0, 13, 0x00, 0x01, uint8_t(0xC1 | (version_ << 1)), 0, 0,
                              0x00, 0x01, uint8_t(0xE0 | (PID_PMT >> 8)), uint8_t(PID_PMT & 0xFF)};
    AppendCrc(s);
    return s;
}

std::vector<uint8_t> TsMuxer::Pmt() const {
    std::vector<uint8_t> s = {0x02, 0xB0, 18, 0x00, 0x01, uint8_t(0xC1 | (version_ << 1)), 0, 0,
                              uint8_t(0xE0 | (PID_VIDEO >> 8)), uint8_t(PID_VIDEO & 0xFF), 0xF0, 0,
                              streamType_, uint8_t(0xE0 | (PID_VIDEO >> 8)), uint8_t(PID_VIDEO & 0xFF), 0xF0, 0};
    AppendCrc(s);
    return s;
}

void TsMuxer::Section(std::vector<uint8_t>& out, int pid, const std::vector<uint8_t>& section) {
    uint8_t pkt[188];
    memset(pkt, 0xFF, sizeof(pkt));
    uint8_t& cc = cc_[pid == PID_PAT ? 0 : 1];
    pkt[0] = 0x47;
    pkt[1] = uint8_t(0x40 | (pid >> 8));
    pkt[2] = uint8_t(pid & 0xFF);
    pkt[3] = uint8_t(0x10 | (cc++ & 0x0F));
    pkt[4] = 0; // pointer_field
    memcpy(pkt + 5, section.data(), section.size());
    out.insert(out.end(), pkt, pkt + 188);
}

void TsMuxer::Mux(const uint8_t* frame, size_t size, int64_t ptsUs, bool key, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(size + size / 184 * 4 + 1024);
    if (key) {
        // PAT/PMT devant chaque image clé : un lecteur qui arrive peut démarrer là.
        Section(out, PID_PAT, Pat());
        Section(out, PID_PMT, Pmt());
    }
    const uint64_t pcr90 = uint64_t(ptsUs) * 9 / 100;
    const uint64_t pts = (pcr90 + 9000) & 0x1FFFFFFFFull; // 100 ms d'avance sur le PCR

    std::vector<uint8_t> pes;
    pes.reserve(size + 14);
    const uint8_t head[9] = {0, 0, 1, 0xE0, 0, 0, 0x80, 0x80, 5};
    pes.insert(pes.end(), head, head + 9);
    pes.push_back(uint8_t(0x21 | ((pts >> 29) & 0x0E)));
    pes.push_back(uint8_t(pts >> 22));
    pes.push_back(uint8_t((pts >> 14) | 1));
    pes.push_back(uint8_t(pts >> 7));
    pes.push_back(uint8_t((pts << 1) | 1));
    pes.insert(pes.end(), frame, frame + size);

    size_t pos = 0;
    bool first = true;
    while (pos < pes.size()) {
        uint8_t pkt[188];
        pkt[0] = 0x47;
        pkt[1] = uint8_t((first ? 0x40 : 0) | (PID_VIDEO >> 8));
        pkt[2] = uint8_t(PID_VIDEO & 0xFF);
        const size_t adaptMin = first ? 8 : 0; // PCR sur le 1er paquet
        const size_t payload = (std::min)(pes.size() - pos, 184 - adaptMin);
        const size_t adaptLen = 184 - payload;
        size_t hdr = 4;
        if (adaptLen > 0) {
            pkt[3] = uint8_t(0x30 | (cc_[2] & 0x0F));
            pkt[4] = uint8_t(adaptLen - 1);
            if (adaptLen > 1) {
                uint8_t flags = 0;
                if (first) flags |= 0x10 | (key ? 0x40 : 0);
                pkt[5] = flags;
                size_t i = 6;
                if (first) {
                    const uint64_t base = pcr90 & 0x1FFFFFFFFull;
                    pkt[6] = uint8_t(base >> 25);
                    pkt[7] = uint8_t(base >> 17);
                    pkt[8] = uint8_t(base >> 9);
                    pkt[9] = uint8_t(base >> 1);
                    pkt[10] = uint8_t(((base & 1) << 7) | 0x7E);
                    pkt[11] = 0;
                    i = 12;
                }
                while (i < 4 + adaptLen) pkt[i++] = 0xFF;
            }
            hdr = 4 + adaptLen;
        } else {
            pkt[3] = uint8_t(0x10 | (cc_[2] & 0x0F));
        }
        cc_[2]++;
        memcpy(pkt + hdr, pes.data() + pos, payload);
        pos += payload;
        first = false;
        out.insert(out.end(), pkt, pkt + 188);
    }
}

// ---------------------------------------------------------------------------
// ObsServer
// ---------------------------------------------------------------------------
static constexpr size_t kMaxQueued = 16u * 1024 * 1024;

bool ObsServer::Start(std::string* error) {
    if (running_) return true;
    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL reuse = TRUE;
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(PHONECAM_OBS_PORT);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(listen_, 8) != 0) {
        if (error) *error = "Port 38510 déjà utilisé : flux OBS indisponible.";
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }
    running_ = true;
    acceptThread_ = std::thread(&ObsServer::AcceptLoop, this);
    return true;
}

void ObsServer::Stop() {
    if (!running_) return;
    running_ = false;
    closesocket(listen_);
    listen_ = INVALID_SOCKET;
    if (acceptThread_.joinable()) acceptThread_.join();
    std::vector<std::shared_ptr<Client>> all;
    {
        std::lock_guard<std::mutex> lk(clientsLock_);
        all.swap(clients_);
    }
    for (auto& c : all) {
        {
            std::lock_guard<std::mutex> lk(c->lock);
            c->closed = true;
        }
        c->cv.notify_all();
        shutdown(c->sock, SD_BOTH);
        closesocket(c->sock);
        if (c->thread.joinable()) c->thread.join();
    }
    clientCount_ = 0;
}

void ObsServer::SetCodec(const std::string& codec) {
    bool changed;
    {
        std::lock_guard<std::mutex> lk(muxLock_);
        changed = muxer_.SetCodec(codec);
    }
    if (!changed) return;
    // Changement de codec : on coupe les lecteurs, OBS se reconnecte tout seul.
    std::lock_guard<std::mutex> lk(clientsLock_);
    for (auto& c : clients_) shutdown(c->sock, SD_BOTH);
}

void ObsServer::PushFrame(const uint8_t* data, size_t size, int64_t ptsUs, bool key) {
    if (!running_ || clientCount_ == 0) return;
    auto ts = std::make_shared<std::vector<uint8_t>>();
    {
        std::lock_guard<std::mutex> lk(muxLock_);
        muxer_.Mux(data, size, ptsUs, key, *ts);
    }
    std::lock_guard<std::mutex> lk(clientsLock_);
    for (auto& c : clients_) {
        std::lock_guard<std::mutex> cl(c->lock);
        if (!c->ready || c->closed) continue;
        if (!c->synced) {
            if (!key) continue;
            c->synced = true;
        }
        if (c->queued > kMaxQueued) {
            // Lecteur trop lent : on vide et on repart à la prochaine image clé.
            c->queue.clear();
            c->queued = 0;
            c->synced = false;
            continue;
        }
        c->queued += ts->size();
        c->queue.push_back(ts);
        c->cv.notify_one();
    }
}

void ObsServer::Reap() {
    std::vector<std::shared_ptr<Client>> done;
    {
        std::lock_guard<std::mutex> lk(clientsLock_);
        for (auto it = clients_.begin(); it != clients_.end();) {
            bool closed;
            {
                std::lock_guard<std::mutex> cl((*it)->lock);
                closed = (*it)->closed;
            }
            if (closed) {
                done.push_back(*it);
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
        RecountLocked();
    }
    // Hors verrou : le fil d'un lecteur peut encore avoir besoin de clientsLock_ pour finir.
    for (auto& c : done) {
        if (c->thread.joinable()) c->thread.join();
        closesocket(c->sock);
    }
}

void ObsServer::AcceptLoop() {
    while (running_) {
        SOCKET s = accept(listen_, nullptr, nullptr);
        if (s == INVALID_SOCKET) break;
        Reap();
        // Le lecteur est suivi dès son arrivée : son fil est toujours rejoint (Reap ou Stop).
        auto c = std::make_shared<Client>();
        c->sock = s;
        {
            std::lock_guard<std::mutex> lk(clientsLock_);
            clients_.push_back(c);
        }
        c->thread = std::thread(&ObsServer::ClientLoop, this, c);
    }
}

// À appeler avec clientsLock_ tenu : nombre de lecteurs qui reçoivent le flux.
void ObsServer::RecountLocked() {
    int n = 0;
    for (auto& x : clients_) {
        std::lock_guard<std::mutex> cl(x->lock);
        if (x->ready && !x->closed) ++n;
    }
    clientCount_ = n;
}

void ObsServer::ClientLoop(std::shared_ptr<Client> c) {
    // Lecture de la requête HTTP (le chemin est ignoré : tout renvoie le flux).
    DWORD timeout = 5000;
    setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::string req;
    char buf[1024];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
        int n = recv(c->sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, size_t(n));
    }
    if (req.find("\r\n\r\n") == std::string::npos) {
        std::lock_guard<std::mutex> lk(c->lock);
        c->closed = true; // socket fermée par Reap/Stop
        return;
    }
    static const char kHead[] =
        "HTTP/1.0 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
    send(c->sock, kHead, int(sizeof(kHead) - 1), 0);
    BOOL nodelay = TRUE;
    setsockopt(c->sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    {
        std::lock_guard<std::mutex> lk(c->lock);
        c->ready = true;
    }
    {
        std::lock_guard<std::mutex> lk(clientsLock_);
        RecountLocked();
    }
    if (onClientJoined) onClientJoined();

    for (;;) {
        std::shared_ptr<std::vector<uint8_t>> chunk;
        {
            std::unique_lock<std::mutex> lk(c->lock);
            c->cv.wait_for(lk, std::chrono::seconds(1), [&] { return c->closed || !c->queue.empty(); });
            if (c->closed) break;
            if (c->queue.empty()) continue;
            chunk = c->queue.front();
            c->queue.pop_front();
            c->queued -= chunk->size();
        }
        const char* p = reinterpret_cast<const char*>(chunk->data());
        size_t left = chunk->size();
        bool ok = true;
        while (left > 0) {
            int n = send(c->sock, p, int((std::min)(left, size_t(1) << 20)), 0);
            if (n <= 0) {
                ok = false;
                break;
            }
            p += n;
            left -= size_t(n);
        }
        if (!ok) break;
    }
    {
        std::lock_guard<std::mutex> lk(c->lock);
        c->closed = true;
    }
    {
        // Décompte immédiat ; la socket et le fil sont libérés au prochain Reap (ou Stop).
        std::lock_guard<std::mutex> lk(clientsLock_);
        RecountLocked();
    }
}

std::wstring ObsServer::LocalAddress() {
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, aa, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, aa, &size) != NO_ERROR) return L"";
    }
    std::wstring fallback;
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            wchar_t ip[64];
            DWORD len = 64;
            if (WSAAddressToStringW(u->Address.lpSockaddr, u->Address.iSockaddrLength, nullptr, ip, &len) != 0) continue;
            // L'adaptateur qui a une passerelle est celui du réseau local (pas Hyper-V, VPN sans passerelle…).
            if (a->FirstGatewayAddress) return ip;
            if (fallback.empty()) fallback = ip;
        }
    }
    return fallback;
}
