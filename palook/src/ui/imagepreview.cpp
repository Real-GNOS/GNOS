#include "imagepreview.h"

#include "zoomimage.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

ImagePreview::ImagePreview(QWidget *parent)
    : QWidget(parent)
{
    auto *top = new QHBoxLayout;
    top->setContentsMargins(4, 4, 4, 0);

    m_prev = new QPushButton(QStringLiteral("◀"), this);
    m_next = new QPushButton(QStringLiteral("▶"), this);
    m_prev->setFixedWidth(28);
    m_next->setFixedWidth(28);
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setMinimumWidth(140);
    m_frameLabel = new QLabel(QStringLiteral("-"), this);
    m_sizeLabel = new QLabel(QString(), this);

    m_fit = new QPushButton(QStringLiteral("Fit"), this);
    m_actual = new QPushButton(QStringLiteral("100%"), this);
    m_fit->setCheckable(true);
    m_fit->setChecked(true);

    top->addWidget(m_prev);
    top->addWidget(m_slider, 1);
    top->addWidget(m_next);
    top->addWidget(m_frameLabel);
    top->addSpacing(12);
    top->addWidget(m_fit);
    top->addWidget(m_actual);
    top->addSpacing(12);
    top->addWidget(m_sizeLabel, 1);

    m_view = new ZoomImageView(this);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addLayout(top);
    lay->addWidget(m_view, 1);

    connect(m_slider, &QSlider::valueChanged, this,
            [this](int) { updateFrame(); });
    connect(m_prev, &QPushButton::clicked, this, [this] {
        m_slider->setValue(m_slider->value() - 1);
    });
    connect(m_next, &QPushButton::clicked, this, [this] {
        m_slider->setValue(m_slider->value() + 1);
    });
    connect(m_fit, &QPushButton::clicked, this, [this](bool on) {
        if (on) {
            m_actual->setChecked(false);
            m_view->fitToWindow();
        }
    });
    connect(m_actual, &QPushButton::clicked, this, [this](bool on) {
        if (on) {
            m_fit->setChecked(false);
            m_view->zoomToActual();
        }
    });

}

void ImagePreview::setResult(const pal::LumpResult &r)
{
    m_frames = r.frames;
    m_labels = r.frameLabels;
    m_isMap = r.isMap;

    const bool multi = m_frames.size() > 1;
    m_prev->setVisible(multi);
    m_next->setVisible(multi);
    m_slider->setVisible(multi);
    m_frameLabel->setVisible(multi);

    m_slider->setRange(0, qMax(0, m_frames.size() - 1));
    m_slider->setValue(0);
    updateFrame();
}

void ImagePreview::clear()
{
    m_frames.clear();
    m_labels.clear();
    m_view->clearImage();
    m_sizeLabel->clear();
    m_frameLabel->setText(QStringLiteral("-"));
    m_slider->setRange(0, 0);
}

QImage ImagePreview::currentImage() const
{
    const int i = m_slider->value();
    if (i >= 0 && i < m_frames.size())
        return m_frames[i];
    return {};
}

QString ImagePreview::currentFrameLabel() const
{
    const int i = m_slider->value();
    if (i >= 0 && i < m_labels.size())
        return m_labels[i];
    return {};
}

void ImagePreview::updateFrame()
{
    const int i = m_slider->value();
    if (i < 0 || i >= m_frames.size()) {
        m_view->clearImage();
        return;
    }
    const QImage img = m_frames[i];
    m_view->setImage(img, /*checkerboard=*/!m_isMap);
    if (m_frames.size() > 1) {
        QString extra;
        if (i < m_labels.size() && !m_labels[i].isEmpty())
            extra = QStringLiteral(" — %1").arg(m_labels[i]);
        m_frameLabel->setText(
            QStringLiteral("%1 / %2%3")
                .arg(i + 1)
                .arg(m_frames.size())
                .arg(extra));
    } else {
        m_frameLabel->setText(QStringLiteral("1 / 1"));
    }
    m_sizeLabel->setText(img.isNull()
                             ? QString()
                             : QStringLiteral("%1×%2 px")
                                   .arg(img.width())
                                   .arg(img.height()));
}
