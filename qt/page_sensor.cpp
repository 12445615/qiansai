#include "page_sensor.h"

#include <QDateTime>
#include <QFrame>
#include <QProcessEnvironment>

namespace {
constexpr double kSmokeAlarmThreshold = 70.0;
constexpr double kCombustibleGasAlarmThreshold = 3000.0;
}

PageSensor::PageSensor(AliyunMqttClient *sharedClient, QWidget *parent)
    : QWidget(parent),
      mqttStatusLabel(new QLabel(QStringLiteral("MQTT: 未配置"), this)),
      tempValue(new QLabel(QStringLiteral("-- ℃"), this)),
      alarmStateValue(new QLabel(QStringLiteral("--"), this)),
      smokeValue(new QLabel(QStringLiteral("--"), this)),
      combustible_gasValue(new QLabel(QStringLiteral("--"), this)),
      power1StateValue(new QLabel(QStringLiteral("--"), this)),
      power2StateValue(new QLabel(QStringLiteral("--"), this)),
      aiDetectStateValue(new QLabel(QStringLiteral("无目标"), this)),
      mqttClient(sharedClient ? sharedClient : new AliyunMqttClient(this)),
      mqttReconnectTimer(new QTimer(this))
{
    setStyleSheet(QStringLiteral("background-color:#1e1e2f;color:white;"));

    QLabel *title = new QLabel(QStringLiteral("环境监测仪表盘"));
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size:26px;font-weight:bold;"));

    mqttStatusLabel->setAlignment(Qt::AlignCenter);
    mqttStatusLabel->setStyleSheet(QStringLiteral("font-size:14px;color:#f0c674;"));

    QList<QLabel *> labels = {
        tempValue,
        alarmStateValue,
        smokeValue,
        power1StateValue,
        power2StateValue,
        combustible_gasValue,
        aiDetectStateValue
    };

    for (QLabel *label : labels) {
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(QStringLiteral("font-size:24px;"));
    }

    auto createCard = [](const QString &titleText, QLabel *valueLabel) {
        QFrame *frame = new QFrame();
        frame->setStyleSheet(
            "QFrame{"
            "background-color:#2c2c3e;"
            "border-radius:10px;"
            "}");

        QLabel *titleLabel = new QLabel(titleText);
        titleLabel->setAlignment(Qt::AlignCenter);
        titleLabel->setStyleSheet(QStringLiteral("font-size:18px;color:#aaa;"));

        QVBoxLayout *layout = new QVBoxLayout(frame);
        layout->addWidget(titleLabel);
        layout->addWidget(valueLabel);

        return frame;
    };

    QGridLayout *grid = new QGridLayout();
    grid->addWidget(createCard(QStringLiteral("温度"), tempValue), 0, 0);
    grid->addWidget(createCard(QStringLiteral("报警状态"), alarmStateValue), 0, 1);
    grid->addWidget(createCard(QStringLiteral("烟雾浓度"), smokeValue), 1, 0);
    grid->addWidget(createCard(QStringLiteral("AI状态"), aiDetectStateValue), 1, 1);
    grid->addWidget(createCard(QStringLiteral("可燃气体检测"), combustible_gasValue), 2, 0);
    grid->addWidget(createCard(QStringLiteral("工作区1电源"), power1StateValue), 2, 1);
    grid->addWidget(createCard(QStringLiteral("工作区2电源"), power2StateValue), 3, 0);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(title);
    mainLayout->addWidget(mqttStatusLabel);
    mainLayout->addSpacing(20);
    mainLayout->addLayout(grid);

    mqttReconnectTimer->setInterval(5000);
    connect(mqttReconnectTimer, &QTimer::timeout,
            this, &PageSensor::reconnectMqttIfNeeded);

    ownsMqttClient = (sharedClient == nullptr);
    bindMqttSignals();

    if (ownsMqttClient)
        initMqttClient();
    else
        updateMqttState(mqttClient->state());
}

