#A part of  TaskSwitcher addon for NVDA
#Copyright (C) 2024 Tony Malykh
#This file is covered by the GNU General Public License.
#See the file COPYING.txt for more details.

import addonHandler
import api
import bisect
import braille
import browseMode
import collections
import config
import controlTypes
import core
import copy
import ctypes
from ctypes import create_string_buffer, byref, wintypes
import cursorManager
import documentBase
import eventHandler
import functools
import editableText
import globalPluginHandler
import gui
from gui import guiHelper, nvdaControls
from gui.settingsDialogs import SettingsPanel, SettingsDialog, BrailleDisplaySelectionDialog
import inputCore
import itertools
import json
import keyboardHandler
from logHandler import log
import NVDAHelper
from NVDAObjects import behaviors
from NVDAObjects.window import winword
from NVDAObjects.IAccessible.ia2TextMozilla import MozillaCompoundTextInfo 
from compoundDocuments import CompoundTextInfo
from NVDAObjects.window.scintilla import ScintillaTextInfo
import nvwave
import operator
import os
import re
from scriptHandler import script, willSayAllResume, isScriptWaiting
import speech
import struct
import textInfos
import threading
import time
import tones
import types
import ui
import watchdog
import wave
import winUser
import wx
import dataclasses
from dataclasses import dataclass
from appModules.devenv import VsWpfTextViewTextInfo
from NVDAObjects import behaviors
import weakref
from NVDAObjects.IAccessible import IAccessible
from NVDAObjects.window.edit import ITextDocumentTextInfo
from textInfos.offsets import OffsetsTextInfo
from NVDAObjects.window.scintilla import ScintillaTextInfo
from NVDAObjects.window.scintilla import Scintilla
from NVDAObjects.UIA import UIATextInfo
from NVDAObjects.window.edit import EditTextInfo
from typing import Optional
from typing import List
import globalVars
from ctypes import cdll, c_void_p, c_wchar_p, c_char_p
import subprocess
import NVDAObjects
import psutil
from enum import IntEnum
import shutil
import winBindings

try:
    REASON_CARET = controlTypes.REASON_CARET
except AttributeError:
    REASON_CARET = controlTypes.OutputReason.CARET


debug = False
if debug:
    import threading
    LOG_FILE_NAME = "C:\\Users\\tony\\1.txt"
    f = open(LOG_FILE_NAME, "w")
    f.close()
    LOG_MUTEX = threading.Lock()
    def mylog(s):
        with LOG_MUTEX:
            f = open(LOG_FILE_NAME, "a", encoding='utf-8')
            print(s, file=f)
            #f.write(s.encode('UTF-8'))
            #f.write('\n')
            f.close()
else:
    def mylog(*arg, **kwarg):
        pass

def myAssert(condition):
    if not condition:
        raise RuntimeError("Assertion failed")

class HWNDObserverException(RuntimeError):
    pass

class CBTState(IntEnum):
    NOT_STARTED = 0
    INITIALIZING = 1
    RUNNING = 2
    TERMINATED = 3

cbtState = CBTState.NOT_STARTED

module = "taskSwitcher"
def initConfiguration():
    confspec = {
        "observerCacheFile" : "string( default='%TMP%\\NVDATaskSwitcherObserverCache.json')",
        "levelDbCacheFile" : "string( default='%TEMP%\\NVDATaskSwitcherCache')",
        "autoMaximize" : "boolean( default=True)",
        "clickVolume" : "integer( default=50, min=0, max=100)",
    }
    config.conf.spec[module] = confspec

def getConfig(key):
    value = config.conf[module][key]
    return value

def setConfig(key, value):
    config.conf[module][key] = value
WM_SYSCOMMAND = 0x0112
SC_MAXIMIZE = 0xF030
SC_MINIMIZE = 0xF020
SC_RESTORE= 0xF120
def maximizeWindow(hwnd):
    watchdog.cancellableSendMessage(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0)


def minimizeWindow(hwnd):
    watchdog.cancellableSendMessage(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0)


def restoreWindow(hwnd):
    watchdog.cancellableSendMessage(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0)

def getTopLevelWindow(obj):
    if obj.simpleParent is None:
        return obj
    desktop = api.getDesktopObject()
    while obj.simpleParent != desktop:
        obj = obj.simpleParent
    return obj


@dataclass
class TSEntry:
    name: str
    appName: str
    appPath: Optional[str] = ""
    launchCmd: Optional[str] = ""
    keystroke: Optional[str] = None
    pattern: Optional[str] = ""
    index: int = 0

@dataclass
class TSConfig:
    entries: List[TSEntry]

class DataclassEncoder(json.JSONEncoder):
    def default(self, obj):
        if dataclasses.is_dataclass(obj):
            return dataclasses.asdict(obj)
        return super().default(obj)

class DataclassDecoder(json.JSONDecoder):
    # This shit doesn't work
    def object_hook(self, dct):
        if 'name' in dct:
            return TSEntry(**dct)
        if 'entries' in dct:
            entries = [self.object_hook(entryDct) for entryDct in dct['entries']]
            return TSConfig(entries=entries)
        return dct

def poorManDecode(dct):
    if 'name' in dct:
        return TSEntry(**dct)
    if 'entries' in dct:
        entries = [poorManDecode(entryDct) for entryDct in dct['entries']]
        return TSConfig(entries=entries)
    return dct


configFileName = os.path.join(globalVars.appArgs.configPath, "taskSwitcherConfig.json")
globalConfig = None
globalGesturesToEntries = None

def getGlobalPluginInstance():
    results = [g for g in list(globalPluginHandler.runningPlugins) if isinstance(g, GlobalPlugin)]
    if len(results) == 1:
        return results[0]
    elif len(results) == 0:
        raise RuntimeError("TaskSwitcher is not running!")
    raise RuntimeError("Woot!")

def updateKeystrokes():
    gp = getGlobalPluginInstance()
    gp._gestureMap = {
        **{
            keystroke: func
            for keystroke, func in gp._gestureMap.items()
            if func != GlobalPlugin.script_taskSwitch
        },
        **{
            inputCore.normalizeGestureIdentifier(f"kb:{entry.keystroke}"): GlobalPlugin.script_taskSwitch
            for entry in globalConfig.entries
            if entry.keystroke
        },
    }
    
    global globalGesturesToEntries
    globalGesturesToEntries = {
        inputCore.normalizeGestureIdentifier(f"kb:{entry.keystroke}").split(':')[1]: entry
        for entry in globalConfig.entries
        if entry.keystroke
    }

def updateKeystrokesWhenPluginsLoaded():
    lastException = None
    t = time.time()
    TIMEOUT_SECS = 10.0
    timeout = t + TIMEOUT_SECS
    while time.time() < timeout:
        try:
            gp = getGlobalPluginInstance()
        except RuntimeError as e:
            lastException = e
            yield 50
            continue
        updateKeystrokes()
        return
    raise RuntimeError("Failed to update gestures as plugin is still not loaded after timeout", lastException)
            

def loadConfig():
    global globalConfig
    try:
        configJsonString = "\n".join(open(configFileName, "r", encoding='utf-8').readlines())
    except OSError:
        globalConfig = TSConfig([])
        return
    if len(configJsonString) == 0:
        globalConfig = TSConfig([])
        return
    j = json.loads(configJsonString, cls=DataclassDecoder)
    #globalConfig = DataclassDecoder.object_hook(DataclassDecoder(), j)
    globalConfig = poorManDecode(j)
    #updateKeystrokes()
    executeAsynchronously(updateKeystrokesWhenPluginsLoaded())

def lazyLoadConfig():
    if globalConfig is not None:
        return
    loadConfig()

def saveConfig():
    global globalConfig
    with open(configFileName, "w", encoding='utf-8') as f:
        print(
            #json.dumps(dataclassToDict(globalConfig)),
            json.dumps(globalConfig, cls=DataclassEncoder, indent=4),
            file=f
        )

