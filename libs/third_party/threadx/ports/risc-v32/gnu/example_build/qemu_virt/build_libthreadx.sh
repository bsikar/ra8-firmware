#!/bin/bash
pushd ../../../../../
cmake -Bbuild -GNinja -DCMAKE_TOOLCHAIN_FILE=cmake/riscv32_gnu.cmake .
cmake --build ./build/
popd