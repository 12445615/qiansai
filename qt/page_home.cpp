#include "page_home.h"

#include <QFont>
#include <QGridLayout>
#include <QVBoxLayout>

PageHome::PageHome(QWidget *parent) : QWidget(parent)
{
    setStyleSheet(QStringLiteral("background-color:#1e1e2f;color:white;"));

    QLabel *title = new QLabel(QStringLiteral("智能多模态环境监测与消防预警系统"));
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size:28px;font-weight:bold;"));

    mqttStatus = new QLabel(QStringLiteral("未连接"));
    dataTimeStatus = new QLabel(QStringLiteral("--"));
    systemSafetyStatus = new QLabel(QStringLiteral("未知"));
    alarmOverview = new QLabel(QStringLiteral("无"));
    aiStatus = new QLabel(QStringLiteral("无目标"));
    environmentStatus = new QLabel(QStringLiteral("等待数据"));
    rtmpStatus = new QLabel(QStringLiteral("未连接"));
    playbackStatus = new QLabel(QStringLiteral("未检测"));
    rkOnlineStatus = new QLabel(QStringLiteral("离线"));
    stm32OnlineStatus = new QLabel(QStringLiteral("离线"));

    QList<QLabel*> statusLabels = {
        mqttStatus,
        dataTimeStatus,
        systemSafetyStatus,
        alarmOverview,
        aiStatus,
        environmentStatus,
        rtmpStatus,
        playbackStatus,
        rkOnlineStatus,
        stm32OnlineStatus
    };
    for (QLabel *label : statusLabels) {
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        label->setStyleSheet(QStringLiteral("font-size:20px;"));
        label->setMinimumHeight(30);
    }

    auto createIcon = []() {
        QLabel *icon = new QLabel;
        icon->setFixedSize(16, 16);
        icon->setStyleSheet(QStringLiteral("background-color:gray;border-radius:8px;"));
        return icon;
    };

    mqttIcon = createIcon();
    dataTimeIcon = createIcon();
    systemSafetyIcon = createIcon();
    alarmIcon = createIcon();
    aiIcon = createIcon();
    environmentIcon = createIcon();
    rtmpIcon = createIcon();
    playbackIcon = createIcon();
    rkOnlineIcon = createIcon();
    stm32OnlineIcon = createIcon();

    QGridLayout *grid = new QGridLayout;
    grid->setHorizontalSpacing(28);
    grid->setVerticalSpacing(18);
    grid->setColumnStretch(2, 1);
    grid->setColumnStretch(5, 1);

    auto addRow = [grid](int row,
                         QLabel *leftIcon,
                         const QString &leftTitle,
                         QLabel *leftValue,
                         QLabel *rightIcon,
                         const QString &rightTitle,
                         QLabel *rightValue) {
        QLabel *leftLabel = new QLabel(leftTitle);
        QLabel *rightLabel = new QLabel(rightTitle);
        leftLabel->setStyleSheet(QStringLiteral("font-size:16px;font-weight:bold;"));
        rightLabel->setStyleSheet(QStringLiteral("font-size:16px;font-weight:bold;"));

        grid->addWidget(leftIcon, row, 0);
        grid->addWidget(leftLabel, row, 1);
        grid->addWidget(leftValue, row, 2);
        grid->addWidget(rightIcon, row, 3);
        grid->addWidget(rightLabel, row, 4);
        grid->addWidget(rightValue, row, 5);
    };

    addRow(0, mqttIcon, QStringLiteral("MQTT连接:"), mqttStatus,
           dataTimeIcon, QStringLiteral("数据更新时间:"), dataTimeStatus);
    addRow(1, systemSafetyIcon, QStringLiteral("系统安全状态:"), systemSafetyStatus,
           alarmIcon, QStringLiteral("最近报警:"), alarmOverview);
    addRow(2, aiIcon, QStringLiteral("AI识别状态:"), aiStatus,
           environmentIcon, QStringLiteral("环境数据状态:"), environmentStatus);
    addRow(3, rtmpIcon, QStringLiteral("视频流状态:"), rtmpStatus,
           playbackIcon, QStringLiteral("录像回放状态:"), playbackStatus);
    addRow(4, rkOnlineIcon, QStringLiteral("RK3588在线:"), rkOnlineStatus,
           stm32OnlineIcon, QStringLiteral("STM32在线:"), stm32OnlineStatus);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(title);
    mainLayout->addSpacing(20);
    mainLayout->addLayout(grid);
    mainLayout->addStretch();
}

