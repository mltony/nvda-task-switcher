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
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include "cbt_hook.h"
#include <algorithm>
#include <stdexcept>

using nlohmann::json;

//#define MYDEBUG
#ifdef MYDEBUG
    std::mutex mylogMtx;
    #define DF_NAME "H:\\od\\2.txt"
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
const std::vector<std::string> arches = {"Win32", "x64" };
std::wstring dllPath;
HINSTANCE hInstance = nullptr;
HWND invisibleHwnd = nullptr;
std::unique_ptr<std::thread> windowThread;
std::unordered_map<std::string, HANDLE> processHandles;
std::string cacheFileName;
std::unique_ptr<std::thread> cacheDumpThread;
std::mutex cacheMtx;
HANDLE hEvent = nullptr;
volatile bool cacheDumpThreadTerminateSignal = false;

UINT64 getTimestamp()
{
    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = now - std::chrono::high_resolution_clock::time_point{};
    std::uint64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return timestamp;
}

struct RequestData {
    std::wstring processFilter;
    bool onlyVisible;
    bool requestTitle;
    json hwnds = json::array();
    json errors = json::array();
    UINT64 timestamp;
    std::unordered_set<UINT32> allHwnds;
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


std::wstring toLowerCase(const std::wstring& str) {
    std::wstring result = str;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return result;
}

std::wstring getFileName(const std::wstring& fullPath) {
    wchar_t sep = L'\\';
    size_t pos = fullPath.find_last_of(sep);
    if (pos != std::wstring::npos) {
        std::wstring fileName = toLowerCase(fullPath.substr(pos + 1));
        size_t pos = fileName.rfind(L".exe");
        if (pos != std::wstring::npos) {
            return fileName.substr(0, pos); // Return the substring without the ".exe" extension
        }
        return fileName;
    }
    return L""; // Return an empty string if no separator is found
}


std::string dumpCache(std::string &fileName)
{
    // mutex must be acquired from calling function!
    //std::lock_guard<std::mutex> guard(cacheMtx);
    std::string tmpFileName = fileName + ".tmp";
    std::remove(tmpFileName.c_str()); // Don't care whether succeeds
    {
        std::ofstream fout(tmpFileName);
        if (!fout) {
            std::string msg = "Error opening " + tmpFileName;
            return msg;
        }
        json j =hwndCache;
        std::string jsonStr= j.dump(4);
        fout << jsonStr;
    }
    std::remove(fileName.c_str());
    std::rename(tmpFileName.c_str(), fileName.c_str());
    std::remove(tmpFileName.c_str());
    return "";
}

void updateCache(std::unordered_set<UINT32>& allHwnds, UINT64 defaultTimestamp)
{
    // Lock must be acquired by a calling function.
    //std::lock_guard<std::mutex> guard(cacheMtx);
    auto& hwndTimes = hwndCache.hwndTimes;
    // 1. Drop all hwnds not found during EnumWindows run
    for (auto it = hwndTimes.begin(); it != hwndTimes.end(); /* no increment */) {
        UINT32 hwnd = it->first;
        if (allHwnds.find(hwnd) == allHwnds.end()) {
            it = hwndTimes.erase(it);
        }
        else {
            ++it;
        }
    }
    // 2. Add all hwnds that are found, but missing in the cache.
    for (auto it = allHwnds.begin(); it != allHwnds.end(); it++) {
        if (hwndTimes.find(*it) == hwndTimes.end()) {
            hwndTimes.emplace(*it, defaultTimestamp);
        }
    }
}

void cacheDumpThreadFunc(std::string fileName)
{
    mylog("CDTF:start");
    while (true) {
        DWORD code = WaitForSingleObject(hEvent, 10000);
        if (code == WAIT_TIMEOUT) {
            // whatever
        }
        if (cacheDumpThreadTerminateSignal) {
            break;
        }
        mylog("CDTF:loop");
        {
            std::lock_guard<std::mutex> guard(cacheMtx);
            std::string error = dumpCache(fileName);
            if (!error.empty()) {
                mylog("Error dumping cache: %s", error.c_str());
            }
        }
    }
    mylog("CDTF:finish");
}

std::string loadCache(std::string &fileName, std::string &bootTime)
{
    std::lock_guard<std::mutex> guard(cacheMtx);
    mylog("Current boot time: %s", bootTime.c_str());
    if (bootTime.length() == 0) {
        mylog("Retrieved boot time is empty");
        return "Retrieved boot time is empty"; 
    }
    mylog("Loading cache from %s", fileName.c_str());
    std::ifstream fin(fileName, std::ios::binary);
    bool fileExists = fin.good();
    if (!fileExists) {
        mylog("Cache file not found - creating a blank cache.");
        hwndCache = HwndCache();
        hwndCache.bootTime = bootTime;
        std::ofstream fout(fileName, std::ios::out | std::ios::binary);
        if (!fout.is_open()) {
            return "Cannot create cache file " + fileName;
        }
    }
    else {
        mylog("Cache file exists - reading cache");
        json j;
        HwndCache loadedCache;
        try {
            fin >> j;
            mylog("Loaded json object from cache file");
            loadedCache = j;
            mylog("Converted json object to cache instance");
        }
        catch (const json::parse_error& e) {
            // Do nothing - will create a blank cache
            mylog("JSON parse error!");
        }
        mylog("Checking boot time");
        // Boot time is reported with up to second accuracy, so comparing difference
        INT64 uBootTime, uCacheBootTime;
        try {
            uBootTime = std::stoull(bootTime);
        }
        catch (std::invalid_argument& e) {
            return "Invalid boot time received from python: " + bootTime;
        }
        try {
            uCacheBootTime = std::stoull(loadedCache.bootTime);
        }
        catch (std::invalid_argument& e) {
            return "Invalid boot time read from cache on disk: " + loadedCache.bootTime;
        }
        INT64 uBootTimeDiff = std::abs(uBootTime - uCacheBootTime);
        mylog("uBootTime =%lld, uCacheBootTime =%lld, uBootTimeDiff =%lld", uBootTime, uCacheBootTime, uBootTimeDiff);
        if (uBootTimeDiff < 5) {
            mylog("bootTime match within 5 seconds! Reusing cache.");
            hwndCache = loadedCache;
        }
        else {
            mylog("bootTime mismatch! System must have been rebooted. Creating a blank cache.");
            hwndCache = HwndCache();
            hwndCache.bootTime = bootTime;
        }
    }

    mylog("All done with cache.");
    mylog("Creating event.");
    hEvent = CreateEvent(NULL, FALSE, FALSE, L"cacheDumpThreadSignalEvent");
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
    cacheDumpThreadTerminateSignal = true;
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
    HWND targetHwnd = (HWND)wParam;
    UINT64 timestamp = getTimestamp();
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
        mylog("WM_CBT_CREATE_WINDOW_MSG HWND=%lu t=%llu", (UINT32)(DWORD)targetHwnd, (UINT64)timestamp);
        //MessageBeep(0xFFFFFFFF);        
        {
            std::lock_guard<std::mutex> guard(cacheMtx);
            hwndCache.hwndTimes[(DWORD)targetHwnd] = timestamp;
        }
        if (!SetEvent(hEvent)) {
            std::string msg = "SetEvent failed; error " + std::to_string(GetLastError());
            mylog(msg.c_str());
        }
        return 0;
    case WM_CBT_DESTROY_WINDOW_MSG:
        mylog("WM_CBT_DESTROY_WINDOW_MSG");
        {
            std::lock_guard<std::mutex> guard(cacheMtx);
            hwndCache.hwndTimes.erase((DWORD)targetHwnd);
        }        
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
    mylog("SP start");
    std::wstring clientPath(dllPath);
    std::size_t pos = clientPath.rfind(L"\\");
    clientPath.resize(pos + 1);
    clientPath += CONVERTER.from_bytes(arch);
    clientPath += L"\\";
    clientPath += L"cbt_client.exe";
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

    if (!CreateProcess(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &siStartInfo, &piProcInfo)) {
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
    std::string bootupTime = request["bootupTime"];
    mylog("init:loading cache");
    std::string error = loadCache(cacheFileName, bootupTime);
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
        mylog("init: loading arch %s", arch.c_str());
        if (!spawnCbtClient(data, arch)) {
            mylog("spawnCbtClient failed!");
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
        DWORD brokenPipe = 232; // (0xE8)
        if ((code != 0) && (code != brokenPipe)) {
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

bool IsWindowMaximized(HWND hWnd) {
    WINDOWPLACEMENT placement;
    placement.length = sizeof(WINDOWPLACEMENT);

    // Get the current window placement
    BOOL result = GetWindowPlacement(hWnd, &placement);

    // Check if the call was successful
    if (result == FALSE) {
        std::cerr << "Failed to get window placement." << std::endl;
        return false; // Or handle error appropriately
    }

    // A window is considered maximized if its showCmd is SW_SHOWMAXIMIZED
    return placement.showCmd == SW_SHOWMAXIMIZED;
}

std::unordered_map<UINT32, std::wstring> appNameCache;
std::unordered_map<UINT32, std::string> fullPathCache;
BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam)
{
    RequestData& data = *reinterpret_cast<RequestData*>(lParam);
    UINT32 uHwnd = (UINT32)hwnd;
    data.allHwnds.emplace(uHwnd);
    bool needCheckFileName = true;
    std::string sPath;
    if (appNameCache.count(uHwnd) > 0) {
        std::wstring wAppName = appNameCache[uHwnd];
        bool passesFilter = data.processFilter == wAppName;
        if (!passesFilter) {
            return true;
        }
        sPath = fullPathCache[uHwnd];
        needCheckFileName = false;
    }
    HWND hParent = GetParent(hwnd);
    if (hParent != nullptr) {
        // This is not a top-level window, skipping.
        return true;
    }    
    BOOL isVisible = IsWindowVisible(hwnd);
    if ((data.onlyVisible) && (!isVisible)) {
        // requested only visible windows and this one is invisible
        return true;
    }
    mylog("cb hwnd=%lu", (UINT32)hwnd);
    if (needCheckFileName) {
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
                wPath.resize(size);
                sPath = CONVERTER.to_bytes(wPath);
                std::wstring wFileName = getFileName(wPath);
                appNameCache[uHwnd] = wFileName;
                fullPathCache[uHwnd] = sPath;
                if (data.processFilter.length() > 0) {
                    passesFilter = data.processFilter == wFileName;
                    std::string actual = CONVERTER.to_bytes(wFileName);
                    std::string pf = CONVERTER.to_bytes(data.processFilter);
                    mylog("asdf '%s' '%s'", actual.c_str(), pf.c_str());
                }
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
    }
    size_t length = GetWindowTextLength(hwnd);
    auto code = GetLastError();
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
    UINT64 timestamp = data.timestamp;
    if (hwndCache.hwndTimes.count((UINT32)hwnd) > 0) {
        timestamp = hwndCache.hwndTimes[(DWORD)hwnd];
    }
    bool isMaximized = IsWindowMaximized(hwnd);
    data.hwnds.push_back({
        {"hwnd", (UINT32)hwnd},
        {"path", sPath},
        {"title", sTitle},
        {"timestamp", timestamp},
        {"isMaximized", isMaximized},
    });
    return TRUE;
}

json queryHwndsImpl(json &request)
{
    mylog("queryHwndsImpl");
    RequestData data;
    data.timestamp = getTimestamp();
    data.onlyVisible = request["onlyVisible"];
    data.requestTitle = request["requestTitle"];    if (request.contains(REQ_PROCESS_FILTER)) {
        data.processFilter = CONVERTER.from_bytes(request[REQ_PROCESS_FILTER]);
    }
    mylog("Calling EnumWindows");
    {
        std::lock_guard<std::mutex> guard(cacheMtx);
        auto start = std::chrono::high_resolution_clock::now();
        EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&data));
        auto stop = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
        std::chrono::milliseconds ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
        int ms_int = ms.count();
        int dt = ms_int;
        mylog("asdf dt %d us", dt);
        updateCache(data.allHwnds, data.timestamp);
    }
    mylog("EnumWindows Done");
    json response = { 
        {"hwnds", data.hwnds},
    {"errors", data.errors},
    };
    return response;
}

json updateTimestamps(json& request)
{
    std::lock_guard<std::mutex> guard(cacheMtx);
    for (const auto& window : request["windows"]) {
        UINT32 hwnd = window["hwnd"];
        UINT64 timestamp = window["timestamp"];
        hwndCache.hwndTimes[hwnd] = timestamp;
    }    
    if (!SetEvent(hEvent)) {
        std::string msg = "SetEvent failed; error " + std::to_string(GetLastError());
        return json({ {"error", msg}});
    }
    return json({});
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
    else if (command == "updateTimestamps") {
        responseJson = updateTimestamps(requestJson);
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
            #endif
            std::wstring wPath;
            wPath.resize(MAX_BUFFER_SIZE);
            //DWORD size = GetModuleFileNameEx((HMODULE)hModule, NULL, &wPath[0], MAX_BUFFER_SIZE); // WTF this doesn't work!?
            DWORD size = GetModuleFileName((HMODULE)hModule, &wPath[0], MAX_BUFFER_SIZE);
            wPath.resize(size);
            DWORD code = GetLastError();
            mylog("hModule=  %lu, Size = %lu, code = %lu", (DWORD)hModule, size, code);
            std::string sPath = CONVERTER.to_bytes(wPath);
            mylog("wPath %s", sPath.c_str());
            sPath = CONVERTER.to_bytes(wPath);
            mylog("wPath %s", sPath.c_str());            
            dllPath = wPath;
            std::string sDllPath = CONVERTER.to_bytes(dllPath);
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
