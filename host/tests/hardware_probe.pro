QT += core network
QT -= gui
CONFIG += console c++17 link_pkgconfig
PKGCONFIG += openssl
TEMPLATE=app
TARGET=hardware_probe
INCLUDEPATH += ../common ../remote-pc/src
SOURCES += hardware_probe.cpp ../common/crypto.cpp ../common/securechannel.cpp ../common/workorders.cpp
