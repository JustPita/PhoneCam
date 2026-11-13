#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "decoder.h"

// Tampon de lissage : ressort les images décodées au rythme de leur horodatage (pts du téléphone),
// avec un retard fixe, pour absorber la gigue du Wi-Fi. Retard 0 = image publiée dès qu'elle est décodée.
//
// Horloge : on garde le plus petit (arrivée - pts) observé sur une fenêtre glissante ; c'est le
// trajet le plus rapide. Une image est publiée à pts + ce décalage + retard.
class Playout {
public:
    using Sink = std::function<void(const DecodedFrame&)>;
    ~Playout() { Stop(); }
    void Start(Sink sink);
    void Stop();
    void Reset();
    // ms >= 0 : retard fixe. ms < 0 : automatique (s'adapte à la gigue mesurée).
    void SetDelayMs(int ms) {
        m_auto = ms < 0;
        if (!m_auto) m_delayUs = int64_t(ms) * 1000;
    }
    int DelayMs() const { return int(m_delayUs / 1000); }
    bool Auto() const { return m_auto; }
    // Copie l'image (le pointeur du décodeur n'est valable que pendant l'appel).
    void Push(const DecodedFrame& f, int64_t ptsUs, int64_t arrivalUs);
    int Queued();
    std::atomic<int> dropped{0}; // images trop en retard, sautées

    static int64_t NowUs();

private:
    struct Frame {
        std::vector<uint8_t> data; // NV12, pas = largeur
        uint32_t width = 0, height = 0;
        int64_t pts = 0;
    };
    void Loop();
    int64_t PlayTime(const Frame& f) const { return f.pts + m_offset + m_delayUs; }

    Sink m_sink;
    std::thread m_thread;
    std::mutex m_lock;
    std::condition_variable m_cv;
    std::deque<Frame> m_queue;
    std::vector<Frame> m_pool;
    bool m_stop = false;
    std::atomic<int64_t> m_delayUs{0};
    std::atomic<bool> m_auto{false};
    int64_t m_jitterPeak = 0;     // plus grand retard récent par rapport au trajet le plus rapide
    int64_t m_lastDecay = 0;

    // Décalage horloge PC - horloge téléphone (minimum glissant sur ~2 s, en deux demi-fenêtres)
    int64_t m_offset = 0;
    bool m_haveOffset = false;
    int64_t m_minCur = INT64_MAX, m_minPrev = INT64_MAX, m_windowStart = 0;
};
