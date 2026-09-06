QT += widgets
CONFIG += c++17 release
TARGET = dlssnr_gui
TEMPLATE = app

INCLUDEPATH += $$PWD/../common

SOURCES += \
    ../layer_linux/src/hotkey.cpp \
    main.cpp \
    mainwindow.cpp \
    passdialog.cpp \
    shm_binder.cpp \
    ../common/runner_discovery.cpp

HEADERS += \
    mainwindow.h \
    passdialog.h \
    shm_binder.h \
    ../common/runner_discovery.h
