cmake_minimum_required(VERSION 3.16)
project(dana VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

add_compile_options(-O3 -march=native -mavx2 -mfma -flto -ffast-math -funroll-loops -Wall -Wextra)
add_compile_definitions(NDEBUG)

add_library(dana_core STATIC
    src/DANABitStream.c
    src/DANACoder.c
    src/DANADecoder.c
    src/DANAPredictor.c
    src/DANAUtility.c
    src/DANAEncoder.c
    src/DANArANS.c
    src/command_line_parser.c
    src/wav.c
)

target_include_directories(dana_core PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include/private>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include/public>
)
target_link_libraries(dana_core m)

add_executable(dana src/main.c)
target_link_libraries(dana dana_core)

add_executable(danaplay src/player.c)
target_link_libraries(danaplay dana_core asound ncursesw pthread m)

add_executable(danaplayd src/danaplayd.c)
target_link_libraries(danaplayd dana_core asound pthread m)
