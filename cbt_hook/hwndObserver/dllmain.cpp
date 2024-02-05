// dllmain.cpp : Defines the entry point for the DLL application.
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <string>
#include <locale>
#include <codecvt>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <thread>
#include <future>
#include "cbt_hook.h"

namespace nj = nlohmann;

//#define MYDEBUG
#ifdef MYDEBUG
    #define DF_NAME "H:\\2.txt"
    FILE* openDebugLog() 
    {
        FILE* df = nullptr;
        if (fopen_s(&df, DF_NAME, "a") != 0) {
            return nullptr;
        }
        return df;
    }
    void inline mylog(const char* format, ...)
    {
        FILE* df = openDebugLog();
        va_list args;
        va_start(args, format);
        vfprintf(df, format, args);
        va_end(args);
        fprintf(df, "\n");
        fflush(df);
        fclose(df);
    }
    void inline ml(const std::string& s)
    {
        FILE* df = openDebugLog();
        fprintf(df, s.c_str());
        fprintf(df, "\n");
        fflush(df);
        fclose(df);
    }
#else
    void inline mylog(const char* format, ...)
    {}
#endif
#define MAX_BUFFER_SIZE 1024

std::wstring_convert<std::codecvt_utf8<wchar_t>> CONVERTER;
const std::string REQ_PROCESS_FILTER("process_filter");
std::wstring dllPath;
HINSTANCE hInstance = nullptr;
std::unique_ptr<std::thread> windowThread;

typedef struct RequestDataStruct {
    std::wstring processFilter;
    nj::json hwnds = nj::json::array();
    nj::json errors = nj::json::array();
} RequestData;

BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam)
{
    RequestData& data = *reinterpret_cast<RequestData*>(lParam);
    mylog("cb hwnd=%lu", (UINT32)hwnd);
    std::string sPath;
    DWORD processId;
    DWORD code = GetWindowThreadProcessId(hwnd, &processId);
    if (code != 0) {
        mylog("ProcessID = %lu", (UINT32)processId);
        bool passesFilter = true;
        HANDLE processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
        if (processHandle == nullptr) {
            mylog("Failed to open process!");
            data.errors.push_back(GetLastError());
            return true;
        } else {
            std::wstring wPath;
            wPath.resize(MAX_BUFFER_SIZE);
            DWORD size = GetModuleFileNameEx(processHandle, NULL, &wPath[0], MAX_BUFFER_SIZE);
            wPath.resize(size - 1); // get rid of null terminator
            if (data.processFilter.length() > 0) {
                passesFilter = data.processFilter == wPath;
            }
            sPath = CONVERTER.to_bytes(wPath);
            CloseHandle(processHandle);
        }
        if (!passesFilter) {
            return true;
        }
    }
    else {
        mylog("Getting processID failed!");
        data.errors.push_back(GetLastError());
        return true;
    }
    size_t length = GetWindowTextLength(hwnd);
    code = GetLastError();
    if ((length == 0) && (code != 0)) {
        data.errors.push_back(code);
        return true;
    }
    std::wstring wTitle;
    wTitle.resize(length + 1);
    length = GetWindowText(hwnd, &wTitle[0], length + 1);
    code = GetLastError();
    if ((length == 0) && (code != 0)) {
        data.errors.push_back(code);
        return true;
    }
    wTitle.resize(length);
    std::string sTitle = CONVERTER.to_bytes(wTitle);
    data.hwnds.push_back({
        {"hwnd", (UINT32)hwnd},
        {"path", sPath},
        {"title", sTitle},
    });
    return TRUE;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_CBT_ACTIVATE_MSG:
        return 0;
    case WM_CBT_CREATE_WINDOW_MSG:
        return 0;
    case WM_CBT_DESTROY_WINDOW_MSG:
        return 0;
    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

void windowThreadFunc(std::promise<std::string> errorPromise) {
    WNDCLASS wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = TEXT("HWNDObserverInvisibleWindowClass");
    if (!RegisterClass(&wc))
    {
        errorPromise.set_value("Call to RegisterClass failed");
        return;
    }
    HWND hwnd = CreateWindow(TEXT("HWNDObserverInvisibleWindowClass"), TEXT("HWND Observer Invisible Window"), WS_OVERLAPPEDWINDOW & ~WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInstance, NULL);
    if (!hwnd)
    {
        errorPromise.set_value("Call to CreateWindow failed!");
        return;
    }    
    errorPromise.set_value("");
    MSG msg{};
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

}

std::string createInvisibleWindow()
{
    std::promise<std::string> errorPromise;
    std::future<std::string> errorFuture = errorPromise.get_future();    
    windowThread.reset(new std::thread(windowThreadFunc, std::move(errorPromise)));
    std::string error = errorFuture.get();

        
}

nj::json init(nj::json& request)
{
    if (request.contains("dbName")) {
        nj::json response = {
            {"error", "dbName not specified"},
        };
        return response;
    }
    createInvisibleWindow();

    nj::json response = {
        {"error", "asdf"},
    };
    return response;
}
nj::json queryHwndsImpl(nj::json &request)
{
    mylog("queryHwndsImpl");
    RequestData data;
    if (request.contains(REQ_PROCESS_FILTER)) {
        data.processFilter = CONVERTER.from_bytes(request[REQ_PROCESS_FILTER]);
    }
    mylog("Calling EnumWindows");
    EnumWindows(EnumWindowsCallback, reinterpret_cast< LPARAM >(&data));
    mylog("EnumWindows Done");
    nj::json response = { 
        {"hwnds", data.hwnds},
    {"errors", data.errors},
    };
    return response;
}

extern "C" __declspec(dllexport) char* queryHwnds(char* request)
{
    mylog("asdf");
    std::string requestStr(request);
    nj::json  requestJson = nj::json::parse(requestStr);
    std::string command;
    if (requestJson.contains("command")) {
        command = requestJson["command"];
    }
    nj::json responseJson;
    if (command == "queryHwnds") {
        responseJson = queryHwndsImpl(requestJson);
    }
    else if (command == "init") {
        responseJson = init(requestJson);
    }
    std::string responseStr = responseJson.dump(4);
    char*  ptr = _strdup(responseStr.c_str());
    return ptr;
}

extern "C" __declspec(dllexport) void freeBuffer(char* buffer)
{
    free(buffer);
}




BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        hInstance = hModule;
        {
            std::wstring wPath;
            wPath.resize(MAX_BUFFER_SIZE);
            DWORD size = GetModuleFileNameEx((HMODULE)hModule, NULL, &wPath[0], MAX_BUFFER_SIZE);
            wPath.resize(size - 1); // get rid of null terminator
            dllPath = wPath;
            #ifdef MYDEBUG
                FILE* df = nullptr;
                if (fopen_s(&df, DF_NAME, "w") != 0) {
                    // ?
                }
                fclose(df);
            #endif
        }
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
