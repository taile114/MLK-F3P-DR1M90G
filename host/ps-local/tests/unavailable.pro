QT += core gui widgets testlib
CONFIG += c++17 testcase
TEMPLATE = app
TARGET = test_unavailable
INCLUDEPATH += ../src
SOURCES += test_unavailable.cpp ../src/klocalwindow.cpp ../src/datasource.cpp ../src/pl_host.cpp
HEADERS += ../src/klocalwindow.h ../src/datasource.h ../src/pl_host.h