observerDll = None
def queryObserver(command, **kwargs):
    if command == 'init':
        assert cbtState == CBTState.INITIALIZING
    else:
        assert cbtState == CBTState.RUNNING, "Task switcher CBT hasn't initialized yet"
    request = {
        **{
            "command": command,
        },
        **kwargs,
    }
    result = observerDll.queryHwnds(json.dumps(request).encode('utf-8'))
    try:
        response_wchar_p = c_char_p(result)
        response_str = response_wchar_p.value
        j = json.loads(response_str.decode('utf-8'))
        if "error" in j and len(j['error']) > 0:
            error = j['error']
            raise HWNDObserverException(f"HWNDObserver error: {error}")
        return j
    finally:
        observerDll.freeBuffer(result)

def queryHwnds(appName):
    j = queryObserver("queryHwnds", process_filter=appName, onlyVisible=True, requestTitle=False)
    hwnds = j['hwnds']
    hwnds.sort(key=lambda item: (item['timestamp'], item['hwnd']))
    return hwnds

def getBootupTime():
    """
        Why the hell this function causes a com error!?
        Traceback (most recent call last):
        _ctypes.COMError: (-2147418094, 'The callee (server [not server application]) is not available and disappeared; all connections are invalid. The call did not execute.', (None, None, None, 0, None))
    """
    raise RuntimeError("Don't use this function")
    result = subprocess.run(['wmic', 'os', 'get', 'lastbootuptime'], capture_output=True, text=True)
    output = result.stdout.strip()
    return output.splitlines()[2]

def getBootupTime2():
    import psutil
    return str(psutil.boot_time())

def kill_processes(process_name):
    for proc in psutil.process_iter(['name', 'pid']):
        try:
            if proc.info['name'].lower() == process_name.lower():
                try:
                    proc.terminate()
                except psutil.NoSuchProcess:
                    pass
                except Exception as e:
                    raise e
                
                # Wait for the process to exit
                proc.wait(timeout=1)
        
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            pass

def expandWindowsVars(s, fake_env):
    # Merge real environment with fake one, uppercased for case-insensitivity
    env = {k.upper(): v for k, v in os.environ.items()}
    env.update({k.upper(): v for k, v in fake_env.items()})

    # Match Windows-style %VAR% patterns
    pattern = re.compile(r'%([^%]+)%')

    def replacer(match):
        var_name = match.group(1).upper()
        return env.get(var_name, match.group(0))  # return original if not found

    return pattern.sub(replacer, s)

def initHwndObserver():
    mylog("initHwndObserver")
    global cbtState
    assert cbtState == CBTState.NOT_STARTED
    cbtState = CBTState.INITIALIZING
    # calling taskkill causes NVDA exception for some reason
    #os.system("taskkill /f /im cbt_client.exe")
    kill_processes("cbt_client.exe")
    global observerDll
    dllPath = os.path.join(os.path.dirname(__file__), 'hwndObserver.dll')
    observerDll = cdll.LoadLibrary(dllPath)
    observerDll.queryHwnds.argtypes = [c_char_p]
    observerDll.queryHwnds.restype = c_void_p
    observerDll.freeBuffer.argtypes = [c_void_p]
    observerDll.freeBuffer.restype = None
    
    #levelDbCacheFileName = os.path.expandvars(getConfig("levelDbCacheFile"))
    #levelDbCacheFileName = levelDbCacheFileName.lower().replace('%CONFIGPATH%'.lower(), globalVars.appArgs.configPath)
    fakeEnv = {'NVDA_CONFIGPATH': globalVars.appArgs.configPath}
    levelDbCacheFileName = expandWindowsVars(getConfig("levelDbCacheFile"), fakeEnv)
    
    levelDbCacheDir = os.path.dirname(levelDbCacheFileName)
    bootupTime = getBootupTime2()
    for attempt in range(2):
        mylog(f"initHwndObserver {attempt=}")
        if not os.path.exists(levelDbCacheDir):
            os.makedirs(levelDbCacheDir)
            mylog("Created empty cache dir")
        try:
            queryObserver("init", levelDbFileName=levelDbCacheFileName, bootupTime=bootupTime)
            break
        except HWNDObserverException as e:
            mylog("Init exception {e}")
            if attempt > 0:
                raise e
            # If first attempt, then log error; try to delete the cache in case it could be corrupted, and try again
            log.exception("Exception while initializing HWND observer. Will purge cache and retry.", e)
            mylog("About to terminate")
            queryObserver("terminate")
            mylog("Terminate successful; purging cache directory {levelDbCacheDir=}")
            shutil.rmtree(levelDbCacheDir)
    mylog("Init success!")
    cbtState = CBTState.RUNNING

def initHwndObserverAsync():
    #core.callLater(10000, initHwndObserver)
    initHwndObserver()

def destroyHwndObserver():
    global cbtState
    assert cbtState == CBTState.RUNNING
    queryObserver("terminate")
    global observerDll
    observerDll = None
    cbtState = CBTState.TERMINATED

addonHandler.initTranslation()
initConfiguration()

SetActiveWindow = winBindings.user32.dll.SetActiveWindow
SetActiveWindow.argtypes = [ctypes.c_void_p]  # HWND is a void pointer
SetActiveWindow.restype = ctypes.c_bool  # Returns BOOL
if False:
    # chatGPT code
    import ctypes
    from ctypes import wintypes
    import time
    import winUser
    
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
    
    SW_RESTORE = 9
    
    GetForegroundWindow = user32.GetForegroundWindow
    GetForegroundWindow.restype = wintypes.HWND
    
    GetWindowThreadProcessId = user32.GetWindowThreadProcessId
    GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    GetWindowThreadProcessId.restype = wintypes.DWORD
    
    AttachThreadInput = user32.AttachThreadInput
    AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
    AttachThreadInput.restype = wintypes.BOOL
    
    GetCurrentThreadId = kernel32.GetCurrentThreadId
    GetCurrentThreadId.restype = wintypes.DWORD
    
    SetForegroundWindow = user32.SetForegroundWindow
    SetForegroundWindow.argtypes = [wintypes.HWND]
    SetForegroundWindow.restype = wintypes.BOOL
    
    SetActiveWindow = user32.SetActiveWindow
    SetActiveWindow.argtypes = [wintypes.HWND]
    SetActiveWindow.restype = wintypes.HWND
    
    SetFocus = user32.SetFocus
    SetFocus.argtypes = [wintypes.HWND]
    SetFocus.restype = wintypes.HWND
    
    BringWindowToTop = user32.BringWindowToTop
    BringWindowToTop.argtypes = [wintypes.HWND]
    BringWindowToTop.restype = wintypes.BOOL
    
    IsIconic = user32.IsIconic
    IsIconic.argtypes = [wintypes.HWND]
    IsIconic.restype = wintypes.BOOL
    
    ShowWindow = user32.ShowWindow
    ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    ShowWindow.restype = wintypes.BOOL
    
    
    def _activate_once(hwnd: int) -> bool:
        # Restore if minimized (Chrome often needs this to generate proper activation/focus transitions)
        if IsIconic(hwnd):
            ShowWindow(hwnd, SW_RESTORE)
    
        # Identify threads
        _pid = wintypes.DWORD()
        target_tid = GetWindowThreadProcessId(wintypes.HWND(hwnd), ctypes.byref(_pid))
        self_tid = GetCurrentThreadId()
    
        attached = False
        try:
            if target_tid and target_tid != self_tid:
                attached = bool(AttachThreadInput(self_tid, target_tid, True))
    
            # Order matters less once attached, but keep it conventional:
            BringWindowToTop(hwnd)
            winUser.setForegroundWindow(hwnd)  # NVDA helper wraps SetForegroundWindow-ish behavior
            SetForegroundWindow(hwnd)
            SetActiveWindow(hwnd)
            winUser.setFocus(hwnd)             # NVDA helper
            SetFocus(hwnd)
    
        finally:
            if attached:
                AttachThreadInput(self_tid, target_tid, False)
    
        return GetForegroundWindow() == hwnd
    
    
    def activateWindowReliable(hwnd: int, retries: int = 1) -> bool:
        if _activate_once(hwnd):
            return True
        for _ in range(retries):
            time.sleep(0.02)  # 20 ms: enough for message pump / activation chain
            if _activate_once(hwnd):
                return True
        return False

