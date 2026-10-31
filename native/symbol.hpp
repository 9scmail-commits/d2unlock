#pragma once
#include <windows.h>
template<class Function> Function symbol(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}
