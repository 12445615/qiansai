#include "page_camera.h"
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QUrl>
#include <QLineEdit>
#include <QPushButton>
#include <QThread>
#include <QTextCursor>
#include <QDebug>
#include <QLabel>
#include <QStandardPaths>
#include <QSettings>
#include <cstdio>

#include "XVideoThread.h"

namespace {
const char *kDefaultRtmpUrl = "rtmp://10.36.113.231:1935/fire_live/test";
const char *kRtmpSettingsKey = "camera/rtmp_url";
}

PageCamera::PageCamera(QWidget *parent) : QWidget(parent)
{
    videoLayout();
    scanVideoFiles();

    pushButton[0]->setCheckable(true);

    connect(pushButton[0], &QPushButton::clicked, this, &PageCamera::btn_play_clicked);
    connect(pushButton[1], &QPushButton::clicked, this, &PageCamera::btn_next_clicked);
    connect(pushButton[2], &QPushButton::clicked, this, &PageCamera::btn_volmedown_clicked);
    connect(pushButton[3], &QPushButton::clicked, this, &PageCamera::btn_volmeup_clicked);
    connect(pushButton[4], &QPushButton::clicked, this, &PageCamera::btn_fullscreen_clicked);

    connect(listWidget, &QListWidget::itemClicked, this, &PageCamera::listWidgetClicked);
    connect(listWidget, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item){
        listWidgetClicked(item);
        playSelectedPlaybackSegment();
    });

    liveVideoThread = nullptr;
    playbackVideoThread = nullptr;

    // ================= RTMP 播放 =================
    connect(rtmpPlayBtn, &QPushButton::clicked, this, [=](){

        QString url = rtmpLineEdit->text().trimmed();
        if(url.isEmpty()) return;

        QSettings settings;
        settings.setValue(kRtmpSettingsKey, url);
        currentRTMPUrl = url;
        startVideoPlayback(liveVideoThread,
                           url,
                           videoLabel,
                           QStringLiteral("RTMP"));
    });

    // ================= 进度条 =================
    connect(this, &PageCamera::positionChanged, this, [=](qint64 pos){
        if (!durationSlider->isSliderDown())
            durationSlider->setValue(pos / 1000);

        int sec = pos / 1000;
        int min = sec / 60;
        sec %= 60;

        label[0]->setText(QString("%1:%2")
                              .arg(min,2,10,QChar('0'))
                              .arg(sec,2,10,QChar('0')));
    });

    connect(this, &PageCamera::durationChanged, this, [=](qint64 dur){
        durationSlider->setRange(0, dur / 1000);

        int sec = dur / 1000;
        int min = sec / 60;
        sec %= 60;

        label[1]->setText(QString("/%1:%2")
                              .arg(min,2,10,QChar('0'))
                              .arg(sec,2,10,QChar('0')));
    });
}

PageCamera::~PageCamera()
{
    stopVideoThread(liveVideoThread, QStringLiteral("RTMP"));
    stopVideoThread(playbackVideoThread, QStringLiteral("回放"));
}

