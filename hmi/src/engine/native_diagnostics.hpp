#pragma once

#include <string>
#include <string_view>

namespace hmi::engine::detail
{
// Product diagnostics use the native name; SDK symbols remain unchanged.
inline std::string native_diagnostic(std::string message)
{
    const std::string_view labels[] = {"audiocpp", "AUDIOCPP", "AudioCpp", "audio.cpp", "Audio.cpp", "AUDIO.CPP"};
    for (const auto label : labels)
    {
        std::size_t cursor = 0;
        while ((cursor = message.find(label, cursor)) != std::string::npos)
        {
            message.replace(cursor, label.size(), "native");
            cursor += 6;
        }
    }
    return message;
}
}