void PageHome::setStateMachine(ModuleStateMachine *stateMachine)
{
    if (m_stateMachine) {
        disconnect(m_stateMachine, nullptr, this, nullptr);
    }

    m_stateMachine = stateMachine;
    if (!m_stateMachine) {
        return;
    }

    connect(m_stateMachine, &ModuleStateMachine::moduleStateChanged,
            this, &PageHome::applyModuleState);

    applyModuleState(ModuleStateMachine::Camera,
                     m_stateMachine->state(ModuleStateMachine::Camera),
                     m_stateMachine->message(ModuleStateMachine::Camera));
    applyModuleState(ModuleStateMachine::Rtmp,
                     m_stateMachine->state(ModuleStateMachine::Rtmp),
                     m_stateMachine->message(ModuleStateMachine::Rtmp));
    applyModuleState(ModuleStateMachine::Sensor,
                     m_stateMachine->state(ModuleStateMachine::Sensor),
                     m_stateMachine->message(ModuleStateMachine::Sensor));
    applyModuleState(ModuleStateMachine::Alarm,
                     m_stateMachine->state(ModuleStateMachine::Alarm),
                     m_stateMachine->message(ModuleStateMachine::Alarm));
}

void PageHome::updateCloudSummary(const QString &dataTime,
                                  const QString &systemSafety,
                                  const QString &recentAlarm,
                                  const QString &aiState,
                                  const QString &environmentState)
{
    dataTimeStatus->setText(dataTime.isEmpty() ? QStringLiteral("--") : dataTime);
    aiStatus->setText(aiState.isEmpty() ? QStringLiteral("无目标") : aiState);
    environmentStatus->setText(environmentState.isEmpty() ? QStringLiteral("等待数据") : environmentState);
    systemSafetyStatus->setText(systemSafety.isEmpty() ? QStringLiteral("未知") : systemSafety);
    alarmOverview->setText(recentAlarm.isEmpty() ? QStringLiteral("无") : recentAlarm);

    setPlainStatus(dataTimeIcon, dataTimeStatus, dataTimeStatus->text(), QStringLiteral("#98c379"));
    setPlainStatus(aiIcon, aiStatus, aiStatus->text(),
                   aiStatus->text() == QStringLiteral("无目标") ? QStringLiteral("#98c379") : QStringLiteral("#f0c674"));
    setPlainStatus(environmentIcon, environmentStatus, environmentStatus->text(),
                   environmentStatus->text() == QStringLiteral("正常") ? QStringLiteral("#98c379") : QStringLiteral("#f0c674"));
    setPlainStatus(systemSafetyIcon, systemSafetyStatus, systemSafetyStatus->text(),
                   systemSafetyStatus->text() == QStringLiteral("正常") ? QStringLiteral("#98c379") : QStringLiteral("#e06c75"));
    setPlainStatus(alarmIcon, alarmOverview, alarmOverview->text(),
                   alarmOverview->text() == QStringLiteral("无") ? QStringLiteral("#98c379") : QStringLiteral("#e06c75"));
}

void PageHome::updateDeviceOnlineState(bool rkOnline, bool stm32Online)
{
    rkOnlineStatus->setText(rkOnline ? QStringLiteral("在线") : QStringLiteral("离线"));
    stm32OnlineStatus->setText(stm32Online ? QStringLiteral("在线") : QStringLiteral("离线"));

    setPlainStatus(rkOnlineIcon, rkOnlineStatus, rkOnlineStatus->text(),
                   rkOnline ? QStringLiteral("#98c379") : QStringLiteral("#e06c75"));
    setPlainStatus(stm32OnlineIcon, stm32OnlineStatus, stm32OnlineStatus->text(),
                   stm32Online ? QStringLiteral("#98c379") : QStringLiteral("#e06c75"));
}

void PageHome::applyModuleState(ModuleStateMachine::Module module,
                                ModuleStateMachine::State state,
                                const QString &message)
{
    switch (module) {
    case ModuleStateMachine::Camera:
        updateIndicator(playbackIcon, playbackStatus, state, message);
        break;
    case ModuleStateMachine::Ai:
        break;
    case ModuleStateMachine::Rtmp:
        updateIndicator(rtmpIcon, rtmpStatus, state, message);
        break;
    case ModuleStateMachine::Sensor:
        updateIndicator(mqttIcon, mqttStatus, state, message);
        break;
    case ModuleStateMachine::Alarm:
        if (message != QStringLiteral("无")) {
            updateIndicator(alarmIcon, alarmOverview, state, message);
        }
        break;
    }
}

void PageHome::updateIndicator(QLabel *icon, QLabel *label,
                               ModuleStateMachine::State state,
                               const QString &message)
{
    QString color = QStringLiteral("gray");
    switch (state) {
    case ModuleStateMachine::Unknown:
        color = QStringLiteral("gray");
        break;
    case ModuleStateMachine::Starting:
    case ModuleStateMachine::Warning:
        color = QStringLiteral("#f0c674");
        break;
    case ModuleStateMachine::Running:
        color = QStringLiteral("#98c379");
        break;
    case ModuleStateMachine::Error:
    case ModuleStateMachine::Stopped:
        color = QStringLiteral("#e06c75");
        break;
    }

    setPlainStatus(icon, label, message.isEmpty() ? QStringLiteral("未知") : message, color);
}

void PageHome::setPlainStatus(QLabel *icon, QLabel *label,
                              const QString &message,
                              const QString &color)
{
    label->setText(message);
    icon->setStyleSheet(QStringLiteral("background-color:%1;border-radius:8px;").arg(color));
}
