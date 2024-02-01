#pragma once

#include <windows.h>

#define WM_CBT_ACTIVATE_MSG (WM_USER+0)
#define WM_CBT_FOCUS_MSG (WM_USER+1)
#define WM_CBT_CREATE_WINDOW_MSG (WM_USER+2)
#define WM_CBT_DESTROY_WINDOW_MSG (WM_USER+3)


#ifdef _USRDLL
#define DLL_EXPORT __declspec(dllexport)
#else
#define DLL_EXPORT __declspec(dllimport)
#endif

DLL_EXPORT  bool InstallCBTHook(HWND hNotifyWnd);
DLL_EXPORT bool UninstallCBTHook();