void PageCamera::videoLayout()
{
    this->resize(1920,1280);

    liveViewBtn = new QPushButton(QStringLiteral("实时监控"), this);
    playbackViewBtn = new QPushButton(QStringLiteral("回放列表"), this);
    liveViewBtn->setCheckable(true);
    playbackViewBtn->setCheckable(true);
    liveViewBtn->setChecked(true);

    // ================= FFmpeg 显示用 =================
    videoLabel = new QLabel(this);
    videoLabel->setAlignment(Qt::AlignCenter);
    videoLabel->setStyleSheet("background:black;");
    videoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    videoLabel->setMinimumSize(960, 540);

    listWidget = new QListWidget();
    listWidget->setMinimumHeight(520);

    playbackVideoLabel = new QLabel(this);
    playbackVideoLabel->setAlignment(Qt::AlignCenter);
    playbackVideoLabel->setStyleSheet("background:black;");
    playbackVideoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    playbackVideoLabel->setMinimumSize(800, 450);

    playbackDeviceCombo = new QComboBox(this);
    playbackDateCombo = new QComboBox(this);
    playbackRefreshBtn = new QPushButton(QStringLiteral("刷新"), this);
    playbackPlayBtn = new QPushButton(QStringLiteral("播放选中片段"), this);
    playbackSpeedCombo = new QComboBox(this);
    playbackSpeedCombo->addItem(QStringLiteral("1x"), 1.0);
    playbackSpeedCombo->addItem(QStringLiteral("2x"), 2.0);
    playbackPositionLabel = new QLabel(QStringLiteral("00:00"), this);
    playbackDurationLabel = new QLabel(QStringLiteral("/00:00"), this);
    playbackDurationSlider = new QSlider(Qt::Horizontal, this);
    playbackDurationSlider->setRange(0, 0);

    durationSlider = new QSlider(Qt::Horizontal);

    volumeSlider = new QSlider(Qt::Horizontal);
    volumeSlider->setRange(0,100);
    volumeSlider->setValue(50);

    for(int i=0;i<5;i++)
        pushButton[i] = new QPushButton(this);

    pushButton[0]->setText("Play");
    pushButton[1]->setText("Next");
    pushButton[2]->setText("-");
    pushButton[3]->setText("+");
    pushButton[4]->setText("Full");

    label[0] = new QLabel("00:00", this);
    label[1] = new QLabel("/00:00", this);

    aiTitleLabel = new QLabel("AI Detection Result", this);

    aiResultText = new QTextEdit(this);
    aiResultText->setReadOnly(true);
    aiResultText->setMinimumHeight(90);
    aiResultText->setStyleSheet(
        "QTextEdit{background:#111;color:#00ff7f;border:1px solid #444;font-family:Consolas;}");

    QHBoxLayout *viewSwitchLayout = new QHBoxLayout();
    viewSwitchLayout->addWidget(liveViewBtn);
    viewSwitchLayout->addWidget(playbackViewBtn);
    viewSwitchLayout->addStretch();

    topLayout = new QHBoxLayout();
    topLayout->addWidget(videoLabel);

    controlLayout = new QHBoxLayout();
    controlLayout->addWidget(pushButton[0]);
    controlLayout->addWidget(pushButton[1]);
    controlLayout->addWidget(pushButton[2]);
    controlLayout->addWidget(volumeSlider);
    controlLayout->addWidget(pushButton[3]);
    controlLayout->addWidget(label[0]);
    controlLayout->addWidget(label[1]);
    controlLayout->addStretch();
    controlLayout->addWidget(pushButton[4]);

    QHBoxLayout *rtmpLayout = new QHBoxLayout();
    rtmpLineEdit = new QLineEdit(this);
    rtmpLineEdit->setPlaceholderText("请输入 RTMP 地址，例如 rtmp://192.168.1.100/live/stream");
    QSettings settings;
    rtmpLineEdit->setText(settings.value(kRtmpSettingsKey, QString::fromLatin1(kDefaultRtmpUrl)).toString());

    rtmpPlayBtn = new QPushButton("播放", this);

    rtmpLayout->addWidget(rtmpLineEdit);
    rtmpLayout->addWidget(rtmpPlayBtn);

    QVBoxLayout *aiLayout = new QVBoxLayout();
    aiLayout->addWidget(aiTitleLabel);
    aiLayout->addWidget(aiResultText);

    livePage = new QWidget(this);
    QVBoxLayout *liveLayout = new QVBoxLayout(livePage);
    liveLayout->setContentsMargins(0,0,0,0);
    liveLayout->addLayout(topLayout, 1);
    liveLayout->addWidget(durationSlider);
    liveLayout->addLayout(controlLayout);
    liveLayout->addLayout(rtmpLayout);
    liveLayout->addLayout(aiLayout);

    playbackPage = new QWidget(this);
    QLabel *playbackTitle = new QLabel(QStringLiteral("本地回放文件"), playbackPage);
    playbackTitle->setStyleSheet(QStringLiteral("font-size:18px;font-weight:bold;"));
    QHBoxLayout *playbackToolLayout = new QHBoxLayout();
    playbackToolLayout->addWidget(new QLabel(QStringLiteral("设备:"), playbackPage));
    playbackToolLayout->addWidget(playbackDeviceCombo);
    playbackToolLayout->addWidget(new QLabel(QStringLiteral("日期:"), playbackPage));
    playbackToolLayout->addWidget(playbackDateCombo);
    playbackToolLayout->addWidget(playbackRefreshBtn);
    playbackToolLayout->addWidget(playbackPlayBtn);
    playbackToolLayout->addWidget(new QLabel(QStringLiteral("倍速:"), playbackPage));
    playbackToolLayout->addWidget(playbackSpeedCombo);
    playbackToolLayout->addStretch();
    QHBoxLayout *playbackContentLayout = new QHBoxLayout();
    playbackContentLayout->addWidget(playbackVideoLabel, 3);
    playbackContentLayout->addWidget(listWidget, 1);
    QVBoxLayout *playbackLayout = new QVBoxLayout(playbackPage);
    playbackLayout->setContentsMargins(0,0,0,0);
    playbackLayout->addWidget(playbackTitle);
    playbackLayout->addLayout(playbackToolLayout);
    playbackLayout->addLayout(playbackContentLayout, 1);
    QHBoxLayout *playbackProgressLayout = new QHBoxLayout();
    playbackProgressLayout->addWidget(playbackPositionLabel);
    playbackProgressLayout->addWidget(playbackDurationSlider, 1);
    playbackProgressLayout->addWidget(playbackDurationLabel);
    playbackLayout->addLayout(playbackProgressLayout);

    cameraStack = new QStackedWidget(this);
    cameraStack->addWidget(livePage);
    cameraStack->addWidget(playbackPage);

    mainLayout = new QVBoxLayout();
    mainLayout->addLayout(viewSwitchLayout);
    mainLayout->addWidget(cameraStack, 1);

    setLayout(mainLayout);

    connect(liveViewBtn, &QPushButton::clicked, this, [this](){
        cameraStack->setCurrentWidget(livePage);
        liveViewBtn->setChecked(true);
        playbackViewBtn->setChecked(false);
    });

    connect(playbackViewBtn, &QPushButton::clicked, this, [this](){
        cameraStack->setCurrentWidget(playbackPage);
        liveViewBtn->setChecked(false);
        playbackViewBtn->setChecked(true);
    });

    connect(playbackDeviceCombo, &QComboBox::currentTextChanged,
            this, &PageCamera::loadPlaybackDates);
    connect(playbackDateCombo, &QComboBox::currentTextChanged,
            this, &PageCamera::loadPlaybackSegments);
    connect(playbackRefreshBtn, &QPushButton::clicked,
            this, &PageCamera::scanVideoFiles);
    connect(playbackPlayBtn, &QPushButton::clicked,
            this, &PageCamera::playSelectedPlaybackSegment);
    connect(playbackDurationSlider, &QSlider::sliderReleased, this, [this](){
        if(playbackVideoThread)
            playbackVideoThread->seekToMs(static_cast<qint64>(playbackDurationSlider->value()) * 1000);
    });
    connect(playbackSpeedCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int){
        if(playbackVideoThread)
            playbackVideoThread->setPlaybackRate(playbackSpeedCombo->currentData().toDouble());
    });
}

