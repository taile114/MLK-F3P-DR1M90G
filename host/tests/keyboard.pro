QT += core gui widgets testlib
CONFIG += console c++17 testcase
CONFIG -= app_bundle
TARGET = test_keyboard
INCLUDEPATH += ../ps-local/src
SOURCES += test_keyboard.cpp ../ps-local/src/virtualkeyboard.cpp
HEADERS += ../ps-local/src/virtualkeyboard.h ../ps-local/src/numberedit.h
