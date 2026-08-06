cmake_minimum_required(VERSION 3.20)
project(module_hello C ASM)

# Add the module target
add_executable(hello_module module/hello_module.c module/ra8app_preamble.S)
target_link_options(hello_module PRIVATE -T ${CMAKE_SOURCE_DIR}/port/threadx/ld/ra8app_module.ld)

# Add the kernel application target
add_executable(module_hello_app main.c)
target_link_libraries(module_hello_app PRIVATE ra8_core threadx)