void PageCamera::scanVideoFiles()
{
    const QString homePath = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    playbackRootPath = QDir(homePath).filePath(QStringLiteral("data/camera_flow/video_segments"));

    playbackDeviceCombo->blockSignals(true);
    playbackDateCombo->blockSignals(true);
    playbackDeviceCombo->clear();
    playbackDateCombo->clear();
    listWidget->clear();
    mediaObjectInfo.clear();

    QDir rootDir(playbackRootPath);
    if(!rootDir.exists()) {
        appendAIResult(QStringLiteral("回放目录不存在: %1").arg(playbackRootPath));
        playbackDeviceCombo->blockSignals(false);
        playbackDateCombo->blockSignals(false);
        return;
    }

    const QFileInfoList deviceDirs = rootDir.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name);

    for(const QFileInfo &deviceDir : deviceDirs)
        playbackDeviceCombo->addItem(deviceDir.fileName());

    playbackDeviceCombo->blockSignals(false);
    playbackDateCombo->blockSignals(false);

    if(playbackDeviceCombo->count() > 0)
        loadPlaybackDates(playbackDeviceCombo->currentText());
    else
        appendAIResult(QStringLiteral("回放目录下没有设备文件夹: %1").arg(playbackRootPath));
}

void PageCamera::loadPlaybackDates(const QString &deviceName)
{
    playbackDateCombo->blockSignals(true);
    playbackDateCombo->clear();
    listWidget->clear();
    mediaObjectInfo.clear();

    if(deviceName.isEmpty()) {
        playbackDateCombo->blockSignals(false);
        return;
    }

    QDir deviceDir(QDir(playbackRootPath).filePath(deviceName));
    const QFileInfoList dateDirs = deviceDir.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name);

    for(const QFileInfo &dateDir : dateDirs)
        playbackDateCombo->addItem(dateDir.fileName());

    playbackDateCombo->blockSignals(false);

    if(playbackDateCombo->count() > 0)
        loadPlaybackSegments(playbackDateCombo->currentText());
    else
        appendAIResult(QStringLiteral("设备目录下没有日期文件夹: %1").arg(deviceDir.absolutePath()));
}