void PageSensor::applySensorData(const AliyunSensorData &data)
{
    if (data.hasTemperature)
        tempValue->setText(formatNumber(data.temperature) + QStringLiteral(" ℃"));

    if (data.hasSmoke)
        smokeValue->setText(formatNumber(data.smoke) + QStringLiteral(" %"));

    if (data.hasCombustibleGas)
        combustible_gasValue->setText(formatNumber(data.combustibleGas) + QStringLiteral(" ppm"));

    if (data.hasAiDetectState)
        aiDetectStateValue->setText(aiDetectStateText(data.aiDetectState));

    if (data.hasAlarmState)
        alarmStateValue->setText(alarmStateText(data.alarmState));

    if (data.hasPowerSwitch1) {
        power1StateValue->setText(powerSwitchText(data.powerSwitch1));
    } else if (data.hasPowerSwitch) {
        power1StateValue->setText(powerSwitchText(data.powerSwitch));
    }

    if (data.hasPowerSwitch2) {
        power2StateValue->setText(powerSwitchText(data.powerSwitch2));
    } else if (data.hasPowerSwitch) {
        power2StateValue->setText(powerSwitchText(data.powerSwitch));
    }

    QStringList environmentWarnings;
    if (data.hasSmoke && data.smoke >= kSmokeAlarmThreshold)
        environmentWarnings << QStringLiteral("烟雾异常");
    if (data.hasCombustibleGas && data.combustibleGas >= kCombustibleGasAlarmThreshold)
        environmentWarnings << QStringLiteral("可燃气体异常");
    if (data.hasTemperature && data.temperature >= 60.0)
        environmentWarnings << QStringLiteral("温度异常");

    const QString aiText = data.hasAiDetectState
        ? aiDetectStateText(data.aiDetectState)
        : aiDetectStateValue->text();
    const QString environmentText = environmentWarnings.isEmpty()
        ? QStringLiteral("正常")
        : environmentWarnings.join(QStringLiteral("、"));

    const bool aiFireState = data.hasAiDetectState
        && (data.aiDetectState == 5 || data.aiDetectState == 6);
    const bool fireAlarm = aiFireState
        || (data.hasFire && data.fireConfidence > 0.0);

    QString recentAlarm = QStringLiteral("无");
    if (data.hasFire && data.fireConfidence > 0.0) {
        recentAlarm = QStringLiteral("火灾报警 %1%")
            .arg(qBound(0, static_cast<int>(data.fireConfidence + 0.5), 100));
    } else if (aiFireState) {
        recentAlarm = QStringLiteral("火灾报警（待复位）");
    } else if (data.hasAiDetectState && data.aiDetectState != 0) {
        recentAlarm = aiText;
    } else if (!environmentWarnings.isEmpty()) {
        recentAlarm = environmentText;
    } else if (data.hasAlarmState && data.alarmState != 0) {
        recentAlarm = QStringLiteral("设备报警");
    }

    const bool alarm = (data.hasAlarmState && data.alarmState != 0)
        || (data.hasAiDetectState && data.aiDetectState != 0)
        || !environmentWarnings.isEmpty()
        || fireAlarm;
    const QString systemSafety = alarm ? QStringLiteral("报警") : QStringLiteral("正常");

    emit homeSummaryUpdated(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                            systemSafety,
                            recentAlarm,
                            aiText,
                            environmentText);

    if(alarm)
        emit sensorAlarmStateChanged(false, recentAlarm);
    else
        emit sensorAlarmStateChanged(true, QStringLiteral("无"));
}

void PageSensor::updateMqttState(QMqttClient::ClientState state)
{
    switch (state) {
    case QMqttClient::Disconnected:
        updateStatusLabel(QStringLiteral("MQTT: 已断开"), QStringLiteral("#e06c75"));
        emit sensorConnectionStateChanged(false, QStringLiteral("已断开"));
        break;
    case QMqttClient::Connecting:
        updateStatusLabel(QStringLiteral("MQTT: 连接中"), QStringLiteral("#f0c674"));
        emit sensorConnectionStateChanged(false, QStringLiteral("连接中"));
        break;
    case QMqttClient::Connected:
        updateStatusLabel(QStringLiteral("MQTT: 已连接"), QStringLiteral("#98c379"));
        emit sensorConnectionStateChanged(true, QStringLiteral("正常"));
        mqttReconnectTimer->stop();
        break;
    }
}

void PageSensor::showMqttError(const QString &message)
{
    updateStatusLabel(QStringLiteral("MQTT: %1").arg(message), QStringLiteral("#e06c75"));
    emit sensorConnectionStateChanged(false, message);
}

void PageSensor::reconnectMqttIfNeeded()
{
    if (mqttClient->state() == QMqttClient::Connected) {
        mqttReconnectTimer->stop();
        return;
    }

    if (mqttClient->state() == QMqttClient::Connecting)
        return;

    updateStatusLabel(QStringLiteral("MQTT: 正在自动重连"), QStringLiteral("#f0c674"));
    emit sensorConnectionStateChanged(false, QStringLiteral("正在自动重连"));
    mqttClient->connectToAliyun();
}

