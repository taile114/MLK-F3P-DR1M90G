QT -= core gui widgets
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = test_pl_speed
INCLUDEPATH += ../src
SOURCES += test_pl_speed.cpp ../src/pl_host.cpp
HEADERS += ../src/pl_host.h ../driver/pl_ctrl_uapi.h
