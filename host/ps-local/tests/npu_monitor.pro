QT += core testlib
QT -= gui
CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = test_npu_monitor
INCLUDEPATH += ../src
SOURCES += test_npu_monitor.cpp ../src/npumonitor.cpp
HEADERS += ../src/npumonitor.h

INCLUDEPATH += ../../common
SOURCES += ../../common/npustats.cpp
HEADERS += ../../common/npustats.h ../../common/networkstatus.h
