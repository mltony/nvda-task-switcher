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
from ctypes import create_string_buffer, byref
import cursorManager
import documentBase
import eventHandler
import functools
import editableText
import globalPluginHandler
import gui
from gui import guiHelper, nvdaControls
from gui.settingsDialogs import SettingsPanel
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

module = "taskSwitcher"
def initConfiguration():
    defaultBulkyRegexp = r'$|(^|(?<=[\s\(\)]))[^\s\(\)]|\b(:\d+)+\b'
    confspec = {
        "overrideMoveByWord" : "boolean( default=True)",
        "enableInBrowseMode" : "boolean( default=True)",
        "enableSelection" : "boolean( default=True)",
        "selectTrailingSpace" : "boolean( default=False)",
        "leftControlAssignmentIndex": "integer( default=3, min=0, max=5)",
        "rightControlAssignmentIndex": "integer( default=1, min=0, max=5)",
        "leftControlWindowsAssignmentIndex": "integer( default=2, min=0, max=5)",
        "rightControlWindowsAssignmentIndex": "integer( default=4, min=0, max=5)",
        "bulkyWordPunctuation" : f"string( default='():')",
        "bulkyWordRegex" : f"string( default='{defaultBulkyRegexp}')",
        "bulkyWordEndRegex" : f"string( default='')",
        "paragraphChimeVolume" : "integer( default=5, min=0, max=100)",
        "wordCount": "integer( default=5, min=1, max=1000)",
        "applicationsBlacklist" : f"string( default='')",
        "disableInGoogleDocs" : "boolean( default=False)",
    }
    config.conf.spec[module] = confspec

def getConfig(key):
    value = config.conf[module][key]
    return value

def setConfig(key, value):
    config.conf[module][key] = value


@dataclass
class TSEntry:
    name: str
    appName: str
    appPath: Optional[str] = ""
    keystroke: Optional[str] = None
    pattern: Optional[str] = ""
    index: int = 0

@dataclass
class TSConfig:
    entries: List[TSEntry]

def dataclass_to_dict(dataclass_instance):
    if hasattr(dataclass_instance, '__dict__'):
        return dataclass_instance.__dict__
    if isinstance(dataclass_instance, list):
        return [dataclass_to_dict(item) for item in dataclass_instance]
    if isinstance(dataclass_instance, tuple):
        return tuple(dataclass_to_dict(item) for item in dataclass_instance)
    if isinstance(dataclass_instance, dict):
        return {key: dataclass_to_dict(value) for key, value in dataclass_instance.items()}
    return dataclass_instance

def dict_to_dataclass(cls, dct):
    if isinstance(dct, dict):
        return cls(**{key: dict_to_dataclass(value, dct[key]) for key, value in dct.items()})
    if isinstance(dct, list):
        return [dict_to_dataclass(cls, item) for item in dct]
    return dct


configFileName = os.path.join(globalVars.appArgs.configPath, "taskSwitcherConfig.json")
globalConfig = None

def loadConfig():
    global globalConfig
    try:
        configJsonString = "\n".join(open(configFileName, "r", encoding='utf-8').readlines())
    except OSError:
        globalConfig = TSConfig([])
        return
    globalConfig = dict_to_dataclass(json.loads(configJsonString))


def saveConfig():
    global globalConfig
    with open(configFileName, "w", encoding='utf-8') as f:
        print(
            json.dumps(dataclass_to_dict(globalConfig)),
            file=f
        )

addonHandler.initTranslation()
initConfiguration()
loadConfig()


