QT += core
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = test_timesync
INCLUDEPATH += ../common
SOURCES += test_timesync.cpp
HEADERS += ../common/timesync.h
