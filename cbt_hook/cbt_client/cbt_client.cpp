// cbt_client.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <windows.h>
#include <cstdlib> // for strtoul
#include "cbt_hook.h"


int main(int argc, char ** argv)
{
    if (argc != 2) {
        std::cout << "Need hwnd as command line argument" << std::endl;
        return 1;
    }
    char* endptrDummy;
    UINT32 hwndValue = strtoul(argv[1], &endptrDummy, 10);
    HWND hwnd = (HWND)hwndValue;
    bool result = InstallCBTHook(hwnd);
    if (!result) {
        std::cout << "Installing CBT hook failed!" << std::endl;
        return 1;
    }
    std::cout << "Successfully installed CBT hook! Press enter to quit." << std::endl;
    std::string dummy;
    std::cin >> dummy;
    std::cout << "Shutting down" << std::endl;
    return 0;
}
