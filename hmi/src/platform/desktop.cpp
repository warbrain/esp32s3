#include "platform/desktop.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "ui/theme.hpp"
#include "base/frame_schedule.hpp"
#include "base/logger.hpp"
#include "platform/capture_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <sstream>
#include <limits>
#include <cstdint>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
#define VOLK_IMPLEMENTATION
#include <volk.h>
#endif

#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
#endif

namespace hmi::platform
{
namespace
{
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 800;

const char *capture_phase_name(base::CapturePhase phase) noexcept
{
    switch (phase)
    {
    case base::CapturePhase::Preparing: return "preparing";
    case base::CapturePhase::Recording: return "recording";
    case base::CapturePhase::Processing: return "processing";
    case base::CapturePhase::AwaitingRelease: return "awaiting_release";
    default: return "hidden";
    }
}

void glfw_error_callback(int error, const char *description)
{
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

void glfw_content_scale_callback(GLFWwindow *, float x_scale, float y_scale)
{
    hmi::ui::HandleContentScale(x_scale, y_scale);
}

void backend_vk_result(VkResult err)
{
    if (err == VK_SUCCESS)
        return;
    std::fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0)
        std::abort();
}

bool is_extension_available(const ImVector<VkExtensionProperties> &properties, const char *extension)
{
    for (const VkExtensionProperties &p : properties)
        if (std::strcmp(p.extensionName, extension) == 0)
            return true;
    return false;
}

bool has_ui_interaction()
{
    const ImGuiIO &io = ImGui::GetIO();
    if (ImGui::IsAnyItemActive() || io.WantTextInput ||
        io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ||
        io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup))
        return true;
    for (int button = 0; button < ImGuiMouseButton_COUNT; ++button)
        if (ImGui::IsMouseDown(button))
            return true;
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
        if (ImGui::IsKeyDown(static_cast<ImGuiKey>(key)))
            return true;
    return false;
}

#ifdef APP_USE_VULKAN_DEBUG_REPORT
VKAPI_ATTR VkBool32 VKAPI_CALL debug_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT object_type,
                                            uint64_t object, size_t location, int32_t message_code,
                                            const char *layer_prefix, const char *message, void *user_data)
{
    (void)flags;
    (void)object;
    (void)location;
    (void)message_code;
    auto *logger = static_cast<base::Logger *>(user_data);
    (void)layer_prefix;
    if (logger)
    {
        try {
            logger->write((flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) != 0 ? base::LogLevel::Error : base::LogLevel::Warning,
                "platform", "event=vulkan_debug object_type=" + std::to_string(object_type) + " detail=" + (message ? message : ""));
        } catch (...) { }
    }
    else
        std::fprintf(stderr, "[vulkan] Debug report from ObjectType: %i\nMessage: %s\n\n", object_type, message);
    return VK_FALSE;
}
#endif
} // namespace

struct Desktop::Impl
{
    explicit Impl(base::Logger *logger) : logger_(logger) {}
    using DiagnosticClock = std::chrono::steady_clock;
    base::Logger *logger_ = nullptr;
    DiagnosticClock::time_point diagnostic_window_ = DiagnosticClock::now();
    std::uint64_t tick_count_ = 0;
    std::uint64_t presented_count_ = 0;
    std::uint64_t slow_ticks_ = 0;
    double tick_ms_total_ = 0;
    double tick_ms_min_ = std::numeric_limits<double>::max();
    double tick_ms_max_ = 0;
    double callback_ms_total_ = 0;
    double render_ms_total_ = 0;
    double present_ms_total_ = 0;
    double event_wait_ms_total_ = 0;
    void check_vk_result(VkResult error);
    void collect_glfw_error();
    void flush_frame_diagnostics(bool force = false) noexcept;
    GLFWwindow *window = nullptr;
    VkAllocationCallbacks *allocator = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = (uint32_t)-1;
    VkQueue queue = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    ImGui_ImplVulkanH_Window main_window{};
    uint32_t min_image_count = 2;
    bool swapchain_rebuild = false;
    float dpi_scale = 1.0f;
    ImVec4 clear_color{};
    base::FrameActivity frame_activity_;
    base::FrameSchedule frame_schedule_;
    CaptureOverlay capture_overlay_;
    base::CapturePhase logged_capture_phase_ = base::CapturePhase::Hidden;
    std::uint64_t logged_capture_request_ = 0;
    std::uint32_t logged_capture_error_ = 0;
    bool logged_capture_visible_ = false;
    void *previous_io_user_data_ = nullptr;

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    VkDebugReportCallbackEXT debug_report = VK_NULL_HANDLE;
#endif

