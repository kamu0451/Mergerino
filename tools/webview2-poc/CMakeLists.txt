add_executable(webview2-poc main.cpp)

target_link_libraries(webview2-poc PRIVATE
    Mergerino::WebView2
)

set_target_properties(webview2-poc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

if(MSVC)
    target_compile_options(webview2-poc PRIVATE /utf-8)
    target_compile_definitions(webview2-poc PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
endif()
