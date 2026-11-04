QT += core network testlib
QT -= gui
CONFIG += c++17 testcase link_pkgconfig
PKGCONFIG += openssl
TARGET = test_discovery
INCLUDEPATH += ../common
SOURCES += test_discovery.cpp ../common/peerdiscovery.cpp ../common/crypto.cpp ../common/securechannel.cpp
HEADERS += ../common/peerdiscovery.h