if False:
    # ChatGpt 2
    import ctypes
    from ctypes import wintypes
    
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
    
    SW_RESTORE = 9
    GA_ROOT = 2
    
    GetAncestor = user32.GetAncestor
    GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
    GetAncestor.restype = wintypes.HWND
    
    GetForegroundWindow = user32.GetForegroundWindow
    GetForegroundWindow.restype = wintypes.HWND
    
    GetWindowThreadProcessId = user32.GetWindowThreadProcessId
    GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    GetWindowThreadProcessId.restype = wintypes.DWORD
    
    GetCurrentThreadId = kernel32.GetCurrentThreadId
    GetCurrentThreadId.restype = wintypes.DWORD
    
    AttachThreadInput = user32.AttachThreadInput
    AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
    AttachThreadInput.restype = wintypes.BOOL
    
    IsIconic = user32.IsIconic
    IsIconic.argtypes = [wintypes.HWND]
    IsIconic.restype = wintypes.BOOL
    
    ShowWindow = user32.ShowWindow
    ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    ShowWindow.restype = wintypes.BOOL
    
    BringWindowToTop = user32.BringWindowToTop
    BringWindowToTop.argtypes = [wintypes.HWND]
    BringWindowToTop.restype = wintypes.BOOL
    
    SetForegroundWindow = user32.SetForegroundWindow
    SetForegroundWindow.argtypes = [wintypes.HWND]
    SetForegroundWindow.restype = wintypes.BOOL
    
    SetActiveWindow = user32.SetActiveWindow
    SetActiveWindow.argtypes = [wintypes.HWND]
    SetActiveWindow.restype = wintypes.HWND
    
    SetFocus = user32.SetFocus
    SetFocus.argtypes = [wintypes.HWND]
    SetFocus.restype = wintypes.HWND
    
    
    def activateWindowBetter(hwnd: int) -> bool:
        hwnd = int(GetAncestor(wintypes.HWND(hwnd), GA_ROOT)) or hwnd
    
        if IsIconic(hwnd):
            ShowWindow(hwnd, SW_RESTORE)
    
        fg = int(GetForegroundWindow()) or 0
    
        _pid = wintypes.DWORD()
        target_tid = int(GetWindowThreadProcessId(wintypes.HWND(hwnd), ctypes.byref(_pid))) or 0
        fg_tid = int(GetWindowThreadProcessId(wintypes.HWND(fg), ctypes.byref(wintypes.DWORD()))) if fg else 0
        self_tid = int(GetCurrentThreadId())
    
        attached_to_fg = False
        attached_to_target = False
        try:
            if fg_tid and fg_tid != self_tid:
                attached_to_fg = bool(AttachThreadInput(self_tid, fg_tid, True))
            if target_tid and target_tid != self_tid:
                attached_to_target = bool(AttachThreadInput(self_tid, target_tid, True))
    
            BringWindowToTop(hwnd)
            SetForegroundWindow(hwnd)
            SetActiveWindow(hwnd)
            SetFocus(hwnd)
    
        finally:
            if attached_to_target:
                AttachThreadInput(self_tid, target_tid, False)
            if attached_to_fg:
                AttachThreadInput(self_tid, fg_tid, False)
    
        return int(GetForegroundWindow()) == hwnd
    
    from ctypes import Structure, sizeof
    import ctypes
    from ctypes import wintypes
    import eventHandler
    import NVDAObjects
    import winUser
    
    class GUITHREADINFO(Structure):
        _fields_ = [
            ("cbSize", wintypes.DWORD),
            ("flags", wintypes.DWORD),
            ("hwndActive", wintypes.HWND),
            ("hwndFocus", wintypes.HWND),
            ("hwndCapture", wintypes.HWND),
            ("hwndMenuOwner", wintypes.HWND),
            ("hwndMoveSize", wintypes.HWND),
            ("hwndCaret", wintypes.HWND),
            ("rcCaret", wintypes.RECT),
        ]
    
    GetGUIThreadInfo = user32.GetGUIThreadInfo
    GetGUIThreadInfo.argtypes = [wintypes.DWORD, ctypes.POINTER(GUITHREADINFO)]
    GetGUIThreadInfo.restype = wintypes.BOOL

    def nvdaRefreshFocusFromOS():
        gti = GUITHREADINFO()
        gti.cbSize = sizeof(GUITHREADINFO)
        if not GetGUIThreadInfo(0, ctypes.byref(gti)):
            return
    
        focusHwnd = int(gti.hwndFocus or 0)
        activeHwnd = int(gti.hwndActive or 0)
        hwnd = focusHwnd or activeHwnd
        if not hwnd:
            return
    
        try:
            obj = NVDAObjects.IAccessible.getNVDAObjectFromEvent(hwnd, winUser.OBJID_CLIENT, 0)
        except Exception:
            return
    
        eventHandler.queueEvent("gainFocus", obj)
