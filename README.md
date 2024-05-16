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

Please note, that it is currently impossible to programmatically find out the order in which several windows of an application have been opened. Therefore task switcher will assume a random order for the windows that were created before task switcher starts. You can manually change the order of such windows via taskswitcher popup menu. Once it starts, Task switcher will monitor and record creation time for all windows and this information will be stored in cache file (its location is configurable in task switcher settings), which means that this information will be shared across NVDA restarts and across instances of task switcher running in different installations of NVDA.
