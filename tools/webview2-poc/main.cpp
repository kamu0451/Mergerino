// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT
//
// Hidden WebView2 host that navigates to a TikTok live page and prints
// the rendered document title, then exits. Proves the SDK + Windows 11
// Runtime are wired end-to-end before we bake WebView2 into mergerino.exe.
//
// Usage:
//   webview2-poc                                                  (default URL)
//   webview2-poc https://www.tiktok.com/@some_user/live           (override)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kDefaultUrl[] = L"https://www.tiktok.com/@gevad1ch/live";
constexpr wchar_t kClassName[]  = L"MergerinoWebView2Poc";
constexpr UINT_PTR kTimeoutId   = 1;
constexpr UINT     kTimeoutMs   = 30'000;

std::atomic<int> g_exitCode{0};
HWND             g_host{};
std::wstring     g_targetUrl;

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_TIMER:
            if (wp == kTimeoutId) {
                std::puts("[timeout] 30s elapsed without completion");
                g_exitCode = 99;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void requestShutdown()
{
    PostMessageW(g_host, WM_CLOSE, 0, 0);
}

void printWide(const wchar_t *label, const std::wstring &value)
{
    std::fputs(label, stdout);
    int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()),
                                       nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        utf8.data(), required, nullptr, nullptr);
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

HRESULT installNavigationHandler(ICoreWebView2 *webview)
{
    EventRegistrationToken token{};
    return webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2 *sender,
               ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                BOOL ok = FALSE;
                args->get_IsSuccess(&ok);
                if (!ok) {
                    COREWEBVIEW2_WEB_ERROR_STATUS status{};
                    args->get_WebErrorStatus(&status);
                    std::printf("[navigation] failed status=%d\n",
                                static_cast<int>(status));
                    g_exitCode = 1;
                    requestShutdown();
                    return S_OK;
                }
                std::puts("[navigation] ok");
                sender->ExecuteScript(
                    L"document.title",
                    Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                        [](HRESULT hr, LPCWSTR result) -> HRESULT {
                            if (FAILED(hr) || result == nullptr) {
                                std::printf("[title] ExecuteScript failed 0x%08lx\n",
                                            hr);
                                g_exitCode = 2;
                                requestShutdown();
                                return S_OK;
                            }
                            std::wstring s(result);
                            if (s.size() >= 2 && s.front() == L'"' &&
                                s.back() == L'"') {
                                s = s.substr(1, s.size() - 2);
                            }
                            printWide(L"[title] ", s);
                            g_exitCode = 0;
                            requestShutdown();
                            return S_OK;
                        })
                        .Get());
                return S_OK;
            })
            .Get(),
        &token);
}

HRESULT onControllerReady(HRESULT hr, ICoreWebView2Controller *controller)
{
    if (FAILED(hr) || controller == nullptr) {
        std::printf("[controller] failed 0x%08lx\n", hr);
        g_exitCode = 5;
        requestShutdown();
        return S_OK;
    }
    std::puts("[controller] ready");
    ComPtr<ICoreWebView2> webview;
    controller->get_CoreWebView2(&webview);
    if (!webview) {
        std::puts("[controller] get_CoreWebView2 returned null");
        g_exitCode = 6;
        requestShutdown();
        return S_OK;
    }
    installNavigationHandler(webview.Get());
    printWide(L"[navigate] ", g_targetUrl);
    webview->Navigate(g_targetUrl.c_str());
    return S_OK;
}

HRESULT onEnvironmentReady(HRESULT hr, ICoreWebView2Environment *env)
{
    if (FAILED(hr) || env == nullptr) {
        std::printf("[env] failed 0x%08lx\n", hr);
        g_exitCode = 3;
        requestShutdown();
        return S_OK;
    }
    std::puts("[env] ready");
    HRESULT created = env->CreateCoreWebView2Controller(
        g_host,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            onControllerReady)
            .Get());
    if (FAILED(created)) {
        std::printf("[controller] CreateCoreWebView2Controller failed 0x%08lx\n",
                    created);
        g_exitCode = 4;
        requestShutdown();
    }
    return S_OK;
}

}  // namespace

int wmain(int argc, wchar_t *argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    g_targetUrl = (argc > 1) ? argv[1] : kDefaultUrl;

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCo)) {
        std::printf("[init] CoInitializeEx failed 0x%08lx\n", hrCo);
        return 10;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);

    g_host = CreateWindowExW(0, kClassName, L"mergerino-webview2-poc",
                             WS_POPUP, 0, 0, 800, 600, nullptr, nullptr,
                             hInst, nullptr);
    if (g_host == nullptr) {
        std::printf("[init] CreateWindowExW failed %lu\n", GetLastError());
        CoUninitialize();
        return 11;
    }
    SetTimer(g_host, kTimeoutId, kTimeoutMs, nullptr);

    std::puts("[init] creating WebView2 environment");
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            onEnvironmentReady)
            .Get());
    if (FAILED(hr)) {
        std::printf("[init] CreateCoreWebView2EnvironmentWithOptions failed 0x%08lx\n",
                    hr);
        DestroyWindow(g_host);
        CoUninitialize();
        return 12;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return g_exitCode.load();
}
