# Task Switcher add-on for NVDA

This add-on provides configurable keystrokes to switch between applications.

## Getting started

1. Switch to desired application.
2. Press `nvda+control+f12`.
3. In popupp menu select "Create a new entry for this application".
4. Tab to "Keystroke" button.
5. Press button then press desired keystroke. For inspiration, try `windows+z`.
6. Press oK.

Now try pressing `windows+z` and it'll switch you to that application.

## Keyboard shortcuts

* `nvda+control+f12`: show task switcher popup menu.
* `nvda+shift+-`: hide current window.
* `nvda+shift+=`: show all hidden windows.
* Configurable keystrokes to switch to applications.

## Order of windows

Please note, that it is currently impossible to programmatically find out the order in which several windows of an application have been opened. Therefore task switcher will assume a random order for the windows that were created before task switcher starts. You can manually change the order of such windows via taskswitcher popup menu. Once it starts, Task switcher will monitor and record creation time for all windows and this information will be stored in cache file.

## Removal and upgrading

Please note, that in order to remove or upgrade this add-on, you would need to restart your computer. Any attempt to remove/upgrade add-on without full system restart will cause NVDA to run into errors removing add-on directory.
This is expected, since task switcher add-on uses CBT hooks in order to observe creation of windows in the system. As a result, certain DLL files within the add-on remain in use even after NVDA shutts down. The only way to remove these files is to reboot your computer.
