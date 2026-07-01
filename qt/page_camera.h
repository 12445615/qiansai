#ifndef PAGE_CAMERA_H
#define PAGE_CAMERA_H

#include <QWidget>
#include <QVideoWidget>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QListWidget>
#include <QComboBox>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QTimer>
#include <QThread>
#include <QImage>
#include "XVideoThread.h"

class XVideoThread;

struct MediaObjectInfo {
    QString fileName;
    QString filePath;
};

class PageCamera : public QWidget
{
    Q_OBJECT
public:
    explicit PageCamera(QWidget *parent = nullptr);
    ~PageCamera() override;

signals:
    void frameReady(const QImage &img);
    void positionChanged(qint64 pos);      // 播放进度
    void durationChanged(qint64 duration); // 视频总时长
    void liveStreamStateChanged(bool ok, const QString &message);
    void playbackStateChanged(bool ok, const QString &message);

private slots:
    void btn_play_clicked();
    void btn_next_clicked();
    void btn_volmeup_clicked();
    void btn_volmedown_clicked();
    void btn_fullscreen_clicked();
    void listWidgetClicked(QListWidgetItem *item);


private:
    void videoLayout();
    void mediaPlayerInit();
    void scanVideoFiles();
    void loadPlaybackDates(const QString &deviceName);
    void loadPlaybackSegments(const QString &dateName);
    void playSelectedPlaybackSegment();
    void startVideoPlayback(XVideoThread *&thread,
                            const QString &url,
                            QLabel *targetLabel,
                            const QString &logPrefix);
    void appendAIResult(const QString &result);
    void stopVideoThread(XVideoThread *&thread, const QString &logPrefix);


    // UI 控件
    QVideoWidget *videoWidget;
    QPushButton *pushButton[5];
    QSlider *durationSlider;
    QSlider *playbackDurationSlider;
    QSlider *volumeSlider;
    QLabel *label[2];       // 改成指针
    QListWidget *listWidget;
    QTextEdit *aiResultText;
    QLabel *aiTitleLabel;
    QStackedWidget *cameraStack;
    QWidget *livePage;
    QWidget *playbackPage;
    QPushButton *liveViewBtn;
    QPushButton *playbackViewBtn;
    QLabel *playbackVideoLabel;
    QComboBox *playbackDeviceCombo;
    QComboBox *playbackDateCombo;
    QComboBox *playbackSpeedCombo;
    QPushButton *playbackRefreshBtn;
    QPushButton *playbackPlayBtn;
    QLabel *playbackPositionLabel;
    QLabel *playbackDurationLabel;
    QVBoxLayout *mainLayout;
    QHBoxLayout *topLayout;
    QHBoxLayout *controlLayout;
    QLineEdit *rtmpLineEdit;
    QPushButton *rtmpPlayBtn;
    QLabel *videoLabel;

    // 播放状态
    QList<MediaObjectInfo> mediaObjectInfo;
    int currentIndex = 0;
    QString currentRTMPUrl;
    QString playbackRootPath;

    XVideoThread *liveVideoThread;     // 实时监控播放线程
    XVideoThread *playbackVideoThread; // 回放播放线程
};

#endif // PAGE_CAMERA_H