    bool initialize();
    void run(const FrameCallback &frame);
    void shutdown();

    bool create_window();
    void center_on_primary_monitor();

    void setup_vulkan(ImVector<const char *> instance_extensions);
    void setup_window(VkSurfaceKHR surface, int width, int height);
    void cleanup_vulkan();
    void cleanup_window();

    void frame_render(ImDrawData *draw_data);
    void frame_present();
};

void Desktop::Impl::check_vk_result(VkResult error)
{
    if (error == VK_SUCCESS) return;
    if (logger_)
    {
        logger_->write(error < 0 ? base::LogLevel::Error : base::LogLevel::Warning,
            "platform", "event=vulkan_result code=" + std::to_string(error));
        if (error < 0) logger_->flush();
    }
    backend_vk_result(error);
}

void Desktop::Impl::collect_glfw_error()
{
    const char *description = nullptr;
    const int code = glfwGetError(&description);
    if (code != GLFW_NO_ERROR && logger_)
        logger_->write(base::LogLevel::Error, "platform", "event=glfw_error code=" + std::to_string(code) +
            " detail=" + (description ? description : ""));
}

void Desktop::Impl::flush_frame_diagnostics(bool force) noexcept
{
    const auto now = DiagnosticClock::now();
    const double window_ms = std::chrono::duration<double, std::milli>(now - diagnostic_window_).count();
    if ((!force && window_ms < 10000.0) || (tick_count_ == 0 && event_wait_ms_total_ == 0.0)) return;
    if (logger_)
    {
        try {
            std::ostringstream text;
            text << "event=frame_window window_ms=" << window_ms << " ticks=" << tick_count_
                 << " presented=" << presented_count_ << " slow_ticks=" << slow_ticks_
                 << " tick_work_ms_min=" << (tick_count_ ? tick_ms_min_ : 0.0)
                 << " tick_work_ms_avg=" << (tick_count_ ? tick_ms_total_ / static_cast<double>(tick_count_) : 0.0)
                 << " tick_work_ms_max=" << tick_ms_max_ << " callback_ms_total=" << callback_ms_total_
                 << " render_submit_ms_total=" << render_ms_total_ << " present_ms_total=" << present_ms_total_
                 << " event_wait_ms_total=" << event_wait_ms_total_ << " timing_scope=cpu_wall gpu_render_time=N/A";
            logger_->write(slow_ticks_ ? base::LogLevel::Warning : base::LogLevel::Info, "platform", text.str());
        } catch (...) { }
    }
    diagnostic_window_ = now;
    tick_count_ = presented_count_ = slow_ticks_ = 0;
    tick_ms_total_ = tick_ms_max_ = callback_ms_total_ = render_ms_total_ = present_ms_total_ = event_wait_ms_total_ = 0;
    tick_ms_min_ = std::numeric_limits<double>::max();
}
bool Desktop::Impl::initialize()
{
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
    {
        collect_glfw_error();
        return false;
    }

    if (!create_window())
    {
        collect_glfw_error();
        if (logger_) logger_->write(base::LogLevel::Error, "platform", "event=window_initialization_failed");
        return false;
    }

    ImVector<const char *> extensions;
    uint32_t extensions_count = 0;
    const char **glfw_extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
    for (uint32_t i = 0; i < extensions_count; i++)
        extensions.push_back(glfw_extensions[i]);
    setup_vulkan(extensions);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult err = glfwCreateWindowSurface(instance, window, allocator, &surface);
    check_vk_result(err);

    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    setup_window(surface, width, height);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    previous_io_user_data_ = io.UserData;
    io.UserData = &frame_activity_;
    io.IniFilename = nullptr;
    hmi::ui::LoadFonts();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    hmi::ui::ApplyTheme(dpi_scale);
    glfwSetWindowContentScaleCallback(window, glfw_content_scale_callback);

    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = instance;
    init_info.PhysicalDevice = physical_device;
    init_info.Device = device;
    init_info.QueueFamily = queue_family;
    init_info.Queue = queue;
    init_info.PipelineCache = pipeline_cache;
    init_info.DescriptorPool = descriptor_pool;
    init_info.MinImageCount = min_image_count;
    init_info.ImageCount = main_window.ImageCount;
    init_info.Allocator = allocator;
    init_info.PipelineInfoMain.RenderPass = main_window.RenderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = backend_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    clear_color = hmi::ui::WindowClearColor();
    return true;
}

