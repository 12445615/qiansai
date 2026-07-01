#include "GaugeWidget.h"
#include <QPainter>

GaugeWidget::GaugeWidget(QWidget *parent)
    : QWidget(parent)
{
    value = 0;
    setMinimumSize(150,150);
}

void GaugeWidget::setValue(int v)
{
    value = v;
    update();
}

void GaugeWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);

    p.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();

    int side = qMin(w,h);

    QRectF rect((w-side)/2,(h-side)/2,side,side);

    // 背景圆
    p.setPen(QPen(Qt::gray,8));
    p.drawArc(rect,45*16,270*16);

    // 数值弧
    p.setPen(QPen(Qt::red,8));

    int span = (270.0 * value / 100) * 16;

    p.drawArc(rect,45*16,-span);

    // 文字
    p.setPen(Qt::white);
    p.setFont(QFont("Arial",16,QFont::Bold));

    p.drawText(rect,Qt::AlignCenter,
               QString::number(value)+"%");
}
