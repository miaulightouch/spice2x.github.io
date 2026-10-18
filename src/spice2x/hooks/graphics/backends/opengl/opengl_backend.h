#pragma once

#ifndef SPICE_OPENGL_TEST
#include <windows.h>
#endif
#include <string>
#include <string_view>

void graphics_opengl_init();
void graphics_opengl_window_created(HWND window, std::string_view title);

std::string graphics_opengl_probe(std::string_view mode = {}, bool reset = false);
