cmake_minimum_required(VERSION 3.21)

project(people_flow_qt_client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

add_executable(people_flow_qt_client WIN32
    src/main.cpp
    src/main_window.cpp
    src/main_window.h
    src/people_flow_api_client.cpp
    src/people_flow_api_client.h
)

target_link_libraries(people_flow_qt_client PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
)

if(MSVC)
    target_compile_options(people_flow_qt_client PRIVATE /utf-8 /W4)
    target_compile_definitions(people_flow_qt_client PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

if(MINGW)
    target_compile_options(people_flow_qt_client PRIVATE -Wall -Wextra -Wpedantic)
endif()
