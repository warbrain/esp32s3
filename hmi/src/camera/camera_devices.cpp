#include "camera_devices.hpp"
#ifdef _WIN32
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace hmi::camera
{
  std::vector<Device> enumerate_devices()
  {
    std::vector<Device> devices;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
      return devices;
    IMFAttributes *raw = nullptr;
    if (SUCCEEDED(MFCreateAttributes(&raw, 1)))
    {
      ComPtr<IMFAttributes> attrs;
      attrs.Attach(raw);
      attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
      IMFActivate **list = nullptr;
      UINT32 count = 0;
      if (SUCCEEDED(MFEnumDeviceSources(attrs.Get(), &list, &count)))
      {
        for (UINT32 i = 0; i < count; ++i)
        {
          WCHAR *name = nullptr;
          UINT32 name_len = 0;
          WCHAR *id = nullptr;
          UINT32 id_len = 0;
          list[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &name_len);
          list[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &id, &id_len);
          devices.push_back({id ? id : L"", name ? name : L"Camera"});
          CoTaskMemFree(name);
          CoTaskMemFree(id);
          list[i]->Release();
        }
        CoTaskMemFree(list);
      }
    }
    MFShutdown();
    return devices;
  }
}
#else
namespace hmi::camera
{
  std::vector<Device> enumerate_devices() { return {}; }
}
#endif

