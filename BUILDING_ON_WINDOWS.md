# Building on Windows

## Prerequisites

- Visual Studio 2022 with `Desktop development with C++`
- Qt 6 with `Qt Image Formats`
- [Conan 2](https://conan.io/downloads.html)
- [CMake](https://cmake.org/)
- [Git](https://git-scm.com/)

Make sure your Qt `bin` directory is available on `PATH`, for example `C:\Qt\6.9.3\msvc2022_64\bin`.

## Build

Open an `x64 Native Tools Command Prompt for VS 2022` and run:

```cmd
git clone --recurse-submodules <your-mergerino-repository-url>
cd <repo-dir>
conan profile detect
conan install . -of build-conan -s build_type=Release --build=missing
cmake -S . -B build-conan -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=build-conan\conan_toolchain.cmake
cmake --build build-conan --parallel
```

To enable plugins, add `-DCHATTERINO_PLUGINS=ON` to the `cmake -S . -B build-conan ...` command.

## Run

```cmd
build-conan\bin\mergerino.exe
```

## Deploy Qt libraries

If you need a standalone local bundle, run:

```cmd
windeployqt build-conan\bin\mergerino.exe --release --no-compiler-runtime --no-translations --no-opengl-sw --dir build-conan\bin
```
