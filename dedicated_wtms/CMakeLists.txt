# dedicated_wtms: a stand-alone ESP-IDF firmware that only runs the water tank manager (wtms). Not part of the miniesp OS build:
# it lives in this sub-directory, reads (never writes) ../.creds and the managed components cached by the parent project.
cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "1.0.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dedicated_wtms)
