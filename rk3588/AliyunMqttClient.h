#ifndef ALIYUNMQTTCLIENT_H
#define ALIYUNMQTTCLIENT_H

#include <QObject>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>
#include <QtNetwork/QSslConfiguration>
#include <QtMqtt/QMqttClient>

struct AliyunSensorData
{
    bool hasTemperature = false;
    double temperature = 0.0;

    bool hasHumidity = false;
    double humidity = 0.0;

    bool hasSmoke = false;
    double smoke = 0.0;

    bool hasFire = false;
    bool fireDetected = false;

    bool hasCombustibleGas = false;
    bool combustibleGasDetected = false;
    double combustibleGas = 0.0;

    bool hasAirPressure = false;
    double airPressure = 0.0;

    bool hasPowerSwitch = false;
    int powerSwitch = 0;

    bool hasAiDetectState = false;
    int aiDetectState = 0;

    bool hasAlarmState = false;
    int alarmState = 0;

    QJsonObject rawParams;
};

Q_DECLARE_METATYPE(AliyunSensorData)

class AliyunMqttClient : public QObject
{
    Q_OBJECT

public:
    struct Config
    {
        QString productKey ;
        QString deviceName ;
        QString deviceSecret ;
        QString password ;
        QString regionId = QStringLiteral("cn-shanghai");
        QString mqttHostUrl;
        QString clientId ;
        QStringList subscribeTopics;
        quint16 port = 8883;
    };

    explicit AliyunMqttClient(QObject *parent = nullptr);

    void configure(const Config &config);
    void setSubscribeTopics(const QStringList &topics);
    QStringList subscribeTopics() const;
    void setAutoReconnectEnabled(bool enabled, int intervalMs = 5000);

    void connectToAliyun();
    void disconnectFromAliyun();
    bool publishJson(const QString &topic, const QJsonObject &payload, int qos = 0, bool retain = false);

    bool isConnected() const;
    QMqttClient::ClientState state() const;
    QString defaultPropertySetTopic() const;

signals:
    void connected();
    void disconnected();
    void stateChanged(QMqttClient::ClientState state);
    void errorOccurred(const QString &message);
    void rawMessageReceived(const QString &topic, const QByteArray &payload);
    void jsonMessageReceived(const QString &topic, const QJsonObject &payload);
    void sensorDataReceived(const AliyunSensorData &data);

private:
    bool validateConfig();
    void applyConnectionOptions();
    void subscribeConfiguredTopics();
    void scheduleReconnect();
    void reconnectToAliyun();
    void handleMessageReceived(const QByteArray &message, const QMqttTopicName &topic);
    void replyPropertySetIfNeeded(const QString &topicName, const QJsonObject &root);

    QString brokerHost() const;
    QString plainClientId() const;
    QString mqttClientId(const QString &timestamp) const;
    QString username() const;
    QString password(const QString &timestamp) const;

    static QString normalizedMqttHost(const QString &host);
    static QString mqttErrorMessage(QMqttClient::ClientError error);
    static bool parseSensorData(const QJsonObject &root, AliyunSensorData *data);

    QMqttClient *m_client = nullptr;
    QTimer *m_reconnectTimer = nullptr;
    Config m_config;
    bool m_autoReconnectEnabled = true;
    bool m_manualDisconnect = false;
    int m_reconnectIntervalMs = 5000;
};

#endif // ALIYUNMQTTCLIENT_H
