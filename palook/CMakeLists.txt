cmake_minimum_required(VERSION 3.16)
project(palook VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(PALOOK_SOURCES
    src/main.cpp
    src/core/mkf.h
    src/core/mkf.cpp
    src/core/decompress.h
    src/core/decompress.cpp
    src/core/rle.h
    src/core/rle.cpp
    src/core/palette.h
    src/core/palette.cpp
    src/core/palmap.h
    src/core/palmap.cpp
    src/core/textdec.h
    src/core/textdec.cpp
    src/core/sound.h
    src/core/sound.cpp
    src/core/rng.h
    src/core/rng.cpp
    src/core/gamepack.h
    src/core/gamepack.cpp
    src/ui/mainwindow.h
    src/ui/mainwindow.cpp
    src/ui/zoomimage.h
    src/ui/zoomimage.cpp
    src/ui/imagepreview.h
    src/ui/imagepreview.cpp
    src/ui/soundpreview.h
    src/ui/soundpreview.cpp
    src/ui/tablepreview.h
    src/ui/tablepreview.cpp
    src/ui/hexpreview.h
    src/ui/hexpreview.cpp
)

qt_add_executable(palook ${PALOOK_SOURCES})

target_include_directories(palook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(palook PRIVATE Qt6::Widgets)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(palook PRIVATE -Wall -Wextra)
endif()
