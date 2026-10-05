// Zoomable image view: wheel zoom, drag pan, fit-to-window, optional
// checkerboard background for transparent sprites.

#pragma once

#include <QGraphicsView>
#include <QImage>

class ZoomImageView : public QGraphicsView
{
    Q_OBJECT
public:
    explicit ZoomImageView(QWidget *parent = nullptr);

    void setImage(const QImage &img, bool checkerboard = true);
    void clearImage();

    void zoomIn();
    void zoomOut();
    void fitToWindow();
    void zoomToActual();

    double scale() const { return m_scale; }

protected:
    void wheelEvent(QWheelEvent *ev) override;
    void resizeEvent(QResizeEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;

private:
    void applyScale();
    void updateBackground(bool checkerboard);

    QGraphicsScene      *m_scene = nullptr;
    QGraphicsPixmapItem *m_item = nullptr;
    bool  m_fit = true;
    double m_scale = 1.0;
    bool  m_panning = false;
    QPoint m_lastPos;
    QImage m_image;
};
