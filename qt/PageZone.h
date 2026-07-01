#ifndef PAGEZONE_H
#define PAGEZONE_H

#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QDateTime>
#include <QWidget>

#include "AliyunMqttClient.h"

class PageZone : public QWidget
{
    Q_OBJECT

public:
    explicit PageZone(AliyunMqttClient *mqttClient = nullptr, QWidget *parent = nullptr);

signals:
    void zoneStateChanged(bool running, const QString &message);
    void deviceOnlineChanged(bool rkOnline, bool stm32Online);

private slots:
    void onStartAutoDetect();
    void onForceRedetect();
    void onConfirmYes();
    void onConfirmNo();
    void onRetryTimeout();
    void onRecheckTimeout();
    void onDeviceHeartbeatTimeout();
    void onMqttStateChanged(QMqttClient::ClientState state);
    void onMqttError(const QString &message);
    void onZoneCommandReceived(const QString &topic, const QJsonObject &payload);

private:
    enum class ZoneMode {
        AutoDetect,
        AwaitManualConfirm,
        ManualConfirmed,
        NormalRunning,
        Blocked
    };

    void buildUi();
    void updateUi();
    void emitDeviceStatus();
    void markRkOnline();
    void markStm32Online();
    void setStatusText(const QString &text);
    void setCountdown(int seconds);
    void resetCountdown();
    void tryStartAutoDetect();
    void startDetectionAttempt();
    void enterAwaitManualConfirm();
    void enterNormalRunning();
    void enterBlocked(const QString &reason);
    void resetForRkOffline();
    void publishZoneControl(const QString &cmd, int enable, const QString &mode);
    void configureMqtt();
    void bindMqttSignals();

    static QString buildZoneControlTopic();

    QLabel *titleLabel = nullptr;
    QLabel *statusLabel = nullptr;
    QLabel *countdownLabel = nullptr;
    QPushButton *startButton = nullptr;
    QPushButton *redetectButton = nullptr;
    QPushButton *yesButton = nullptr;
    QPushButton *noButton = nullptr;

    AliyunMqttClient *mqttClient = nullptr;
    QTimer *retryTimer = nullptr;
    QTimer *confirmTimer = nullptr;
    QTimer *recheckTimer = nullptr;
    QTimer *deviceHeartbeatTimer = nullptr;

    ZoneMode mode = ZoneMode::AutoDetect;
    int retryCount = 0;
    int countdownSeconds = 0;
    QDateTime lastRkHeartbeat;
    QDateTime lastStm32Heartbeat;
    bool ownsMqttClient = false;
    bool autoDetectStarted = false;
    bool manualOverrideActive = false;
    bool lastRkOnlineState = false;
};

#endif // PAGEZONE_H