if True:
    # ChatGPT take 3
    import ctypes
    from ctypes import wintypes, Structure, sizeof
    
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
    
    SW_RESTORE = 9
    GA_ROOT = 2
    
    GetAncestor = user32.GetAncestor
    GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
    GetAncestor.restype = wintypes.HWND
    
    GetForegroundWindow = user32.GetForegroundWindow
    GetForegroundWindow.restype = wintypes.HWND
    
    GetWindowThreadProcessId = user32.GetWindowThreadProcessId
    GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    GetWindowThreadProcessId.restype = wintypes.DWORD
    
    GetCurrentThreadId = kernel32.GetCurrentThreadId
    GetCurrentThreadId.restype = wintypes.DWORD
    
    AttachThreadInput = user32.AttachThreadInput
    AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
    AttachThreadInput.restype = wintypes.BOOL
    
    IsIconic = user32.IsIconic
    IsIconic.argtypes = [wintypes.HWND]
    IsIconic.restype = wintypes.BOOL
    
    ShowWindow = user32.ShowWindow
    ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    ShowWindow.restype = wintypes.BOOL
    
    BringWindowToTop = user32.BringWindowToTop
    BringWindowToTop.argtypes = [wintypes.HWND]
    BringWindowToTop.restype = wintypes.BOOL
    
    SetForegroundWindow = user32.SetForegroundWindow
    SetForegroundWindow.argtypes = [wintypes.HWND]
    SetForegroundWindow.restype = wintypes.BOOL
    
    SetActiveWindow = user32.SetActiveWindow
    SetActiveWindow.argtypes = [wintypes.HWND]
    SetActiveWindow.restype = wintypes.HWND
    
    SetFocus = user32.SetFocus
    SetFocus.argtypes = [wintypes.HWND]
    SetFocus.restype = wintypes.HWND
    
    FindWindowExW = user32.FindWindowExW
    FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
    FindWindowExW.restype = wintypes.HWND

    GetClassNameW = user32.GetClassNameW
    GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    GetClassNameW.restype = ctypes.c_int

    EnumWindowsProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    EnumWindows = user32.EnumWindows
    EnumWindows.argtypes = [EnumWindowsProc, wintypes.LPARAM]
    EnumWindows.restype = wintypes.BOOL

    IsWindowVisible = user32.IsWindowVisible
    IsWindowVisible.argtypes = [wintypes.HWND]
    IsWindowVisible.restype = wintypes.BOOL

    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    OpenProcess = kernel32.OpenProcess
    OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    OpenProcess.restype = wintypes.HANDLE

    CloseHandle = kernel32.CloseHandle
    CloseHandle.argtypes = [wintypes.HANDLE]
    CloseHandle.restype = wintypes.BOOL

    QueryFullProcessImageNameW = kernel32.QueryFullProcessImageNameW
    QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE,
        wintypes.DWORD,
        wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD),
    ]
    QueryFullProcessImageNameW.restype = wintypes.BOOL
    
    
    class GUITHREADINFO(Structure):
        _fields_ = [
            ("cbSize", wintypes.DWORD),
            ("flags", wintypes.DWORD),
            ("hwndActive", wintypes.HWND),
            ("hwndFocus", wintypes.HWND),
            ("hwndCapture", wintypes.HWND),
            ("hwndMenuOwner", wintypes.HWND),
            ("hwndMoveSize", wintypes.HWND),
            ("hwndCaret", wintypes.HWND),
            ("rcCaret", wintypes.RECT),
        ]
    
    GetGUIThreadInfo = user32.GetGUIThreadInfo
    GetGUIThreadInfo.argtypes = [wintypes.DWORD, ctypes.POINTER(GUITHREADINFO)]
    GetGUIThreadInfo.restype = wintypes.BOOL
    
    
    def _find_scintilla_child(root_hwnd: int) -> int:
        # Notepad++ editor is Scintilla; focus there if OS focus is stuck elsewhere.
        h = FindWindowExW(wintypes.HWND(root_hwnd), wintypes.HWND(0), "Scintilla", None)
        return int(h or 0)

    def _get_class_name(hwnd: int) -> str:
        buf = ctypes.create_unicode_buffer(256)
        if GetClassNameW(wintypes.HWND(hwnd), buf, 256) == 0:
            return ""
        return buf.value

    def _get_process_name_for_hwnd(hwnd: int) -> Optional[str]:
        pid = wintypes.DWORD()
        GetWindowThreadProcessId(wintypes.HWND(hwnd), ctypes.byref(pid))
        if not pid.value:
            return None
        hproc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid.value)
        if not hproc:
            return None
        try:
            buf_len = wintypes.DWORD(260)
            buf = ctypes.create_unicode_buffer(buf_len.value)
            if QueryFullProcessImageNameW(hproc, 0, buf, ctypes.byref(buf_len)):
                return os.path.basename(buf.value).lower()
        finally:
            CloseHandle(hproc)
        return None

    def _detect_start_menu_window():
        found = {"hwnd": 0, "class": "", "proc": ""}
        start_menu_procs = {
            "startmenuexperiencehost.exe",
            "searchhost.exe",
            "shellexperiencehost.exe",
        }
        start_menu_classes = {"Windows.UI.Core.CoreWindow"}

        def _cb(hwnd, _lparam):
            if not IsWindowVisible(wintypes.HWND(hwnd)):
                return True
            cls = _get_class_name(int(hwnd))
            if cls not in start_menu_classes:
                return True
            proc = _get_process_name_for_hwnd(int(hwnd))
            if proc and proc in start_menu_procs:
                found["hwnd"] = int(hwnd)
                found["class"] = cls
                found["proc"] = proc
                return False
            return True

        EnumWindows(EnumWindowsProc(_cb), 0)
        return found

    def _log_activation_failure_context(hwnd: int) -> None:
        fg = int(GetForegroundWindow() or 0)
        fg_class = _get_class_name(fg) if fg else ""
        fg_proc = _get_process_name_for_hwnd(fg) if fg else None
        start_menu = _detect_start_menu_window()
        log.info(
            "activateWindowBetter failed: target=%s fg=%s fgClass=%s fgProc=%s startMenu=%s startMenuClass=%s startMenuProc=%s",
            hwnd,
            fg,
            fg_class,
            fg_proc,
            start_menu["hwnd"],
            start_menu["class"],
            start_menu["proc"],
        )

    def maybeCloseStartMenu():
        start_menu = _detect_start_menu_window()
        hwnd = start_menu["hwnd"]
        className = start_menu["class"]
        appName = start_menu["proc"]
        if not hwnd:
            hwnd = int(GetForegroundWindow() or 0)
            if not hwnd:
                return
            className = _get_class_name(hwnd)
            appName = _get_process_name_for_hwnd(hwnd) or ""
        START_MENU_CLASS_NAMES = [
            "Windows.UI.Core.CoreWindow",
        ]
        START_MENU_PROCS = {
            "searchhost.exe",
            "startmenuexperiencehost.exe",
            "shellexperiencehost.exe",
        }
        if appName in START_MENU_PROCS and className in START_MENU_CLASS_NAMES:
            WM_CLOSE = 0x0010
            watchdog.cancellableSendMessage(hwnd, WM_CLOSE, 0, 0)
            currentTime = time.time()
            MAX_DELAY = 0.5
            endTime = currentTime + MAX_DELAY
            while time.time() < endTime:
                time.sleep(0.1)
                if int(GetForegroundWindow() or 0) != hwnd:
                    return
            raise Exception(f"Failed to close start menu after timeout. {appName=} {className=}")

    def _activate_window_impl(hwnd: int, forceChildFocus: bool = False) -> bool:
        hwnd = int(GetAncestor(wintypes.HWND(hwnd), GA_ROOT)) or hwnd

        if IsIconic(wintypes.HWND(hwnd)):
            ShowWindow(wintypes.HWND(hwnd), SW_RESTORE)

        fg = int(GetForegroundWindow() or 0)

        _pid = wintypes.DWORD()
        target_tid = int(GetWindowThreadProcessId(wintypes.HWND(hwnd), ctypes.byref(_pid)) or 0)

        _fg_pid = wintypes.DWORD()
        fg_tid = int(GetWindowThreadProcessId(wintypes.HWND(fg), ctypes.byref(_fg_pid)) or 0) if fg else 0

        self_tid = int(GetCurrentThreadId())

        attached_to_fg = False
        attached_to_target = False
        try:
            if fg_tid and fg_tid != self_tid:
                attached_to_fg = bool(AttachThreadInput(self_tid, fg_tid, True))
            if target_tid and target_tid != self_tid:
                attached_to_target = bool(AttachThreadInput(self_tid, target_tid, True))

            BringWindowToTop(wintypes.HWND(hwnd))
            SetForegroundWindow(wintypes.HWND(hwnd))
            SetActiveWindow(wintypes.HWND(hwnd))

            if forceChildFocus:
                # Notepad++ fix: push focus into editor, not the frame/tab bar.
                gti = GUITHREADINFO()
                gti.cbSize = sizeof(GUITHREADINFO)
                focus_hwnd = 0
                if target_tid and GetGUIThreadInfo(wintypes.DWORD(target_tid), ctypes.byref(gti)):
                    focus_hwnd = int(gti.hwndFocus or 0) or int(gti.hwndActive or 0)

                if focus_hwnd and focus_hwnd != hwnd:
                    SetFocus(wintypes.HWND(focus_hwnd))
                else:
                    scint = _find_scintilla_child(hwnd)
                    if scint:
                        SetFocus(wintypes.HWND(scint))

        finally:
            if attached_to_target:
                AttachThreadInput(self_tid, target_tid, False)
            if attached_to_fg:
                AttachThreadInput(self_tid, fg_tid, False)

        return int(GetForegroundWindow() or 0) == hwnd
    
    def activateWindowBetter(hwnd: int, forceChildFocus: bool = False) -> bool:
        done = threading.Event()
        result = {"ok": False}

        def _run():
            try:
                result["ok"] = _activate_window_impl(hwnd, forceChildFocus)
            except Exception:
                log.exception("activateWindowBetter worker failed for hwnd %s", hwnd)
            finally:
                done.set()

        threading.Thread(
            target=_run,
            name="TaskSwitcher-ActivateWindow",
            daemon=True,
        ).start()
        if not done.wait(1.0):
            log.warning("activateWindowBetter timed out after 1.0s for hwnd %s", hwnd)
            return False
        return result["ok"]

    def nvdaRefreshFocusFromOS():
        try:
            # NVDA chooses the correct backend (UIA / IA2 / IAccessible).
            obj = api.getDesktopObject().objectWithFocus()
        except Exception:
            return
        if not obj:
            return
        eventHandler.queueEvent("gainFocus", obj)
    
    