class SettingsDialog(SettingsPanel):
    # Translators: Title for the settings dialog
    title = _("Task Switcher")
    controlAssignmentText = [
        _("Default NVDA word navigation (WordNav disabled)"),
        _("Notepad++ style navigation"),
        _("Bulky word navigation"),
        _("Fine word navigation - good for programming"),
        _("MultiWord navigation - reads multiple words at once"),
        _("Custom regular expression word navigation"),
    ]
    controlWindowsAssignmentText = [
        _("Unassigned"),
    ] + controlAssignmentText[1:]

    def makeSettings(self, settingsSizer):
        sHelper = gui.guiHelper.BoxSizerHelper(self, sizer=settingsSizer)
      # checkbox override move by word
        # Translators: Checkbox for override move by word
        label = _("Enable WordNav in editables")
        self.overrideMoveByWordCheckbox = sHelper.addItem(wx.CheckBox(self, label=label))
        self.overrideMoveByWordCheckbox.Value = getConfig("overrideMoveByWord")
      # checkbox enableInBrowseMode
        # Translators: Checkbox for enableInBrowseMode
        label = _("Enable WordNav in browse mode.")
        self.enableInBrowseModeCheckbox = sHelper.addItem(wx.CheckBox(self, label=label))
        self.enableInBrowseModeCheckbox.Value = getConfig("enableInBrowseMode")
      # checkbox enableSelection
        label = _("Enable WordNav word selection")
        self.enableSelectionCheckbox = sHelper.addItem(wx.CheckBox(self, label=label))
        self.enableSelectionCheckbox.Value = getConfig("enableSelection")
      # checkbox select trailing space
        label = _("Include trailing space when selecting words with control+shift+rightArrow")
        self.selectTrailingSpaceCheckbox = sHelper.addItem(wx.CheckBox(self, label=label))
        self.selectTrailingSpaceCheckbox.Value = getConfig("selectTrailingSpace")
      # left control assignment Combo box
        # Translators: Label for left control assignment combo box
        label = _("Left control behavior:")
        self.leftControlAssignmentCombobox = sHelper.addLabeledControl(label, wx.Choice, choices=self.controlAssignmentText)
        self.leftControlAssignmentCombobox.Selection = getConfig("leftControlAssignmentIndex")
      # right control assignment Combo box
        # Translators: Label for right control assignment combo box
        label = _("Right control behavior:")
        self.rightControlAssignmentCombobox = sHelper.addLabeledControl(label, wx.Choice, choices=self.controlAssignmentText)
        self.rightControlAssignmentCombobox.Selection = getConfig("rightControlAssignmentIndex")
      # Left Control+Windows assignment Combo box
        # Translators: Label for control+windows assignment combo box
        label = _("Left Control+Windows behavior:")
        self.leftControlWindowsAssignmentCombobox = sHelper.addLabeledControl(label, wx.Choice, choices=self.controlWindowsAssignmentText)
        self.leftControlWindowsAssignmentCombobox.Selection = getConfig("leftControlWindowsAssignmentIndex")

      # Right Control+Windows assignment Combo box
        # Translators: Label for control+windows assignment combo box
        label = _("Right Control+Windows behavior:")
        self.rightControlWindowsAssignmentCombobox = sHelper.addLabeledControl(label, wx.Choice, choices=self.controlWindowsAssignmentText)
        self.rightControlWindowsAssignmentCombobox.Selection = getConfig("rightControlWindowsAssignmentIndex")
      # bulkyWordPunctuation
        # Translators: Label for bulkyWordPunctuation edit box
        self.bulkyWordPunctuationEdit = sHelper.addLabeledControl(_("Bulky word separators:"), wx.TextCtrl)
        self.bulkyWordPunctuationEdit.Value = getConfig("bulkyWordPunctuation")

      # Custom word regex
        self.customWordRegexEdit = sHelper.addLabeledControl(_("Custom word regular expression:"), wx.TextCtrl)
        self.customWordRegexEdit.Value = getConfig("bulkyWordRegex")
      # Custom word end regex
        self.customWordEndRegexEdit = sHelper.addLabeledControl(_("Optional Custom word end regular expression for word selection:"), wx.TextCtrl)
        self.customWordEndRegexEdit.Value = getConfig("bulkyWordEndRegex")
      # MultiWord word count
        # Translators: Label for multiWord wordCount edit box
        self.wordCountEdit = sHelper.addLabeledControl(_("Word count for multiWord navigation:"), wx.TextCtrl)
        self.wordCountEdit.Value = str(getConfig("wordCount"))
      # paragraphChimeVolumeSlider
        # Translators: Paragraph crossing chime volume
        label = _("Volume of chime when crossing paragraph border")
        self.paragraphChimeVolumeSlider = sHelper.addLabeledControl(label, wx.Slider, minValue=0,maxValue=100)
        self.paragraphChimeVolumeSlider.SetValue(getConfig("paragraphChimeVolume"))

      # applicationsBlacklist edit
        # Translators: Label for blacklisted applications edit box
        self.applicationsBlacklistEdit = sHelper.addLabeledControl(_("Disable WordNav in applications (comma-separated list)"), wx.TextCtrl)
        self.applicationsBlacklistEdit.Value = getConfig("applicationsBlacklist")
      # checkbox Disable in Google Docs
        label = _("Disable in Google Docs")
        self.DisableInGoogleDocsCheckbox = sHelper.addItem(wx.CheckBox(self, label=label))
        self.DisableInGoogleDocsCheckbox.Value = getConfig("disableInGoogleDocs")

    def onSave(self):
        try:
            if int(self.wordCountEdit.Value) <= 1:
                raise Exception()
        except:
            self.wordCountEdit.SetFocus()
            ui.message(_("WordCount must be a positive integer greater than 2."))
            return
        setConfig("overrideMoveByWord", self.overrideMoveByWordCheckbox.Value)
        setConfig("enableInBrowseMode", self.enableInBrowseModeCheckbox.Value)
        setConfig("enableSelection", self.enableSelectionCheckbox.Value)
        setConfig("selectTrailingSpace", self.selectTrailingSpaceCheckbox.Value)
        setConfig("leftControlAssignmentIndex", self.leftControlAssignmentCombobox.Selection)
        setConfig("rightControlAssignmentIndex", self.rightControlAssignmentCombobox.Selection)
        setConfig("leftControlWindowsAssignmentIndex", self.leftControlWindowsAssignmentCombobox.Selection)
        setConfig("rightControlWindowsAssignmentIndex", self.rightControlWindowsAssignmentCombobox.Selection)
        setConfig("bulkyWordPunctuation", self.bulkyWordPunctuationEdit.Value)
        setConfig("bulkyWordRegex", self.customWordRegexEdit.Value)
        setConfig("bulkyWordEndRegex", self.customWordEndRegexEdit.Value)
        setConfig("wordCount", int(self.wordCountEdit.Value))
        setConfig("paragraphChimeVolume", self.paragraphChimeVolumeSlider.Value)
        setConfig("applicationsBlacklist", self.applicationsBlacklistEdit.Value)
        setConfig("disableInGoogleDocs", self.DisableInGoogleDocsCheckbox.Value)



