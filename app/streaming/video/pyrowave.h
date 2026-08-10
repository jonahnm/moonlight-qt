/**
 * @file app/streaming/video/pyrowave.h
 * @brief PyroWave (Vulkan wavelet, intra-only) client decoder for Moonlight.
 *
 * Stage B4 (GPU zero-copy present): PyroWave decodes on its own (headless) Vulkan device into
 * exportable dmabuf plane images; those planes are imported into a libplacebo Vulkan device which
 * does YUV->RGB + swapchain present. No CPU readback.
 *
 * Compiled only when HAVE_PYROWAVE is defined.
 */
#pragma once

#ifdef HAVE_PYROWAVE

  #include "decoder.h"
  #include "overlaymanager.h"
  #include "../bandwidth.h"

  #include <SDL.h>
  #include <SDL_vulkan.h>
  // VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME (mandatory for MoltenVK devices) lives in
  // vulkan_beta.h, which vulkan.h only pulls in with this define.
  #ifndef VK_ENABLE_BETA_EXTENSIONS
    #define VK_ENABLE_BETA_EXTENSIONS 1
  #endif
  // Pulls in vulkan_metal.h (VkMetalSurfaceCreateInfoEXT, VK_EXT_metal_surface) for the
  // direct Metal surface creation in createAppleSurface().
  #ifndef VK_USE_PLATFORM_METAL_EXT
    #define VK_USE_PLATFORM_METAL_EXT 1
  #endif
  #include <vulkan/vulkan.h>

  #include <libplacebo/vulkan.h>
  #include <libplacebo/renderer.h>

  #include <atomic>
  #include <mutex>
  #include <vector>

struct pyrowave_device_opaque;
struct pyrowave_decoder_opaque;
struct pyrowave_sync_object_opaque;

class PyroWaveVideoDecoder : public IVideoDecoder, public Overlay::IOverlayRenderer {
public:
    PyroWaveVideoDecoder(bool testOnly);
    virtual ~PyroWaveVideoDecoder() override;

    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool isHardwareAccelerated() override;
    virtual bool isAlwaysFullScreen() override;
    virtual bool isHdrSupported() override;
    virtual int getDecoderCapabilities() override;
    virtual int getDecoderColorspace() override;
    virtual int getDecoderColorRange() override;
    virtual QSize getDecoderMaxResolution() override;
    virtual int submitDecodeUnit(PDECODE_UNIT du) override;
    virtual void renderFrameOnMainThread() override;
    virtual void setHdrMode(bool enabled) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) override;

    // Overlay::IOverlayRenderer
    virtual void notifyOverlayUpdated(Overlay::OverlayType type) override;

private:
    bool createOverlay(pl_overlay* overlay, SDL_Surface* surface);
    static void overlayUploadComplete(void* opaque);
    void addVideoStats(VIDEO_STATS& src, VIDEO_STATS& dst);
    void stringifyVideoStats(VIDEO_STATS& stats, char* output, int length);
    // One exportable R8 decode-plane image on PyroWave's device, imported into libplacebo.
    struct Plane {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;   // for PyroWave decode target
        int w = 0, h = 0;
        VkDeviceSize size = 0;
        uint64_t modifier = 0;
        VkDeviceSize offset = 0, pitch = 0;
        int fd = -1;
        pl_tex plTex = nullptr;              // libplacebo import of the same dmabuf
    };

    bool createPyroDevice();
    bool createPlanes();
    bool createLibplacebo(PDECODER_PARAMETERS params);
    bool importPlanes();
    bool createSharedTimeline();  // libplacebo-created timeline sem, imported into PyroWave

#if defined(__APPLE__)
    // macOS/MoltenVK: the Vulkan instance, window surface and device are created here so that
    // libplacebo (via pl_vulkan_import) and PyroWave (via pyrowave_create_device) share one
    // MoltenVK device. No external memory/semaphore interop exists on macOS, so the decode
    // planes are plain storable libplacebo textures and the timeline semaphore is shared by
    // direct handle (m_PwSem == m_PlSem).
    bool createAppleVulkan();    // instance + surface + device + libplacebo import/swapchain/renderer
    bool createAppleSurface();   // SDL surface + libplacebo swapchain/renderer (real streams only)
    bool createApplePyroDevice();// pyrowave_create_device() sharing the handles above
    bool createApplePlanes();    // storable/sampleable pl_tex planes
