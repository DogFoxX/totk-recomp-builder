# Building from Source

## Table of Contents

- [Windows](#windows)
  - [Prerequisites](#prerequisites)
    - [C++ Build Tools](#c-build-tools)
    - [Python](#python)
  - [Preparation](#preparation)
    - [Configure `PATH` for `vcpkg` and `cmake`](#configure-path-for-vcpkg-and-cmake)
    - [Set `VCPKG_ROOT` User Environment Variable](#set-vcpkg_root-user-environment-variable)
  - [Build the Project](#build-the-project)
    - [Option 1: VS Code](#option-1-vs-code)
    - [Option 2: PowerShell](#option-2-powershell)
- [Linux](#linux)
  - [Option 1: Portable Static Build (vcpkg)](#option-1-portable-static-build-vcpkg)
    - [Prerequisites](#prerequisites-1)
    - [Bootstrap `vcpkg`](#bootstrap-vcpkg)
    - [Build the Project](#build-the-project-1)
  - [Option 2: System Packages (Ubuntu / Debian)](#option-2-system-packages-ubuntu--debian)
    - [Prerequisites](#prerequisites-2)
    - [Build the Project](#build-the-project-2)

## Windows

### Prerequisites

#### C++ Build Tools

- Download and run the [Microsoft C++ Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/) installer.
- Check **Desktop development with C++**.
- Under **Optional** (on the right panel), ensure the following components are selected:
  ```text
  MSVC Build Tools for x64/x86
  Windows 11 SDK
  C++ CMake tools for Windows
  Testing tools core features - Build Tools
  MSVC AddressSanitizer
  vcpkg package manager
  ```

#### Python

- Download and install [Python](https://www.python.org/downloads/) (Python 3.10+, tested with 3.15.2; the standalone installer is recommended).
- Ensure Python is added to `PATH` during installation.

### Preparation

#### Configure `PATH` for `vcpkg` and `cmake`

- Open **System Properties** -> **Environment Variables**.
- Edit `Path` under **System variables** and add both paths on separate lines:
  ```text
  C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin
  C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\vcpkg
  ```

#### Set `VCPKG_ROOT` User Environment Variable

`CMakePresets.json` uses `VCPKG_ROOT` to locate `vcpkg.cmake` automatically.

- In **Environment Variables**, click **New...** under **User variables** and enter:
  - **Variable name:** `VCPKG_ROOT`
  - **Variable value:** `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\vcpkg`
- Click **OK** to save, or set it via PowerShell:
  ```powershell
  [Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\vcpkg", "User")
  ```
- Open a new PowerShell terminal and verify the environment:
  ```powershell
  vcpkg version
  cmake --version
  $env:VCPKG_ROOT
  ```

### Build the Project

#### Option 1: VS Code

Open the project folder in VS Code (restart VS Code first if you just set `VCPKG_ROOT`) and press `Ctrl + Shift + B` to run the `CMake: Build` task (select `CMake: Build` if prompted).

#### Option 2: PowerShell

```powershell
cmake --preset windows-x64
cmake --build --preset windows-release --parallel
```

This will:

- Install dependencies with the `x64-windows-static` triplet into `build/vcpkg_installed`
- Embed `include/auto-build.py` and compile `cmake/version.rc.in` (including `include/icon.ico` if present)
- Output the standalone executable at `build/Release/totk-auto-build.exe`

## Linux

### Option 1: Portable Static Build (vcpkg)

Recommended for distribution. Builds `libcurl`, OpenSSL, `libarchive`, and `libstdc++` statically into `totk-auto-build` so the binary runs across Ubuntu, Debian, Arch Linux, Fedora, and SteamOS without external shared library dependencies.

> [!TIP]
> Building inside an **Ubuntu 22.04** container or environment targets `glibc 2.35`, ensuring backward compatibility with older LTS distributions.

#### Prerequisites

```shell
sudo apt-get update
sudo apt-get install -y \
  autoconf \
  automake \
  build-essential \
  ca-certificates \
  cmake \
  curl \
  git \
  libtool \
  ninja-build \
  perl \
  pkg-config \
  python3 \
  tar \
  unzip \
  zip
```

#### Bootstrap `vcpkg`

```shell
export VCPKG_ROOT="$HOME/vcpkg"
git clone [https://github.com/microsoft/vcpkg.git](https://github.com/microsoft/vcpkg.git) "$VCPKG_ROOT"
"$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
```

#### Build the Project

```shell
cmake --preset linux-x64 \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-linux

cmake --build --preset linux-release --parallel
strip build/totk-auto-build
```

This outputs the portable executable at `build/totk-auto-build`. You can verify that no external `libcurl`, `libarchive`, or `libstdc++` shared libraries are required with:

```shell
ldd build/totk-auto-build
```

### Option 2: System Packages (Ubuntu / Debian)

Faster for local development if you only need the binary to run on the same machine it was built on.

#### Prerequisites

```shell
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  python3 \
  libcurl4-openssl-dev \
  libarchive-dev \
  nlohmann-json3-dev
```

#### Build the Project

```shell
cmake --preset linux-x64
cmake --build --preset linux-release --parallel
```

This outputs the dynamically linked executable at `build/totk-auto-build`.
