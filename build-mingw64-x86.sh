#!/bin/sh

cmake -DCMAKE_TOOLCHAIN_FILE=./mingw-w64-x86.cmake -B build-mingw64-x86 -S .
cd build-mingw64-x86
make clean
make
cd ..