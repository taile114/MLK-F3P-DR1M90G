QT += core testlib
QT -= gui
CONFIG += console c++17 testcase link_pkgconfig
CONFIG -= app_bundle
PKGCONFIG += openssl
TARGET = test_capture
INCLUDEPATH += ../common ../remote-pc/src
SOURCES += test_capture.cpp ../common/capturestore.cpp ../common/crypto.cpp
HEADERS += ../common/capturestore.h

HEADERS += ../common/sensorunits.h
