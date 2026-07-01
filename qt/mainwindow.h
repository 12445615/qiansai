#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QPushButton>
#include <QStackedWidget>
#include <QListWidget>
#include "page_home.h"
#include "page_camera.h"
#include "page_sensor.h"
#include "PageZone.h"
#include "AliyunMqttClient.h"
#include "page_mechinemanager.h"
#include "page_resourcemanager.h"
#include "ModuleStateMachine.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    QWidget *central;
    QListWidget *menu;
    QStackedWidget *stack;

    PageHome *home;
    PageCamera *camera;
    PageMechineManager *mechinemanager;
    PageResourceManager *resourcemanager;
    PageSensor *sensor;
    PageZone *zonePage;
    AliyunMqttClient *sharedMqttClient;
    ModuleStateMachine *stateMachine;


};
#endif // MAINWINDOW_H
