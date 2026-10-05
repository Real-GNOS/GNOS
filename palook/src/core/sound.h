// Sound resources: WAV parsing, VOC -> WAV conversion, waveform envelope.
// VOC handling follows sdlpal's SOUND_LoadVOCData (type 01 blocks).

#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace pal {

struct SoundInfo {
    bool ok = false;
    QString kind;          // "WAV", "VOC", ...
    int rate = 0;
    int channels = 0;
    int bits = 0;
    qint64 frames = 0;     // sample frames (per channel)
    double duration = 0;
    QByteArray wav;        // normalized RIFF/WAVE bytes for playback/export
    QString error;
};

SoundInfo parseWav(const QByteArray &data);
SoundInfo parseVoc(const QByteArray &data);

// Sniff: RIFF/WAVE -> parseWav, Creative Voice -> parseVoc, else fail.
SoundInfo parseSound(const QByteArray &data);

// Min/max envelope of the PCM samples, for drawing a waveform.
struct Envelope {
    QVector<qint16> minV;
    QVector<qint16> maxV;
    bool valid = false;
};
Envelope waveform(const SoundInfo &si, int columns);

// Very small MIDI header summary (format/tracks/division).
struct MidiInfo {
    bool ok = false;
    int format = 0;
    int tracks = 0;
    int division = 0;      // ticks per quarter note when >= 0
    QString error;
};
MidiInfo parseMidi(const QByteArray &data);

} // namespace pal
