// dllmain.cpp : Defines the entry point for the DLL application.
#include <pybind11/pybind11.h>
#include <pybind11/embed.h>
#include <Windows.h>


namespace py = pybind11;

void testBeep() {
    auto my_module = py::module::import("tones");
    auto beep = my_module.attr("beep");
    beep(1000, 100);
}

PYBIND11_MODULE(hwnd_observer, m) {
    m.def("testBeep", &testBeep, "Testing tones.beep");
}

