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
#include <ctime>
#include <leveldb/db.h>
#include <leveldb/write_batch.h>

using nlohmann::json;

std::wstring logFileName;

#define MYDEBUG
#ifdef MYDEBUG
    std::mutex mylogMtx;
    //#define DF_NAME "H:\\od\\2.txt"
    FILE* openDebugLog() 
    {
        FILE* df = nullptr;
        _wfopen_s(&df, logFileName.c_str(), L"a");
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
#define MAX_CBT_RESTART_COUNT 10

class CBTException : public std::exception {
  private:
    std::string message;

  public:
    CBTException(const std::string& msg) : message(msg) {}

    const char* what() const throw() override {
        return message.c_str();
    }
};

std::wstring string_to_wide_string(const std::string& string)
{
    if (string.empty())
    {
        return L"";
    }

    const auto size_needed = MultiByteToWideChar(CP_UTF8, 0, string.data(), (int)string.size(), nullptr, 0);
    if (size_needed <= 0)
    {
        throw std::runtime_error("MultiByteToWideChar() failed: " + std::to_string(size_needed));
    }

    std::wstring result(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &string[0], (int)string.size(), &result[0], size_needed);
    return result;
}

std::string wide_string_to_string(const std::wstring& wide_string)
{
    if (wide_string.empty())
    {
        return "";
    }

    const auto size_needed = WideCharToMultiByte(CP_UTF8, 0, wide_string.data(), (int)wide_string.size(), nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0)
    {
        throw std::runtime_error("WideCharToMultiByte() failed: " + std::to_string(size_needed));
    }

    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wide_string[0], (int)wide_string.size(), &result[0], size_needed, nullptr, nullptr);
    return result;
}


std::wstring_convert<std::codecvt_utf8<wchar_t>> CONVERTER;
const std::string REQ_PROCESS_FILTER("process_filter");
const std::string BOOT_TIME_COMMAND = "Wmic os get lastbootuptime";
const std::vector<std::string> arches = {
    "Win32", 
    "x64",
};
std::unordered_map<std::string, std::thread> cbtClientMonitoringThreads;

std::mutex watchdogMtx;
std::unordered_map<std::string, size_t> restartCountByArch;
std::unordered_map<std::string, DWORD> processIDByArch;
//std::unordered_map<std::string, std::time_t> timestampByArch;
std::time_t creationTimestamp = 0;

std::wstring dllPath;
HINSTANCE hInstance = nullptr;
HWND invisibleHwnd = nullptr;
std::unique_ptr<std::thread> windowThread;
std::unordered_map<std::string, HANDLE> processHandles;
volatile bool cbtClientTerminateSignal = false;

uint64_t getCurrentUnixTimeMillis() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

UINT64 getTimestamp()
{
    return getCurrentUnixTimeMillis();
}

bool IsProcessRunning(DWORD pid)