class Beeper:
    BASE_FREQ = speech.IDT_BASE_FREQUENCY
    def getPitch(self, indent):
        return self.BASE_FREQ*2**(indent/24.0) #24 quarter tones per octave.

    BEEP_LEN = 10 # millis
    PAUSE_LEN = 5 # millis
    MAX_CRACKLE_LEN = 400 # millis
    MAX_BEEP_COUNT = MAX_CRACKLE_LEN // (BEEP_LEN + PAUSE_LEN)

    def __init__(self):
        self.player = nvwave.WavePlayer(
            channels=2,
            samplesPerSec=int(tones.SAMPLE_RATE),
            bitsPerSample=16,
            outputDevice=config.conf["speech"]["outputDevice"],
            wantDucking=False
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

class EditEntryDialog(wx.Dialog):
    def __init__(self, parent, entry, entryIndex=None):
        title=_("Edit Task Switcher entry")
        super().__init__(parent,title=title)
        self.entry = entry
        
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
      # Translators: Window Title pattern
        label = _("Window &title regex pattern (optional):")
        self.patternTextCtrl=sHelper.addLabeledControl(label, wx.TextCtrl)
        self.patternTextCtrl.SetValue(self.entry.pattern)
      # Index spinCtrl
        label = _("Index of selected window (or set to zero to cycle through all available windows):")
        self.indexEdit = sHelper.addLabeledControl(
            label,
            nvdaControls.SelectOnFocusSpinCtrl,
            min=-10,
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

        entry = TSEntry(
            name=self.nameTextCtrl.Value,
            pattern= pattern,
            keystroke= self.keystroke,
            appName=self.appNameTextCtrl.Value,
            appPath=self.appPathTextCtrl.Value,
            index=self.indexEdit .Value,
        )
        return entry

    def makeNewSite(self):
        if not self.allowSiteSelection:
            return self.oldSite
        newSite = self.config.sites[self.siteComboBox.control.GetSelection()]
        if newSite != self.oldSite:
            result = gui.messageBox(
                _("Warning: you are about to move this bookmark to site %(new_site)s. "
                "This bookmark will disappear from the old site %(old_site)s. Would you like to proceed?") % {"new_site": newSite.getDisplayName(), "old_site": self.oldSite.getDisplayName()},
                _("Bookmark Entry warning"),
                wx.YES|wx.NO|wx.ICON_WARNING,
                self
            )
            if result == wx.YES:
                return newSite
            else:
                self.siteComboBox.control.SetFocus()
                return None
        return self.oldSite

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
        elif self.bookmark.category.name.startswith('QUICK_JUMP') and 'shift+' in g:
            msg = _("Invalid keystroke %s: Cannot use keystrokes with shift modifier for quickJump bookmarks!") % g
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



def openEntryDialog(focus=None):
    appName = focus.appModule.appName
    entry = TSEntry(
        name=appName,
        appName=appName,
        appPath=focus.appModule.appPath,
    )
    dialog = EditEntryDialog(parent=None, entry=entry)
    if dialog.ShowModal()==wx.ID_OK:
        tones.beep(500, 50)
class GlobalPlugin(globalPluginHandler.GlobalPlugin):
    scriptCategory = _("Task Switcher")

    def __init__(self, *args, **kwargs):
        super(GlobalPlugin, self).__init__(*args, **kwargs)
        self.createMenu()
        self.injectHooks()
        self.beeper = Beeper()

    def createMenu(self):
        gui.settingsDialogs.NVDASettingsDialog.categoryClasses.append(SettingsDialog)

    def terminate(self):
        self.removeHooks()
        gui.settingsDialogs.NVDASettingsDialog.categoryClasses.remove(SettingsDialog)

    def injectHooks(self):
        pass

    def  removeHooks(self):
        pass

    @script(description="Show task switcher op-up menu", gestures=['kb:nvda+`'])
    def script_taskSwitcherPopupMenu(self, gesture):
        focus = api.getFocusObject()
        gui.mainFrame.prePopup()
        try:
            frame = wx.Frame(None, -1,"Fake popup frame", pos=(1, 1),size=(1, 1))
            menu = wx.Menu()
            item = menu.Append(wx.ID_ANY, _("&Create new entry for this application"))
            frame.Bind(
                wx.EVT_MENU,
                lambda evt, focus=focus: openEntryDialog(focus=focus),
                item,
            )
            frame.Bind(
                wx.EVT_MENU_CLOSE,
                lambda evt: frame.Close()
            )
            frame.Show()
            wx.CallAfter(lambda: frame.PopupMenu(menu))
        finally:
            gui.mainFrame.postPopup()