void PageCamera::loadPlaybackSegments(const QString &dateName)
{
    listWidget->clear();
    mediaObjectInfo.clear();

    if(dateName.isEmpty())
        return;

    const QString deviceName = playbackDeviceCombo->currentText();
    if(deviceName.isEmpty())
        return;

    QDir segmentDir(QDir(QDir(playbackRootPath).filePath(deviceName)).filePath(dateName));
    const QFileInfoList files = segmentDir.entryInfoList(
        QStringList() << QStringLiteral("*.ts"),
        QDir::Files,
        QDir::Name);

    for(const QFileInfo &file : files) {
        MediaObjectInfo info{file.fileName(), file.absoluteFilePath()};
        mediaObjectInfo.append(info);
        QListWidgetItem *item = new QListWidgetItem(info.fileName, listWidget);
        item->setToolTip(info.filePath);
    }

    appendAIResult(QStringLiteral("加载回放目录: %1，片段数: %2")
                       .arg(segmentDir.absolutePath())
                       .arg(files.size()));

    if(!mediaObjectInfo.isEmpty()) {
        currentIndex = 0;
        listWidget->setCurrentRow(0);
    } else {
        currentIndex = -1;
    }
}

void PageCamera::playSelectedPlaybackSegment()
{
    if(currentIndex < 0 || currentIndex >= mediaObjectInfo.size()) {
        appendAIResult(QStringLiteral("请先选择一个回放片段"));
        return;
    }

    cameraStack->setCurrentWidget(playbackPage);
    playbackDurationSlider->setValue(0);
    playbackPositionLabel->setText(QStringLiteral("00:00"));
    playbackDurationLabel->setText(QStringLiteral("/00:00"));
    startVideoPlayback(playbackVideoThread,
                       mediaObjectInfo.at(currentIndex).filePath,
                       playbackVideoLabel,
                       QStringLiteral("回放"));
}

