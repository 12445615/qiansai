QT       += core gui network multimedia multimediawidgets charts mqtt

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    ModuleStateMachine.cpp \
    AliyunMqttClient.cpp \
    GaugeWidget.cpp \
    XVideoThread.cpp \
    httpclient.cpp \
    main.cpp \
    mainwindow.cpp \
    page_camera.cpp \
    page_home.cpp \
    page_mechinemanager.cpp \
    page_resourcemanger.cpp \
    PageZone.cpp \
    page_sensor.cpp \
    worker.cpp

HEADERS += \
    ModuleStateMachine.h \
    AliyunMqttClient.h \
    GaugeWidget.h \
    XVideoThread.h \
    httpclient.h \
    mainwindow.h \
    page_camera.h \
    page_home.h \
    page_mechinemanager.h \
    page_resourcemanager.h \
    PageZone.h \
    page_sensor.h \
    worker.h


# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
INCLUDEPATH += /usr/local/ffmpeg/FFmpeg-master
LIBS += -L/usr/local/ffmpeg/FFmpeg-master\
        -lavformat\
        -lavfilter\
        -lavcodec\
        -lavutil\
        -lswscale\
        -lswresample\
        -lavdevice
RESOURCES +=