void PageSensor::bindMqttSignals()
{
    connect(mqttClient, &AliyunMqttClient::sensorDataReceived,
            this, &PageSensor::applySensorData);
    connect(mqttClient, &AliyunMqttClient::stateChanged,
            this, &PageSensor::updateMqttState);
    connect(mqttClient, &AliyunMqttClient::errorOccurred,
            this, &PageSensor::showMqttError);
}

void PageSensor::initMqttClient()
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    const QString productKey = env.value(QStringLiteral("ALIYUN_PRODUCT_KEY"),QStringLiteral("k29ovUMboAH"));
    const QString deviceName = env.value(QStringLiteral("ALIYUN_DEVICE_NAME"),QStringLiteral("0122-qt"));
    const QString deviceSecret = env.value(QStringLiteral("ALIYUN_DEVICE_SECRET"));
    const QString fixedPassword = env.value(QStringLiteral("ALIYUN_PASSWORD"),
                                            QStringLiteral("91a8d87f1aaffcab93b149580098d24574b97a447c90823e6943f72142560513"));
    const QString regionId = env.value(QStringLiteral("ALIYUN_REGION_ID"), QStringLiteral("cn-shanghai"));
    const QString mqttHostUrl = env.value(QStringLiteral("ALIYUN_MQTT_HOST_URL"),
                                          QStringLiteral("iot-06z00be8pk7p1uz.mqtt.iothub.aliyuncs.com"));
    const QString clientId = env.value(QStringLiteral("ALIYUN_CLIENT_ID"),
                                       QStringLiteral("k29ovUMboAH.0122-qt|securemode=2,signmethod=hmacsha256,timestamp=1781166475258|"));
    const QString customTopic = env.value(QStringLiteral("ALIYUN_SUB_TOPIC"),
                                          QStringLiteral("/sys/k29ovUMboAH/0122-qt/thing/service/property/set"));
    const quint16 mqttPort = static_cast<quint16>(
        env.value(QStringLiteral("ALIYUN_MQTT_PORT"), QStringLiteral("1883")).toUInt());

    if (productKey.isEmpty() || deviceName.isEmpty() || (deviceSecret.isEmpty() && fixedPassword.isEmpty())) {
        updateStatusLabel(QStringLiteral("MQTT: 缺少阿里云环境变量配置"), QStringLiteral("#f0c674"));
        emit sensorConnectionStateChanged(false, QStringLiteral("配置不完整"));
        return;
    }

    AliyunMqttClient::Config config;
    config.productKey = productKey;
    config.deviceName = deviceName;
    config.deviceSecret = deviceSecret;
    config.password = fixedPassword;
    config.regionId = regionId;
    config.mqttHostUrl = mqttHostUrl;
    config.clientId = clientId;
    config.port = mqttPort;

    if (!customTopic.isEmpty())
        config.subscribeTopics << customTopic;

    mqttClient->configure(config);
    mqttClient->connectToAliyun();
}

void PageSensor::updateStatusLabel(const QString &text, const QString &color)
{
    mqttStatusLabel->setText(text);
    mqttStatusLabel->setStyleSheet(
        QStringLiteral("font-size:14px;color:%1;").arg(color));
}

QString PageSensor::formatNumber(double value, int precision)
{
    return QString::number(value, 'f', precision);
}

QString PageSensor::aiDetectStateText(int state)
{
    switch (state) {
    case 0:
        return QStringLiteral("无目标");
    case 1:
        return QStringLiteral("PPE合规");
    case 2:
        return QStringLiteral("未戴安全帽");
    case 3:
        return QStringLiteral("未穿防护服");
    case 4:
        return QStringLiteral("同时PPE不合规");
    case 5:
        return QStringLiteral("工作区火光");
    case 6:
        return QStringLiteral("非工作区火光");
    case 7:
        return QStringLiteral("危险区人员闯入");
    default:
        return QStringLiteral("未知状态 %1").arg(state);
    }
}

QString PageSensor::alarmStateText(int state)
{
    return state == 0 ? QStringLiteral("未报警") : QStringLiteral("报警");
}

QString PageSensor::powerSwitchText(int state)
{
    return state == 0 ? QStringLiteral("关闭") : QStringLiteral("开启");
}
