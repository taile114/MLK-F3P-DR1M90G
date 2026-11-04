QT += core gui widgets network

CONFIG += c++17 link_pkgconfig
CONFIG -= app_bundle
PKGCONFIG += openssl

TEMPLATE = app
TARGET = mlk_ps_local

INCLUDEPATH += ../common ../remote-pc/src src

SOURCES += \
    ../common/crypto.cpp \
    ../common/securechannel.cpp \
    src/boardtime.cpp \
    src/datasource.cpp \
    src/klocalwindow.cpp \
    src/main.cpp \
    src/npu.cpp \
    src/npumonitor.cpp \
    src/replaycontroller.cpp \
    ../common/replaypanel.cpp \
    ../common/inferencejournal.cpp \
    src/pl_host.cpp \
    src/plsettingsdialog.cpp \
    src/systemmonitor.cpp \
    src/theme.cpp \
    src/virtualkeyboard.cpp \
    ../remote-pc/src/faultstore.cpp \
    ../remote-pc/src/plotwidget.cpp \
    ../remote-pc/src/receiver.cpp \
    ../remote-pc/src/sensorpanel.cpp

HEADERS += \
    ../common/crypto.h \
    ../common/securechannel.h \
    src/boardtime.h \
    src/datasource.h \
    src/dma_frames.h \
    src/klocalwindow.h \
    src/npu.h \
    src/npumonitor.h \
    src/replaycontroller.h \
    ../common/replaypanel.h \
    ../common/diagnosisui.h \
    ../common/rul_engine_api.h \
    ../common/inferencejournal.h \
    src/pl_host.h \
    src/plsettingsdialog.h \
    src/systemmonitor.h \
    src/theme.h \
    src/virtualkeyboard.h \
    ../remote-pc/src/appconfig.h \
    ../remote-pc/src/fft.h \
    ../remote-pc/src/faultstore.h \
    ../remote-pc/src/frame.h \
    ../remote-pc/src/plotwidget.h \
    ../remote-pc/src/receiver.h \
    ../remote-pc/src/sensorpanel.h

SOURCES += ../common/workorders.cpp ../common/workorderoutbox.cpp
HEADERS += ../common/workorders.h ../common/workorderoutbox.h ../common/sharedconfig.h
SOURCES += src/wifidialog.cpp src/networksettingsdialog.cpp src/portaldialog.cpp
HEADERS += src/wifidialog.h src/networksettingsdialog.h src/portaldialog.h ../common/captiveportal.h

SOURCES += ../common/peerdiscovery.cpp
HEADERS += ../common/peerdiscovery.h

SOURCES += src/alarmcontroller.cpp src/modelfaultserver.cpp
HEADERS += src/alarmcontroller.h src/modelfaultserver.h

SOURCES += ../common/capturestore.cpp ../common/capturedialog.cpp
HEADERS += ../common/capturestore.h ../common/capturedialog.h

SOURCES += ../common/npustats.cpp ../common/npupanel.cpp
HEADERS += ../common/npustats.h ../common/npupanel.h ../common/networkstatus.h ../common/ifaddr.h

HEADERS += ../common/sensorunits.h
HEADERS += src/plsampleclock.h
HEADERS += src/faultrecordpolicy.h

# 单主故障模型接入：五类契约、API 2、引擎 ABI、原生块与实时诊断
SOURCES += ../common/faultresult.cpp
HEADERS += ../common/faulttypes.h ../common/faultresult.h ../common/faultstatus.h \
    ../common/fault_engine_api.h
SOURCES += src/faultinputadapter.cpp src/faultpreprocess.cpp src/faultruntime.cpp \
    src/faultworker.cpp src/faultlivecontroller.cpp src/faultpanel.cpp src/faultlog.cpp \
    src/faultdataset.cpp src/faultevaluator.cpp src/faultevalpanel.cpp
HEADERS += src/faultinputadapter.h src/faultpreprocess.h src/faultruntime.h \
    src/faultworker.h src/faultlivecontroller.h src/faultpanel.h src/faultlog.h \
    src/faultdataset.h src/faultevaluator.h src/faultevalpanel.h