bool Desktop::Impl::create_window()
{
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    // Create hidden, position, then show - otherwise the window flashes at the
    // OS default spot before jumping to the center.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    dpi_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());

    int window_width = static_cast<int>(kDefaultWidth * dpi_scale);
    int window_height = static_cast<int>(kDefaultHeight * dpi_scale);
    GLFWmonitor *primary_monitor = glfwGetPrimaryMonitor();
    int area_x = 0, area_y = 0, area_width = 0, area_height = 0;
    if (primary_monitor)
        glfwGetMonitorWorkarea(primary_monitor, &area_x, &area_y, &area_width, &area_height);
    if (area_width > 0 && area_height > 0)
    {
        // Never open larger than the usable desktop, otherwise a 150% DPI
        // display would push the title bar under the screen edge.
        window_width = std::min(window_width, area_width);
        window_height = std::min(window_height, area_height);
    }

    window = glfwCreateWindow(window_width, window_height, "语音助手", nullptr, nullptr);
    if (!window)
        return false;

    const int minimum_width = std::min(window_width, static_cast<int>(640.0f * dpi_scale));
    const int minimum_height = std::min(window_height, static_cast<int>(480.0f * dpi_scale));
    glfwSetWindowSizeLimits(window, minimum_width, minimum_height, GLFW_DONT_CARE, GLFW_DONT_CARE);
    center_on_primary_monitor();

    if (!glfwVulkanSupported())
    {
        std::fprintf(stderr, "GLFW: Vulkan Not Supported\n");
        if (logger_) logger_->write(base::LogLevel::Error, "platform", "event=vulkan_not_supported");
        return false;
    }
    return true;
}

void Desktop::Impl::center_on_primary_monitor()
{
    GLFWmonitor *monitor = glfwGetPrimaryMonitor();
    if (!monitor)
        return;

    int area_x = 0, area_y = 0, area_width = 0, area_height = 0;
    glfwGetMonitorWorkarea(monitor, &area_x, &area_y, &area_width, &area_height);
    if (area_width <= 0 || area_height <= 0)
        return;

    int window_width = 0, window_height = 0;
    glfwGetWindowSize(window, &window_width, &window_height);
    glfwSetWindowPos(window, area_x + (area_width - window_width) / 2,
                     area_y + (area_height - window_height) / 2);
}

