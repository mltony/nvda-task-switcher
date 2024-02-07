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
#include <iostream>
#include <sstream>
#include <tchar.h>
#include <fstream>
#include <mutex>
#include "cbt_hook.h"

using nlohmann::json;

#define MYDEBUG
#ifdef MYDEBUG
    std::mutex mylogMtx;
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
        std::lock_guard<std::mutex> guard(mylogMtx);
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
        std::lock_guard<std::mutex> guard(mylogMtx);
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
const std::string BOOT_TIME_COMMAND = "Wmic os get lastbootuptime";
const std::vector<std::string> arches = {/*"Win32",*/ "x64" };
std::wstring dllPath;
HINSTANCE hInstance = nullptr;
HWND invisibleHwnd = nullptr;
std::unique_ptr<std::thread> windowThread;
std::unordered_map<std::string, HANDLE> processHandles;
std::string cacheFileName;
std::unique_ptr<std::thread> cacheDumpThread;
std::mutex cacheMtx;
HANDLE hEvent = nullptr;

struct RequestData {
    std::wstring processFilter;
    json hwnds = json::array();
    json errors = json::array();
};

struct InitRequestData {
    HWND hwnd = nullptr;
    std::string error;
    std::unordered_map<std::string, UINT32> processIds;
};

void to_json(json& j, const InitRequestData& data) {
    j = json{
        {"hwnd ", (UINT32)data.hwnd },
        {"error", data.error},
        {"ProcessIds", data.processIds},
    };
}

struct HwndCache {
    std::string bootTime;
    std::unordered_map<UINT32, UINT64> hwndTimes;
};

HwndCache hwndCache;
void to_json(json& j, const HwndCache& data) {
    j = json{
        {"bootTime", data.bootTime},
        {"hwndTimes", data.hwndTimes},
    };
}

void from_json(const json& j, HwndCache& cache) {
    j.at("bootTime").get_to(cache.bootTime);
    j.at("hwndTimes").get_to(cache.hwndTimes);
}

std::string getBootTime()
{
    FILE* pipe = _popen(BOOT_TIME_COMMAND.c_str(), "r");
    if (!pipe) {
        return "Cannot open pipe!";
    }
    char buffer[128];
    std::string result;
    size_t i = 0;
    //while (fscanf(pipe, "%127s", buffer) != EOF) {
    while (fscanf_s(pipe, "%127s", buffer, sizeof(buffer)) != EOF) {
        buffer[sizeof(buffer) - 1] = '\0';
        if (i == 1) {
            result = buffer;
        }
        i++;
    }
    _pclose(pipe);
    return result;
}

std::string dumpCache(std::string &fileName)
{
    std::lock_guard<std::mutex> guard(cacheMtx);
    std::string tmpFileName = fileName + ".tmp";
    std::remove(tmpFileName.c_str()); // Don't care whether succeeds
    {
        std::ofstream fout(tmpFileName);
        if (!fout) {
            std::string msg = "Error opening " + tmpFileName;
            return msg;
        }
        json j = hwndCache;
        fout << j;
    }
    std::remove(fileName.c_str());
    std::rename(tmpFileName.c_str(), fileName.c_str());
    std::remove(tmpFileName.c_str());
    return "";
}

void cacheDumpThreadFunc(std::string fileName)
{
    mylog("CDTF:start");
    while (WAIT_TIMEOUT == WaitForSingleObject(hEvent, 10000)) {
        mylog("CDTF:loop");
        std::string error = dumpCache(fileName);
        if (!error.empty()) {
            mylog("Error dumping cache: %s", error.c_str());
        }
    }
    mylog("CDTF:finish");
}

