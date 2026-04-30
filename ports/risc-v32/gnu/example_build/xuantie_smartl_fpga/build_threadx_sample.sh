#!/bin/bash

bash build_libthreadx.sh

rm -rf build
cmake -Bbuild -GNinja -DCMAKE_TOOLCHAIN_FILE=xuantie_e906_gnu.cmake .
cmake --build ./build/
