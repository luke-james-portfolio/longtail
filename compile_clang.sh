#!/usr/bin/env bash

set +e
mkdir -p build

OPT=-O3
#DISASSEMBLY='-S -masm=intel'
ASAN=""
#CXXFLAGS="$CXXFLAGS -Wall -Weverything -pedantic -Wno-zero-as-null-pointer-constant -Wno-old-style-cast -Wno-global-constructors -Wno-padded"
ARCH=-m64

clang++ -o ./build/test $OPT $DISASSEMBLY $ARCH -std=c++14 $CXXFLAGS $ASAN -Isrc test/test.cpp test/main.cpp third-party/nadir/src/nadir.cpp -pthread