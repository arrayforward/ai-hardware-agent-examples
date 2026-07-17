@echo off
setlocal

set "PROJECT_ROOT=%~dp0"
set "TOOLCHAIN_BIN=%PROJECT_ROOT%examples\goldieos\tools\build\tools\compiler\riscv\cc_riscv32_musl_105\cc_riscv32_musl_fp_win\bin"
set "LINK_TOOLS=%PROJECT_ROOT%examples\goldieos\tools\build\tools"

set "PATH=%TOOLCHAIN_BIN%;%LINK_TOOLS%;%PATH%"

where riscv32-linux-musl-gcc >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Cross compiler not found: %TOOLCHAIN_BIN%
    exit /b 1
)
where ws63_link_v4.exe >nul 2>&1
if errorlevel 1 (
    echo [ERROR] ws63_link_v4.exe not found: %LINK_TOOLS%
    exit /b 1
)
if not exist "%PROJECT_ROOT%include\convai\convai_api.h" (
    echo [ERROR] SDK headers missing. Extract ai-hardware-agent-sdk.26.6.0 into %PROJECT_ROOT%
    exit /b 1
)
if not exist "%PROJECT_ROOT%libs\ws63\libconvai_sdk.a" (
    echo [ERROR] SDK library missing: %PROJECT_ROOT%libs\ws63\libconvai_sdk.a
    exit /b 1
)

if not exist "%PROJECT_ROOT%build" mkdir "%PROJECT_ROOT%build"

cd /d "%PROJECT_ROOT%build"

echo [STEP 1/3] Running CMake configure...
cmake -G "MinGW Makefiles" -DCONVAI_PLATFORM=ws63 ^
      -DCMAKE_C_COMPILER=riscv32-linux-musl-gcc ^
      -DCMAKE_CXX_COMPILER=riscv32-linux-musl-g++ ^
      -DCMAKE_C_COMPILER_WORKS=1 -DCMAKE_CXX_COMPILER_WORKS=1 ^
      -DCMAKE_MAKE_PROGRAM=make ..
if errorlevel 1 (
    echo [ERROR] CMake configure failed.
    exit /b 1
)

echo [STEP 2/3] Building library...
make
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo [STEP 3/3] Generating firmware with ws63_link_v4.exe...
make ws63_firmware
if errorlevel 1 (
    echo [ERROR] Firmware generation failed.
    exit /b 1
)

echo.
echo [DONE] Build artifacts:
dir /b "examples\goldieos\out\goldieos.*" 2>nul
endlocal