#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <chrono>
#include<iostream>

std::vector<std::pair<std::string, std::string>> windowData;

BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam)
{
    DWORD processId;
    GetWindowThreadProcessId(hwnd, &processId);

    HANDLE processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);

    TCHAR path[MAX_PATH];
    GetModuleFileNameEx(processHandle, NULL, path, MAX_PATH);

    CloseHandle(processHandle);

    int length = GetWindowTextLength(hwnd) + 1;
    wchar_t* buffer = new wchar_t[length];
    GetWindowText(hwnd, buffer, length);
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, NULL, 0, NULL, NULL);
    char* narrowBuffer = new char[sizeNeeded];
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrowBuffer, sizeNeeded, NULL, NULL);
    sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
    char* narrowPath= new char[sizeNeeded];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, narrowPath, sizeNeeded, NULL, NULL);    
    windowData.push_back({ narrowBuffer, narrowPath }); //path

    delete[] buffer;

    return TRUE;
}

int main()
{
    auto start = std::chrono::high_resolution_clock::now();
    EnumWindows(EnumWindowsCallback, NULL);
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "Time taken by function: " << diff.count() << " s\n";
    // Now windowData contains pairs of window titles and corresponding executable names

    return 0;
}
