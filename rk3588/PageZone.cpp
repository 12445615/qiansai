#include "PageZone.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonValue>
#include <QProcessEnvironment>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kAutoRetryLimit = 2;
constexpr int kConfirmWindowSeconds = 60;
constexpr int kRecheckIntervalSeconds = 10 * 60;
constexpr int kDetectTimeoutSeconds = 60;
constexpr int kDeviceOfflineTimeoutSeconds = 10;
}

PageZone::PageZone(AliyunMqttClient *sharedClient, QWidget *parent)
    : QWidget(parent),
      mqttClient(sharedClient ? sharedClient : new AliyunMqttClient(this)),
      retryTimer(new QTimer(this)),
      confirmTimer(new QTimer(this)),
      recheckTimer(new QTimer(this)),
      deviceHeartbeatTimer(new QTimer(this))
{
    buildUi();
    ownsMqttClient = (sharedClient == nullptr);
    if (ownsMqttClient)
        configureMqtt();

    retryTimer->setSingleShot(true);
    retryTimer->setInterval(kDetectTimeoutSeconds * 1000);
    connect(retryTimer, &QTimer::timeout, this, &PageZone::onRetryTimeout);

    recheckTimer->setInterval(kDetectTimeoutSeconds * 1000);
    connect(recheckTimer, &QTimer::timeout, this, &PageZone::onRecheckTimeout);

    deviceHeartbeatTimer->setInterval(1000);
    connect(deviceHeartbeatTimer, &QTimer::timeout,
            this, &PageZone::onDeviceHeartbeatTimeout);
    deviceHeartbeatTimer->start();

    confirmTimer->setInterval(1000);
    connect(confirmTimer, &QTimer::timeout, this, [this](){
        if (countdownSeconds <= 0)
            return;
        setCountdown(countdownSeconds - 1);
        if (countdownSeconds == 0) {
            if (mode == ZoneMode::AwaitManualConfirm) {
                enterBlocked(QStringLiteral("区域未确认，电源关闭"));
            } else if (mode == ZoneMode::ManualConfirmed) {
                enterBlocked(QStringLiteral("确认超时，电源关闭"));
            }
        }
    });

    bindMqttSignals();

    if (ownsMqttClient)
        mqttClient->connectToAliyun();
    updateUi();
    QTimer::singleShot(0, this, &PageZone::tryStartAutoDetect);
}

void PageZone::buildUi()
{
    setStyleSheet(QStringLiteral("background-color:#20222f;color:white;"));

    titleLabel = new QLabel(QStringLiteral("工作区 / 危险区控制"), this);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet(QStringLiteral("font-size:26px;font-weight:bold;"));

    statusLabel = new QLabel(QStringLiteral("等待启动"), this);
    statusLabel->setAlignment(Qt::AlignCenter);
    statusLabel->setStyleSheet(QStringLiteral("font-size:20px;color:#f0c674;"));

    countdownLabel = new QLabel(QStringLiteral("倒计时：--"), this);
    countdownLabel->setAlignment(Qt::AlignCenter);
    countdownLabel->setStyleSheet(QStringLiteral("font-size:18px;color:#9cdcfe;"));

    startButton = new QPushButton(QStringLiteral("开始自动识别"), this);
    yesButton = new QPushButton(QStringLiteral("是"), this);
    noButton = new QPushButton(QStringLiteral("否"), this);

    connect(startButton, &QPushButton::clicked, this, &PageZone::onStartAutoDetect);
    connect(yesButton, &QPushButton::clicked, this, &PageZone::onConfirmYes);
    connect(noButton, &QPushButton::clicked, this, &PageZone::onConfirmNo);

    QHBoxLayout *buttonRow = new QHBoxLayout();
    buttonRow->addWidget(startButton);
    buttonRow->addWidget(yesButton);
    buttonRow->addWidget(noButton);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(titleLabel);
    layout->addWidget(statusLabel);
    layout->addWidget(countdownLabel);
    layout->addLayout(buttonRow);
    layout->addStretch();
}

void PageZone::bindMqttSignals()
{
    connect(mqttClient, &AliyunMqttClient::stateChanged,
            this, &PageZone::onMqttStateChanged);
    connect(mqttClient, &AliyunMqttClient::errorOccurred,
            this, &PageZone::onMqttError);
    connect(mqttClient, &AliyunMqttClient::jsonMessageReceived,
            this, &PageZone::onZoneCommandReceived);
}

