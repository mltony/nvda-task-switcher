// cbt_client.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <windows.h>
#include "cbt_hook.h"

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_DESTROY:
        UninstallCBTHook();
        std::cout << "WM_DESTROY" << std::endl;
        PostQuitMessage(0);
        return 0;
    case WM_CBT_ACTIVATE_MSG:
        //std::cout << "WM_CBT_ACTIVATE_MSG " << wParam << std::endl;
        return 0;
    case WM_CBT_CREATE_WINDOW_MSG:
        std::cout << "create " << wParam << std::endl;
        return 0;
    case WM_CBT_DESTROY_WINDOW_MSG:
        std::cout << "destroy " << wParam << std::endl;
        return 0;
    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

int main()
{
    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASS wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = TEXT("InvisibleWindowClass");
    if (!RegisterClass(&wc))
    {
        std::cout << "Call to RegisterClass failed!" << std::endl;
        return 1;
    }
    HWND hwnd = CreateWindow(TEXT("InvisibleWindowClass"), TEXT("Invisible Window"), WS_OVERLAPPEDWINDOW & ~WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInstance, NULL);
    if (!hwnd)
    {
        std::cout << "Call to CreateWindow failed!" << std::endl;
        return 1;
    }
    std::cout << "HWND " << (long long)hwnd << std::endl;
    bool result = InstallCBTHook(hwnd);
    if (!result) {
        std::cout << "Installing CBT hook failed!" << std::endl;
        return 1;
    }
    MSG msg{};
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    std::cout << "Hello World!\n";
}
