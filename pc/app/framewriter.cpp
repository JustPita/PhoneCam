#include "framewriter.h"

#include <cstring>
#include <vector>

void FrameWriter::Close() {
    if (m_header) UnmapViewOfFile(m_header);
    if (m_mapping) CloseHandle(m_mapping);
    if (m_event) CloseHandle(m_event);
    m_header = nullptr;
    m_mapping = nullptr;
    m_event = nullptr;
}

bool FrameWriter::TryOpen() {
    ULONGLONG now = GetTickCount64();
    if (now - m_lastTry < 500) return false;
    m_lastTry = now;
    m_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, PHONECAM_MAPPING_NAME);
    if (!m_mapping) return false;
    m_header = static_cast<PhoneCamHeader*>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!m_header) {
        Close();
        return false;
    }
    m_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, PHONECAM_EVENT_NAME);
    return true;
}

// Copie une image NV12 (avec miroir / rotation à 180° / mise à l'échelle) vers dst (pas = dw).
static void CopyNv12(const DecodedFrame& f, uint8_t* dst, uint32_t dw, uint32_t dh, bool mirror, bool rot180) {
    const uint32_t sw = f.width, sh = f.height;
    const bool flipX = mirror != rot180; // rotation 180° = miroir horizontal + vertical
    const bool flipY = rot180;
    uint8_t* dstUV = dst + size_t(dw) * dh;

    if (sw == dw && sh == dh && !flipX) {
        for (uint32_t y = 0; y < dh; ++y) {
            uint32_t sy = flipY ? dh - 1 - y : y;
            memcpy(dst + size_t(dw) * y, f.y + LONG_PTR(f.pitch) * sy, dw);
        }
        for (uint32_t y = 0; y < dh / 2; ++y) {
            uint32_t sy = flipY ? dh / 2 - 1 - y : y;
            memcpy(dstUV + size_t(dw) * y, f.uv + LONG_PTR(f.pitch) * sy, dw);
        }
        return;
    }

    std::vector<uint32_t> xmap(dw);
    for (uint32_t x = 0; x < dw; ++x) {
        uint32_t sx = uint32_t(uint64_t(x) * sw / dw);
        xmap[x] = flipX ? sw - 1 - sx : sx;
    }
    for (uint32_t y = 0; y < dh; ++y) {
        uint32_t sy = uint32_t(uint64_t(y) * sh / dh);
        if (flipY) sy = sh - 1 - sy;
        const uint8_t* s = f.y + LONG_PTR(f.pitch) * sy;
        uint8_t* d = dst + size_t(dw) * y;
        for (uint32_t x = 0; x < dw; ++x) d[x] = s[xmap[x]];
    }
    for (uint32_t y = 0; y < dh / 2; ++y) {
        uint32_t sy = uint32_t(uint64_t(y) * (sh / 2) / (dh / 2));
        if (flipY) sy = sh / 2 - 1 - sy;
        const uint8_t* s = f.uv + LONG_PTR(f.pitch) * sy;
        uint8_t* d = dstUV + size_t(dw) * y;
        for (uint32_t x = 0; x < dw / 2; ++x) {
            uint32_t sx = xmap[x * 2] & ~1u;
            d[2 * x] = s[sx];
            d[2 * x + 1] = s[sx + 1];
        }
    }
}

bool FrameWriter::Write(const DecodedFrame& f, bool mirror, bool rotate180) {
    if (!m_header && !TryOpen()) return false;
    PhoneCamHeader* h = m_header;
    if (h->magic != PHONECAM_MAGIC || !h->width || !h->height ||
        uint64_t(h->width) * h->height > PHONECAM_MAX_PIXELS) {
        return false;
    }
    // Tampon cible : ni le plus récent, ni celui en cours de lecture.
    LONG latest = h->latest, reading = h->reading;
    LONG target = 0;
    while (target == latest || target == reading) ++target;

    CopyNv12(f, PhoneCamFrame(h, target), h->width, h->height, mirror, rotate180);

    MemoryBarrier();
    InterlockedExchange(&h->latest, target);
    InterlockedIncrement64(&h->frameCounter);
    InterlockedExchange64(&h->lastWriteTick, LONG64(GetTickCount64()));
    if (m_event) SetEvent(m_event);
    return Consumer();
}

bool FrameWriter::Consumer() const {
    return m_header && GetTickCount64() - (ULONGLONG)m_header->lastReadTick < 2000;
}
