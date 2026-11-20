#include "action/actions.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

namespace hmi::action
{
namespace
{
std::string parameter(const intent::Action &action, std::string_view name)
{
    const auto found = action.params.find(name);
    return found == action.params.end() ? std::string{} : found->second;
}

std::wstring wide(std::string_view text)
{
    if (text.empty())
        return {};
    if (text.size() > 32767 || text.find('\0') != std::string_view::npos)
        throw std::invalid_argument("text is too long or contains NUL");
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (length == 0)
        throw std::invalid_argument("invalid UTF-8");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), length);
    return result;
}

WORD virtual_key(std::string_view token)
{
    if (token.size() == 1)
    {
        const char c = token.front();
        if (c >= 'a' && c <= 'z') return static_cast<WORD>(c - 'a' + 'A');
        if (c >= '0' && c <= '9') return static_cast<WORD>(c);
    }
    struct NamedKey { std::string_view name; WORD value; };
    constexpr NamedKey keys[] = {
        {"ctrl", VK_CONTROL}, {"control", VK_CONTROL}, {"shift", VK_SHIFT}, {"alt", VK_MENU},
        {"win", VK_LWIN}, {"return", VK_RETURN}, {"enter", VK_RETURN}, {"escape", VK_ESCAPE},
        {"esc", VK_ESCAPE}, {"space", VK_SPACE}, {"tab", VK_TAB}, {"backspace", VK_BACK},
        {"delete", VK_DELETE}, {"del", VK_DELETE}, {"insert", VK_INSERT}, {"home", VK_HOME},
        {"end", VK_END}, {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"left", VK_LEFT},
        {"right", VK_RIGHT}, {"up", VK_UP}, {"down", VK_DOWN}};
    for (const auto &key : keys)
        if (token == key.name)
            return key.value;
    if (token.size() >= 2 && token.front() == 'f')
    {
        int index = 0;
        const auto parsed = std::from_chars(token.data() + 1, token.data() + token.size(), index);
        if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() && index >= 1 && index <= 24)
            return static_cast<WORD>(VK_F1 + index - 1);
    }
    throw std::invalid_argument("unknown key: " + std::string(token));
}

bool modifier(WORD key)
{
    return key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU || key == VK_LWIN;
}

bool extended(WORD key)
{
    return key == VK_LWIN || key == VK_INSERT || key == VK_DELETE || key == VK_HOME || key == VK_END ||
           key == VK_PRIOR || key == VK_NEXT || key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN;
}

void send_chord(std::string_view text)
{
    const std::string chord = intent::normalize(text);
    if (chord.empty() || chord.size() > 128)
        throw std::invalid_argument("chord must contain 1..128 bytes");
    std::vector<WORD> keys;
    std::size_t offset = 0;
    while (offset < chord.size())
    {
        const auto plus = chord.find('+', offset);
        const std::size_t end = plus == std::string::npos ? chord.size() : plus;
        const std::string token = intent::normalize(std::string_view(chord).substr(offset, end - offset));
        const WORD key = virtual_key(token);
        if (std::find(keys.begin(), keys.end(), key) != keys.end())
            throw std::invalid_argument("duplicate key in chord");
        if (!keys.empty() && !modifier(keys.back()))
            throw std::invalid_argument("a chord contains modifiers followed by exactly one key");
        keys.push_back(key);
        if (keys.size() > 5 || (plus != std::string::npos && plus + 1 == chord.size()))
            throw std::invalid_argument("invalid chord");
        offset = end + 1;
    }
    if (keys.empty() || modifier(keys.back()))
        throw std::invalid_argument("chord has no main key");
    std::vector<INPUT> inputs;
    for (const WORD key : keys)
    {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = key;
        input.ki.dwFlags = extended(key) ? KEYEVENTF_EXTENDEDKEY : 0;
        inputs.push_back(input);
    }
    for (auto it = keys.rbegin(); it != keys.rend(); ++it)
    {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = *it;
        input.ki.dwFlags = KEYEVENTF_KEYUP | (extended(*it) ? KEYEVENTF_EXTENDEDKEY : 0);
        inputs.push_back(input);
    }
    const UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    if (sent != inputs.size())
    {
        // A partial SendInput must not leave modifiers latched down.
        std::vector<INPUT> release(inputs.begin() + static_cast<std::ptrdiff_t>(keys.size()), inputs.end());
        SendInput(static_cast<UINT>(release.size()), release.data(), sizeof(INPUT));
        throw std::runtime_error("SendInput did not accept the complete chord (possibly blocked by UIPI)");
    }
}
}

Result execute(const intent::Action &action, const Context &context)
{
    Result result;
    const auto begin = std::chrono::steady_clock::now();
    try
    {
        if (action.kind == "launch")
        {
            result.via = "launch";
            const std::wstring path = wide(parameter(action, "path"));
            if (path.empty())
                throw std::invalid_argument("launch requires path");
            const std::wstring arguments = wide(parameter(action, "args"));
            const std::wstring directory = wide(parameter(action, "directory"));
            const auto status = reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open", path.c_str(),
                arguments.empty() ? nullptr : arguments.c_str(), directory.empty() ? nullptr : directory.c_str(), SW_SHOWNORMAL));
            if (status <= 32)
                throw std::runtime_error("ShellExecute failed (code " + std::to_string(status) + ")");
            result.ok = true;
            result.detail = "launched " + parameter(action, "path");
        }
        else if (action.kind == "key")
        {
            result.via = "key";
            std::string chord = parameter(action, "chord");
            if (chord.empty())
                chord = parameter(action, "keys");
            send_chord(chord);
            result.ok = true;
            result.detail = "sent " + chord;
        }
        else if (action.kind == "wait")
        {
            result.via = "wait";
            const std::string text = parameter(action, "ms");
            int milliseconds = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), milliseconds);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || milliseconds < 0 || milliseconds > 10000)
                throw std::invalid_argument("wait.ms must be an integer in 0..10000");
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
            result.ok = true;
            result.detail = "waited " + text + " ms";
        }
        else if (action.kind == "text" || action.kind == "speak")
        {
            const bool speaking = action.kind == "speak";
            result.via = speaking ? "tts" : "sendinput";
            const std::string text = parameter(action, "text");
            if (wide(text).empty())
                throw std::invalid_argument(action.kind + " requires nonempty text");
            const auto &callback = speaking ? context.speak : context.insert_text;
            if (!callback)
                throw std::runtime_error(action.kind + " service is unavailable");
            result.ok = callback(text);
            result.detail = result.ok ? (speaking ? "speech queued" : "text inserted") : action.kind + " failed";
        }
        else
            result.detail = "unsupported action kind: " + action.kind;
    }
    catch (const std::exception &exception)
    {
        result.ok = false;
        result.detail = exception.what();
    }
    catch (...)
    {
        result.ok = false;
        result.detail = "action failed with an unknown error";
    }
    result.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    return result;
}
}
