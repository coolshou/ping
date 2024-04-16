#!/bin/sh

cmake -DCMAKE_TOOLCHAIN_FILE=./mingw-w64-x86_64.cmake -B build-mingw64 -S .
