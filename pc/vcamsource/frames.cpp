#include "vcamsource.h"

#include <sddl.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------------------
// Format annoncé : lu dans C:\ProgramData\PhoneCam\format.txt (« largeur hauteur fps »)
// ---------------------------------------------------------------------------
SourceFormat SourceFormat::Load() {
    SourceFormat f;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, PHONECAM_FORMAT_FILE, L"r") == 0 && fp) {
        unsigned w = 0, h = 0, fps = 0;
        if (fscanf_s(fp, "%u %u %u", &w, &h, &fps) == 3 && w >= 160 && h >= 120 &&
            uint64_t(w) * h <= PHONECAM_MAX_PIXELS && fps >= 1 && fps <= 960) {
            f.width = w & ~1u;
            f.height = h & ~1u;
            f.fps = fps;
        }
        fclose(fp);
    }
    return f;
}

// ---------------------------------------------------------------------------
// FrameReader
// ---------------------------------------------------------------------------
bool FrameReader::Open(uint32_t width, uint32_t height, uint32_t fps) {
    Close();
    const size_t bytes = PhoneCamMappingBytes();

    // Accès complet pour SYSTEM, LocalService, les administrateurs et l'utilisateur interactif.
    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;IU)",
                                                         SDDL_REVISION_1, &sd, nullptr);
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};

    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE,
                                   DWORD(uint64_t(bytes) >> 32), DWORD(bytes), PHONECAM_MAPPING_NAME);
    bool created = m_mapping && GetLastError() != ERROR_ALREADY_EXISTS;
    if (!m_mapping) {
        // Pas le privilège de créer un objet global (DLL chargée hors du service) : on ouvre l'existant.
        m_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, PHONECAM_MAPPING_NAME);
    }
    m_event = CreateEventW(sd ? &sa : nullptr, FALSE, FALSE, PHONECAM_EVENT_NAME);
    if (!m_event) m_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, PHONECAM_EVENT_NAME);
    if (sd) LocalFree(sd);

    if (!m_mapping) {
        Log(L"FrameReader: mapping impossible (%lu)", GetLastError());
        return false;
    }
    m_header = static_cast<PhoneCamHeader*>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!m_header) {
        Close();
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(m_header, &mbi, sizeof(mbi));
    if (mbi.RegionSize < bytes) {
        Log(L"FrameReader: mapping existant trop petit");
        Close();
        return false;
    }
    if (created || m_header->magic != PHONECAM_MAGIC || m_header->width != width || m_header->height != height) {
        m_header->magic = 0;
        MemoryBarrier();
        m_header->version = PHONECAM_VERSION;
        m_header->width = width;
        m_header->height = height;
        m_header->fps = fps;
        m_header->latest = -1;
        m_header->reading = -1;
        m_header->frameCounter = 0;
        m_header->lastWriteTick = 0;
        m_header->lastReadTick = 0;
        MemoryBarrier();
        m_header->magic = PHONECAM_MAGIC;
    }
    Log(L"FrameReader: ouvert %ux%u@%u (créé=%d)", width, height, fps, created ? 1 : 0);
    return true;
}

void FrameReader::Close() {
    if (m_header) UnmapViewOfFile(m_header);
    if (m_mapping) CloseHandle(m_mapping);
    if (m_event) CloseHandle(m_event);
    m_header = nullptr;
    m_mapping = nullptr;
    m_event = nullptr;
}

bool FrameReader::WaitNew(LONG64 lastCounter, LONGLONG deadline) {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    for (;;) {
        if (m_header && m_header->frameCounter != lastCounter) return true;
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= deadline) return false;
        LONGLONG remainingMs = (deadline - now.QuadPart) * 1000 / freq.QuadPart;
        // Attente courte puis attente active fine sur la dernière milliseconde.
        DWORD wait = DWORD(remainingMs > 2 ? std::min<LONGLONG>(remainingMs - 1, 5) : 0);
        if (m_event)
            WaitForSingleObject(m_event, wait);
        else
            Sleep(wait);
    }
}

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------
static inline uint8_t Clamp8(int v) { return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v)); }

