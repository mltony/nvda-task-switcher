// cbt_client.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <windows.h>
#include <cstdlib> // for strtoul
#include <tchar.h>
#include "cbt_hook.h"


int main(int argc, char ** argv)
{
    HWND hwnd = FindWindow(_T(HWND_OBSERVER_WINDOW_CLASS), NULL);
    if (hwnd == nullptr) {
        std::cout << "Cannot find target window" << std::endl;
        return 1;
    }
    bool result = InstallCBTHook(hwnd);
    if (!result) {
        std::cout << "Installing CBT hook failed!" << std::endl;
        return 1;
    }
    std::cout << "Successfully installed CBT hook! Press enter to quit." << std::endl;
    //Beep(500, 50);
    std::string dummy;
    std::cin >> dummy;
    std::cout << "Shutting down" << std::endl;
    //Beep(500, 50);
    return 0;
}