void PageZone::configureMqtt()
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    AliyunMqttClient::Config config;
    config.productKey = env.value(QStringLiteral("ALIYUN_PRODUCT_KEY"), QStringLiteral("k29ovUMboAH"));
    config.deviceName = env.value(QStringLiteral("ALIYUN_DEVICE_NAME"), QStringLiteral("0122-qt"));
    config.deviceSecret = env.value(QStringLiteral("ALIYUN_DEVICE_SECRET"));
    config.password = env.value(QStringLiteral("ALIYUN_PASSWORD"),
                                QStringLiteral("91a8d87f1aaffcab93b149580098d24574b97a447c90823e6943f72142560513"));
    config.regionId = env.value(QStringLiteral("ALIYUN_REGION_ID"), QStringLiteral("cn-shanghai"));
    config.mqttHostUrl = env.value(QStringLiteral("ALIYUN_MQTT_HOST_URL"),
                                   QStringLiteral("iot-06z00be8pk7p1uz.mqtt.iothub.aliyuncs.com"));
    config.clientId = env.value(QStringLiteral("ALIYUN_CLIENT_ID"),
                                QStringLiteral("k29ovUMboAH.0122-qt|securemode=2,signmethod=hmacsha256,timestamp=1781166475258|"));
    config.subscribeTopics << QStringLiteral("/k29ovUMboAH/0122-qt/user/get");

    mqttClient->configure(config);
}

void PageZone::onStartAutoDetect()
{
    autoDetectStarted = true;
    manualOverrideActive = false;
    retryCount = 0;
    startDetectionAttempt();
}

void PageZone::onConfirmYes()
{
    if (mode != ZoneMode::AwaitManualConfirm && mode != ZoneMode::ManualConfirmed)
        return;

    mode = ZoneMode::ManualConfirmed;
    manualOverrideActive = true;
    setStatusText(QStringLiteral("请在规定时间内确认电源可开启"));
    setCountdown(kRecheckIntervalSeconds);
    updateUi();
    confirmTimer->start();
    if (!recheckTimer->isActive())
        recheckTimer->start();
    publishZoneControl(QStringLiteral("zone_confirm_yes"), 1, QStringLiteral("a_mode"));
}

void PageZone::onConfirmNo()
{
    if (mode != ZoneMode::AwaitManualConfirm && mode != ZoneMode::ManualConfirmed)
        return;

    retryTimer->stop();
    recheckTimer->stop();
    confirmTimer->stop();
    mode = ZoneMode::Blocked;
    manualOverrideActive = false;
    setStatusText(QStringLiteral("区域未划分，保持关闭"));
    resetCountdown();
    updateUi();
    publishZoneControl(QStringLiteral("zone_confirm_no"), 0, QStringLiteral("blocked"));
}

void PageZone::onRetryTimeout()
{
    if (mode != ZoneMode::AutoDetect)
        return;

    ++retryCount;
    if (retryCount > kAutoRetryLimit) {
        enterAwaitManualConfirm();
        return;
    }

    startDetectionAttempt();
}

void PageZone::onRecheckTimeout()
{
    if (mode != ZoneMode::ManualConfirmed)
        return;

    setStatusText(QStringLiteral("等待 RK 自动重新识别区域"));
}

void PageZone::onDeviceHeartbeatTimeout()
{
    emitDeviceStatus();
}

void PageZone::onMqttStateChanged(QMqttClient::ClientState state)
{
    if (state == QMqttClient::Connected) {
        tryStartAutoDetect();
    }
}

void PageZone::onMqttError(const QString &message)
{
    setStatusText(QStringLiteral("MQTT: %1").arg(message));
}

void PageZone::onZoneCommandReceived(const QString &topic, const QJsonObject &payload)
{
    if (topic != QStringLiteral("/k29ovUMboAH/0122-qt/user/get"))
        return;

    const QString cmd = payload.value(QStringLiteral("cmd")).toString();
    if (cmd == QStringLiteral("heartbeat") || cmd == QStringLiteral("rk_heartbeat")) {
        markRkOnline();
        if (payload.value(QStringLiteral("stm32_online")).toInt() == 1)
            markStm32Online();
    } else if (cmd == QStringLiteral("stm32_heartbeat") || cmd == QStringLiteral("stm32_online")) {
        markStm32Online();
    } else if (cmd == QStringLiteral("zone_detect_result")) {
        const int ok = payload.value(QStringLiteral("ok")).toInt();
        if (ok == 1) {
            enterNormalRunning();
        } else if (manualOverrideActive) {
            setStatusText(QStringLiteral("请在规定时间内确认电源可开启"));
        } else if (mode == ZoneMode::AutoDetect) {
            onRetryTimeout();
        } else if (mode != ZoneMode::AwaitManualConfirm && mode != ZoneMode::Blocked) {
            enterAwaitManualConfirm();
        }
    } else if (cmd == QStringLiteral("zone_reset")) {
        enterNormalRunning();
    }
}