void PageCamera::startVideoPlayback(XVideoThread *&thread,
                                    const QString &url,
                                    QLabel *targetLabel,
                                    const QString &logPrefix)
{
    fprintf(stderr, "[VIDEO][ui] play prefix=%s url=%s\n",
            logPrefix.toUtf8().constData(),
            url.toUtf8().constData());

    appendAIResult(QStringLiteral("%1 播放地址：%2").arg(logPrefix, url));

    stopVideoThread(thread, logPrefix);

    thread = new XVideoThread(this);
    fprintf(stderr, "[VIDEO][ui] new %s thread=%p\n",
            logPrefix.toUtf8().constData(),
            static_cast<void *>(thread));
    thread->setUrl(url);

    connect(thread, &XVideoThread::sig_sendInitState,
            this,
            [this, logPrefix](bool ok){
                appendAIResult(ok ? QStringLiteral("%1 初始化成功").arg(logPrefix)
                                  : QStringLiteral("%1 初始化失败").arg(logPrefix));
                if(logPrefix == QStringLiteral("RTMP"))
                    emit liveStreamStateChanged(ok, ok ? QStringLiteral("正常") : QStringLiteral("初始化失败"));
                else if(logPrefix == QStringLiteral("回放"))
                    emit playbackStateChanged(ok, ok ? QStringLiteral("播放中") : QStringLiteral("初始化失败"));
            },
            Qt::QueuedConnection);

    connect(thread, &XVideoThread::sig_errorMessage,
            this,
            [this, logPrefix](const QString &message){
                appendAIResult(QStringLiteral("%1 错误：%2").arg(logPrefix, message));
                if(logPrefix == QStringLiteral("RTMP"))
                    emit liveStreamStateChanged(false, message);
                else if(logPrefix == QStringLiteral("回放"))
                    emit playbackStateChanged(false, message);
            },
            Qt::QueuedConnection);

    connect(thread, &XVideoThread::sig_SendOneFrame,
            this,
            [targetLabel](const QImage &img){
                targetLabel->setPixmap(
                    QPixmap::fromImage(img).scaled(
                        targetLabel->size(),
                        Qt::KeepAspectRatio,
                        Qt::SmoothTransformation));
            },
            Qt::QueuedConnection);

    if(logPrefix == QStringLiteral("回放")){
        thread->setPlaybackRate(playbackSpeedCombo->currentData().toDouble());
        auto formatMs = [](qint64 ms) {
            qint64 totalSec = qMax<qint64>(0, ms / 1000);
            qint64 min = totalSec / 60;
            qint64 sec = totalSec % 60;
            return QStringLiteral("%1:%2")
                .arg(min, 2, 10, QChar('0'))
                .arg(sec, 2, 10, QChar('0'));
        };
        connect(thread, &XVideoThread::sig_durationChanged,
                this,
                [this, formatMs](qint64 durationMs){
                    playbackDurationSlider->setRange(0, static_cast<int>(qMax<qint64>(0, durationMs / 1000)));
                    playbackDurationLabel->setText(QStringLiteral("/%1").arg(formatMs(durationMs)));
                },
                Qt::QueuedConnection);
        connect(thread, &XVideoThread::sig_positionChanged,
                this,
                [this, formatMs](qint64 positionMs){
                    if(!playbackDurationSlider->isSliderDown())
                        playbackDurationSlider->setValue(static_cast<int>(qMax<qint64>(0, positionMs / 1000)));
                    playbackPositionLabel->setText(formatMs(positionMs));
                },
                Qt::QueuedConnection);
    }

    thread->start();
    fprintf(stderr, "[VIDEO][ui] %s thread started=%p\n",
            logPrefix.toUtf8().constData(),
            static_cast<void *>(thread));
}

void PageCamera::appendAIResult(const QString &result)
{
    aiResultText->append(result);
    QTextCursor cursor = aiResultText->textCursor();
    cursor.movePosition(QTextCursor::End);
    aiResultText->setTextCursor(cursor);
}

void PageCamera::stopVideoThread(XVideoThread *&thread, const QString &logPrefix)
{
    if(!thread){
        fprintf(stderr, "[VIDEO][ui] stop %s thread skipped, no thread\n",
                logPrefix.toUtf8().constData());
        return;
    }

    fprintf(stderr, "[VIDEO][ui] stop %s thread begin thread=%p running=%d\n",
            logPrefix.toUtf8().constData(),
            static_cast<void *>(thread),
            thread->isRunning() ? 1 : 0);
    disconnect(thread, nullptr, this, nullptr);
    thread->requestInterruption();
    thread->wait();
    delete thread;
    thread = nullptr;
    fprintf(stderr, "[VIDEO][ui] stop %s thread end\n",
            logPrefix.toUtf8().constData());
}

// ================= 按钮逻辑 =================

void PageCamera::btn_play_clicked()
{

}

void PageCamera::btn_next_clicked()
{
    if(mediaObjectInfo.isEmpty()) return;

    currentIndex++;
    if(currentIndex >= mediaObjectInfo.size())
        currentIndex = 0;
}

void PageCamera::btn_volmeup_clicked()
{
    volumeSlider->setValue(qMin(volumeSlider->value()+5,100));
}

void PageCamera::btn_volmedown_clicked()
{
    volumeSlider->setValue(qMax(volumeSlider->value()-5,0));
}

void PageCamera::btn_fullscreen_clicked()
{
    if(videoLabel->isFullScreen()){
        videoLabel->showNormal();
    } else {
        videoLabel->showFullScreen();
    }
}

void PageCamera::listWidgetClicked(QListWidgetItem *item)
{
    currentIndex = listWidget->row(item);
}
