#ifndef GAUGEWIDGET_H
#define GAUGEWIDGET_H

#include <QWidget>

class GaugeWidget : public QWidget
{
    Q_OBJECT

public:
    explicit GaugeWidget(QWidget *parent = nullptr);

    void setValue(int v);

protected:
    void paintEvent(QPaintEvent *);

private:
    int value;
};

#endif
