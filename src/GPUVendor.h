#pragma once

#include <vector>
#include <string>

extern "C" {
#include <libavutil/hwcontext.h>
}

enum class GPUVendor { Nvidia, AMD, Intel, Apple, Unknown };

GPUVendor detect_gpu(); 
std::vector<std::pair<std::string, AVHWDeviceType>> make_priority_list(GPUVendor v);
