#pragma once

#include <string>
#include <vector>

namespace hmi::audio
{
    struct InputDevice
    {
        std::wstring id;
        std::wstring name;
        bool is_system_default = false;
    };

    // Enumerates active Windows capture endpoints and stores an app-local selection.
    // Choosing an endpoint here never changes Windows' system-wide default input.
    class InputDevices
    {
    public:
        bool refresh();
        bool select(const std::wstring &id);

        const std::vector<InputDevice> &devices() const noexcept { return devices_; }
        const std::wstring &selected_id() const noexcept { return selected_id_; }

    private:
        std::vector<InputDevice> devices_;
        std::wstring selected_id_;
    };
}