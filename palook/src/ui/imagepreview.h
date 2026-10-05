// Preview for image-like lumps: sprites, palettes, backgrounds, maps
// (layer selector via the frame slider) and RNG animation frames.

#pragma once

#include "../core/gamepack.h"

#include <QWidget>

class QSlider;
class QLabel;
class QPushButton;
class ZoomImageView;

class ImagePreview : public QWidget
{
    Q_OBJECT
public:
    explicit ImagePreview(QWidget *parent = nullptr);

    void setResult(const pal::LumpResult &r);
    void clear();

    QImage currentImage() const;
    QString currentFrameLabel() const;
    int frameCount() const { return m_frames.size(); }

signals:
    void exportRequested();

private:
    void updateFrame();

    ZoomImageView *m_view = nullptr;
    QSlider       *m_slider = nullptr;
    QLabel        *m_frameLabel = nullptr;
    QLabel        *m_sizeLabel = nullptr;
    QPushButton   *m_prev = nullptr;
    QPushButton   *m_next = nullptr;
    QPushButton   *m_fit = nullptr;
    QPushButton   *m_actual = nullptr;

    QVector<QImage> m_frames;
    QStringList     m_labels;
    bool            m_isMap = false;
};
