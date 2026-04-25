cmake_minimum_required(VERSION 3.20)

if(NOT WIN32)
    return()
endif()

add_executable(MergerinoUpdater WIN32 main.cpp)

set_target_properties(MergerinoUpdater PROPERTIES
    OUTPUT_NAME "MergerinoUpdater"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/updater.1"
)

# Console subsystem with wmain entry point.
set_target_properties(MergerinoUpdater PROPERTIES WIN32_EXECUTABLE OFF)

target_compile_definitions(MergerinoUpdater PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_link_options(MergerinoUpdater PRIVATE
        /SUBSYSTEM:CONSOLE
        /ENTRY:wmainCRTStartup
    )
endif()
