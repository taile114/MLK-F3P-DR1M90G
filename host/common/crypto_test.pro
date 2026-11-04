QT += core

CONFIG += c++17 console link_pkgconfig
CONFIG -= app_bundle
PKGCONFIG += openssl

TEMPLATE = app
TARGET = crypto_test

SOURCES += \
    crypto.cpp \
    securechannel.cpp \
    test_crypto.cpp

HEADERS += \
    crypto.h \
    securechannel.h
