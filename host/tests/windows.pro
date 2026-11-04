include(rul.pro)
TARGET = test_windows
SOURCES -= test_rul.cpp
SOURCES += test_windows.cpp \
    ../ps-local/src/boardtime.cpp ../ps-local/src/klocalwindow.cpp ../ps-local/src/datasource.cpp ../ps-local/src/pl_host.cpp \
    ../ps-local/src/plsettingsdialog.cpp ../ps-local/src/systemmonitor.cpp ../ps-local/src/npumonitor.cpp \
    ../ps-local/src/theme.cpp ../ps-local/src/virtualkeyboard.cpp \
    ../remote-pc/src/mainwindow.cpp ../remote-pc/src/faultstore.cpp \
    ../remote-pc/src/injectdialog.cpp ../remote-pc/src/plotwidget.cpp \
    ../remote-pc/src/sensorpanel.cpp ../remote-pc/src/settingsdialog.cpp
HEADERS += ../ps-local/src/boardtime.h ../ps-local/src/klocalwindow.h ../ps-local/src/datasource.h \
    ../ps-local/src/plsettingsdialog.h ../ps-local/src/systemmonitor.h ../ps-local/src/virtualkeyboard.h \
    ../remote-pc/src/mainwindow.h ../remote-pc/src/injectdialog.h \
    ../remote-pc/src/plotwidget.h ../remote-pc/src/sensorpanel.h ../remote-pc/src/settingsdialog.h

SOURCES += ../common/workorders.cpp ../common/workorderoutbox.cpp
HEADERS += ../common/workorders.h ../common/workorderoutbox.h ../common/sharedconfig.h
SOURCES += ../ps-local/src/wifidialog.cpp ../ps-local/src/networksettingsdialog.cpp ../ps-local/src/portaldialog.cpp
HEADERS += ../ps-local/src/wifidialog.h ../ps-local/src/networksettingsdialog.h ../ps-local/src/portaldialog.h ../common/captiveportal.h

SOURCES += ../common/peerdiscovery.cpp
HEADERS += ../common/peerdiscovery.h

SOURCES += ../ps-local/src/alarmcontroller.cpp ../ps-local/src/modelfaultserver.cpp
HEADERS += ../ps-local/src/alarmcontroller.h ../ps-local/src/modelfaultserver.h

SOURCES += ../common/mlk_fault_client.c

SOURCES += ../common/capturestore.cpp ../common/capturedialog.cpp
HEADERS += ../common/capturestore.h ../common/capturedialog.h

SOURCES += ../common/npustats.cpp ../common/npupanel.cpp
HEADERS += ../common/npustats.h ../common/npupanel.h ../common/networkstatus.h ../common/ifaddr.h

# 实时故障诊断与数据集评估模块与 KLocalWindow 一起编译，链接时不能缺实现
SOURCES += ../common/faultresult.cpp \
    ../ps-local/src/faultinputadapter.cpp ../ps-local/src/faultpreprocess.cpp \
    ../ps-local/src/faultruntime.cpp ../ps-local/src/faultworker.cpp \
    ../ps-local/src/faultlivecontroller.cpp ../ps-local/src/faultpanel.cpp \
    ../ps-local/src/faultlog.cpp ../ps-local/src/faultdataset.cpp \
    ../ps-local/src/faultevaluator.cpp ../ps-local/src/faultevalpanel.cpp
HEADERS += ../common/faulttypes.h ../common/faultresult.h ../common/faultstatus.h \
    ../common/fault_engine_api.h \
    ../ps-local/src/faultinputadapter.h ../ps-local/src/faultpreprocess.h \
    ../ps-local/src/faultruntime.h ../ps-local/src/faultworker.h \
    ../ps-local/src/faultlivecontroller.h ../ps-local/src/faultpanel.h \
    ../ps-local/src/faultlog.h ../ps-local/src/faultdataset.h \
    ../ps-local/src/faultevaluator.h ../ps-local/src/faultevalpanel.h
