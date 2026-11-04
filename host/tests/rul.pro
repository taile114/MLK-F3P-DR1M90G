QT += core gui widgets network testlib
CONFIG += c++17 testcase link_pkgconfig
PKGCONFIG += openssl
TEMPLATE = app
TARGET = test_rul
INCLUDEPATH += ../common ../remote-pc/src ../ps-local/src
SOURCES += test_rul.cpp ../common/crypto.cpp ../common/securechannel.cpp \
           ../common/inferencejournal.cpp ../common/replaypanel.cpp ../remote-pc/src/receiver.cpp \
           ../ps-local/src/npu.cpp ../ps-local/src/replaycontroller.cpp
HEADERS += ../common/replaypanel.h ../remote-pc/src/receiver.h ../ps-local/src/replaycontroller.h
