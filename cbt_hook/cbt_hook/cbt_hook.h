#pragma once

#include <windows.h>

#define HWND_OBSERVER_WINDOW_CLASS "HWNDObserverInvisibleWindowClass"
#define CBT_CLIENT_WINDOW_CLASS_PREFIX L"CBTClientInvisibleWindowClass_"

#define WM_CBT_ACTIVATE_MSG (WM_USER+0)
#define WM_CBT_FOCUS_MSG (WM_USER+1)
#define WM_CBT_CREATE_WINDOW_MSG (WM_USER+2)
#define WM_CBT_DESTROY_WINDOW_MSG (WM_USER+3)
#define WM_HWND_OBSERVER_DESTROY_WINDOW (WM_USER +  4)

#ifdef _USRDLL
#define DLL_EXPORT __declspec(dllexport)
#else
#define DLL_EXPORT __declspec(dllimport)
#endif

DLL_EXPORT  bool InstallCBTHook(HWND hNotifyWnd);
DLL_EXPORT bool UninstallCBTHook();
