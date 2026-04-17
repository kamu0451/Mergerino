# Fetches the Microsoft.Web.WebView2 NuGet package (headers + static loader lib)
# and exposes Mergerino::WebView2 as an interface target.
#
# The WebView2 Runtime itself is expected on the user's machine; on Windows 11
# it ships as part of the OS. The static loader (WebView2LoaderStatic.lib)
# locates and calls into it, so no DLL needs to ride alongside mergerino.exe.

if(TARGET Mergerino::WebView2)
    return()
endif()

if(NOT WIN32)
    return()
endif()

include(FetchContent)

set(WEBVIEW2_VERSION "1.0.3912.50")
set(WEBVIEW2_SHA256  "8dd696301d8e5ad8389f26b636329bbf35754b793974e82168b1f6dda234eb07")

FetchContent_Declare(
    webview2
    URL      "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}"
    URL_HASH "SHA256=${WEBVIEW2_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(webview2)

if(NOT EXISTS "${webview2_SOURCE_DIR}/build/native/include/WebView2.h")
    message(FATAL_ERROR "WebView2 NuGet extracted but WebView2.h not found at expected path")
endif()

add_library(Mergerino_WebView2 INTERFACE)
add_library(Mergerino::WebView2 ALIAS Mergerino_WebView2)

target_include_directories(Mergerino_WebView2 INTERFACE
    "${webview2_SOURCE_DIR}/build/native/include"
)
target_link_libraries(Mergerino_WebView2 INTERFACE
    "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
)

message(STATUS "WebView2: SDK ${WEBVIEW2_VERSION} (${webview2_SOURCE_DIR})")
