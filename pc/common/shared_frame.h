// Mémoire partagée entre PhoneCam.exe (écrivain) et PhoneCamSource.dll (lecteur, chargée par
// le service Frame Server de Windows, en session 0 sous LocalService).
//
// C'est la DLL qui crée l'objet dans l'espace « Global\ » (le service a le privilège
// SeCreateGlobalPrivilege, pas l'application) avec une ACL qui autorise l'utilisateur interactif.
// L'application l'ouvre ensuite en écriture.
#pragma once
#include <windows.h>
#include <cstdint>

// {A3561806-3B96-4B7C-A0D4-32F548E3A851}
static const GUID CLSID_PhoneCamSource = {0xa3561806, 0x3b96, 0x4b7c, {0xa0, 0xd4, 0x32, 0xf5, 0x48, 0xe3, 0xa8, 0x51}};
#define PHONECAM_CLSID_STRING L"{A3561806-3B96-4B7C-A0D4-32F548E3A851}"

#define PHONECAM_MAPPING_NAME L"Global\\PhoneCamFrames"
#define PHONECAM_EVENT_NAME L"Global\\PhoneCamNewFrame"
#define PHONECAM_FRIENDLY_NAME L"PhoneCam"

// Fichier lu par la DLL à son démarrage : « largeur hauteur fps ». Écrit par l'application.
#define PHONECAM_FORMAT_DIR L"C:\\ProgramData\\PhoneCam"
#define PHONECAM_FORMAT_FILE L"C:\\ProgramData\\PhoneCam\\format.txt"

constexpr uint32_t PHONECAM_MAGIC = 0x4D414350; // 'PCAM'
constexpr uint32_t PHONECAM_VERSION = 1;
constexpr int PHONECAM_BUFFERS = 3;

#pragma pack(push, 8)
struct PhoneCamHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;   // NV12, pas = largeur, plan UV juste après le plan Y
    uint32_t height;
    uint32_t fps;
    uint32_t reserved0;
    volatile LONG latest;          // index du dernier tampon complet, -1 si aucun
    volatile LONG reading;         // index lu en ce moment par la DLL, -1 sinon
    volatile LONG64 frameCounter;  // incrémenté à chaque image publiée
    volatile LONG64 lastWriteTick; // GetTickCount64() de la dernière écriture
    volatile LONG64 lastReadTick;  // GetTickCount64() de la dernière image servie à une application
    uint8_t pad[64 - 56];
};
#pragma pack(pop)
static_assert(sizeof(PhoneCamHeader) == 64, "en-tête de 64 octets");

// Capacité fixe : la taille d'image annoncée peut changer sans recréer l'objet partagé.
constexpr uint64_t PHONECAM_MAX_PIXELS = 4096ull * 3072ull;

inline size_t PhoneCamFrameBytes(uint32_t w, uint32_t h) { return size_t(w) * h * 3 / 2; }
inline size_t PhoneCamMappingBytes() {
    return sizeof(PhoneCamHeader) + size_t(PHONECAM_MAX_PIXELS * 3 / 2) * PHONECAM_BUFFERS;
}
inline uint8_t* PhoneCamFrame(PhoneCamHeader* h, int index) {
    return reinterpret_cast<uint8_t*>(h) + sizeof(PhoneCamHeader) + PhoneCamFrameBytes(h->width, h->height) * index;
}
