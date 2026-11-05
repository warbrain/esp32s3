#include "microphone_devices.hpp"

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <wrl/client.h>

#include <algorithm>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace
{
    class ScopedComInitialization
    {
    public:
        ScopedComInitialization()
            : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
              must_uninitialize_(SUCCEEDED(result_))
        {
        }

        ~ScopedComInitialization()
        {
            if (must_uninitialize_)
                CoUninitialize();
        }

        HRESULT result() const noexcept { return result_; }

    private:
        HRESULT result_;
        bool must_uninitialize_;
    };

    std::wstring ReadDefaultInputId(IMMDeviceEnumerator *enumerator)
    {
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device)))
            return {};

        LPWSTR raw_id = nullptr;
        if (FAILED(device->GetId(&raw_id)) || !raw_id)
            return {};

        std::wstring id(raw_id);
        CoTaskMemFree(raw_id);
        return id;
    }
}
#endif

namespace hmi::audio
{
    bool InputDevices::refresh()
    {
#ifdef _WIN32
        ScopedComInitialization com;
        if (FAILED(com.result()) && com.result() != RPC_E_CHANGED_MODE)
        {
            devices_.clear();
            selected_id_.clear();
            return false;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS(&enumerator));
        if (FAILED(result))
        {
            devices_.clear();
            selected_id_.clear();
            return false;
        }

        ComPtr<IMMDeviceCollection> collection;
        result = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(result))
        {
            devices_.clear();
            selected_id_.clear();
            return false;
        }

        const std::wstring default_id = ReadDefaultInputId(enumerator.Get());
        UINT count = 0;
        result = collection->GetCount(&count);
        if (FAILED(result))
        {
            devices_.clear();
            selected_id_.clear();
            return false;
        }

        std::vector<InputDevice> refreshed;
        refreshed.reserve(count);
        for (UINT index = 0; index < count; ++index)
        {
            ComPtr<IMMDevice> endpoint;
            if (FAILED(collection->Item(index, &endpoint)))
                continue;

            LPWSTR raw_id = nullptr;
            if (FAILED(endpoint->GetId(&raw_id)) || !raw_id)
                continue;

            InputDevice device;
            device.id = raw_id;
            CoTaskMemFree(raw_id);
            device.is_system_default = device.id == default_id;

            ComPtr<IPropertyStore> properties;
            if (SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ, &properties)))
            {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
                    value.vt == VT_LPWSTR && value.pwszVal)
                {
                    device.name = value.pwszVal;
                }
                PropVariantClear(&value);
            }
            if (device.name.empty())
                device.name = L"Microphone";

            refreshed.push_back(std::move(device));
        }

        devices_ = std::move(refreshed);

        const auto previous = std::find_if(devices_.begin(), devices_.end(),
                                           [this](const InputDevice &device) {
                                               return device.id == selected_id_;
                                           });
        if (previous != devices_.end())
            return true;

        const auto system_default = std::find_if(devices_.begin(), devices_.end(),
                                                 [](const InputDevice &device) {
                                                     return device.is_system_default;
                                                 });
        selected_id_ = system_default != devices_.end()
                           ? system_default->id
                           : devices_.empty() ? std::wstring{} : devices_.front().id;
        return true;
#else
        devices_.clear();
        selected_id_.clear();
        return false;
#endif
    }

    bool InputDevices::select(const std::wstring &id)
    {
        const auto found = std::find_if(devices_.begin(), devices_.end(),
                                        [&id](const InputDevice &device) {
                                            return device.id == id;
                                        });
        if (found == devices_.end())
            return false;

        selected_id_ = found->id;
        return true;
    }
}