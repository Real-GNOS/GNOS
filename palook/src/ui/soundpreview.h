// Sound preview: waveform envelope, playback through an external player
// (ffplay / pw-play / paplay / aplay) and WAV export.

#pragma once

#include "core/sound.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QProcess;
class QTemporaryFile;

class WaveWidget;   // internal waveform widget (defined in the .cpp)

class SoundPreview : public QWidget
{
    Q_OBJECT
public:
    explicit SoundPreview(QWidget *parent = nullptr);
    ~SoundPreview() override;

    void setResult(const pal::SoundInfo &si, const QString &detail);
    void clear();

    QByteArray wav() const { return m_si.wav; }

private:
    void play();
    void stop();

    WaveWidget *m_wave = nullptr;
    QLabel *m_info = nullptr;
    QPushButton *m_play = nullptr;
    QPushButton *m_stop = nullptr;

    pal::SoundInfo m_si;
    QProcess *m_proc = nullptr;
    QTemporaryFile *m_tmp = nullptr;
    QString m_player;
};
