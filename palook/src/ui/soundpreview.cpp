#include "soundpreview.h"

#include <QFileInfo>
#include <QLabel>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QVBoxLayout>

// Internal waveform widget: draws the min/max envelope of the PCM stream.
class WaveWidget : public QWidget
{
public:
    explicit WaveWidget(QWidget *parent) : QWidget(parent)
    {
        setMinimumHeight(140);
    }

    pal::Envelope env;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(30, 30, 34));
        p.setPen(QColor(70, 70, 76));
        p.drawLine(0, height() / 2, width(), height() / 2);

        if (!env.valid || env.minV.isEmpty()) {
            p.setPen(QColor(140, 140, 140));
            p.drawText(rect(), Qt::AlignCenter,
                       QStringLiteral("no audio data"));
            return;
        }

        const double mid = height() / 2.0;
        const double amp = mid - 4.0;
        const int cols = env.minV.size();
        p.setPen(QColor(120, 220, 130));
        for (int x = 0; x < width(); ++x) {
            const int c = int(qint64(x) * cols / qMax(1, width()));
            const double lo = env.minV[c] / 32768.0;
            const double hi = env.maxV[c] / 32768.0;
            const double y0 = mid - hi * amp;
            const double y1 = mid - lo * amp;
            p.drawLine(x, int(y0), x, int(y1));
        }
    }
};

SoundPreview::SoundPreview(QWidget *parent)
    : QWidget(parent)
{
    auto *top = new QHBoxLayout;
    top->setContentsMargins(4, 4, 4, 0);

    m_play = new QPushButton(QStringLiteral("▶ Play"), this);
    m_stop = new QPushButton(QStringLiteral("■ Stop"), this);
    m_stop->setEnabled(false);
    m_info = new QLabel(this);

    top->addWidget(m_play);
    top->addWidget(m_stop);
    top->addSpacing(12);
    top->addWidget(m_info, 1);

    m_wave = new WaveWidget(this);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addLayout(top);
    lay->addWidget(m_wave, 1);

    // Pick the first available player. ffplay is preferred (fast, no
    // daemon needed); pw-play/paplay need PipeWire/PulseAudio running.
    static const char *candidates[] = {"ffplay", "pw-play", "paplay", "aplay"};
    for (const char *c : candidates) {
        const QString exe =
            QStandardPaths::findExecutable(QString::fromLatin1(c));
        if (!exe.isEmpty()) {
            m_player = exe;
            break;
        }
    }

    connect(m_play, &QPushButton::clicked, this, &SoundPreview::play);
    connect(m_stop, &QPushButton::clicked, this, &SoundPreview::stop);
}

SoundPreview::~SoundPreview()
{
    stop();
}

void SoundPreview::setResult(const pal::SoundInfo &si, const QString &detail)
{
    stop();
    m_si = si;

    m_info->setText(detail.isEmpty()
                        ? (m_player.isEmpty()
                               ? QStringLiteral("No audio player found "
                                                "(install ffplay / pw-play / aplay)")
                               : QStringLiteral("player: %1").arg(m_player))
                        : detail);

    m_wave->env = si.ok ? pal::waveform(si, 1024) : pal::Envelope();
    m_wave->update();
    m_play->setEnabled(si.ok && !si.wav.isEmpty());
}

void SoundPreview::clear()
{
    stop();
    m_si = pal::SoundInfo();
    m_wave->env = pal::Envelope();
    m_wave->update();
    m_info->clear();
    m_play->setEnabled(false);
}

void SoundPreview::play()
{
    if (m_si.wav.isEmpty() || m_player.isEmpty())
        return;
    stop();

    m_tmp = new QTemporaryFile(QStringLiteral("palook-XXXXXX.wav"), this);
    m_tmp->setAutoRemove(true);
    if (!m_tmp->open()) {
        m_info->setText(QStringLiteral("cannot create temporary file"));
        return;
    }
    m_tmp->write(m_si.wav);
    m_tmp->flush();
    const QString path = m_tmp->fileName();
    m_tmp->close();

    m_proc = new QProcess(this);
    QStringList args;
    const QString base = QFileInfo(m_player).fileName();
    if (base == QLatin1String("ffplay")) {
        args << QStringLiteral("-nodisp") << QStringLiteral("-autoexit")
             << QStringLiteral("-loglevel") << QStringLiteral("quiet") << path;
    } else if (base == QLatin1String("aplay")) {
        args << QStringLiteral("-q") << path;
    } else {
        args << path;
    }

    m_proc->start(m_player, args);
    m_play->setEnabled(false);
    m_stop->setEnabled(true);

    connect(m_proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
                m_play->setEnabled(!m_si.wav.isEmpty());
                m_stop->setEnabled(false);
            });
}

void SoundPreview::stop()
{
    if (m_proc) {
        m_proc->disconnect(this);
        if (m_proc->state() != QProcess::NotRunning) {
            m_proc->kill();
            m_proc->waitForFinished(500);
        }
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    if (m_tmp) {
        m_tmp->deleteLater();
        m_tmp = nullptr;
    }
    m_play->setEnabled(!m_si.wav.isEmpty());
    m_stop->setEnabled(false);
}
