#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

constexpr uint16_t PHONECAM_PORT = 38500;
constexpr uint16_t PHONECAM_BEACON_PORT = 38501;

// Connexion TCP au téléphone (protocole décrit dans PROTOCOL.md).
class PhoneConnection {
public:
    struct Callbacks {
        std::function<void(const std::string&)> onHello;
        std::function<void(const std::string&)> onFormat;
        std::function<void(const std::vector<uint8_t>&)> onCodecConfig;
        std::function<void(const uint8_t*, size_t, bool key, int64_t ptsUs)> onFrame;
        std::function<void(const std::string&)> onPhoneError;
        std::function<void(const std::string&)> onClosed; // raison
    };

    ~PhoneConnection() { Disconnect(); }
    // Bloquant (délai de 3 s). Démarre ensuite le fil de réception.
    bool Connect(const std::string& host, uint16_t port, Callbacks cb, std::string* error);
    void Disconnect();
    bool Connected() const { return m_sock != INVALID_SOCKET; }
    bool Send(char type, const std::string& payload);

    std::atomic<uint64_t> bytesReceived{0};
    std::atomic<uint64_t> framesReceived{0};

private:
    void RecvLoop();
    bool RecvAll(void* buf, size_t len);

    SOCKET m_sock = INVALID_SOCKET;
    std::thread m_thread;
    std::mutex m_sendLock;
    Callbacks m_cb;
    std::atomic<bool> m_stop{false};
};

// Écoute les balises « PHONECAM name=… port=… » diffusées par les téléphones en Wi-Fi.
class Discovery {
public:
    struct Phone {
        std::string name;
        std::string address;
        uint16_t port;
        uint64_t lastSeen;
    };
    void Start();
    void Stop();
    std::vector<Phone> List(); // vus dans les 5 dernières secondes

private:
    void Loop();
    SOCKET m_sock = INVALID_SOCKET;
    std::thread m_thread;
    std::mutex m_lock;
    std::map<std::string, Phone> m_phones;
    std::atomic<bool> m_stop{false};
};

// USB : « adb forward tcp:38500 tcp:38500 ». Renvoie false avec un message si adb manque ou si aucun appareil.
bool AdbForward(std::string* message);
// Lance l'application PhoneCam sur le téléphone USB si elle ne tourne pas. Renvoie true si elle a dû être lancée.
bool AdbEnsureApp();
std::wstring FindAdb();
// Lance une commande sans fenêtre, récupère sa sortie et son code de retour.
bool RunHidden(const std::wstring& cmdline, std::string* output, unsigned long* exitCode);
