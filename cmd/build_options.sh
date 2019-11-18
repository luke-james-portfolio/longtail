#!/usr/bin/env bash

export TARGET=longtail
export SRC=
export TEST_SRC="main.cpp ../src/longtail.c"
export THIRDPARTY_SRC="nadir/src/nadir.cpp lizard/lib/*.c lizard/lib/entropy/*.c lizard/lib/xxhash/*.c trove/src/trove.cpp"
export CXXFLAGS="$CXXFLAGS -pthread"
export CXXFLAGS_DEBUG="$CXXFLAGS_DEBUG -DBIKESHED_ASSERTS -DLONGTAIL_VERBOSE_LOGS"