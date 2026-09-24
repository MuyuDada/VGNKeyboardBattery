@echo off
rem ===========================================================================
rem  build.bat  --  编译 VGNKeyboardBattery.dll
rem
rem  自动检测编译器：优先使用 MSVC(cl)，其次使用 MinGW(g++)
rem  注意：TrafficMonitor 主程序是 64 位，请使用 x64 工具链编译
rem ===========================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set SRC=src\VgnKeyboardBattery.cpp src\VgnHidReader.cpp
set OUT=VGNKeyboardBattery.dll

where cl.exe >nul 2>nul
if %errorlevel%==0 goto :msvc

where g++.exe >nul 2>nul
if %errorlevel%==0 goto :mingw

echo [错误] 没有找到 C++ 编译器（cl.exe 或 g++.exe）。
echo        请先安装 Visual Studio（含"使用 C++ 的桌面开发"工作负载）
echo        或 MinGW-w64，然后重新运行本脚本。
exit /b 1

rem --------------------------------------------------------------------------
:msvc
echo [1/2] 使用 MSVC 编译 %OUT% ...
cl /nologo /LD /EHsc /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
   /Isrc %SRC% ^
   /Fe:%OUT% ^
   /link setupapi.lib hid.lib user32.lib
if errorlevel 1 (
    echo [错误] 编译失败
    exit /b 1
)
goto :done

rem --------------------------------------------------------------------------
:mingw
echo [1/2] 使用 MinGW 编译 %OUT% ...
g++ -shared -O2 -static-libgcc -static-libstdc++ ^
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN ^
    -Isrc %SRC% -o %OUT% ^
    -lsetupapi -lhid -luser32 -lkernel32
if errorlevel 1 (
    echo [错误] 编译失败
    exit /b 1
)
goto :done

rem --------------------------------------------------------------------------
:done
echo.
echo [2/2] 完成： %CD%\%OUT%
echo.
echo 安装方法：
echo   把 %OUT% 复制到 TrafficMonitor 主程序目录下的 plugins 子目录，
echo   重启 TrafficMonitor，然后在右键菜单 -^> 其他功能 -^> 插件管理 中确认。
echo.
endlocal
