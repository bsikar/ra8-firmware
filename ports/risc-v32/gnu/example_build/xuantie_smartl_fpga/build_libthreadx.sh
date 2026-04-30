#!/bin/bash

pushd ../../../../../
rm -rf build
cmake -Bbuild -GNinja -DCMAKE_TOOLCHAIN_FILE=ports/risc-v32/gnu/example_build/xuantie_smartl_fpga/xuantie_e906_gnu.cmake .
cmake --build ./build/
popd
