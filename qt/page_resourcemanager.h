#ifndef PAGE_RESOURCEMANAGER_H
#define PAGE_RESOURCEMANAGER_H

#include <QWidget>
#include <QtCharts>
#include <QLineSeries>
#include <QChartView>
#include <QProgressBar>
#include <QLabel>
#include <QTableWidget>


class PageResourceManager : public QWidget
{
    Q_OBJECT
public:
    explicit PageResourceManager(QWidget *parent = nullptr);

    // 更新 AI 推演结果
    void updateAI(float fire, float smoke, float gas);

private:
    void addAlarm(const QString &time, const QString &location, float prob);
    void rebuildChartFromAlarmTable();

private:
    QLineSeries *series;
    QChart *chart;
    QChartView *chartView;
    QProgressBar *riskBar;
    QLabel *riskLabel;
    QTableWidget *alarmTable;
    int timeIndex;
};

#endif // PAGE_RESOURCEMANAGER_H
