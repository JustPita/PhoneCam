#include "playout.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

int64_t Playout::NowUs() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return int64_t(c.QuadPart / freq * 1000000 + (c.QuadPart % freq) * 1000000 / freq);
}

void Playout::Start(Sink sink) {
    Stop();
    m_sink = std::move(sink);
    m_stop = false;
    m_thread = std::thread(&Playout::Loop, this);
}

void Playout::Stop() {
    {
        std::lock_guard<std::mutex> lk(m_lock);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void Playout::Reset() {
    std::lock_guard<std::mutex> lk(m_lock);
    for (auto& f : m_queue) m_pool.push_back(std::move(f));
    m_queue.clear();
    m_haveOffset = false;
    m_minCur = m_minPrev = INT64_MAX;
    m_windowStart = 0;
    m_jitterPeak = 0;
}

int Playout::Queued() {
    std::lock_guard<std::mutex> lk(m_lock);
    return int(m_queue.size());
}

void Playout::Push(const DecodedFrame& src, int64_t ptsUs, int64_t arrivalUs) {
    Frame f;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        if (!m_pool.empty()) {
            f = std::move(m_pool.back());
            m_pool.pop_back();
        }
    }
    const uint32_t w = src.width, h = src.height;
    f.data.resize(size_t(w) * h * 3 / 2);
    for (uint32_t y = 0; y < h; ++y) memcpy(f.data.data() + size_t(w) * y, src.y + LONG_PTR(src.pitch) * y, w);
    uint8_t* uv = f.data.data() + size_t(w) * h;
    for (uint32_t y = 0; y < h / 2; ++y) memcpy(uv + size_t(w) * y, src.uv + LONG_PTR(src.pitch) * y, w);
    f.width = w;
    f.height = h;
    f.pts = ptsUs;

    {
        std::lock_guard<std::mutex> lk(m_lock);
        // Minimum glissant de (arrivée - pts) : deux demi-fenêtres d'une seconde.
        const int64_t off = arrivalUs - ptsUs;
        if (!m_haveOffset || arrivalUs - m_windowStart > 1000000) {
            m_minPrev = m_minCur;
            m_minCur = INT64_MAX;
            m_windowStart = arrivalUs;
        }
        m_minCur = (std::min)(m_minCur, off);
        m_offset = (std::min)(m_minCur, m_minPrev);
        m_haveOffset = true;

        if (m_auto) {
            // Retard de cette image par rapport au trajet le plus rapide = gigue.
            // On monte tout de suite au pic, puis on redescend de 5 ms par seconde.
            const int64_t lateness = off - m_offset;
            if (lateness > m_jitterPeak) m_jitterPeak = lateness;
            if (arrivalUs - m_lastDecay > 1000000) {
                m_jitterPeak = (std::max<int64_t>)(0, m_jitterPeak - 5000);
                m_lastDecay = arrivalUs;
            }
            m_delayUs = std::clamp<int64_t>(m_jitterPeak + 15000, 20000, 400000);
        }
        // Sécurité mémoire : pas plus d'une seconde d'images en attente.
        while (m_queue.size() > 240) {
            m_pool.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
            dropped++;
        }
        m_queue.push_back(std::move(f));
    }
    m_cv.notify_one();
}

void Playout::Loop() {
    std::unique_lock<std::mutex> lk(m_lock);
    for (;;) {
        m_cv.wait(lk, [&] { return m_stop || !m_queue.empty(); });
        if (m_stop) return;

        int64_t now = NowUs();
        // En retard : on saute directement à la plus récente image déjà échue.
        while (m_queue.size() > 1 && PlayTime(m_queue[1]) <= now) {
            m_pool.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
            dropped++;
        }
        const int64_t due = PlayTime(m_queue.front());
        if (due > now + 500) {
            // Pas encore l'heure : attente (réveillée si une image arrive ou si le retard change).
            int64_t waitUs = (std::min<int64_t>)(due - now, 200000);
            if (waitUs > 2000) {
                m_cv.wait_for(lk, std::chrono::microseconds(waitUs - 1000));
                continue;
            }
            // Dernière milliseconde : attente active pour la précision.
            lk.unlock();
            while (NowUs() < due) YieldProcessor();
            lk.lock();
            if (m_queue.empty()) continue;
        }
        Frame f = std::move(m_queue.front());
        m_queue.pop_front();
        lk.unlock();
        DecodedFrame df{f.data.data(), f.data.data() + size_t(f.width) * f.height, LONG(f.width), f.width, f.height};
        m_sink(df);
        lk.lock();
        m_pool.push_back(std::move(f));
        if (m_pool.size() > 8) m_pool.resize(8);
    }
}
