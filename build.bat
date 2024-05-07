@echo off
rm addon\globalPlugins\hwndObserver.dll
rm -r addon\globalPlugins\Win32
rm -r addon\globalPlugins\x64

cp cbt_hook\Release\hwndObserver.dll addon\globalPlugins\
mkdir addon\globalPlugins\Win32
mkdir addon\globalPlugins\x64
cp cbt_hook\Release\Win32\cbt_hook.dll addon\globalPlugins\Win32\
cp cbt_hook\Release\Win32\cbt_client.exe addon\globalPlugins\Win32\
cp cbt_hook\Release\x64\cbt_hook.dll addon\globalPlugins\x64\
cp cbt_hook\Release\x64\cbt_client.exe addon\globalPlugins\x64\
::scons -c && scons
echo  
