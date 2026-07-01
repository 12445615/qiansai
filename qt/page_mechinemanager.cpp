#include "page_mechinemanager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFileDialog>
#include <QGroupBox>

PageMechineManager::PageMechineManager(QWidget *parent) : QWidget(parent)
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    /* -------------------- 资源监控 -------------------- */

    QGroupBox *resourceBox = new QGroupBox("系统资源监控");

    cpuBar = new QProgressBar();
    memBar = new QProgressBar();
    npuBar = new QProgressBar();
    diskBar = new QProgressBar();

    cpuBar->setRange(0,100);
    memBar->setRange(0,100);
    npuBar->setRange(0,100);
    diskBar->setRange(0,100);

    QGridLayout *resourceLayout = new QGridLayout();

    resourceLayout->addWidget(new QLabel("CPU"),0,0);
    resourceLayout->addWidget(cpuBar,0,1);

    resourceLayout->addWidget(new QLabel("Memory"),1,0);
    resourceLayout->addWidget(memBar,1,1);

    resourceLayout->addWidget(new QLabel("NPU"),2,0);
    resourceLayout->addWidget(npuBar,2,1);

    resourceLayout->addWidget(new QLabel("Disk"),3,0);
    resourceLayout->addWidget(diskBar,3,1);

    resourceBox->setLayout(resourceLayout);


    /* -------------------- OTA升级 -------------------- */

    QGroupBox *otaBox = new QGroupBox("OTA升级");

    versionLabel = new QLabel("当前版本: v1.0.0");

    btnSelectFile = new QPushButton("选择升级包");
    btnStartOTA = new QPushButton("开始升级");

    otaProgress = new QProgressBar();
    otaProgress->setRange(0,100);

    QVBoxLayout *otaLayout = new QVBoxLayout();

    otaLayout->addWidget(versionLabel);
    otaLayout->addWidget(btnSelectFile);
    otaLayout->addWidget(btnStartOTA);
    otaLayout->addWidget(otaProgress);

    otaBox->setLayout(otaLayout);


    /* -------------------- 顶部布局 -------------------- */

    QHBoxLayout *topLayout = new QHBoxLayout();
    topLayout->addWidget(resourceBox);
    topLayout->addWidget(otaBox);


    /* -------------------- 网络配置 -------------------- */

    QGroupBox *networkBox = new QGroupBox("网络配置");

    editIP = new QLineEdit("192.168.1.10");
    editMQTT = new QLineEdit("192.168.1.100");
    editRTSP = new QLineEdit("8554");

    QGridLayout *networkLayout = new QGridLayout();

    networkLayout->addWidget(new QLabel("设备IP"),0,0);
    networkLayout->addWidget(editIP,0,1);

    networkLayout->addWidget(new QLabel("MQTT服务器"),1,0);
    networkLayout->addWidget(editMQTT,1,1);

    networkLayout->addWidget(new QLabel("RTSP端口"),2,0);
    networkLayout->addWidget(editRTSP,2,1);

    networkBox->setLayout(networkLayout);


    /* -------------------- 系统信息 -------------------- */

    QGroupBox *infoBox = new QGroupBox("系统信息");

    deviceLabel = new QLabel("设备型号: RK3588 Edge Node");
    versionInfo = new QLabel("系统版本: v1.0.0");
    uptimeLabel = new QLabel("运行时间: 00:00:00");

    QVBoxLayout *infoLayout = new QVBoxLayout();

    infoLayout->addWidget(deviceLabel);
    infoLayout->addWidget(versionInfo);
    infoLayout->addWidget(uptimeLabel);

    infoBox->setLayout(infoLayout);


    /* -------------------- 中部布局 -------------------- */

    QHBoxLayout *midLayout = new QHBoxLayout();
    midLayout->addWidget(networkBox);
    midLayout->addWidget(infoBox);


    /* -------------------- 日志 -------------------- */

    QGroupBox *logBox = new QGroupBox("系统日志");

    logText = new QTextEdit();
    logText->setReadOnly(true);

    QVBoxLayout *logLayout = new QVBoxLayout();
    logLayout->addWidget(logText);

    logBox->setLayout(logLayout);


    /* -------------------- 主布局 -------------------- */

    mainLayout->addLayout(topLayout);
    mainLayout->addLayout(midLayout);
    mainLayout->addWidget(logBox);


    /* -------------------- OTA选择文件 -------------------- */

    connect(btnSelectFile,&QPushButton::clicked,[=](){

        QString file = QFileDialog::getOpenFileName(this,"选择升级包");

        if(!file.isEmpty())
        {
            appendLog("选择升级包: " + file);
        }

    });

}


/* -------------------- 更新资源 -------------------- */

void PageMechineManager::updateCPU(float value)
{
    cpuBar->setValue(value);
}

void PageMechineManager::updateMemory(float value)
{
    memBar->setValue(value);
}

void PageMechineManager::updateNPU(float value)
{
    npuBar->setValue(value);
}

void PageMechineManager::updateDisk(float value)
{
    diskBar->setValue(value);
}


/* -------------------- OTA进度 -------------------- */

void PageMechineManager::updateOTAProgress(int value)
{
    otaProgress->setValue(value);
}

void PageMechineManager::appendOTALog(QString log)
{
    logText->append("[OTA] " + log);
}


/* -------------------- 日志 -------------------- */

void PageMechineManager::appendLog(QString log)
{
    logText->append(log);
}
