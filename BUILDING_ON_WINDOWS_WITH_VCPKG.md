# Building on Windows with vcpkg

This route needs substantial disk space and is best used when you already manage your toolchain through vcpkg.

## Prerequisites

1. Install [Visual Studio](https://visualstudio.microsoft.com/) with `Desktop development with C++`
2. Install [CMake](https://cmake.org/)
3. Install [Git](https://git-scm.com/)
4. Install [vcpkg](https://vcpkg.io/)

## Build

1. Clone the repository with submodules
   ```powershell
   git clone --recurse-submodules <your-mergerino-repository-url>
   cd .\<repo-dir>\
   ```
2. Install dependencies
   ```powershell
   vcpkg install
   ```
3. Configure and build
   ```powershell
   cmake -B build -DCMAKE_TOOLCHAIN_FILE="$Env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
   cmake --build build --parallel <threads> --config Release
   ```
4. Run
   ```powershell
   .\build\bin\mergerino.exe
   ```