std::string loadCache(std::string &fileName)
{
    std::lock_guard<std::mutex> guard(cacheMtx);
    std::string bootTime = getBootTime();
    mylog("Current boot time: %s", bootTime.c_str());
    if (bootTime.length() == 0) {
        mylog("Retrieved boot time is empty");
        return "Retrieved boot time is empty"; 
    }
    mylog("Loading cache from %s", fileName.c_str());
    std::ifstream fin(fileName.c_str(), std::ios::binary);
    bool fileExists = fin.good();
    if (!fileExists) {
        mylog("Cache file not found - creating a blank cache.");
        hwndCache = HwndCache();
    }
    else {
        json j;
        HwndCache loadedCache;
        try {
            fin >> j;
            loadedCache = j;
        }
        catch (const json::parse_error& e) {
            // Do nothing - will create a blank cache
        }
        if (loadedCache.bootTime == bootTime) {
            mylog("bootTime match! Reusing cache.");
            hwndCache = loadedCache;
        }
        else {
            mylog("bootTime mismatch! System must have been rebooted. Creating a blank cache.");
            hwndCache = HwndCache();
        }
    }

    mylog("All done with cache.");
    mylog("Creating event.");
    hEvent = CreateEvent(NULL, FALSE, FALSE, L"cacheDumpThreadTerminateEvent");
    if (hEvent == NULL) {
        std::string msg = "Create event failed; error = " + std::to_string(GetLastError());
        return msg;
    }
    mylog("Launching cacheDumpThread");
    cacheDumpThread = std::make_unique<std::thread>(cacheDumpThreadFunc, fileName);
    mylog("InitCache succeeded!");
    return "";
}

