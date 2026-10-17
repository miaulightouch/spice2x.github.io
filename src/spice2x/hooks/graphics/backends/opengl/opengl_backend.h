#pragma once

#ifndef SPICE_OPENGL_TEST
#include <windows.h>
#endif
#include <string_view>

void graphics_opengl_init();
void graphics_opengl_window_created(HWND window, std::string_view title);