static void Nv12ToNv12(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, LONG pitch, uint32_t dw,
                       uint32_t dh) {
    const uint8_t* srcUV = src + size_t(sw) * sh;
    uint8_t* dstUV = dst + LONG_PTR(pitch) * dh;
    if (sw == dw && sh == dh) {
        for (uint32_t y = 0; y < dh; ++y) memcpy(dst + LONG_PTR(pitch) * y, src + size_t(sw) * y, dw);
        for (uint32_t y = 0; y < dh / 2; ++y) memcpy(dstUV + LONG_PTR(pitch) * y, srcUV + size_t(sw) * y, dw);
        return;
    }
    if (sw == dw * 2 && sh == dh * 2) {
        // Réduction exacte par 2 : moyenne 2×2 (pas de crénelage).
        for (uint32_t y = 0; y < dh; ++y) {
            const uint8_t* a = src + size_t(sw) * (y * 2);
            const uint8_t* b = a + sw;
            uint8_t* d = dst + LONG_PTR(pitch) * y;
            for (uint32_t x = 0; x < dw; ++x) d[x] = uint8_t((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
        }
        for (uint32_t y = 0; y < dh / 2; ++y) {
            const uint8_t* a = srcUV + size_t(sw) * (y * 2);
            const uint8_t* b = a + sw;
            uint8_t* d = dstUV + LONG_PTR(pitch) * y;
            for (uint32_t x = 0; x < dw / 2; ++x) {
                d[2 * x] = uint8_t((a[4 * x] + a[4 * x + 2] + b[4 * x] + b[4 * x + 2] + 2) >> 2);
                d[2 * x + 1] = uint8_t((a[4 * x + 1] + a[4 * x + 3] + b[4 * x + 1] + b[4 * x + 3] + 2) >> 2);
            }
        }
        return;
    }
    // Cas général : plus proche voisin.
    std::vector<uint32_t> xmap(dw);
    for (uint32_t x = 0; x < dw; ++x) xmap[x] = uint32_t(uint64_t(x) * sw / dw);
    for (uint32_t y = 0; y < dh; ++y) {
        const uint8_t* s = src + size_t(sw) * (uint64_t(y) * sh / dh);
        uint8_t* d = dst + LONG_PTR(pitch) * y;
        for (uint32_t x = 0; x < dw; ++x) d[x] = s[xmap[x]];
    }
    for (uint32_t y = 0; y < dh / 2; ++y) {
        const uint8_t* s = srcUV + size_t(sw) * (uint64_t(y) * (sh / 2) / (dh / 2));
        uint8_t* d = dstUV + LONG_PTR(pitch) * y;
        for (uint32_t x = 0; x < dw / 2; ++x) {
            uint32_t sx = (xmap[x * 2] / 2) * 2;
            d[2 * x] = s[sx];
            d[2 * x + 1] = s[sx + 1];
        }
    }
}

// BT.709, plage limitée (ce que produisent les encodeurs des téléphones en HD).
static void Nv12ToRgb32(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, LONG pitch, uint32_t dw,
                        uint32_t dh) {
    const uint8_t* srcUV = src + size_t(sw) * sh;
    for (uint32_t y = 0; y < dh; ++y) {
        uint32_t sy = uint32_t(uint64_t(y) * sh / dh);
        const uint8_t* ys = src + size_t(sw) * sy;
        const uint8_t* uvs = srcUV + size_t(sw) * (sy / 2);
        uint32_t* d = reinterpret_cast<uint32_t*>(dst + LONG_PTR(pitch) * y);
        for (uint32_t x = 0; x < dw; ++x) {
            uint32_t sx = uint32_t(uint64_t(x) * sw / dw);
            int c = (ys[sx] - 16) * 298;
            int u = uvs[(sx & ~1u)] - 128;
            int v = uvs[(sx & ~1u) + 1] - 128;
            int r = (c + 459 * v + 128) >> 8;
            int g = (c - 55 * u - 136 * v + 128) >> 8;
            int b = (c + 541 * u + 128) >> 8;
            d[x] = 0xFF000000u | (uint32_t(Clamp8(r)) << 16) | (uint32_t(Clamp8(g)) << 8) | Clamp8(b);
        }
    }
}

void ConvertFrame(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, LONG dstPitch, uint32_t dw, uint32_t dh,
                  bool rgb32) {
    if (rgb32)
        Nv12ToRgb32(src, sw, sh, dst, dstPitch, dw, dh);
    else
        Nv12ToNv12(src, sw, sh, dst, dstPitch, dw, dh);
}

// Image d'attente : gris sombre avec une barre qui se déplace (montre que le flux est vivant).
void FillPlaceholder(uint8_t* dst, LONG pitch, uint32_t dw, uint32_t dh, bool rgb32, uint64_t tick) {
    uint32_t barX = uint32_t((tick / 8) % (dw ? dw : 1));
    uint32_t barW = std::max<uint32_t>(dw / 40, 4);
    uint32_t y0 = dh * 45 / 100, y1 = dh * 55 / 100;
    for (uint32_t y = 0; y < dh; ++y) {
        uint8_t* row = dst + LONG_PTR(pitch) * y;
        bool band = y >= y0 && y < y1;
        if (rgb32) {
            uint32_t* d = reinterpret_cast<uint32_t*>(row);
            for (uint32_t x = 0; x < dw; ++x)
                d[x] = (band && x >= barX && x < barX + barW) ? 0xFF1E88E5u : 0xFF202428u;
        } else {
            memset(row, 40, dw);
            if (band) memset(row + barX, 120, std::min(barW, dw - barX));
        }
    }
    if (!rgb32) {
        uint8_t* uv = dst + LONG_PTR(pitch) * dh;
        for (uint32_t y = 0; y < dh / 2; ++y) {
            uint8_t* row = uv + LONG_PTR(pitch) * y;
            memset(row, 128, dw);
            uint32_t yy = y * 2;
            if (yy >= y0 && yy < y1) {
                for (uint32_t x = barX & ~1u; x < std::min(barX + barW, dw); x += 2) {
                    row[x] = 180;     // U
                    row[x + 1] = 90;  // V
                }
            }
        }
    }
}
