#pragma once
#include "../common/shared_frame.h"
#include "decoder.h"

// Publie les images décodées dans la mémoire partagée lue par la caméra virtuelle.
// L'objet n'existe que lorsqu'une application utilise la caméra : on réessaie régulièrement de l'ouvrir.
class FrameWriter {
public:
    ~FrameWriter() { Close(); }
    // Renvoie true si une application regarde la caméra en ce moment.
    bool Write(const DecodedFrame& f, bool mirror, bool rotate180);
    void Close();
    // Une application a lu une image dans les 2 dernières secondes.
    bool Consumer() const;

private:
    bool TryOpen();
    HANDLE m_mapping = nullptr;
    HANDLE m_event = nullptr;
    PhoneCamHeader* m_header = nullptr;
    ULONGLONG m_lastTry = 0;
};
