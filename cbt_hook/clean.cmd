@echo off

msbuild cbt_hook.sln /t:Clean
rm -rf Release
