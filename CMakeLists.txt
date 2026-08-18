cmake_minimum_required(VERSION 3.16)
project(dana VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

include(CheckCCompilerFlag)

# This is for all architectures
add_compile_options(-O3 -flto -ffast-math -funroll-loops -Wall -Wextra)
add_compile_definitions(NDEBUG)

# Only if we are using x86-like architectures, for AVX instr
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|amd64|AMD64|i686|i386|x86)")
    check_c_compiler_flag("-march=native" HAS_MARCH_NATIVE)
    if(HAS_MARCH_NATIVE)
        add_compile_options(-march=native)
    endif()
    
    check_c_compiler_flag("-mavx2" HAS_AVX2)
    if(HAS_AVX2)
        add_compile_options(-mavx2)
    endif()
    
    check_c_compiler_flag("-mfma" HAS_FMA)
    if(HAS_FMA)
        add_compile_options(-mfma)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "(arm|aarch64|ARM64)")
    check_c_compiler_flag("-mcpu=native" HAS_MCPU_NATIVE)
    if(HAS_MCPU_NATIVE)
        add_compile_options(-mcpu=native)
    endif()
endif()

add_library(dana_core STATIC
    src/DANABitStream.c
    src/DANACoder.c
    src/DANADecoder.c
    src/DANAPredictor.c
    src/DANAUtility.c
    src/DANAEncoder.c
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

add_executable(danaplay
    src/player/main.c
    src/player/state.c
    src/player/file_list.c
    src/player/vis_math.c
    src/player/audio.c
    src/player/ui_common.c
    src/player/ui_animations.c
    src/player/ui_files.c
    src/player/ui_info.c
    src/player/ui_vis.c
    src/player/ui_player.c
    src/player/ui.c
)
target_link_libraries(danaplay dana_core ncursesw pthread m ${CMAKE_DL_LIBS})

add_executable(danaplayd src/danaplayd.c)
target_link_libraries(danaplayd dana_core pthread m ${CMAKE_DL_LIBS})