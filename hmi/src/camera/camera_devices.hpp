#pragma once
#include <string>
#include <vector>
namespace hmi::camera
{
    struct Device
    {
        std::wstring id;
        std::wstring name;
    };
    // Enumerates Windows Media Foundation video capture sources. Frame streaming is the next integration layer.
    std::vector<Device> enumerate_devices();
}

