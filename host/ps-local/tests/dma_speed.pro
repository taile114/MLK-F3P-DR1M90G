QT -= core gui widgets
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = test_dma_speed
INCLUDEPATH += ../src
SOURCES += test_dma_speed.cpp
HEADERS += ../src/dma_frames.h
