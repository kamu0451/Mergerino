// SPDX-FileCopyrightText: 2026 Mergerino contributors
//
// SPDX-License-Identifier: MIT

// MergerinoUpdater
//
// Tiny portable-zip updater. Invoked by mergerino.exe as:
//
//     MergerinoUpdater.exe <zip-path> [restart]
//
// Lives at <app>/updater.1/MergerinoUpdater.exe; <app> is the directory that
// contains mergerino.exe. Behaviour:
//   1. Wait for the parent to release file locks.
//   2. Extract the zip into <app>/, skipping anything under updater.1/ so we
//      do not try to overwrite ourselves while running.
//   3. If `restart` was passed, relaunch <app>/mergerino.exe detached.
//
// Uses Windows' bundled tar.exe (libarchive, present since Win10 1803) for
// extraction — Mergerino already requires a newer Win10 than that.

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr int kMaxAttempts = 15;
constexpr DWORD kInitialWaitMs = 1500;
constexpr DWORD kRetryWaitMs = 2000;

std::filesystem::path selfDirectory()
{
    std::vector<wchar_t> buf(MAX_PATH);
    DWORD len = 0;
    while (true)
    {
        len = GetModuleFileNameW(nullptr, buf.data(),
                                 static_cast<DWORD>(buf.size()));
        if (len == 0)
        {
            return {};
        }
        if (len < buf.size())
        {
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf.data()).parent_path();
}

DWORD runAndWait(const std::wstring &cmdLine, const std::wstring &workingDir)
{
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    BOOL ok =
        CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr,
                       workingDir.empty() ? nullptr : workingDir.c_str(), &si,
                       &pi);
    if (!ok)
    {
        return static_cast<DWORD>(-1);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode;
}

bool extractZip(const std::wstring &zipPath, const std::wstring &appDir)
{
    // tar -x -f "<zip>" -C "<appDir>" --exclude=updater.1/* --exclude=updater.1
    std::wstring cmd = L"tar.exe -x -f \"";
    cmd += zipPath;
    cmd += L"\" -C \"";
    cmd += appDir;
    cmd += L"\" --exclude=updater.1/* --exclude=updater.1";

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
    {
        DWORD code = runAndWait(cmd, appDir);
        if (code == 0)
        {
            return true;
        }
        Sleep(kRetryWaitMs);
    }
    return false;
}

bool relaunchApp(const std::filesystem::path &appDir)
{
    std::filesystem::path exe = appDir / L"mergerino.exe";
    std::wstring exeStr = exe.wstring();
    std::wstring workDir = appDir.wstring();

    // CreateProcessW wants a writable lpCommandLine.
    std::vector<wchar_t> cmdLine(exeStr.begin(), exeStr.end());
    cmdLine.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exeStr.c_str(), cmdLine.data(), nullptr, nullptr,
                             FALSE, DETACHED_PROCESS, nullptr, workDir.c_str(),
                             &si, &pi);
    if (!ok)
    {
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t *argv[])
{
    if (argc < 2)
    {
        return 2;
    }

    std::wstring zipPath = argv[1];
    bool restart = false;
    for (int i = 2; i < argc; ++i)
    {
        if (std::wstring(argv[i]) == L"restart")
        {
            restart = true;
        }
    }

    std::filesystem::path updaterDir = selfDirectory();
    if (updaterDir.empty())
    {
        return 4;
    }
    std::filesystem::path appDir = updaterDir.parent_path();
    if (appDir.empty())
    {
        return 5;
    }

    Sleep(kInitialWaitMs);

    if (!extractZip(zipPath, appDir.wstring()))
    {
        return 3;
    }

    // Best-effort cleanup of the downloaded zip; not fatal on failure.
    DeleteFileW(zipPath.c_str());

    if (restart)
    {
        if (!relaunchApp(appDir))
        {
            return 6;
        }
    }
    return 0;
}
