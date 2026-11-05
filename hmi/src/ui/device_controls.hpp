#pragma once

namespace hmi::app { class Services; }
namespace hmi::audio { class InputDevices; }

namespace hmi::ui
{
void DrawMicrophonePicker(hmi::audio::InputDevices &microphones);

// Renders device information and refresh controls inside the shell.
void DrawDevicesPage(hmi::app::Services &services);
}