#endif

    bool m_TestOnly;
    int m_Width;
    int m_Height;
    bool m_YUV444;
    bool m_TenBit;
    std::atomic<bool> m_HdrEnabled;      // host HDR state (control stream); read on render thread
    pl_color_space m_LastColorspace;     // render thread only; drives swapchain colorspace hints
    SDL_Window* m_Window;

    // PyroWave decode side (its own headless device).
    pyrowave_device_opaque* m_PyroDevice;
    pyrowave_decoder_opaque* m_Decoder;
    VkDevice m_VkDev;
    VkPhysicalDevice m_VkPhys;
    VkQueue m_VkQueue;
    uint32_t m_VkFamily;
    VkCommandPool m_VkPool;
    VkCommandBuffer m_VkCmd;
    Plane m_Planes[3];
    PFN_vkGetMemoryFdKHR m_GetMemoryFd;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT m_GetImgMod;

    // libplacebo present side (its own device + swapchain on the window surface).
    pl_log m_Log;
    pl_vk_inst m_PlVkInstance;
    VkSurfaceKHR m_VkSurface;
    pl_vulkan m_Vulkan;
    pl_swapchain m_Swapchain;
    pl_renderer m_Renderer;
    PFN_vkDestroySurfaceKHR m_DestroySurface;

    // Shared timeline semaphore for cross-device decode->present sync (replaces vkDeviceWaitIdle).
    // Created+exported by libplacebo, imported into PyroWave; both signal/wait the same timeline.
    VkSemaphore m_PlSem;                       // libplacebo-side handle
    pyrowave_sync_object_opaque* m_SyncObj;    // PyroWave import
    VkSemaphore m_PwSem;                        // PyroWave-side handle
    std::atomic<uint64_t> m_TlNext;            // monotonic value dispenser
    std::atomic<uint64_t> m_LastHoldVal;       // last value libplacebo hold_ex will signal
    std::atomic<uint64_t> m_LastReleaseVal;    // last value PyroWave decode release signals
    bool m_TimelineReady;

    std::mutex m_FrameLock;
    bool m_FrameReady;

    // Performance-overlay stats (compact version of FFmpegVideoDecoder's). Decode/network stats are
    // updated on the decode thread in submitDecodeUnit; render stats come from the main thread via
    // atomics folded in at each 1-second window flip.
    VIDEO_STATS m_ActiveWndVideoStats = {};
    VIDEO_STATS m_LastWndVideoStats = {};
    BandwidthTracker m_BwTracker;
    uint32_t m_LastFrameNumber;
    std::atomic<uint32_t> m_RenderedFrames;
    std::atomic<uint64_t> m_TotalRenderTimeUs;

    // Performance/status overlay compositing (same model as PlVkRenderer): notifyOverlayUpdated
    // uploads text surfaces to staging textures; renderFrameOnMainThread promotes + composites them.
    SDL_SpinLock m_OverlayLock;
    struct OverlayState {
        bool hasOverlay = false;
        pl_overlay overlay = {};
        bool hasStagingOverlay = false;
        pl_overlay stagingOverlay = {};
    } m_Overlays[Overlay::OverlayMax];

#if defined(__APPLE__)
    // Lifecycle data for the instance/device Moonlight created for both libplacebo and PyroWave.
    // The create-info structs must outlive the pyrowave device, so they live as members.
    VkInstance m_VkInstance;
    VkApplicationInfo m_AppleAppInfo;
    VkInstanceCreateInfo m_AppleInstCreateInfo;
    VkDeviceQueueCreateInfo m_AppleQueueCreateInfo;
    VkDeviceCreateInfo m_AppleDevCreateInfo;
    VkPhysicalDeviceFeatures2 m_AppleDevFeatures;
    VkPhysicalDeviceVulkan11Features m_AppleDevVk11Features;
    VkPhysicalDeviceVulkan12Features m_AppleDevVk12Features;
    VkPhysicalDeviceVulkan13Features m_AppleDevVk13Features;
    VkPhysicalDeviceShaderFloat16Int8Features m_AppleDevFloat16Int8Features;
    std::vector<const char*> m_AppleInstanceExtensions;
    std::vector<const char*> m_AppleDeviceExtensions;
    PFN_vkGetInstanceProcAddr m_AppleGetProcAddr;
    float m_AppleQueuePriority;
    uint32_t m_AppleQueueFamily;
    SDL_MetalView m_AppleMetalView;
#endif
};

#endif  // HAVE_PYROWAVE
