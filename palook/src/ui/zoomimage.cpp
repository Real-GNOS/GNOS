#include "zoomimage.h"

#include <QBrush>
#include <QGraphicsPixmapItem>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTransform>
#include <QWheelEvent>

ZoomImageView::ZoomImageView(QWidget *parent)
    : QGraphicsView(parent)
{
    m_scene = new QGraphicsScene(this);
    setScene(m_scene);
    m_item = m_scene->addPixmap(QPixmap());
    m_item->setTransformationMode(Qt::FastTransformation);

    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setRenderHints(QPainter::SmoothPixmapTransform);
    setDragMode(QGraphicsView::NoDrag);
    setFrameShape(QFrame::NoFrame);
    setInteractive(false);
    updateBackground(true);
}

void ZoomImageView::updateBackground(bool checkerboard)
{
    QBrush brush;
    if (checkerboard) {
        QPixmap tile(16, 16);
        tile.fill(QColor(64, 64, 64));
        QPainter p(&tile);
        p.fillRect(0, 0, 8, 8, QColor(88, 88, 88));
        p.fillRect(8, 8, 8, 8, QColor(88, 88, 88));
        p.end();
        brush = QBrush(tile);
    } else {
        brush = QBrush(QColor(40, 40, 40));
    }
    setBackgroundBrush(brush);
}

void ZoomImageView::setImage(const QImage &img, bool checkerboard)
{
    m_image = img;
    updateBackground(checkerboard);
    if (img.isNull()) {
        m_item->setPixmap(QPixmap());
        m_scene->setSceneRect(QRectF());
        return;
    }
    m_item->setPixmap(QPixmap::fromImage(img));
    m_scene->setSceneRect(0, 0, img.width(), img.height());
    if (m_fit)
        fitToWindow();
    else
        applyScale();
}

void ZoomImageView::clearImage()
{
    setImage(QImage(), true);
}

void ZoomImageView::applyScale()
{
    setTransform(QTransform::fromScale(m_scale, m_scale));
}

void ZoomImageView::fitToWindow()
{
    if (m_image.isNull() || width() <= 0 || height() <= 0) {
        m_fit = true;
        return;
    }
    const double sx = double(width()) / m_image.width();
    const double sy = double(height()) / m_image.height();
    m_scale = qMax(0.05, qMin(sx, sy));
    // Avoid upscaling small sprites beyond 8x.
    m_scale = qMin(m_scale, 8.0);
    m_fit = true;
    applyScale();
    centerOn(m_item);
}

void ZoomImageView::zoomToActual()
{
    m_fit = false;
    m_scale = 1.0;
    applyScale();
    centerOn(m_item);
}

void ZoomImageView::zoomIn()
{
    m_fit = false;
    m_scale = qMin(32.0, m_scale * 1.25);
    applyScale();
}

void ZoomImageView::zoomOut()
{
    m_fit = false;
    m_scale = qMax(0.05, m_scale / 1.25);
    applyScale();
}

void ZoomImageView::wheelEvent(QWheelEvent *ev)
{
    if (m_image.isNull())
        return;
    m_fit = false;
    const double factor = ev->angleDelta().y() > 0 ? 1.25 : 1.0 / 1.25;
    const QPointF anchor = ev->position();
    const QPointF scenePos = mapToScene(anchor.toPoint());

    m_scale = qBound(0.05, m_scale * factor, 32.0);
    applyScale();

    const QPointF after = mapToScene(anchor.toPoint());
    const QPointF delta = after - scenePos;
    horizontalScrollBar()->setValue(int(horizontalScrollBar()->value() - delta.x()));
    verticalScrollBar()->setValue(int(verticalScrollBar()->value() - delta.y()));
    ev->accept();
}

void ZoomImageView::resizeEvent(QResizeEvent *ev)
{
    QGraphicsView::resizeEvent(ev);
    if (m_fit)
        fitToWindow();
}

void ZoomImageView::mousePressEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::MiddleButton || ev->button() == Qt::LeftButton) {
        m_panning = true;
        m_lastPos = ev->pos();
        setCursor(Qt::ClosedHandCursor);
        ev->accept();
        return;
    }
    QGraphicsView::mousePressEvent(ev);
}

void ZoomImageView::mouseMoveEvent(QMouseEvent *ev)
{
    if (m_panning) {
        const QPoint delta = ev->pos() - m_lastPos;
        m_lastPos = ev->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        ev->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(ev);
}

void ZoomImageView::mouseReleaseEvent(QMouseEvent *ev)
{
    if (m_panning) {
        m_panning = false;
        setCursor(Qt::ArrowCursor);
        ev->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(ev);
}