class SettingsDialog(SettingsPanel):
    # Translators: Title for the settings dialog
    title = _("Task Switcher")

    def makeSettings(self, settingsSizer):
        sHelper = gui.guiHelper.BoxSizerHelper(self, sizer=settingsSizer)
      # checkbox auto maximize
        label = _("Automatically maximize target window")
        self.AutoMaxCheckbox = sHelper.addItem(wx.CheckBox(self, label=label))
        self.AutoMaxCheckbox.Value = getConfig("autoMaximize")
      # click volume slider
        label = _("Volume of click")
        self.clickVolumeSlider = sHelper.addLabeledControl(label, wx.Slider, minValue=0,maxValue=100)
        self.clickVolumeSlider.SetValue(getConfig("clickVolume"))
      # Edit cache file
        #self.cacheFileEdit = sHelper.addLabeledControl(_("Cache file location (requires restart)"), wx.TextCtrl)
        #self.cacheFileEdit.Value = getConfig("observerCacheFile")
      # Edit LevelDB cache file
        self.levelDbCacheFileEdit = sHelper.addLabeledControl(_("Cache file location (requires restart)"), wx.TextCtrl)
        self.levelDbCacheFileEdit.Value = getConfig("levelDbCacheFile")


    def onSave(self):
        setConfig("autoMaximize", self.AutoMaxCheckbox.Value)
        setConfig("clickVolume", self.clickVolumeSlider.Value)
        #setConfig("observerCacheFile", self.cacheFileEdit.Value)
        setConfig("levelDbCacheFile", self.levelDbCacheFileEdit.Value)