void PageZone::emitDeviceStatus()
{
    const QDateTime now = QDateTime::currentDateTime();
    const bool rkOnline = lastRkHeartbeat.isValid()
        && lastRkHeartbeat.secsTo(now) <= kDeviceOfflineTimeoutSeconds;
    const bool stm32Online = lastStm32Heartbeat.isValid()
        && lastStm32Heartbeat.secsTo(now) <= kDeviceOfflineTimeoutSeconds;
    emit deviceOnlineChanged(rkOnline, stm32Online);
}

void PageZone::markRkOnline()
{
    lastRkHeartbeat = QDateTime::currentDateTime();
    emitDeviceStatus();
    tryStartAutoDetect();
}

void PageZone::markStm32Online()
{
    lastStm32Heartbeat = QDateTime::currentDateTime();
    emitDeviceStatus();
}

void PageZone::updateUi()
{
    const bool canConfirm = (mode == ZoneMode::AwaitManualConfirm || mode == ZoneMode::ManualConfirmed);
    yesButton->setEnabled(canConfirm);
    noButton->setEnabled(canConfirm);
    startButton->setEnabled(mode != ZoneMode::AutoDetect);

    if (mode == ZoneMode::NormalRunning)
        startButton->setText(QStringLiteral("区域已划分正常运行"));
    else if (mode == ZoneMode::AwaitManualConfirm)
        startButton->setText(QStringLiteral("区域未划分是否继续"));
    else if (mode == ZoneMode::ManualConfirmed)
        startButton->setText(QStringLiteral("请在规定时间内确认电源可开启"));
    else
        startButton->setText(QStringLiteral("开始自动识别"));
}

void PageZone::setStatusText(const QString &text)
{
    statusLabel->setText(text);
}

void PageZone::setCountdown(int seconds)
{
    countdownSeconds = qMax(0, seconds);
    countdownLabel->setText(QStringLiteral("倒计时：%1 秒").arg(countdownSeconds));
}

void PageZone::resetCountdown()
{
    confirmTimer->stop();
    setCountdown(0);
}

void PageZone::tryStartAutoDetect()
{
    if (autoDetectStarted || !mqttClient->isConnected() || !lastRkHeartbeat.isValid())
        return;

    onStartAutoDetect();
}

void PageZone::startDetectionAttempt()
{
    mode = ZoneMode::AutoDetect;
    setStatusText(QStringLiteral("等待 RK 自动识别工作区和危险区"));
    setCountdown(kDetectTimeoutSeconds);
    updateUi();
    retryTimer->start();
}

void PageZone::enterAwaitManualConfirm()
{
    retryTimer->stop();
    mode = ZoneMode::AwaitManualConfirm;
    setStatusText(QStringLiteral("区域未划分是否继续"));
    setCountdown(kConfirmWindowSeconds);
    updateUi();
    publishZoneControl(QStringLiteral("zone_need_confirm"), 0, QStringLiteral("pending"));
    confirmTimer->start();
}

void PageZone::enterNormalRunning()
{
    retryTimer->stop();
    recheckTimer->stop();
    mode = ZoneMode::NormalRunning;
    manualOverrideActive = false;
    resetCountdown();
    setStatusText(QStringLiteral("区域已划分正常运行"));
    updateUi();
    publishZoneControl(QStringLiteral("zone_detect_ok"), 1, QStringLiteral("normal"));
}

void PageZone::enterBlocked(const QString &reason)
{
    retryTimer->stop();
    recheckTimer->stop();
    mode = ZoneMode::Blocked;
    manualOverrideActive = false;
    setStatusText(reason);
    resetCountdown();
    updateUi();
    publishZoneControl(QStringLiteral("zone_timeout"), 0, QStringLiteral("blocked"));
}

void PageZone::publishZoneControl(const QString &cmd, int enable, const QString &modeText)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), cmd);
    obj.insert(QStringLiteral("enable"), enable);
    obj.insert(QStringLiteral("mode"), modeText);
    obj.insert(QStringLiteral("ts"), QDateTime::currentMSecsSinceEpoch());
    if (!mqttClient->publishJson(buildZoneControlTopic(), obj, 1, false))
        setStatusText(QStringLiteral("MQTT 未连接，命令暂未发出"));
}

QString PageZone::buildZoneControlTopic()
{
    return QStringLiteral("/k29ovUMboAH/0122-qt/user/update");
}
