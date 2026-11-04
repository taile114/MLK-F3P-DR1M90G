QT += core network testlib
QT -= gui
CONFIG += console c++17 testcase link_pkgconfig
CONFIG -= app_bundle
PKGCONFIG += openssl
TARGET = test_sensor_source
INCLUDEPATH += ../common ../ps-local/src ../remote-pc/src
SOURCES += test_sensor_source.cpp ../ps-local/src/datasource.cpp ../ps-local/src/pl_host.cpp \
    ../remote-pc/src/receiver.cpp ../common/crypto.cpp ../common/securechannel.cpp
HEADERS += ../ps-local/src/datasource.h ../remote-pc/src/receiver.h ../common/securechannel.h
QMAKE_LFLAGS += -Wl,--wrap=open
