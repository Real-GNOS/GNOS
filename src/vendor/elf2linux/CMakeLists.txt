cmake_minimum_required(VERSION 3.20)
project(elf2linux VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

# Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

# common library
add_library(elf2linux_core STATIC
    src/common/elf_reader.cpp
    src/common/dep_collector.cpp
    src/common/initramfs.cpp
    src/common/kernel_resolver.cpp
)
target_include_directories(elf2linux_core PUBLIC src/common)
target_link_libraries(elf2linux_core PRIVATE Qt6::Core)

# CLI
add_executable(elf2linux src/cli/main.cpp)
target_link_libraries(elf2linux PRIVATE elf2linux_core Qt6::Core)

# GUI
add_executable(gelf2linux
    src/gui/main.cpp
    src/gui/MainWindow.cpp
    src/gui/BuildWorker.cpp
)
target_link_libraries(gelf2linux PRIVATE elf2linux_core Qt6::Widgets)

# assets
install(TARGETS elf2linux gelf2linux RUNTIME DESTINATION bin)
install(DIRECTORY assets/ DESTINATION share/elf2linux)
