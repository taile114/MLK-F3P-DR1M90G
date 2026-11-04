QT += core gui widgets network

CONFIG += c++17 link_pkgconfig
CONFIG -= app_bundle
PKGCONFIG += openssl

TEMPLATE = app
TARGET = mlk_udp_host

INCLUDEPATH += ../common

SOURCES += \
    ../common/crypto.cpp \
    ../common/securechannel.cpp \
    ../common/replaypanel.cpp \
    ../common/inferencejournal.cpp \
    ../common/faultresult.cpp \
    src/faultstore.cpp \
    src/injectdialog.cpp \
    src/main.cpp \
    src/mainwindow.cpp \
    src/plotwidget.cpp \
    src/receiver.cpp \
    src/sensorpanel.cpp

HEADERS += \
    ../common/crypto.h \
    ../common/securechannel.h \
    ../common/replaypanel.h \
    ../common/diagnosisui.h \
    ../common/faulttypes.h \
    ../common/faultresult.h \
    ../common/faultstatus.h \
    ../common/fault_engine_api.h \
    ../common/inferencejournal.h \
    src/appconfig.h \
    src/faultstore.h \
    src/fft.h \
    src/frame.h \
    src/injectdialog.h \
    src/mainwindow.h \
    src/plotwidget.h \
    src/receiver.h \
    src/sensorpanel.h

SOURCES += ../common/workorders.cpp
HEADERS += ../common/workorders.h ../common/sharedconfig.h
INCLUDEPATH += ../ps-local/src src
SOURCES += ../ps-local/src/theme.cpp ../ps-local/src/plsettingsdialog.cpp
HEADERS += ../ps-local/src/plsettingsdialog.h

SOURCES += ../common/peerdiscovery.cpp
HEADERS += ../common/peerdiscovery.h

SOURCES += ../common/capturestore.cpp ../common/capturedialog.cpp
HEADERS += ../common/capturestore.h ../common/capturedialog.h
SOURCES += ../ps-local/src/modelfaultserver.cpp
HEADERS += ../ps-local/src/modelfaultserver.h

SOURCES += ../common/npustats.cpp ../common/npupanel.cpp
HEADERS += ../common/npustats.h ../common/npupanel.h ../common/networkstatus.h

HEADERS += ../common/sensorunits.h
