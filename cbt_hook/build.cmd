@echo off
setlocal

rem Build 32-bit cbt_client
msbuild cbt_client\cbt_client.vcxproj /p:Platform=Win32 /p:Configuration=Release
if %ERRORLEVEL% NEQ 0 (
    echo Build failed
    exit /b 1
)

rem Build 32-bit cbt_hook
msbuild cbt_hook\cbt_hook.vcxproj /p:Platform=Win32 /p:Configuration=Release
if %ERRORLEVEL% NEQ 0 (
    echo Build failed
    exit /b 1
)

rem Build 64-bit cbt_client
msbuild cbt_client\cbt_client.vcxproj /p:Platform=x64 /p:Configuration=Release
if %ERRORLEVEL% NEQ 0 (
    echo Build failed
    exit /b 1
)

rem Build 64-bit cbt_hook
msbuild cbt_hook\cbt_hook.vcxproj /p:Platform=x64 /p:Configuration=Release
if %ERRORLEVEL% NEQ 0 (
    echo Build failed
    exit /b 1
)

rem Build 64-bit hwnd_observer only
::msbuild hwndObserver\hwndObserver.vcxproj /p:Platform=x64 /p:Configuration=Release

msbuild cbt_hook.sln /p:Configuration=Release /p:Platform=x64
if %ERRORLEVEL% NEQ 0 (
    echo Build failed
    exit /b 1
)

echo Build succeeded


endlocal