class Beeper:
    BASE_FREQ = speech.IDT_BASE_FREQUENCY
    def getPitch(self, indent):
        return self.BASE_FREQ*2**(indent/24.0) #24 quarter tones per octave.

    BEEP_LEN = 10 # millis
    PAUSE_LEN = 5 # millis
    MAX_CRACKLE_LEN = 400 # millis
    MAX_BEEP_COUNT = MAX_CRACKLE_LEN // (BEEP_LEN + PAUSE_LEN)

    def __init__(self):
        try:
            outputDevice=config.conf["speech"]["outputDevice"]
        except KeyError:
            outputDevice=config.conf["audio"]["outputDevice"]
        self.player = nvwave.WavePlayer(
            channels=2,
            samplesPerSec=int(tones.SAMPLE_RATE),
            bitsPerSample=16,
            outputDevice=outputDevice,
            wantDucking=False,
            purpose=nvwave.AudioPurpose.SOUNDS,
        )



    def fancyCrackle(self, levels, volume):
        levels = self.uniformSample(levels, self.MAX_BEEP_COUNT )
        beepLen = self.BEEP_LEN
        pauseLen = self.PAUSE_LEN
        pauseBufSize = NVDAHelper.generateBeep(None,self.BASE_FREQ,pauseLen,0, 0)
        beepBufSizes = [NVDAHelper.generateBeep(None,self.getPitch(l), beepLen, volume, volume) for l in levels]
        bufSize = sum(beepBufSizes) + len(levels) * pauseBufSize
        buf = ctypes.create_string_buffer(bufSize)
        bufPtr = 0
        for l in levels:
            bufPtr += NVDAHelper.generateBeep(
                ctypes.cast(ctypes.byref(buf, bufPtr), ctypes.POINTER(ctypes.c_char)),
                self.getPitch(l), beepLen, volume, volume)
            bufPtr += pauseBufSize # add a short pause
        self.player.stop()
        self.player.feed(buf.raw)

    def simpleCrackle(self, n, volume):
        return self.fancyCrackle([0] * n, volume)


    NOTES = "A,B,H,C,C#,D,D#,E,F,F#,G,G#".split(",")
    NOTE_RE = re.compile("[A-H][#]?")
    BASE_FREQ = 220
    def getChordFrequencies(self, chord):
        myAssert(len(self.NOTES) == 12)
        prev = -1
        result = []
        for m in self.NOTE_RE.finditer(chord):
            s = m.group()
            i =self.NOTES.index(s)
            while i < prev:
                i += 12
            result.append(int(self.BASE_FREQ * (2 ** (i / 12.0))))
            prev = i
        return result

    def fancyBeep(self, chord, length, left=10, right=10):
        beepLen = length
        freqs = self.getChordFrequencies(chord)
        intSize = 8 # bytes
        bufSize = max([NVDAHelper.generateBeep(None,freq, beepLen, right, left) for freq in freqs])
        if bufSize % intSize != 0:
            bufSize += intSize
            bufSize -= (bufSize % intSize)
        self.player.stop()
        bbs = []
        result = [0] * (bufSize//intSize)
        for freq in freqs:
            buf = ctypes.create_string_buffer(bufSize)
            NVDAHelper.generateBeep(buf, freq, beepLen, right, left)
            bytes = bytearray(buf)
            unpacked = struct.unpack("<%dQ" % (bufSize // intSize), bytes)
            result = map(operator.add, result, unpacked)
        maxInt = 1 << (8 * intSize)
        result = map(lambda x : x %maxInt, result)
        packed = struct.pack("<%dQ" % (bufSize // intSize), *result)
        self.player.feed(packed)

    def uniformSample(self, a, m):
        n = len(a)
        if n <= m:
            return a
        # Here assume n > m
        result = []
        for i in range(0, m*n, n):
            result.append(a[i  // m])
        return result
    def stop(self):
        self.player.stop()


def executeAsynchronously(gen):
    """
    This function executes a generator-function in such a manner, that allows updates from the operating system to be processed during execution.
    For an example of such generator function, please see GlobalPlugin.script_editJupyter.
    Specifically, every time the generator function yilds a positive number,, the rest of the generator function will be executed
    from within wx.CallLater() call.
    If generator function yields a value of 0, then the rest of the generator function
    will be executed from within wx.CallAfter() call.
    This allows clear and simple expression of the logic inside the generator function, while still allowing NVDA to process update events from the operating system.
    Essentially the generator function will be paused every time it calls yield, then the updates will be processed by NVDA and then the remainder of generator function will continue executing.
    """
    if not isinstance(gen, types.GeneratorType):
        raise Exception("Generator function required")
    try:
        value = gen.__next__()
    except StopIteration:
        return
    l = lambda gen=gen: executeAsynchronously(gen)
    if value == 0:
        wx.CallAfter(l)
    else:
        wx.CallLater(value, l)

def getKeystrokeFromGesture(gesture):
    keystroke = gesture.normalizedIdentifiers[-1].split(':')[1]
    return keystroke

class EditEntryDialog(wx.Dialog):
    def __init__(self, parent, entry, index=None, config=None):
        title=_("Edit Task Switcher entry")
        super().__init__(parent,title=title)
        self.entry = entry
        self.index = index
        self.config = config or globalConfig
        mainSizer=wx.BoxSizer(wx.VERTICAL)
        sHelper = guiHelper.BoxSizerHelper(self, orientation=wx.VERTICAL)
        self.keystroke = self.entry.keystroke
      # name Edit box
        label = _("&Name:")
        self.nameTextCtrl=sHelper.addLabeledControl(label, wx.TextCtrl)
        self.nameTextCtrl.SetValue(self.entry.name)
      # appName Edit box
        label = _("&Application name (executable file name without .exe extension): (required):")
        self.appNameTextCtrl=sHelper.addLabeledControl(label, wx.TextCtrl)
        self.appNameTextCtrl.SetValue(self.entry.appName)
      # Keystroke button
        self.customeKeystrokeButton = sHelper.addItem (wx.Button (self, label = _("&Keystroke")))
        self.customeKeystrokeButton.Bind(wx.EVT_BUTTON, self.OnCustomKeystrokeClick)
        self.updateCustomKeystrokeButtonLabel()
      # appPath Edit box
        label = _("&Application full path to executable (optional)")
        self.appPathTextCtrl=sHelper.addLabeledControl(label, wx.TextCtrl)
        self.appPathTextCtrl.SetValue(self.entry.appPath)
      # LaunchCMD editable
        label = _("&Launch command (optional)")
        self.launchCmdTextCtrl=sHelper.addLabeledControl(label, wx.TextCtrl)
        self.launchCmdTextCtrl.SetValue(self.entry.launchCmd)
      # Translators: Window Title pattern
        label = _("Window &title regex pattern (optional):")
        self.patternTextCtrl=sHelper.addLabeledControl(label, wx.TextCtrl)
        self.patternTextCtrl.SetValue(self.entry.pattern)
      # Index spinCtrl
        label = _("Index of selected window (or set to zero to cycle through all available windows):")
        self.indexEdit = sHelper.addLabeledControl(
            label,
            nvdaControls.SelectOnFocusSpinCtrl,
            min=0,
            max=10,
            initial=self.entry.index,
        )
      #  OK/cancel buttons
        sHelper.addDialogDismissButtons(self.CreateButtonSizer(wx.OK|wx.CANCEL))

        mainSizer.Add(sHelper.sizer,border=20,flag=wx.ALL)
        mainSizer.Fit(self)
        self.SetSizer(mainSizer)
        self.nameTextCtrl.SetFocus()
        self.Bind(wx.EVT_BUTTON,self.onOk,id=wx.ID_OK)

    def make(self):
        pattern = self.patternTextCtrl.Value
        pattern = pattern.rstrip("\r\n")
        errorMsg = None
        try:
            re.compile(pattern)
        except re.error as e:
            errorMsg = _('Failed to compile regular expression: %s') % str(e)

        if errorMsg is not None:
            # Translators: This is an error message to let the user know that the pattern field is not valid.
            gui.messageBox(errorMsg, _("Task Switcher entry error"), wx.OK|wx.ICON_WARNING, self)
            self.patternTextCtrl.SetFocus()
            return

        name = self.nameTextCtrl.Value
        appName = self.appNameTextCtrl.Value
        keystroke = self.keystroke
        if name in [e.name for i,e in enumerate(self.config.entries) if i != self.index]:
            errorMsg = _("Error: this name is already used for another entry. Please specify a unique name.")
            gui.messageBox(errorMsg, _("Task Switcher entry error"), wx.OK|wx.ICON_WARNING, self)
            self.nameTextCtrl.SetFocus()
            return
        if keystroke and keystroke in [e.keystroke for i,e in enumerate(self.config.entries) if i != self.index]:
            errorMsg = _("Error: this keystroke is already used for another entry. Please specify a unique keystroke.")
            gui.messageBox(errorMsg, _("Task Switcher entry error"), wx.OK|wx.ICON_WARNING, self)
            self.customeKeystrokeButton.SetFocus()
            return

        if len(appName) == 0:
            errorMsg = _("Error: application name must be specified.")
            gui.messageBox(errorMsg, _("Task Switcher entry error"), wx.OK|wx.ICON_WARNING, self)
            self.appNameTextCtrl.SetFocus()
            return


        entry = TSEntry(
            name=name,
            pattern= pattern,
            keystroke= keystroke,
            appName=appName,
            appPath=self.appPathTextCtrl.Value,
            launchCmd=self.launchCmdTextCtrl.Value,
            index=self.indexEdit .Value,
        )
        return entry

    def updateCustomKeystrokeButtonLabel(self):
        keystroke = self.keystroke
        if keystroke:
            self.customeKeystrokeButton.SetLabel(_("&Keystroke: %s") % (keystroke))
        else:
            self.customeKeystrokeButton.SetLabel(_("&Keystroke: %s") % "None")

    def OnCustomKeystrokeClick(self,evt):
        if inputCore.manager._captureFunc:
            # don't add while already in process of adding.
            return
        def addGestureCaptor(gesture: inputCore.InputGesture):
            if gesture.isModifier:
                return False
            inputCore.manager._captureFunc = None
            wx.CallAfter(self._addCaptured, gesture)
            return False
        inputCore.manager._captureFunc = addGestureCaptor
        core.callLater(50, ui.message, _("Press desired keystroke now"))

    blackListedKeystrokes = "escape enter numpadenter space nvda+space nvda+n nvda+q nvda+j j tab uparrow downarrow leftarrow rightarrow home end control+home control+end delete".split()

    def _addCaptured(self, gesture):
        g = getKeystrokeFromGesture(gesture)
        if g in ["escape", "delete"]:
            self.keystroke = None
            msg = _("Keystroke deleted and entry is disable until you select another keystroke")
        elif g  in self.blackListedKeystrokes:
            msg = _("Invalid keystroke %s: cannot overload essential  NVDA keystrokes!") % g
        else:
            self.keystroke = g
            msg = None
        if msg:
            core.callLater(50, ui.message, msg)
        self.updateCustomKeystrokeButtonLabel()

    def onOk(self,evt):
        entry = self.make()
        if entry is not None:
            self.entry = entry
            evt.Skip()


class SettingsEntriesDialog(SettingsDialog):
    title = _("TaskSwitcher entries")

    #def __init__(self, *args, **kwargs):
        #super().__init__(*args, **kwargs)

    def makeSettings(self, settingsSizer):
        global globalConfig
        self.config = copy.deepcopy(globalConfig)

        sHelper = gui.guiHelper.BoxSizerHelper(self, sizer=settingsSizer)
      # Entries table
        label = _("&Entries")
        self.entriesList = sHelper.addLabeledControl(
            label,
            nvdaControls.AutoWidthColumnListCtrl,
            autoSizeColumn=3,
            itemTextCallable=self.getItemTextForList,
            style=wx.LC_REPORT | wx.LC_SINGLE_SEL | wx.LC_VIRTUAL
        )

        self.entriesList.InsertColumn(0, _("Name"), width=self.scaleSize(150))
        self.entriesList.InsertColumn(1, _("Keystroke"))
        self.entriesList.InsertColumn(2, _("AppName"))
        self.entriesList.InsertColumn(3, _("AppPath"))
        self.entriesList.InsertColumn(4, _("WindowRegex"))
        self.entriesList.InsertColumn(5, _("Index"))
        self.entriesList.Bind(wx.EVT_LIST_ITEM_FOCUSED, self.onListItemFocused)
        self.entriesList.ItemCount = len(self.config.entries)

        bHelper = sHelper.addItem(guiHelper.ButtonHelper(orientation=wx.HORIZONTAL))
      # Buttons
        self.addButton = bHelper.addButton(self, label=_("&Add"))
        self.addButton.Bind(wx.EVT_BUTTON, self.OnAddClick)
        self.editButton = bHelper.addButton(self, label=_("&Edit"))
        self.editButton.Bind(wx.EVT_BUTTON, self.OnEditClick)
        self.removeButton = bHelper.addButton(self, label=_("&Remove"))
        self.removeButton.Bind(wx.EVT_BUTTON, self.OnRemoveClick)
        #self.moveUpButton = bHelper.addButton(self, label=_("Move &up"))
        #self.moveUpButton.Bind(wx.EVT_BUTTON, lambda evt: self.OnMoveClick(evt, -1))
        #self.moveDownButton = bHelper.addButton(self, label=_("Move &down"))
        #self.moveDownButton.Bind(wx.EVT_BUTTON, lambda evt: self.OnMoveClick(evt, 1))
        #self.sortButton = bHelper.addButton(self, label=_("&Sort"))
        #self.sortButton.Bind(wx.EVT_BUTTON, self.OnSortClick)

    def postInit(self):
        self.sitesList.SetFocus()

    def getItemTextForList(self, item, column):
        entry = self.config.entries[item]
        if column == 0:
            return entry.name
        elif column == 1:
            return entry.keystroke or "None"
        elif column == 2:
            return entry.appName
        elif column == 3:
            return entry.appPath
        elif column == 4:
            return entry.pattern
        elif column == 5:
            return str(entry.index)
        else:
            raise ValueError("Unknown column: %d" % column)

    def onListItemFocused(self, evt):
        if self.entriesList.GetSelectedItemCount()!=1:
            return
        index=self.entriesList.GetFirstSelected()
        entry = self.config.entries[index]

    def OnAddClick(self,evt):
        errorMsg = _("In order to add new entry, please switch to the desired application, open Task Swittcher menu by pressing NVDA+control+f12 and select 'create a new entry'")
        gui.messageBox(errorMsg, _("Bookmark Error"), wx.OK|wx.ICON_WARNING, self),

    def OnEditClick(self,evt):
        if self.entriesList.GetSelectedItemCount()!=1:
            return
        editIndex=self.entriesList.GetFirstSelected()
        if editIndex<0:
            return
        entry = self.config.entries[editIndex]
        dialog = EditEntryDialog(parent=self, entry=entry, index=editIndex, config=self.config)
        if dialog.ShowModal()==wx.ID_OK:
            self.config.entries[editIndex] = dialog.entry
            self.OnSortClick(None)
            self.entriesList.SetFocus()

    def OnRemoveClick(self,evt):
        entries = list(self.config.entries)
        index=self.entriesList.GetFirstSelected()
        while index>=0:
            self.entriesList.DeleteItem(index)
            del entries[index]
            index=self.entriesList.GetNextSelected(index)
        self.config .entries = entries
        self.entriesList.SetFocus()

    def OnSortClick(self,evt):
        self.config.entries.sort(key=lambda e:e.name)

    def onSave(self):
        global globalConfig
        globalConfig = self.config
        saveConfig()
        loadConfig()

def openEntryDialog(focus=None, entry=None):
    global globalConfig
    originalEntry = entry
    entryIndex = globalConfig.entries.index(entry) if entry is not None else None
    if focus is not None:
        appName = focus.appModule.appName
        appPath = focus.appModule.appPath
        entry = TSEntry(
            name=appName,
            appName=appName,
            appPath=appPath,
            launchCmd=f'"{focus.appModule.appPath}"',
            index=0,
        )
    dialog = EditEntryDialog(parent=None, entry=entry, index=entryIndex)
    if dialog.ShowModal()==wx.ID_OK:
        if entryIndex is not None:
            globalConfig.entries[entryIndex] = dialog.entry
        else:
            globalConfig.entries.append(dialog.entry)
        globalConfig.entries.sort(key=lambda e:e.name)
        saveConfig()
        loadConfig()

class ReorderWindowsDialog(
    gui.dpiScalingHelper.DpiScalingHelperMixinWithoutInit,
    wx.Dialog,
):
    def __init__(self, parent, appName):
        title=_("Rearrange windows for %s") % appName
        super().__init__(parent,title=title)
        self.appName = appName
        self.hwnds = fetchTitles(queryHwnds(appName))
        mainSizer=wx.BoxSizer(wx.VERTICAL)
        sHelper = guiHelper.BoxSizerHelper(self, orientation=wx.VERTICAL)
      # windows table
        label = _("&Windows")
        self.windowsList = sHelper.addLabeledControl(
            label,
            nvdaControls.AutoWidthColumnListCtrl,
            autoSizeColumn=2,
            itemTextCallable=self.getItemTextForList,
            style=wx.LC_REPORT | wx.LC_SINGLE_SEL | wx.LC_VIRTUAL
        )
        self.windowsList.InsertColumn(0, _("Title"), width=self.scaleSize(150))
        self.windowsList.InsertColumn(0, _("Timestamp"))
        self.windowsList.Bind(wx.EVT_LIST_ITEM_FOCUSED, self.onListItemFocused)
        self.windowsList.ItemCount = len(self.hwnds)

        bHelper = sHelper.addItem(guiHelper.ButtonHelper(orientation=wx.HORIZONTAL))
      # Buttons
        self.moveUpButton = bHelper.addButton(self, label=_("Move &up"))
        self.moveUpButton.Bind(wx.EVT_BUTTON, lambda evt: self.OnMoveClick(evt, -1))
        self.moveDownButton = bHelper.addButton(self, label=_("Move &down"))
        self.moveDownButton.Bind(wx.EVT_BUTTON, lambda evt: self.OnMoveClick(evt, 1))
        self.moveTopButton = bHelper.addButton(self, label=_("Move to &top"))
        self.moveTopButton.Bind(wx.EVT_BUTTON, lambda evt: self.OnMoveClick(evt, -1000))
        self.moveBottomButton = bHelper.addButton(self, label=_("Move to &top"))
        self.moveBottomButton.Bind(wx.EVT_BUTTON, lambda evt: self.OnMoveClick(evt, 1000))
      # OK/Cancel buttons
        sHelper.addDialogDismissButtons(self.CreateButtonSizer(wx.OK|wx.CANCEL))
        mainSizer.Add(sHelper.sizer,border=20,flag=wx.ALL)
        mainSizer.Fit(self)
        self.SetSizer(mainSizer)
        self.Bind(wx.EVT_BUTTON,self.onOk,id=wx.ID_OK)
        self.windowsList.SetFocus()

    def getItemTextForList(self, item, column):
        hwnd = self.hwnds[item]
        if column == 0:
            return hwnd['title']
        elif column == 1:
            return str(hwnd['timestamp'])
        else:
            raise ValueError("Unknown column: %d" % column)

    def onListItemFocused(self, evt):
        if self.windowsList.GetSelectedItemCount()!=1:
            return
        index=self.windowsList.GetFirstSelected()
        hwnd = self.hwnds[index]

    def OnMoveClick(self,evt, increment):
        if self.windowsList.GetSelectedItemCount()!=1:
            return
        index=self.windowsList.GetFirstSelected()
        if index<0:
            return
        newIndex = index + increment
        newIndex = max(0, min(len(self.hwnds)-1, newIndex))
        if index != newIndex:
            # Swap
            tmp = self.hwnds[index]
            self.hwnds[index] = self.hwnds[newIndex]
            self.hwnds[newIndex] = tmp
            self.windowsList.Select(newIndex)
            self.windowsList.Focus(newIndex)
        else:
            return

    def onOk(self,evt):
        queryObserver(
            "updateTimestamps",
            windows=[
                {
                    "hwnd": entry['hwnd'],
                    "timestamp": i,
                }
                for i,entry in enumerate(self.hwnds)
            ]
        )
        evt.Skip()

def openReorderDialog(appName):
    dialog = ReorderWindowsDialog(parent=None, appName=appName)
    if dialog.ShowModal()==wx.ID_OK:
        pass


def fetchTitles(hwnds):
    for d in hwnds:
        # For some apps, like Google Chrome, GetWindowText returns incorrect title.
        # Thatt is not the title reported by NVDA+T command.
        # Trying to retrieve more accurate title via IA2.
        name = None
        try:
            obj = NVDAObjects.IAccessible.getNVDAObjectFromEvent(d['hwnd'], winUser.OBJID_CLIENT, 0)
            name = obj.name
        except KeyError:
            pass
        if name is None:
            name = winUser.getWindowText(d['hwnd'])
        if name is not None:
            d['title'] = name
    return hwnds
    
class GlobalPlugin(globalPluginHandler.GlobalPlugin):
    scriptCategory = _("Task Switcher")

    def __init__(self, *args, **kwargs):
        super(GlobalPlugin, self).__init__(*args, **kwargs)
        self.createMenu()
        self.injectHooks()
        self.beeper = Beeper()
        initHwndObserverAsync()
        loadConfig()
        self.lastEntry = None
        self.lastGestureCounter = 0
        self.lastKeyCounter = 0

    def createMenu(self):
        gui.settingsDialogs.NVDASettingsDialog.categoryClasses.append(SettingsEntriesDialog)
        gui.settingsDialogs.NVDASettingsDialog.categoryClasses.append(SettingsDialog)

    def terminate(self):
        destroyHwndObserver()
        self.removeHooks()
        gui.settingsDialogs.NVDASettingsDialog.categoryClasses.remove(SettingsDialog)
        gui.settingsDialogs.NVDASettingsDialog.categoryClasses.remove(SettingsEntriesDialog)

    def injectHooks(self):
        pass

    def  removeHooks(self):
        pass

    @script(description="Show task switcher op-up menu", gestures=['kb:nvda+control+f12'])
    def script_taskSwitcherPopupMenu(self, gesture):
        focus = api.getFocusObject()
        fg = api.getForegroundObject()
        hwnd = fg.windowHandle
        appName = fg.appModule.appName
        appPath = fg.appModule.appPath
        gui.mainFrame.prePopup()
        try:
            frame = wx.Frame(None, -1,"Fake popup frame", pos=(1, 1),size=(1, 1))
            menu = wx.Menu()
          # Create new entry
            item = menu.Append(wx.ID_ANY, _("&Create a new entry for this application"))
            frame.Bind(
                wx.EVT_MENU,
                lambda evt, focus=focus: openEntryDialog(focus=focus),
                item,
            )
          # Edit entry and launch options
            for entry in globalConfig.entries:
                if appName != entry.appName:
                    continue
                if entry.appPath and appPath != entry.appPath:
                    continue
                hwnds = self.queryEntry(entry)
                hwnds = [x['hwnd'] for x in hwnds]
                if hwnd in hwnds:
                  # Edit entry
                    item = menu.Append(wx.ID_ANY, _("&Edit %s") % entry.name)
                    frame.Bind(
                        wx.EVT_MENU,
                        lambda evt, entry=entry: openEntryDialog(focus=None, entry=entry),
                        item,
                    )
                  # Launch entry
                    if not entry.launchCmd:
                        continue
                    item = menu.Append(wx.ID_ANY, _("&Launch %s") % entry.name)
                    frame.Bind(
                        wx.EVT_MENU,
                        lambda evt, entry=entry: self.launchApp(entry),
                        item,
                    )
          # hide this window
            item = menu.Append(wx.ID_ANY, _("&Hide this window"))
            frame.Bind(
                wx.EVT_MENU,
                lambda evt, fg=fg: self.script_HideWindow(None, fg),
                item,
            )
          # Show hidden windows
            item = menu.Append(wx.ID_ANY, _("&Show hidden windows"))
            frame.Bind(
                wx.EVT_MENU,
                lambda evt: self.script_showWindows(None),
                item,
            )
            item.Enable(len(self.hiddenWindows) > 0)
          # reorder windows
            appName = focus.appModule.appName
            item = menu.Append(wx.ID_ANY, _("&Reorder %s windows") % appName)
            frame.Bind(
                wx.EVT_MENU,
                lambda evt, appName=appName: openReorderDialog( appName=appName),
                item,
            )
          # Show all entries
            # For some reason the window won't show up
            if False:
                item = menu.Append(wx.ID_ANY, _("&Show all entries"))
                try:
                    popupFunc = gui.mainFrame._popupSettingsDialog
                except AttributeError:
                    popupFunc = gui.mainFrame.popupSettingsDialog
                def showEntriesWindow(evt):
                    wx.CallAfter(lambda: popupFunc(SettingsEntriesDialog))
                frame.Bind(
                    wx.EVT_MENU,
                    showEntriesWindow,
                    item,
                )
          # Close menu handler
            frame.Bind(
                wx.EVT_MENU_CLOSE,
                lambda evt: frame.Close()
            )
            frame.Show()
            wx.CallAfter(lambda: frame.PopupMenu(menu))
        finally:
            gui.mainFrame.postPopup()

    def queryEntry(self, entry):
        hwnds = queryHwnds(entry.appName)
        if entry.appPath:
            hwnds = [
                hwnd
                for hwnd in hwnds
                if hwnd['path'].lower() == entry.appPath.lower()
            ]
        
        if entry.pattern:
            hwnds = fetchTitles(hwnds)
            regex = re.compile(entry.pattern)
            hwnds = [
                hwnd
                for hwnd in hwnds
                if regex.search(hwnd['title']) is not None
            ]
        n = len(hwnds)
        if entry.index > 0:
            hwndIndex = entry.index - 1
            try:
                hwnds = [hwnds[hwndIndex]]
            except IndexError:
                hwnds = []
        return hwnds
        
    def launchApp(self, entry):
        cmd = entry.launchCmd
        if not cmd:
            ui.message(f"Cannot launch {entry.name} because launch command is empty!")
            return
        p = subprocess.Popen(cmd, shell=True)
        ui.message(f"Launched {entry.name}")
        def checkProcessHealth():
            yield 1000
            exitCode = p.poll()
            if exitCode is not None:
                speech.cancelSpeech()
                if exitCode == 0:
                    ui.message(f"Application {entry.name} has quit")
                else:
                    ui.message(f"Application {entry.name} has failed with error code {exitCode}")
        executeAsynchronously(checkProcessHealth())
        return

    @script(description="Task Switcher script", gestures=['kb:Windows+z'])
    def script_taskSwitch(self, gesture):
        toneHz = 100
        fg = api.getForegroundObject()

        entry = globalGesturesToEntries[getKeystrokeFromGesture(gesture)]
        if entry == self.lastEntry and keyboardHandler.keyCounter == self.lastKeyCounter + 1:
            self.lastGestureCounter += 1
        else:
            self.lastEntry = entry
            self.lastGestureCounter = 0
        self.lastKeyCounter = keyboardHandler.keyCounter
        gestureCounter = self.lastGestureCounter
        hwnds = self.queryEntry(entry)
        n = len(hwnds)
        if n == 0:
            # Launch app
            return self.launchApp(entry)
        elif entry.index == 0:
            hwndIndex = gestureCounter % n
            if gestureCounter > 0 and hwndIndex == 0:
                toneHz = 1000
        else:
            hwndIndex = 0
        hwnd = hwnds[hwndIndex]['hwnd']
        isMaximized = hwnds[hwndIndex]['isMaximized']
        maybeCloseStartMenu()
        forceChildFocus = (entry.appName.lower() == "notepad++")
        ok = activateWindowBetter(hwnd)
        if not ok:
            _log_activation_failure_context(hwnd)
            raise RuntimeError("not ok")
        #winUser.setForegroundWindow(hwnd)
        #winUser.setFocus(hwnd)
        #SetActiveWindow(hwnd)

        core.callLater(0, nvdaRefreshFocusFromOS)
        

        
        autoMaximize = getConfig("autoMaximize")
        if  autoMaximize and not isMaximized:
            maximizeWindow(hwnd)
        volume = getConfig("clickVolume")
        tones.beep(toneHz, 20, left=volume, right=volume)

    hiddenWindows = []
    @script(description=_("Hide current window."), gestures=['kb:NVDA+Shift+-'])
    def script_HideWindow(self, gesture, fg=None):
        fg = fg or api.getForegroundObject()
        handle = fg.windowHandle
        self.hiddenWindows.append(handle)
        winBindings.user32.dll.ShowWindow(handle, winUser.SW_HIDE)
        keyboardHandler.KeyboardInputGesture.fromName("Alt+Tab").send()
        def delayedSpeak():
            speech.cancelSpeech()
            ui.message(_("Hid current window. Now there are %d windows hidden.") % len(self.hiddenWindows))
        core.callLater(100, delayedSpeak)

    @script(description=_("Show hidden windows."), gestures=['kb:NVDA+Shift+='])
    def script_showWindows(self, gesture):
        if len(self.hiddenWindows) == 0:
            ui.message(_("No windows hidden or all hidden windows have been already shown."))
            return
        n = len(self.hiddenWindows)
        for handle in self.hiddenWindows:
            time.sleep(0.1)
            SW_SHOW = 5
            winBindings.user32.dll.ShowWindow(handle, SW_SHOW)
        winUser.setForegroundWindow(self.hiddenWindows[-1])
        def delayedSpeak():
            speech.cancelSpeech()
            ui.message(_("%d windows shown") % n)
        core.callLater(100, delayedSpeak)
        self.hiddenWindows = []

    @script(description=_("Print health check."), gestures=['kb:NVDA+control+f11'])
    def script_printHealthCheck(self, gesture):
        z = queryObserver("healthCheck")
        log.info(z['result'])
        tones.beep(500, 50)