{
    // Attempt to open the process with the given PID
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (hProcess == NULL)
    {
        // If the handle is NULL, the process does not exist
        return false;
    }

    DWORD exitCode;
    if (GetExitCodeProcess(hProcess, &exitCode))
    {
        // If the exit code is STILL_ACTIVE, the process is running
        if (exitCode == STILL_ACTIVE)
        {
            CloseHandle(hProcess);
            return true;
        }
    }

    // Close the handle as it is no longer needed
    CloseHandle(hProcess);
    return false;
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

std::unique_ptr< leveldb::DB> hwndCache;
leveldb::WriteOptions writeOptions;
leveldb::ReadOptions readOptions;
std::string keyBootTime = "bootTime";
std::string hwndTimestampPrefix = "ht";
std::string timestampKey(DWORD hwnd) {
    return hwndTimestampPrefix + std::to_string(hwnd);

}

void cacheTimestamp(DWORD targetHwnd, uint64_t timestamp) {
    leveldb::Status s = hwndCache->Put(writeOptions, timestampKey((DWORD)targetHwnd), std::to_string(timestamp));
    if (!s.ok()) {
        mylog("HWND=%d, but failed to cache it: %s", (int)targetHwnd, s.ToString().c_str());
    }
}
void deleteTimestamp(DWORD targetHwnd) {
    leveldb::Status s = hwndCache->Delete(writeOptions, timestampKey((DWORD)targetHwnd));
    if (!s.ok()) {
        mylog("HWND=%d, but failed to delete cache entry: %s", (int)targetHwnd, s.ToString().c_str());
    }
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


void updateCache(std::unordered_set<UINT32>& allHwnds, UINT64 defaultTimestamp)
{
    {
        // 1. Drop all hwnds not found during EnumWindows run
        leveldb::WriteBatch batch;
        leveldb::Iterator* it = hwndCache->NewIterator(readOptions);
        std::string& start = hwndTimestampPrefix;
        std::string limit = hwndTimestampPrefix + ":";
        for (it->Seek(start);
                it->Valid() && it->key().ToString() < limit;
                it->Next()
        ) {
            std::string strHwnd = it->key().data();
            strHwnd = strHwnd.substr(hwndTimestampPrefix.size());
            DWORD hwnd = std::stoul(strHwnd);
            if (allHwnds.find(hwnd) == allHwnds.end()) {
                batch.Delete(it->key());
            }
        }
        leveldb::Status s = hwndCache->Write(writeOptions, &batch);
        if (!s.ok()) {
            mylog("Failed to BULK DELETE FROM updateCache: %s", s.ToString().c_str());
        }
    }
    {
        // 2. Add all hwnds that are found, but missing in the cache.
        leveldb::WriteBatch batch;
        for (auto it = allHwnds.begin(); it != allHwnds.end(); it++) {
            DWORD hwnd = *it;
            std::string timestampStr;
            leveldb::Status status = hwndCache->Get(readOptions, timestampKey((DWORD)hwnd), &timestampStr);
            if (status.IsNotFound()) {
                batch.Put(timestampKey((DWORD)hwnd), std::to_string(defaultTimestamp));
            } else if (!status.ok()) {
                mylog("Failed to read HWND %d from updateCache: %s", (int)hwnd, status.ToString().c_str());
            }
        }
        leveldb::Status s = hwndCache->Write(writeOptions, &batch);
        if (!s.ok()) {
            mylog("Failed to bulk store new entries from updateCache: %s", s.ToString().c_str());
        }    
    }
}

bool deleteAllEntries() {
    leveldb::WriteBatch batch;
    leveldb::Iterator* it = hwndCache->NewIterator(readOptions);
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        batch.Delete(it->key());
    }
    leveldb::Status s = hwndCache->Write(writeOptions, &batch);
    if (!s.ok()) {
        mylog("Failed to BULK DELETE FROM deleteAllEntries: %s", s.ToString().c_str());
        return false;
    }
    return true;
}

std::string loadCache(std::string &fileName, std::string &bootTime)
{
    mylog("Current boot time: %s", bootTime.c_str());
    if (bootTime.length() == 0) {
        mylog("Retrieved boot time is empty");
        return "Retrieved boot time is empty"; 
    }
    mylog("Loading cache from %s", fileName.c_str());
    leveldb::Options options;
    options.create_if_missing = true;
    leveldb::DB* db;
    leveldb::Status status = leveldb::DB::Open(options, fileName, &db);    hwndCache.reset();
    if (!status.ok()) {
        return "Failed to open levelDB cache file " + fileName + " : " +  status.ToString();
    }
    hwndCache.reset(db);
    if (true) {
        mylog("Checking boot time");
        // Boot time is reported with up to second accuracy, so comparing difference
        INT64 uBootTime, uCacheBootTime;
        try {
            uBootTime = std::stoull(bootTime);
        }
        catch (std::invalid_argument& e) {
            return "Invalid boot time received from python: " + bootTime;
        }
        std::string strCacheBootTime;
        status = db->Get(readOptions, keyBootTime, &strCacheBootTime);
        if (status.IsNotFound()) {
            // Using current boot time received from Python and storing it - likely we just created a new cache file
            strCacheBootTime = "0";
        }
        else if (!status.ok()) {
            return "Failed to read timestamp from levelDB cache file " + fileName + " : " + status.ToString();
        }
        try {
            uCacheBootTime = std::stoull(strCacheBootTime);
        }
        catch (std::invalid_argument& e) {
            return "Invalid boot time read from cache on disk: " + strCacheBootTime;
        }
        INT64 uBootTimeDiff = std::abs(uBootTime - uCacheBootTime);
        mylog("uBootTime =%lld, uCacheBootTime =%lld, uBootTimeDiff =%lld", uBootTime, uCacheBootTime, uBootTimeDiff);
        if (uBootTimeDiff < 5) {
            mylog("bootTime match within 5 seconds! Reusing cache.");
        }
        else {
            mylog("bootTime mismatch! System must have been rebooted. Creating a blank cache.");
            if (!deleteAllEntries()) {
                return "deleteAllEntries failed!";
            }
            strCacheBootTime = bootTime;
            status = db->Put(writeOptions, keyBootTime, strCacheBootTime);
            if (!status.ok()) {
                return "Failed to write timestamp to levelDB cache file " + fileName + " : " + status.ToString();
            }
        }
    }
    mylog("InitCache succeeded!");
    return "";
}

std::string terminateCache()
{
    hwndCache.reset();
    return "";
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    HWND targetHwnd = (HWND)wParam;
    UINT64 timestamp = getTimestamp();
    leveldb::Status s;
    switch (uMsg)
    {
    case WM_DESTROY:
        mylog("WM_DESTROY received");
        PostQuitMessage(0);
        return 0;
    case WM_HWND_OBSERVER_DESTROY_WINDOW:
        mylog("WM_HWND_OBSERVER_DESTROY_WINDOW received; Calling DestroyWindow");
        DestroyWindow(invisibleHwnd);
        invisibleHwnd = nullptr;
        return 0;
    case WM_CBT_ACTIVATE_MSG:
        //mylog("WM_CBT_ACTIVATE_MSG received");
        return 0;
    case WM_CBT_CREATE_WINDOW_MSG:
        //mylog("WM_CBT_CREATE_WINDOW_MSG HWND=%lu t=%llu", (UINT32)(DWORD)targetHwnd, (UINT64)timestamp);
        //MessageBeep(0xFFFFFFFF);        
        //Beep(500, 50);
        cacheTimestamp((DWORD)targetHwnd, timestamp);
        {
            creationTimestamp = std::time(nullptr);
        }
        return 0;
    case WM_CBT_DESTROY_WINDOW_MSG:
        //mylog("WM_CBT_DESTROY_WINDOW_MSG");
        deleteTimestamp((DWORD)targetHwnd);
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

HWND findCBTClientWindow(const std::string& arch) {
    std::wstring cls = CBT_CLIENT_WINDOW_CLASS_PREFIX;
    //cls += CONVERTER.from_bytes(arch);
    cls += string_to_wide_string(arch);
    HWND hwnd = FindWindow(cls.c_str(), NULL);
    return hwnd;
}
std::tuple<DWORD, HANDLE> spawnCbtClient(const std::string& arch, bool initialStart=true)
{
    // Don't throw any exceptions in this func!
    // For some reason when it is running from another thread, any exception will crash the whole app.
    // I just love C++.
    mylog("SP2 start");
    
    if (findCBTClientWindow(arch) != nullptr) {
        mylog("SP: CBT window already exists for arch %s", arch.c_str());
        std::string errorMsg = "CBT client window for arch " + arch + " already exists. Previous cbt_client must not have terminated properly.";
        //throw CBTException(errorMsg);
        //throw std::exception();
        return std::make_tuple(0, nullptr);
    }
    mylog("SP: CBT window doesn't exist for arch %s", arch.c_str());

    std::wstring clientPath(dllPath);
    std::size_t pos = clientPath.rfind(L"\\");
    clientPath.resize(pos + 1);
    //clientPath += CONVERTER.from_bytes(arch);
    clientPath += string_to_wide_string(arch);
    
    clientPath += L"\\";
    clientPath += L"cbt_client.exe";
    //mylog("Launching: %s", CONVERTER.to_bytes(clientPath).c_str());
    mylog("Launching: %s", wide_string_to_string(clientPath).c_str());
    
    wchar_t* command = _wcsdup(clientPath.c_str());
    STARTUPINFOW si = { 0 };
    si.cb = sizeof(STARTUPINFOW);
    si.dwFlags = STARTF_USESHOWWINDOW;

    // Process information
    PROCESS_INFORMATION pi = { 0 };

    mylog("SP created si pi now calling CreateProcess");

    // Create the process
    if (!CreateProcess(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        mylog("SP: CreateProcess failed; will throw");
        if (false && !initialStart) {
            mylog("CSTF checkpoint error");
            //throw CBTException("CSTF checkpoint");
            return std::make_pair(0, nullptr);
        }            DWORD errorCode = GetLastError();
        std::string errorMsg = "CBT process creation failed: error code ";
        errorMsg += std::to_string(errorCode);
        //throw CBTException(errorMsg);
        return std::make_tuple(pi.dwProcessId, pi.hProcess);
    }
    mylog("SP: CreateProcess succeeded");

    if (false && !initialStart) {
        mylog("CSTF checkpoint good");
        //throw CBTException("CSTF checkpoint");
        return std::make_pair(0, nullptr);
    }        
    free(command);
    CloseHandle(pi.hThread);
    mylog("SP: waiting for CBT window to appear....");
    for (size_t i = 0; i < 20; i++) {
        Sleep(100);
        HWND hwnd = findCBTClientWindow(arch);
        if (hwnd != nullptr) {
            mylog("SP: Success!!! hwnd=%lld, pid=%lld, hProcess=%lld", (long long)hwnd, (long long)pi.dwProcessId, (long long)pi.hProcess);
            DWORD waitResult = WaitForSingleObject(pi.hProcess, 0);
            if (waitResult == WAIT_TIMEOUT) {
                mylog("SP: process alive!");
            }
            else {
                mylog("SP: process dead!");
            }
            return std::make_tuple(pi.dwProcessId, pi.hProcess);
        }
    }
    mylog("SP: timed out waiting for CBT window to appear. Will thro.");
    std::string errorMsg = "CBT client failed to start up: CBT window didn't appear after time out of 2000 ms. arch = " + arch;
    //throw CBTException(errorMsg);
    return std::make_tuple(0, nullptr);
}

void cbtClientSpawnerThreadFunc(const std::string arch, HANDLE hProcessInitial)
{
    HANDLE hProcess = hProcessInitial;
    while (true) {
        mylog("CSTF loop");
        WaitForSingleObject(hProcess, INFINITE);
        mylog("CSTF wait finished, hProcess=%lld", (long long)hProcess);
        DWORD waitResult = WaitForSingleObject(hProcess, 0);
        if (waitResult == WAIT_TIMEOUT) {
            mylog("CSTF: process alive!");
        }
        else {
            mylog("CSTF: process dead!");
        }        CloseHandle(hProcess);
        mylog("CSTF closed handle");
        hProcess = nullptr;
        size_t restartCount;
        {
            std::lock_guard<std::mutex> guard(watchdogMtx);
            restartCount = restartCountByArch[arch] += 1;
            processIDByArch[arch] = 0;
        }
        mylog("CSTF restartCnt=%d, cbtClientTerminateSignal=%d", (int)restartCount, (int)cbtClientTerminateSignal);
        if (cbtClientTerminateSignal || (restartCount >= MAX_CBT_RESTART_COUNT)) {
            mylog("CSTF: Terminating CBT client spawner watchdog for arch %s", arch.c_str());
            return; 
        }
        std::tuple<DWORD, HANDLE> process;
        mylog("CSTF About to spawn again");
        
            try {
                process = spawnCbtClient(arch, false);
                mylog("CSTF spawn successful");
                hProcess = std::get<1>(process);
                mylog("CSTF hProcess=%lld", (long long)hProcess);
            }
            catch (const std::exception& e) {
                mylog("CSTF failed - exiting. arch=%s, restartCnt=%d, error=%s", arch, (int)restartCount, e.what());
                return;
            }
            catch (...) {
                mylog("CSTF failed - exiting. arch=%s, restartCnt=%d, error=unknown", arch, (int)restartCount);
                return;
            }
        {
            std::lock_guard<std::mutex> guard(watchdogMtx);
            processIDByArch[arch] = std::get<0>(process);
        }
    }
}

bool spawnCbtClientAndMonitor(InitRequestData& data, const std::string& arch)
{
    //std::tuple<DWORD, HANDLE> process = spawnCbtClient(arch);
    //DWORD processId = std::get<0>(process);
    DWORD processId = 0;
    data.processIds[arch] = processId;
    //HANDLE hProcess = std::get<1>(process);
    HANDLE hProcess = nullptr;
    //size_t& restartCount = restartCountByArch[arch];
    std::thread thread(cbtClientSpawnerThreadFunc, arch, hProcess);
    cbtClientMonitoringThreads[arch] = std::move(thread);
    return true;
}

DWORD killCbtClient(std::string arch)
{
    mylog("Kill %s start", arch.c_str());
    HWND hwnd = findCBTClientWindow(arch);
    if (hwnd == nullptr) {
        std::string errorMsg = "Cannot find window for arch " + arch + " to terminate cbt client for arch " + arch;
        //throw CBTException(errorMsg);
        // Actually do nothing. Sometimes we just call terminate just in case.
    }
    else {
        PostMessage(hwnd, WM_HWND_OBSERVER_DESTROY_WINDOW, 0, 0);
    }

    bool isWindowFound = true, isProcessAlive = true;
    size_t i = 0;
    for (i = 0; i < 20; i++) {
    Sleep(100);
    hwnd = findCBTClientWindow(arch);
        isWindowFound = (hwnd != nullptr);
        {
            std::lock_guard<std::mutex> guard(watchdogMtx);
            isProcessAlive = IsProcessRunning(processIDByArch[arch]);
        }
        if ((!isProcessAlive) && (!isWindowFound)) {
            break;
        }
    }

    mylog("kill status arch=%s i=%d isProcessAlive=%d isWindowFound=%d", arch.c_str(), (int)i, (int)isProcessAlive, (int)isWindowFound);
    if ((!isProcessAlive) && (!isWindowFound)){
        return 0;
    }
    mylog("asdf kill failiure!");
    return 1;
}

DWORD killCbtClientOld(std::string arch)
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
    if (!request.contains("levelDbFileName")) {
        data.error = "LevelDB cacheFileName not specified";
        return data;
    }
    std::string cacheFileName = request["levelDbFileName"];
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
        if (!spawnCbtClientAndMonitor(data, arch)) {
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
    cbtClientTerminateSignal = true;
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
        //result["error"] = "invisibleHwnd is null";
        // Don't throw since now we call terminate just in case
        //return result;
    }
    else {
        PostMessage(invisibleHwnd, WM_HWND_OBSERVER_DESTROY_WINDOW, 0, 0);
    }

    mylog("Terminate: killing windowThread");
    if (windowThread == nullptr) {
        //result["error"] = "windowThread is null";
        //return result;
        // Don't throw
    }
    else {
        mylog("Terminate: killing windowThread: calling thread::join()");
        windowThread->join();
        mylog("Terminate: killing windowThread: thread::join() done");
        windowThread = nullptr;
    }
    mylog("Terminate: terminating cache");
    std::string error = terminateCache();
    if (!error.empty()) {
        result["error"] = "Error terminating cache: " + error;
        return result;
    }
    mylog("Terminate: success");
    result["error"] = "";
    mylog("Terminate: success");
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
    UINT64 timestamp = data.timestamp;
    std::string timestampStr;
    leveldb::Status status = hwndCache->Get(readOptions, timestampKey((DWORD)hwnd), &timestampStr);
    if (status.ok() && (!status.IsNotFound())) {
        timestamp = std::stoull(timestampStr);
    }
    bool isMaximized = IsWindowMaximized(hwnd);
    data.hwnds.push_back({
        {"hwnd", (UINT32)hwnd},
        {"path", sPath},
        {"timestamp", timestamp},
        {"isMaximized", isMaximized},
    });
    return TRUE;
}

json queryHwndsImpl(json &request)
{
    //MessageBeep(0xFFFFFFFF);
    //Beep(500, 50);    
    mylog("queryHwndsImpl");
    RequestData data;
    data.timestamp = getTimestamp();
    data.onlyVisible = request["onlyVisible"];
    data.requestTitle = request["requestTitle"];    if (request.contains(REQ_PROCESS_FILTER)) {
        data.processFilter = CONVERTER.from_bytes(request[REQ_PROCESS_FILTER]);
    }
    mylog("Calling EnumWindows");
    {
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
    for (const auto& window : request["windows"]) {
        UINT32 hwnd = window["hwnd"];
        UINT64 timestamp = window["timestamp"];
        cacheTimestamp(hwnd, timestamp);
    }    
    return json({});
}

json healthCheck(json& request)
{
    std::ostringstream oss;
    auto deltaSec = std::time(nullptr) - creationTimestamp;
    oss << "dt=" << deltaSec << "sec\n";
    for (auto arch : arches) {
        oss << "[" << arch << "]\n";
        size_t restartCount;
        DWORD processID;
        {
            std::lock_guard<std::mutex> guard(watchdogMtx);
            restartCount = restartCountByArch[arch];
            processID = processIDByArch[arch];
        }
        bool windowFound = findCBTClientWindow(arch) != nullptr;
        bool threadRunning = cbtClientMonitoringThreads[arch].joinable();
        bool isProcessRunning = IsProcessRunning(processID);
        oss << "  processID=" << processID << std::endl;
        oss << "  isWindowFound=" << windowFound << std::endl;
        oss << "  isProcessRunning=" << isProcessRunning << std::endl;
        oss << "  isThreadRunning=" << threadRunning << std::endl;
        oss << "  restartCount=" << restartCount << std::endl;
    }
    std::string result = oss.str();
    json jsonResult = { {"result", result} };
    return jsonResult;
}

extern "C" __declspec(dllexport) char* queryHwnds(char* request)
{
    mylog("Request received");
    std::string requestStr(request);
    json  requestJson;
    json responseJson = {};
    try {
        requestJson = json::parse(requestStr);
        std::string command;
        if (requestJson.contains("command")) {
            command = requestJson["command"];
        }
        else {
            throw CBTException("No command field in input JSON");
        }
        mylog("command = %s", command.c_str());
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
        else if (command == "healthCheck") {
            responseJson = healthCheck(requestJson);
        }
        else {
            responseJson = { {"error", "Unknown command"} };
        }
    }
    catch (const std::exception& e) {
        mylog("Caught exception: %s", e.what());
        responseJson["error"] = e.what();
    }
    catch (...) {
        mylog("Caught unknown exception!");
        responseJson["error"] = "Unknown exception";
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
            //DWORD size = GetModuleFileNameEx((HMODULE)hModule, NULL, &wPath[0], MAX_BUFFER_SIZE); // WTF this doesn't work!?
            DWORD size = GetModuleFileName((HMODULE)hModule, &wPath[0], MAX_BUFFER_SIZE);
            wPath.resize(size);
            DWORD code = GetLastError();

            #ifdef MYDEBUG
                std::wstring path(wPath);
                std::size_t pos = path.rfind(L"\\");
                path.resize(pos + 1);
                std::wstring oldLog = path + L"observer.log.old";
                DeleteFile(oldLog.c_str());
                // Don't care if error
                std::wstring currentLog = path + L"observer.log";
                MoveFile(currentLog.c_str(), oldLog.c_str());
                logFileName = currentLog;
                // Don't care if error

                FILE* df = nullptr;
                _wfopen_s(&df, logFileName.c_str(), L"w");
                fclose(df);
            #endif
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
