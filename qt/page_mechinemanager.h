#ifndef PAGE_MECHINEMANAGER_H
#define PAGE_MECHINEMANAGER_H

#include <QWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QLineEdit>
#include <QTextEdit>
#include <QLabel>
#include <QFrame>

class PageMechineManager : public QWidget
{
    Q_OBJECT

public:
    PageMechineManager(QWidget *parent=nullptr);

public slots:

    /* 资源更新 */
    void updateCPU(float value);
    void updateMemory(float value);
    void updateNPU(float value);
    void updateDisk(float value);

    /* OTA */
    void updateOTAProgress(int value);
    void appendOTALog(QString log);

    /* 系统日志 */
    void appendLog(QString log);

private:

    /* 资源监控 */
    QProgressBar *cpuBar;
    QProgressBar *memBar;
    QProgressBar *npuBar;
    QProgressBar *diskBar;

    /* OTA */
    QLabel *versionLabel;
    QPushButton *btnSelectFile;
    QPushButton *btnStartOTA;
    QProgressBar *otaProgress;

    /* 网络配置 */
    QLineEdit *editIP;
    QLineEdit *editMQTT;
    QLineEdit *editRTSP;

    /* 系统信息 */
    QLabel *deviceLabel;
    QLabel *versionInfo;
    QLabel *uptimeLabel;

    /* 日志 */
    QTextEdit *logText;
};

#endif // PAGE_MECHINEMANAGER_H