std::string terminateCache(std::string& fileName)
{
    if (!SetEvent(hEvent)) {
        std::string msg = "SetEvent failed; error " + std::to_string(GetLastError());
        return msg;
    }
    cacheDumpThread->join();
    cacheDumpThread = nullptr;
    CloseHandle(hEvent);
    std::string error = dumpCache(fileName);
    return error;
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    //mylog("MSG %lu %lu %lu %lu", (DWORD)hwnd, (DWORD)uMsg, (DWORD)wParam, (DWORD)lParam);
    switch (uMsg)
    {
    case WM_DESTROY:
        mylog("WM_DESTROY");
        PostQuitMessage(0);
        return 0;
    case WM_HWND_OBSERVER_DESTROY_WINDOW:
        mylog("WM_HWND_OBSERVER_DESTROY_WINDOW; Calling DestroyWindow");
        DestroyWindow(invisibleHwnd);
        invisibleHwnd = nullptr;
        return 0;
    case WM_CBT_ACTIVATE_MSG:
        //mylog("WM_CBT_ACTIVATE_MSG");
        return 0;
    case WM_CBT_CREATE_WINDOW_MSG:
        //mylog("WM_CBT_CREATE_WINDOW_MSG");
        //Beep(750, 300);
        MessageBeep(0xFFFFFFFF);        return 0;
    case WM_CBT_DESTROY_WINDOW_MSG:
        //mylog("WM_CBT_DESTROY_WINDOW_MSG");
        return 0;
    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

void windowThreadFunc(std::promise<InitRequestData > errorPromise) {
    InitRequestData data;
    WNDCLASS wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = TEXT(HWND_OBSERVER_WINDOW_CLASS);
    if (!RegisterClass(&wc))
    {
        data.error = "Call to RegisterClass failed";
        errorPromise.set_value(data);
        return;
    }
    HWND hwnd = CreateWindow(TEXT(HWND_OBSERVER_WINDOW_CLASS), TEXT("HWND Observer Invisible Window"), WS_OVERLAPPEDWINDOW & ~WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInstance, NULL);
    if (!hwnd)
    {
        data.error = "Call to CreateWindow failed!";
        errorPromise.set_value(data);
        return;        
    }
    data.hwnd = hwnd;
    invisibleHwnd = hwnd;
    mylog("WTF: returning promise");
    errorPromise.set_value(data);
    mylog("WTF: Entering message loop");
    MSG msg{};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    mylog("WTF: exited message loop; quitting");
}

bool createInvisibleWindow(InitRequestData &data)
{
    std::promise<InitRequestData > errorPromise;
    std::future<InitRequestData > errorFuture = errorPromise.get_future();
    windowThread = std::make_unique<std::thread>(windowThreadFunc, std::move(errorPromise));
    data= errorFuture.get();
    return data.error.length() == 0;
}

std::tuple< HANDLE, HANDLE> createPipe()
{
    HANDLE g_hChildStd_IN_Rd = NULL;
    HANDLE g_hChildStd_IN_Wr = NULL;
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    // Create a pipe for the child process's STDIN.
    if (!CreatePipe(&g_hChildStd_IN_Rd, &g_hChildStd_IN_Wr, &saAttr, 0)) {
        return std::make_tuple(g_hChildStd_IN_Rd, g_hChildStd_IN_Wr);
    }

    // Ensure the write handle to the pipe for STDIN is not inherited.
    if (!SetHandleInformation(g_hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0)) {
        return std::make_tuple((HANDLE)nullptr, (HANDLE)nullptr);
    }
    return std::make_tuple(g_hChildStd_IN_Rd, g_hChildStd_IN_Wr);
}

bool spawnCbtClient(InitRequestData& data, const std::string& arch)
{
    std::wstring clientPath(dllPath);
    std::size_t pos = clientPath.rfind(L"\\");
    clientPath.resize(pos + 1);
    clientPath += CONVERTER.from_bytes(arch);
    clientPath += L"\\";
    clientPath += L"cbt_client.exe";
    //clientPath += L" ";
    //clientPath += std::to_wstring((UINT32)data.hwnd);
    mylog("Launching: %s", CONVERTER.to_bytes(clientPath).c_str());
    size_t len = clientPath.length();
    wchar_t* command = _wcsdup(clientPath.c_str());

    std::tuple< HANDLE, HANDLE> pipeHandleds = createPipe();
    HANDLE g_hChildStd_IN_Rd = std::get<0>(pipeHandleds), g_hChildStd_IN_Wr = std::get<1>(pipeHandleds);
    if ((g_hChildStd_IN_Rd == nullptr) || (g_hChildStd_IN_Wr == nullptr)) {
        data.error = "Failed to create pipes for CBT client child process";
        return false;
    }
    STARTUPINFO siStartInfo;
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
    siStartInfo.cb = sizeof(STARTUPINFO);
    siStartInfo.hStdError = g_hChildStd_IN_Wr;
    siStartInfo.hStdOutput = g_hChildStd_IN_Wr;
    siStartInfo.hStdInput = g_hChildStd_IN_Rd;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION piProcInfo;
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

    // Replace "command" with the actual command you wish to execute.
    if (!CreateProcess(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &siStartInfo, &piProcInfo)) {
        DWORD code = GetLastError();
        data.error = "CreateProcess failed. ErrorCode = ";
        data.error += std::to_string(code);
        return false;
    }
    DWORD processId = piProcInfo.dwProcessId;
    data.processIds[arch] = processId;

    free(command);
    CloseHandle(piProcInfo.hProcess);
    CloseHandle(piProcInfo.hThread);
    CloseHandle(g_hChildStd_IN_Rd);
    processHandles[arch] = g_hChildStd_IN_Wr;
    return true;
}

DWORD killCbtClient(std::string arch)
{
    HANDLE g_hChildStd_IN_Wr = processHandles[arch];
    // Write to the pipe that is the standard input for a child process.
    const char* inputText = "quit";
    DWORD dwWritten;
    if (!WriteFile(g_hChildStd_IN_Wr, inputText, strlen(inputText), &dwWritten, NULL)) {
        DWORD error = GetLastError();
        return error;
    }
    if (!CloseHandle(g_hChildStd_IN_Wr)) {
        DWORD error = GetLastError();
        return error;
    }
    return  0;
}

json init(json& request)
{
    mylog("init");
    InitRequestData data;
    if (!request.contains("cacheFileName")) {
        data.error = "cacheFileName not specified";
        return data;
    }
    cacheFileName = request["cacheFileName"];
    mylog("init:loading cache");
    std::string error = loadCache(cacheFileName);
    if (!error.empty()) {
        data.error = "Failed to load cache from file: " + error;
        return data;
    }
    mylog("init:checking if invisible window class already exists/...");
    HWND hwnd = FindWindow(_T(HWND_OBSERVER_WINDOW_CLASS), NULL);
    if (hwnd != nullptr) {
        data.error = "Monitoring window already exists. Cannot initialize.";
        return data;
    }
    mylog("init:creating invisible window");
    if (!createInvisibleWindow(data)) {
        return data;
    }
    mylog("init:launching cbt clients...");
    for (std::string arch : arches) {
        if (!spawnCbtClient(data, arch)) {
            return data;
        }
    }
    mylog("Init:success");
    return data;
}

json terminate(json& request)
{
    mylog("Terminate");
    json result;

    mylog("Terminate: killing cbt clients");
    for (std::string arch : arches) {
        DWORD code = killCbtClient(arch);
        if (code != 0) {
            std::string msg = "Error ";
            msg += std::to_string(code);
            msg += " while terminating CBT client " + arch;
            mylog("Terminate: %s", msg.c_str());
            result["error"] = msg;
            return result;
        }
    }
    mylog("Terminate: destroying invisible window");
    if (invisibleHwnd == nullptr) {
        result["error"] = "invisibleHwnd is null";
        return result;
    }
    PostMessage(invisibleHwnd, WM_HWND_OBSERVER_DESTROY_WINDOW, 0, 0);
    /*
    if (!DestroyWindow(invisibleHwnd)) {
        DWORD code = GetLastError();
        mylog("Terminate: DestroyWindow failed: code = %lu", code);
        result["error"] = "DestroyWindow failed : code = " +std::to_string(code);
        return result;
    }
    invisibleHwnd = nullptr;
    */

    mylog("Terminate: killing windowThread");
    if (windowThread == nullptr) {
        result["error"] = "windowThread is null";
        return result;
    }    
    mylog("Terminate: killing windowThread: calling thread::join()");
    windowThread->join();
    mylog("Terminate: killing windowThread: thread::join() done");
    windowThread = nullptr;
    mylog("Terminate: terminating cache");
    std::string error = terminateCache(cacheFileName);
    if (!error.empty()) {
        result["error"] = "Error terminating cache: " + error;
        return result;
    }
    mylog("Terminate: success");
    result["error"] = "";
    return result;
}

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
        }
        else {
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

json queryHwndsImpl(json &request)
{
    mylog("queryHwndsImpl");
    RequestData data;
    if (request.contains(REQ_PROCESS_FILTER)) {
        data.processFilter = CONVERTER.from_bytes(request[REQ_PROCESS_FILTER]);
    }
    mylog("Calling EnumWindows");
    EnumWindows(EnumWindowsCallback, reinterpret_cast< LPARAM >(&data));
    mylog("EnumWindows Done");
    json response = { 
        {"hwnds", data.hwnds},
    {"errors", data.errors},
    };
    return response;
}

extern "C" __declspec(dllexport) char* queryHwnds(char* request)
{
    mylog("Request received");
    std::string requestStr(request);
    json  requestJson;
    try {
        requestJson = json::parse(requestStr);
    }
    catch (json::parse_error& e) {
        char* ptr = _strdup("{\"error\":\"Error parsing input json!\"}");
        return ptr;
    }
    std::string command;
    if (requestJson.contains("command")) {
        command = requestJson["command"];
    }
    mylog("command = %s", command.c_str());
    json responseJson;
    if (command == "queryHwnds") {
        responseJson = queryHwndsImpl(requestJson);
    }
    else if (command == "init") {
        responseJson = init(requestJson);
    }
    else if (command == "terminate") {
        responseJson = terminate(requestJson);
    }
    else {
        responseJson = { {"error", "Unknown command"}};
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
            #ifdef MYDEBUG
                FILE* df = nullptr;
                if (fopen_s(&df, DF_NAME, "w") != 0) {
                    // ?
                }
                fclose(df);
                std::string sDllPath = CONVERTER.to_bytes(dllPath);
                
            #endif
            
            std::wstring wPath;
            wPath.resize(MAX_BUFFER_SIZE);
            //DWORD size = GetModuleFileNameEx((HMODULE)hModule, NULL, &wPath[0], MAX_BUFFER_SIZE); // WTF this doesn't work!?
            DWORD size = GetModuleFileName((HMODULE)hModule, &wPath[0], MAX_BUFFER_SIZE);
            DWORD code = GetLastError();
            mylog("hModule + %lu, Size = %lu, code = %lu", (DWORD)hModule, size, code);
            std::string sPath = CONVERTER.to_bytes(wPath);
            mylog("wPath %s", sPath.c_str());
            wPath.resize(size);
            sPath = CONVERTER.to_bytes(wPath);
            mylog("wPath %s", sPath.c_str());            
            dllPath = wPath;
            mylog("DLL_PROCESS_ATTACH %s", sDllPath.c_str());
        }
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
