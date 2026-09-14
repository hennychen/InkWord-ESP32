# InkWord BigScreen 顶层 CMake（espidf 框架）
# 组件发现：components/epdiy 由 IDF 默认组件目录机制自动纳入；
# src/ 为 main 组件（见 src/CMakeLists.txt）。
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(InkWord_Firmware_BigScreen)
