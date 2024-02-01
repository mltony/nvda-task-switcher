// dllmain.cpp : Defines the entry point for the DLL application.
// This file was heavily inspired by:
// https://github.com/yinkaisheng/MyLiteSpy
// Which is distributed without any license as far as I can tell.
#include "pch.h"
#include <stdio.h>
#include <windows.h>
#include "cbt_hook.h"

#pragma data_seg("Shared")
HWND g_hNotifyWnd = NULL;
HWND g_hCaptureWnd = NULL;
HINSTANCE g_hInstance = NULL;
HHOOK g_hCBTHook = NULL;
#pragma data_seg()

#pragma comment(linker,"/section:Shared,RWS")

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        g_hInstance = hModule;
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

static LRESULT CALLBACK CBTProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0)
    {
        if (nCode == HCBT_ACTIVATE)  //Called when the application window is activated
        {
            ::PostMessage(g_hNotifyWnd, WM_CBT_ACTIVATE_MSG, wParam, NULL);
        }
        else if (nCode == HCBT_SETFOCUS)
        {
            ::PostMessage(g_hNotifyWnd, WM_CBT_FOCUS_MSG, wParam, NULL);
        }
        else if (nCode == HCBT_CREATEWND) {
            ::PostMessage(g_hNotifyWnd, WM_CBT_CREATE_WINDOW_MSG, wParam, NULL);
        }
        else if (nCode == HCBT_DESTROYWND)
        {
            ::PostMessage(g_hNotifyWnd, WM_CBT_DESTROY_WINDOW_MSG, wParam, NULL);
        }
    }
    return CallNextHookEx(g_hCBTHook, nCode, wParam, lParam);
}

DLL_EXPORT  bool InstallCBTHook(HWND hNotifyWnd)
{
    g_hNotifyWnd = hNotifyWnd;

    if (!g_hCBTHook)
    {
        g_hCBTHook = SetWindowsHookEx(WH_CBT, (HOOKPROC)CBTProc, g_hInstance, 0);

        if (g_hCBTHook)
        {
            OutputDebugStringA("Hook CBT succeed\n");
            return true;
        }
        else
        {
            DWORD dwError = GetLastError();
            char szError[MAX_PATH];
            _snprintf_s(szError, MAX_PATH, "Hook CBT failed, error = %u\n", dwError);
            OutputDebugStringA(szError);
        }
    }

    return false;
}

DLL_EXPORT bool UninstallCBTHook()
{
    if (g_hCBTHook)
    {
        UnhookWindowsHookEx(g_hCBTHook);
        g_hCBTHook = NULL;
    }
    return true;
}
