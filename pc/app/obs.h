#pragma once
// Flux réseau pour OBS : le PC rediffuse le flux du téléphone en MPEG-TS sur HTTP
// (http://IP-du-PC:38510/), sans réencodage. Même format que le flux OBS de l'application Android.
#include <winsock2.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

constexpr uint16_t PHONECAM_OBS_PORT = 38510;

// Multiplexeur MPEG-TS minimal : un programme, une piste vidéo H.264 ou HEVC, PTS à 90 kHz.
class TsMuxer {
public:
    bool SetCodec(const std::string& codec); // true si le codec a changé
    void Mux(const uint8_t* frame, size_t size, int64_t ptsUs, bool key, std::vector<uint8_t>& out);

private:
    void Section(std::vector<uint8_t>& out, int pid, const std::vector<uint8_t>& section);
    std::vector<uint8_t> Pat() const;
    std::vector<uint8_t> Pmt() const;
    uint8_t streamType_ = 0x1B;
    uint8_t version_ = 0;
    uint8_t cc_[3] = {}; // PAT, PMT, vidéo
};

class ObsServer {
public:
    ~ObsServer() { Stop(); }
    bool Start(std::string* error);
    void Stop();
    bool Running() const { return running_; }
    int Clients() const { return clientCount_; }
    void SetCodec(const std::string& codec);
    void PushFrame(const uint8_t* data, size_t size, int64_t ptsUs, bool key);
    // Appelé quand un lecteur arrive (il faut une image clé).
    std::function<void()> onClientJoined;

    static std::wstring LocalAddress(); // IPv4 du PC sur le réseau local

private:
    struct Client {
        SOCKET sock = INVALID_SOCKET;
        std::mutex lock;
        std::condition_variable cv;
        std::deque<std::shared_ptr<std::vector<uint8_t>>> queue;
        size_t queued = 0;
        bool ready = false;  // en-tête HTTP envoyé : peut recevoir le flux
        bool synced = false; // a reçu une image clé
        bool closed = false;
        std::thread thread;
    };
    void AcceptLoop();
    void ClientLoop(std::shared_ptr<Client> c);
    void Reap();
    void RecountLocked();

    SOCKET listen_ = INVALID_SOCKET;
    std::thread acceptThread_;
    std::atomic<bool> running_{false};
    std::atomic<int> clientCount_{0};
    std::mutex clientsLock_;
    std::vector<std::shared_ptr<Client>> clients_;
    std::mutex muxLock_;
    TsMuxer muxer_;
};