void Desktop::Impl::setup_vulkan(ImVector<const char *> instance_extensions)
{
    VkResult err;
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
    volkInitialize();
#endif

    // Create Vulkan Instance
    {
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        // Enumerate available extensions
        uint32_t properties_count = 0;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
        check_vk_result(err);

        // Enable required extensions
        if (is_extension_available(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (is_extension_available(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
        {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

        // Enabling validation layers
#ifdef APP_USE_VULKAN_DEBUG_REPORT
        const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;
        instance_extensions.push_back("VK_EXT_debug_report");
#endif

        // Create Vulkan Instance
        create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&create_info, allocator, &instance);
        check_vk_result(err);
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
        volkLoadInstance(instance);
#endif

        // Setup the debug report callback
#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto f_vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT");
        IM_ASSERT(f_vkCreateDebugReportCallbackEXT != nullptr);
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
        debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        debug_report_ci.pfnCallback = ::hmi::platform::debug_report;
        debug_report_ci.pUserData = logger_;
        err = f_vkCreateDebugReportCallbackEXT(instance, &debug_report_ci, allocator, &debug_report);
        check_vk_result(err);
#endif
    }

    // Select Physical Device (GPU)
    physical_device = ImGui_ImplVulkanH_SelectPhysicalDevice(instance);
    IM_ASSERT(physical_device != VK_NULL_HANDLE);

    // Select graphics queue family
    queue_family = ImGui_ImplVulkanH_SelectQueueFamilyIndex(physical_device);
    IM_ASSERT(queue_family != (uint32_t)-1);

    // Create Logical Device (with 1 queue)
    {
        ImVector<const char *> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");

        // Enumerate physical device extension
        uint32_t properties_count = 0;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &properties_count, properties.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (is_extension_available(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

        const float queue_priority[] = {1.0f};
        VkDeviceQueueCreateInfo queue_info[1] = {};
        queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = queue_family;
        queue_info[0].queueCount = 1;
        queue_info[0].pQueuePriorities = queue_priority;
        VkDeviceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = sizeof(queue_info) / sizeof(queue_info[0]);
        create_info.pQueueCreateInfos = queue_info;
        create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
        create_info.ppEnabledExtensionNames = device_extensions.Data;
        err = vkCreateDevice(physical_device, &create_info, allocator, &device);
        check_vk_result(err);
        vkGetDeviceQueue(device, queue_family, 0, &queue);
    }

    // Create Descriptor Pool
    {
        VkDescriptorPoolSize pool_sizes[] =
            {
                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE},
                {VK_DESCRIPTOR_TYPE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE},
            };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 0;
        for (VkDescriptorPoolSize &pool_size : pool_sizes)
            pool_info.maxSets += pool_size.descriptorCount;
        pool_info.poolSizeCount = (uint32_t)IM_COUNTOF(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        err = vkCreateDescriptorPool(device, &pool_info, allocator, &descriptor_pool);
        check_vk_result(err);
    }
}

void Desktop::Impl::setup_window(VkSurfaceKHR surface, int width, int height)
{
    // Check for WSI support
    VkBool32 res = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, queue_family, surface, &res);
    if (res != VK_TRUE)
    {
        std::fprintf(stderr, "Error no WSI support on physical device 0\n");
        std::exit(-1);
    }

    // Select Surface Format
    const VkFormat requestSurfaceImageFormat[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM};
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    main_window.Surface = surface;
    main_window.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(physical_device, main_window.Surface, requestSurfaceImageFormat, (size_t)IM_COUNTOF(requestSurfaceImageFormat), requestSurfaceColorSpace);

    // Select Present Mode
#ifdef APP_USE_UNLIMITED_FRAME_RATE
    VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR};
#else
    VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
#endif
    main_window.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(physical_device, main_window.Surface, &present_modes[0], IM_COUNTOF(present_modes));

    // Create SwapChain, RenderPass, Framebuffer, etc.
    IM_ASSERT(min_image_count >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(instance, physical_device, device, &main_window, queue_family, allocator, width, height, min_image_count, 0);
}

void Desktop::Impl::cleanup_vulkan()
{
    if (device != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(device, descriptor_pool, allocator);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto f_vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugReportCallbackEXT");
        if (f_vkDestroyDebugReportCallbackEXT)
            f_vkDestroyDebugReportCallbackEXT(instance, debug_report, allocator);
#endif

        vkDestroyDevice(device, allocator);
        device = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(instance, allocator);
        instance = VK_NULL_HANDLE;
    }
}

void Desktop::Impl::cleanup_window()
{
    if (main_window.Surface != VK_NULL_HANDLE && device != VK_NULL_HANDLE)
    {
        ImGui_ImplVulkanH_DestroyWindow(instance, device, &main_window, allocator);
        vkDestroySurfaceKHR(instance, main_window.Surface, allocator);
        main_window.Surface = VK_NULL_HANDLE;
    }
}

void Desktop::Impl::frame_render(ImDrawData *draw_data)
{
    VkSemaphore image_acquired_semaphore = main_window.FrameSemaphores[main_window.SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = main_window.FrameSemaphores[main_window.SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(device, main_window.Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &main_window.FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        swapchain_rebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);

    ImGui_ImplVulkanH_Frame *fd = &main_window.Frames[main_window.FrameIndex];
    {
        err = vkWaitForFences(device, 1, &fd->Fence, VK_TRUE, UINT64_MAX); // wait indefinitely instead of periodically checking
        check_vk_result(err);

        err = vkResetFences(device, 1, &fd->Fence);
        check_vk_result(err);
    }
    {
        err = vkResetCommandPool(device, fd->CommandPool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_vk_result(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = main_window.RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = main_window.Width;
        info.renderArea.extent.height = main_window.Height;
        info.clearValueCount = 1;
        info.pClearValues = &main_window.ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    // Record dear imgui primitives into command buffer
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    // Submit command buffer
    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_vk_result(err);
        err = vkQueueSubmit(queue, 1, &info, fd->Fence);
        check_vk_result(err);
    }
}

void Desktop::Impl::frame_present()
{
    if (swapchain_rebuild)
        return;
    VkSemaphore render_complete_semaphore = main_window.FrameSemaphores[main_window.SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &main_window.Swapchain;
    info.pImageIndices = &main_window.FrameIndex;
    VkResult err = vkQueuePresentKHR(queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        swapchain_rebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);
    main_window.SemaphoreIndex = (main_window.SemaphoreIndex + 1) % main_window.SemaphoreCount;
}

void Desktop::Impl::run(const FrameCallback &frame)
{
    struct FlushDiagnostics
    {
        Impl &owner;
        ~FlushDiagnostics()
        {
            owner.capture_overlay_.hide();
            try { owner.flush_frame_diagnostics(true); } catch (...) {}
        }
    } flush{*this};
    diagnostic_window_ = DiagnosticClock::now();
    glfwShowWindow(window);
    if (logger_) logger_->write(base::LogLevel::Info, "platform", "event=frame_sampling_started window_ms=10000 slow_tick_threshold_ms=50 backend_callback_errors=stderr direct_vk_errors=file");

    while (!glfwWindowShouldClose(window))
    {
        const double wait_seconds = frame_schedule_.wait_seconds(glfwGetTime());
        if (wait_seconds > 0.0)
        {
            // Events may wake early; the deadline prevents mouse movement
            // from turning the service loop into an unbounded render loop.
            const auto waiting = DiagnosticClock::now();
            glfwWaitEventsTimeout(wait_seconds);
            event_wait_ms_total_ += std::chrono::duration<double, std::milli>(DiagnosticClock::now() - waiting).count();
            collect_glfw_error();
            flush_frame_diagnostics();
            continue;
        }
        const auto tick_wall = DiagnosticClock::now();
        glfwPollEvents();
        collect_glfw_error();
        if (glfwWindowShouldClose(window))
            break;
        const double tick_started = glfwGetTime();

        // Resize swap chain?
        int fb_width = 0, fb_height = 0;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        const bool iconified = glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0;
        const bool surface_changed = !iconified && fb_width > 0 && fb_height > 0 &&
            (swapchain_rebuild || main_window.Width != fb_width || main_window.Height != fb_height);
        if (surface_changed)
        {
            ImGui_ImplVulkan_SetMinImageCount(min_image_count);
            ImGui_ImplVulkanH_CreateOrResizeWindow(instance, physical_device, device, &main_window, queue_family, allocator, fb_width, fb_height, min_image_count, 0);
            main_window.FrameIndex = 0;
            swapchain_rebuild = false;
        }
        // Start the Dear ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const auto callback_started = DiagnosticClock::now();
        if (frame)
            frame(window);
        callback_ms_total_ += std::chrono::duration<double, std::milli>(DiagnosticClock::now() - callback_started).count();

        // This native window remains drawable while the main window is minimized.
        capture_overlay_.update(frame_activity_.capture);
        const auto capture_phase = frame_activity_.capture.phase;
        const auto capture_request = frame_activity_.capture.request_id;
        const auto capture_error = capture_overlay_.last_error();
        const bool capture_visible = capture_overlay_.visible();
        if (capture_phase != logged_capture_phase_ || capture_request != logged_capture_request_ ||
            capture_error != logged_capture_error_ || capture_visible != logged_capture_visible_)
        {
            if (logger_)
                logger_->write(capture_error ? base::LogLevel::Warning : base::LogLevel::Info, "platform",
                    std::string("event=capture_overlay phase=") + capture_phase_name(capture_phase) +
                    " request_id=" + std::to_string(capture_request) +
                    " visible=" + (capture_visible ? "true" : "false") +
                    " error=" + std::to_string(capture_error));
            logged_capture_phase_ = capture_phase;
            logged_capture_request_ = capture_request;
            logged_capture_error_ = capture_error;
            logged_capture_visible_ = capture_visible;
        }

        const bool interaction = has_ui_interaction();
        // End every logical frame to preserve ImGui input/widget state even
        // when no GPU work is submitted. Services also run while iconified.
        ImGui::Render();
        ImDrawData *draw_data = ImGui::GetDrawData();
        const bool drawable = !iconified && fb_width > 0 && fb_height > 0 &&
            draw_data->DisplaySize.x > 0.0f && draw_data->DisplaySize.y > 0.0f;
        if (frame_schedule_.present_due(tick_started, drawable,
                                      frame_activity_.animation_active, interaction, surface_changed))
        {
            main_window.ClearValue.color.float32[0] = clear_color.x * clear_color.w;
            main_window.ClearValue.color.float32[1] = clear_color.y * clear_color.w;
            main_window.ClearValue.color.float32[2] = clear_color.z * clear_color.w;
            main_window.ClearValue.color.float32[3] = clear_color.w;
            const auto render_started = DiagnosticClock::now();
            frame_render(draw_data);
            const auto present_started = DiagnosticClock::now();
            render_ms_total_ += std::chrono::duration<double, std::milli>(present_started - render_started).count();
            frame_present();
            present_ms_total_ += std::chrono::duration<double, std::milli>(DiagnosticClock::now() - present_started).count();
            ++presented_count_;
        }
        frame_schedule_.finish_tick(tick_started);
        const double work_ms = std::chrono::duration<double, std::milli>(DiagnosticClock::now() - tick_wall).count();
        ++tick_count_;
        tick_ms_total_ += work_ms;
        tick_ms_min_ = std::min(tick_ms_min_, work_ms);
        tick_ms_max_ = std::max(tick_ms_max_, work_ms);
        if (work_ms > 50.0) ++slow_ticks_;
        flush_frame_diagnostics();
    }
    flush_frame_diagnostics(true);
}

void Desktop::Impl::shutdown()
{
    capture_overlay_.hide();
    if (device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(device);

    if (ImGui::GetCurrentContext())
    {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGuiIO &io = ImGui::GetIO();
        if (io.UserData == &frame_activity_)
            io.UserData = previous_io_user_data_;
        ImGui::DestroyContext();
    }

    cleanup_window();
    cleanup_vulkan();

    if (window)
    {
        glfwDestroyWindow(window);
        window = nullptr;
    }
    glfwTerminate();
}

Desktop::Desktop(base::Logger *logger) : impl_(std::make_unique<Impl>(logger))
{
    initialized_ = impl_->initialize();
}

Desktop::~Desktop()
{
    if (initialized_ || impl_->window || ImGui::GetCurrentContext())
        impl_->shutdown();
}

bool Desktop::available() const noexcept
{
    return initialized_ && impl_ && impl_->window != nullptr;
}

void Desktop::run(const FrameCallback &frame, base::Logger *logger)
{
    if (logger) impl_->logger_ = logger;
    if (available())
        impl_->run(frame);
}
}
