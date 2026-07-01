#ifndef PAGE_SENSOR_H
#define PAGE_SENSOR_H

#include <QGridLayout>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "AliyunMqttClient.h"

class PageSensor : public QWidget
{
    Q_OBJECT

public:
    explicit PageSensor(AliyunMqttClient *mqttClient = nullptr, QWidget *parent = nullptr);

signals:
    void sensorConnectionStateChanged(bool ok, const QString &message);
    void sensorAlarmStateChanged(bool ok, const QString &message);
    void homeSummaryUpdated(const QString &dataTime,
                            const QString &systemSafety,
                            const QString &recentAlarm,
                            const QString &aiState,
                            const QString &environmentState);

private slots:
    void applySensorData(const AliyunSensorData &data);
    void updateMqttState(QMqttClient::ClientState state);
    void showMqttError(const QString &message);
    void reconnectMqttIfNeeded();

private:
    void initMqttClient();
    void bindMqttSignals();
    void updateStatusLabel(const QString &text, const QString &color);
    static QString formatNumber(double value, int precision = 1);
    static QString aiDetectStateText(int state);
    static QString alarmStateText(int state);
    static QString powerSwitchText(int state);

    QLabel *mqttStatusLabel;
    QLabel *tempValue;
    QLabel *alarmStateValue;
    QLabel *smokeValue;
    QLabel *combustible_gasValue;
    QLabel *power1StateValue;
    QLabel *power2StateValue;
    QLabel *aiDetectStateValue;
    AliyunMqttClient *mqttClient;
    QTimer *mqttReconnectTimer;
    bool ownsMqttClient = false;
};

#endif // PAGE_SENSOR_H
