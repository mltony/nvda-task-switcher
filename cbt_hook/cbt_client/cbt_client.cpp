// cbt_client.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <windows.h>
#include <cstdlib> // for strtoul
#include <tchar.h>
#include "cbt_hook.h"

std::wstring getWindowClass()
{
    std::wstring cls = CBT_CLIENT_WINDOW_CLASS_PREFIX;
    #ifdef _WIN64
        cls += L"x64";
        return cls;
    #endif
    #ifdef _WIN32
        cls += L"Win32";
        return cls;
    #endif
    std::cerr << "Oh no, undefined architecture, throwing an exception!" << std::endl;
    throw std::exception("Unsupported architecture!");
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int main(int argc, char** argv)
{
    std::cout << "2 Starting cout" << std::endl;
    std::wstring cls = getWindowClass();
    std::string clsNarrow(cls.begin(), cls.end());
    std::cout << "Starting CBT client with class " << clsNarrow << std::endl;
    if (FindWindow(cls.c_str(), NULL) != nullptr) {
        std::cerr << "Error: Window of " << clsNarrow << " class already exists. CBT client for this architecture must be already running." << std::endl;
        return 1;
    }
    HWND hwnd = FindWindow(_T(HWND_OBSERVER_WINDOW_CLASS), NULL);
    if (hwnd == nullptr) {
        std::cerr << "Error: Cannot find target window" << std::endl;
        return 1;
    }
    bool result = InstallCBTHook(hwnd);
    if (!result) {
        std::cerr << "Error: Installing CBT hook failed!" << std::endl;
        return 1;
    }
    std::cout << "Successfully installed CBT hook! Press enter to quit." << std::endl;


    HMODULE hModule = GetModuleHandle(NULL);
    static const WNDCLASSEX wc = {
        sizeof(WNDCLASSEX),         // Size
        CS_HREDRAW | CS_VREDRAW,   // Style
        WindowProc,                // lpfnWndProc
        0,                         // cbClsExtra
        0,                         // cbWndExtra
        hModule,    // hInstance
        NULL,                     // hIcon
        LoadCursor(NULL, IDC_ARROW), // hCursor
        NULL,                     // hbrBackground
        NULL,                     // lpszMenuName
        cls.c_str(),
        NULL,                     // hIconSm
    };
    if (!RegisterClassEx(&wc)) {
        std::wcerr << L"Error: Failed to register class " << cls << L"." << std::endl;
        return 0;
    }
    HWND hWnd = CreateWindowEx(
        WS_EX_TRANSPARENT,
        wc.lpszClassName,
        TEXT("Invisible CBT client window"),
        WS_POPUP,
        0, 0, 100, 100,
        NULL,
        NULL,
        hModule,
        NULL
    );

    ShowWindow(hWnd, SW_HIDE);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);

        if (msg.message == WM_HWND_OBSERVER_DESTROY_WINDOW)
        {
            // Handle shutdown request
            PostQuitMessage(0);
        }
    }


    if (false) {
        //teep(500, 50);
        std::string dummy;
        std::cin >> dummy;
        std::cout << "Shutting down" << std::endl;
        //Beep(500, 50);
    }
    return 0;
}
