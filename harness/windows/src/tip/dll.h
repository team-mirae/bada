// The DLL's module handle and lock count (dllmain.cpp).
#pragma once

#include <windows.h>

namespace bada::tip {

HINSTANCE DllInstance();
void DllAddRef();
void DllRelease();

}  // namespace bada::tip
