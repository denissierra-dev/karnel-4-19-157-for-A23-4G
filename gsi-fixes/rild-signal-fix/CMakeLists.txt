cmake_minimum_required(VERSION 3.18)
project(a23_rild_signal_fix C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(rild rild_signal_fix.c)

target_compile_options(rild PRIVATE
    -Os
    -Wall
    -Wextra
    -Werror
    -fPIE
)

target_link_options(rild PRIVATE
    -pie
    -Wl,--gc-sections
)

target_link_libraries(rild PRIVATE
    log
    dl
)
