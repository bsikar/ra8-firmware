#!/bin/bash

pushd ../../../../../
#cmake -Bbuild -GNinja -DCMAKE_TOOLCHAIN_FILE=cmake/riscv32_clang.cmake .
cmake -Bbuild -DCMAKE_TOOLCHAIN_FILE=cmake/riscv32_clang.cmake .
cmake --build ./build/
popd
