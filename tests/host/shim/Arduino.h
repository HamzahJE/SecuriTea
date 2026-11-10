// Host shim: just enough Arduino to compile the pure .ir parser on a Mac.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <cstring>
#include <cstdlib>
#include <strings.h>
struct String : std::string {
    String() {}
    String(const char *s) : std::string(s ? s : "") {}
    String(const std::string &s) : std::string(s) {}
    String &operator=(const char *s) { assign(s ? s : ""); return *this; }
};
extern unsigned long g_delayCalls;
inline void vTaskDelay(int) { g_delayCalls++; }
