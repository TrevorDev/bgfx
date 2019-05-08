// XR
#include "pch.h"
#include "common.h"
#include <common/xr_linear.h>

// BGFX
#include <bx/bx.h>
#include <bx/file.h>
#include <bx/sort.h>
#include "bgfx\bgfx.h"
#include "bgfx\platform.h"
#include <unordered_map>

struct Swapchain {
    XrSwapchain handle;
    int32_t width;
    int32_t height;
};

class OpenXRLib {
   public:
    OpenXRLib() {}

    void init(const std::vector<std::string> platformExtensions = {},
              const std::vector<std::string> graphicsExtensions = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME},
              XrBaseInStructure* createInstanceExtension = nullptr, XrFormFactor formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
              // XrViewConfigurationType configurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
              // XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
              const char* xrApplicationName = "XR App") {
        // TODO enumerate layers and extensions if needed

        CHECK(m_instance == XR_NULL_HANDLE);

        // Create union of extensions required by platform and graphics plugins.
        std::vector<const char*> extensions;

        // Transform platform and graphics extension std::strings to C strings.
        std::transform(platformExtensions.begin(), platformExtensions.end(), std::back_inserter(extensions),
                       [](const std::string& ext) { return ext.c_str(); });
        std::transform(graphicsExtensions.begin(), graphicsExtensions.end(), std::back_inserter(extensions),
                       [](const std::string& ext) { return ext.c_str(); });

        XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        createInfo.next = createInstanceExtension;
        createInfo.enabledExtensionCount = (uint32_t)extensions.size();
        createInfo.enabledExtensionNames = extensions.data();

        strcpy(createInfo.applicationInfo.applicationName, xrApplicationName);
        createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

        CHECK_XRCMD(xrCreateInstance(&createInfo, &m_instance));

        // Log instance info
        CHECK(m_instance != XR_NULL_HANDLE);

        XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
        CHECK_XRCMD(xrGetInstanceProperties(m_instance, &instanceProperties));

        Log::Write(Log::Level::Info, Fmt("Instance RuntimeName=%s RuntimeVersion=%s", instanceProperties.runtimeName,
                                         GetXrVersionString(instanceProperties.runtimeVersion).c_str()));

        // Initialize system
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = formFactor;
        CHECK_XRCMD(xrGetSystem(m_instance, &systemInfo, &m_systemId));
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_systemId != XR_NULL_SYSTEM_ID);

        // TODO Init gfx device now using m_instance and m_systemId
    }

    bool isSessionRunning() {
        switch (m_sessionState) {
            case XR_SESSION_STATE_RUNNING:
            case XR_SESSION_STATE_VISIBLE:
            case XR_SESSION_STATE_FOCUSED:
                return true;
        }
        return false;
    }

    void initializeSession() {
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_session == XR_NULL_HANDLE);

        // Create session
        {
            Log::Write(Log::Level::Verbose, Fmt("Creating session..."));

            XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};

            m_graphicsBinding.device = getDeviceReference();
            createInfo.next = &m_graphicsBinding;

            createInfo.systemId = m_systemId;
            CHECK_XRCMD(xrCreateSession(m_instance, &createInfo, &m_session));
        }

        // Log reference space
        CHECK(m_session != XR_NULL_HANDLE);
        uint32_t spaceCount;
        CHECK_XRCMD(xrEnumerateReferenceSpaces(m_session, 0, &spaceCount, nullptr));
        std::vector<XrReferenceSpaceType> spaces(spaceCount);
        CHECK_XRCMD(xrEnumerateReferenceSpaces(m_session, spaceCount, &spaceCount, spaces.data()));
        Log::Write(Log::Level::Info, Fmt("Available reference spaces: %d", spaceCount));
        for (XrReferenceSpaceType space : spaces) {
            Log::Write(Log::Level::Verbose, Fmt("  Name: %s", GetXrReferenceSpaceTypeString(space).c_str()));
        }

        // CreateVisualizedSpaces
        CHECK(m_session != XR_NULL_HANDLE);

        XrReferenceSpaceType visualizedSpaces[] = {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                                   XR_REFERENCE_SPACE_TYPE_STAGE};

        for (auto visualizedSpace : visualizedSpaces) {
            XrReferenceSpaceCreateInfo referenceSpaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            XrPosef t{};
            t.orientation.w = 1;
            referenceSpaceCreateInfo.poseInReferenceSpace = t;
            referenceSpaceCreateInfo.referenceSpaceType = visualizedSpace;

            XrSpace space;
            XrResult res = xrCreateReferenceSpace(m_session, &referenceSpaceCreateInfo, &space);
            if (XR_SUCCEEDED(res)) {
                m_visualizedSpaces.push_back(space);
            } else {
                Log::Write(Log::Level::Warning,
                           Fmt("Failed to create one of the reference spaces with error %d for visualization", res));
            }
        }

        // Set the app space
        {
            XrReferenceSpaceCreateInfo referenceSpaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            XrPosef t{};
            t.orientation.w = 1;
            referenceSpaceCreateInfo.poseInReferenceSpace = t;
            referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            CHECK_XRCMD(xrCreateReferenceSpace(m_session, &referenceSpaceCreateInfo, &m_appSpace));
        }
    }

    void createSwapchains() {
        CHECK(m_session != XR_NULL_HANDLE);
        CHECK(m_swapchains.size() == 0);
        CHECK(m_configViews.empty());

        // Read graphics properties for preferred swapchain length and logging.
        XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
        CHECK_XRCMD(xrGetSystemProperties(m_instance, m_systemId, &systemProperties));

        // Log system properties.
        Log::Write(Log::Level::Info,
                   Fmt("System Properties: Name=%s VendorId=%d", systemProperties.systemName, systemProperties.vendorId));
        Log::Write(Log::Level::Info, Fmt("System Graphics Properties: MaxWidth=%d MaxHeight=%d MaxViews=%d",
                                         systemProperties.graphicsProperties.maxSwapchainImageWidth,
                                         systemProperties.graphicsProperties.maxSwapchainImageHeight,
                                         systemProperties.graphicsProperties.maxViewCount));
        Log::Write(Log::Level::Info, Fmt("System Tracking Properties: OrientationTracking=%s PositionTracking=%s",
                                         systemProperties.trackingProperties.orientationTracking ? "True" : "False",
                                         systemProperties.trackingProperties.positionTracking ? "True" : "False"));

        // Note: No other view configurations exist at the time this code was written. If this condition
        // is not met, the project will need to be audited to see how support should be added.
        CHECK_MSG(m_viewConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, "Unsupported view configuration type");

        // Query and cache view configuration views.
        uint32_t viewCount;
        CHECK_XRCMD(xrEnumerateViewConfigurationViews(m_instance, m_systemId, m_viewConfigType, 0, &viewCount, nullptr));
        m_configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        CHECK_XRCMD(xrEnumerateViewConfigurationViews(m_instance, m_systemId, m_viewConfigType, viewCount, &viewCount,
                                                      m_configViews.data()));

        // Create and cache view buffer for xrLocateViews later.
        m_views.resize(viewCount, {XR_TYPE_VIEW});

        // Create the swapchain and get the images.
        if (viewCount > 0) {
            // Select a swapchain format.
            uint32_t swapchainFormatCount;
            CHECK_XRCMD(xrEnumerateSwapchainFormats(m_session, 0, &swapchainFormatCount, nullptr));
            std::vector<int64_t> swapchainFormats(swapchainFormatCount);
            CHECK_XRCMD(xrEnumerateSwapchainFormats(m_session, (uint32_t)swapchainFormats.size(), &swapchainFormatCount,
                                                    swapchainFormats.data()));
            CHECK(swapchainFormatCount == swapchainFormats.size());
            m_colorSwapchainFormat = selectColorSwapchainFormat(swapchainFormats);

            // Print swapchain formats and the selected one.
            {
                std::string swapchainFormatsString;
                for (int64_t format : swapchainFormats) {
                    const bool selected = format == m_colorSwapchainFormat;
                    swapchainFormatsString += " ";
                    if (selected) swapchainFormatsString += "[";
                    swapchainFormatsString += std::to_string(format);
                    if (selected) swapchainFormatsString += "]";
                }
                Log::Write(Log::Level::Verbose, Fmt("Swapchain Formats:%s", swapchainFormatsString.c_str()));
            }

            // Create a swapchain for each view.
            for (uint32_t i = 0; i < viewCount; i++) {
                const XrViewConfigurationView& vp = m_configViews[i];
                Log::Write(Log::Level::Info,
                           Fmt("Creating swapchain for view %d with dimensions Width=%d Height=%d SampleCount=%d", i,
                               vp.recommendedImageRectWidth, vp.recommendedImageRectHeight, vp.recommendedSwapchainSampleCount));

                // Create the swapchain.
                XrSwapchainCreateInfo swapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                swapchainCreateInfo.arraySize = 1;
                swapchainCreateInfo.format = m_colorSwapchainFormat;
                swapchainCreateInfo.width = vp.recommendedImageRectWidth;
                swapchainCreateInfo.height = vp.recommendedImageRectHeight;
                swapchainCreateInfo.mipCount = 1;
                swapchainCreateInfo.faceCount = 1;
                swapchainCreateInfo.sampleCount = vp.recommendedSwapchainSampleCount;
                swapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                Swapchain swapchain;
                swapchain.width = swapchainCreateInfo.width;
                swapchain.height = swapchainCreateInfo.height;
                CHECK_XRCMD(xrCreateSwapchain(m_session, &swapchainCreateInfo, &swapchain.handle));

                m_swapchains.push_back(swapchain);

                uint32_t imageCount;
                CHECK_XRCMD(xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr));
                // XXX This should really just return XrSwapchainImageBaseHeader*
                std::vector<XrSwapchainImageBaseHeader*> swapchainImages =
                    allocateSwapchainImageStructs(imageCount, swapchainCreateInfo);
                CHECK_XRCMD(xrEnumerateSwapchainImages(swapchain.handle, imageCount, &imageCount, swapchainImages[0]));

                m_swapchainImages.insert(std::make_pair(swapchain.handle, std::move(swapchainImages)));
            }
        }
    }

    // Return event if one is available, otherwise return null.
    const XrEventDataBaseHeader* TryReadNextEvent() {
        // It is sufficient to clear the just the XrEventDataBuffer header to XR_TYPE_EVENT_DATA_BUFFER
        XrEventDataBaseHeader* baseHeader = reinterpret_cast<XrEventDataBaseHeader*>(&m_eventDataBuffer);
        *baseHeader = {XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult xr = xrPollEvent(m_instance, &m_eventDataBuffer);
        if (xr == XR_SUCCESS) {
            if (baseHeader->type == XR_TYPE_EVENT_DATA_EVENTS_LOST) {
                const XrEventDataEventsLost* const eventsLost = reinterpret_cast<const XrEventDataEventsLost*>(baseHeader);
                Log::Write(Log::Level::Warning, Fmt("%d events lost", eventsLost));
            }

            return baseHeader;
        } else if (xr == XR_EVENT_UNAVAILABLE) {
            return nullptr;
        } else {
            THROW_XR(xr, "xrPollEvent");
        }
    }

    void ManageSession(const XrEventDataSessionStateChanged& lifecycle, bool* exitRenderLoop, bool* requestRestart) {
        static std::map<XrSessionState, const std::string> stateName = {
            {XR_SESSION_STATE_UNKNOWN, "UNKNOWN"},   {XR_SESSION_STATE_IDLE, "IDLE"},
            {XR_SESSION_STATE_READY, "READY"},       {XR_SESSION_STATE_RUNNING, "RUNNING"},
            {XR_SESSION_STATE_VISIBLE, "VISIBLE"},   {XR_SESSION_STATE_FOCUSED, "FOCUSED"},
            {XR_SESSION_STATE_STOPPING, "STOPPING"}, {XR_SESSION_STATE_LOSS_PENDING, "LOSS_PENDING"},
            {XR_SESSION_STATE_EXITING, "EXITING"},
        };

        XrSessionState oldState = m_sessionState;
        m_sessionState = lifecycle.state;

        const std::string& oldStateName = stateName[oldState];
        const std::string& newStateName = stateName[m_sessionState];
        Log::Write(Log::Level::Info, Fmt("XrEventDataSessionStateChanged: state %s->%s session=%lld time=%lld",
                                         oldStateName.c_str(), newStateName.c_str(), lifecycle.session, lifecycle.time));

        if (lifecycle.session && (lifecycle.session != m_session)) {
            Log::Write(Log::Level::Error, "XrEventDataSessionStateChanged for unknown session");
            return;
        }

        switch (m_sessionState) {
            case XR_SESSION_STATE_READY: {
                CHECK(m_session != XR_NULL_HANDLE);
                XrSessionBeginInfo sessionBeginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                sessionBeginInfo.primaryViewConfigurationType = m_viewConfigType;
                XrResult res = xrBeginSession(m_session, &sessionBeginInfo);
                if (res == XR_SESSION_VISIBILITY_UNAVAILABLE) {
                    Log::Write(Log::Level::Warning, "xrBeginSession returned XR_SESSION_VISIBILITY_UNAVAILABLE");
                } else {
                    CHECK_XRRESULT(res, "xrBeginSession");
                }
                break;
            }
            case XR_SESSION_STATE_STOPPING: {
                CHECK(m_session != XR_NULL_HANDLE);
                CHECK_XRCMD(xrEndSession(m_session))
                break;
            }
            case XR_SESSION_STATE_EXITING: {
                *exitRenderLoop = true;
                // Do not attempt to restart because user closed this session.
                *requestRestart = false;
                break;
            }
            case XR_SESSION_STATE_LOSS_PENDING: {
                *exitRenderLoop = true;
                // Poll for a new instance
                *requestRestart = true;
                break;
            }
        }
    }

    void pollEvents(bool* exitRenderLoop, bool* requestRestart) {
        *exitRenderLoop = *requestRestart = false;

        // Process all pending messages.
        while (const XrEventDataBaseHeader* event = TryReadNextEvent()) {
            switch (event->type) {
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
                    const auto& instanceLossPending = *reinterpret_cast<const XrEventDataInstanceLossPending*>(event);
                    Log::Write(Log::Level::Warning, Fmt("XrEventDataInstanceLossPending by %lld", instanceLossPending.lossTime));
                    *exitRenderLoop = true;
                    *requestRestart = true;
                    return;
                }
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    ManageSession(*reinterpret_cast<const XrEventDataSessionStateChanged*>(event), exitRenderLoop, requestRestart);
                    break;
                }
                case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                default: {
                    Log::Write(Log::Level::Verbose, Fmt("Ignoring event type %d", event->type));
                    break;
                }
            }
        }
    }

    bool IsSessionVisible() {
        switch (m_sessionState) {
            case XR_SESSION_STATE_VISIBLE:
            case XR_SESSION_STATE_FOCUSED:
                return true;
        }
        return false;
    }

    bool RenderLayer(XrTime predictedDisplayTime, std::vector<XrCompositionLayerProjectionView>& projectionLayerViews,
                     XrCompositionLayerProjection& layer) {
        XrResult res;

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCapacityInput = (uint32_t)m_views.size();
        uint32_t viewCountOutput;

        XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        viewLocateInfo.displayTime = predictedDisplayTime;
        viewLocateInfo.space = m_appSpace;

        res = xrLocateViews(m_session, &viewLocateInfo, &viewState, viewCapacityInput, &viewCountOutput, m_views.data());
        CHECK_XRRESULT(res, "xrLocateViews");
        if (XR_UNQUALIFIED_SUCCESS(res)) {
            CHECK(viewCountOutput == viewCapacityInput);
            CHECK(viewCountOutput == m_configViews.size());
            CHECK(viewCountOutput == m_swapchains.size());

            projectionLayerViews.resize(viewCountOutput);

            // For each locatable space that we want to visualize, render a 25cm cube.
            /*  std::vector<Cube> cubes;
              for (XrSpace visualizedSpace : m_visualizedSpaces) {
                  XrSpaceRelation spaceRelation{XR_TYPE_SPACE_RELATION};
                  res = xrLocateSpace(visualizedSpace, m_appSpace, predictedDisplayTime, &spaceRelation);
                  CHECK_XRRESULT(res, "xrLocateSpace");
                  if (XR_UNQUALIFIED_SUCCESS(res)) {
                      if ((spaceRelation.relationFlags & XR_SPACE_RELATION_POSITION_VALID_BIT) != 0 &&
                          (spaceRelation.relationFlags & XR_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0) {
                          cubes.push_back(Cube{spaceRelation.pose, {0.25f, 0.25f, 0.25f}});
                      }
                  } else {
                      Log::Write(Log::Level::Verbose, Fmt("Unable to relate a visualized space to app space: %d", res));
                  }
              }*/

            // Render view to the appropriate part of the swapchain image.
            for (uint32_t i = 0; i < viewCountOutput; i++) {
                // Each view has a separate swapchain which is acquired, rendered to, and released.
                const Swapchain viewSwapchain = m_swapchains[i];

                XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

                uint32_t swapchainImageIndex;
                CHECK_XRCMD(xrAcquireSwapchainImage(viewSwapchain.handle, &acquireInfo, &swapchainImageIndex));

                XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                waitInfo.timeout = XR_INFINITE_DURATION;
                CHECK_XRCMD(xrWaitSwapchainImage(viewSwapchain.handle, &waitInfo));

                projectionLayerViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                projectionLayerViews[i].pose = m_views[i].pose;
                projectionLayerViews[i].fov = m_views[i].fov;
                projectionLayerViews[i].subImage.swapchain = viewSwapchain.handle;
                projectionLayerViews[i].subImage.imageRect.offset = {0, 0};
                projectionLayerViews[i].subImage.imageRect.extent = {viewSwapchain.width, viewSwapchain.height};

                const XrSwapchainImageBaseHeader* const swapchainImage =
                    m_swapchainImages[viewSwapchain.handle][swapchainImageIndex];
                renderView(projectionLayerViews[i], swapchainImage, m_colorSwapchainFormat);

                XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                CHECK_XRCMD(xrReleaseSwapchainImage(viewSwapchain.handle, &releaseInfo));
            }

            layer.space = m_appSpace;
            layer.viewCount = (uint32_t)projectionLayerViews.size();
            layer.views = projectionLayerViews.data();
            return true;
        } else {
            Log::Write(Log::Level::Verbose, Fmt("xrLocateViews returned qualified success code: %d", res));
            return false;
        }
    }

    void renderFrame() {
        CHECK(m_session != XR_NULL_HANDLE);

        XrFrameWaitInfo frameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        CHECK_XRCMD(xrWaitFrame(m_session, &frameWaitInfo, &frameState));

        XrFrameBeginInfo frameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        CHECK_XRCMD(xrBeginFrame(m_session, &frameBeginInfo));

        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::vector<XrCompositionLayerProjectionView> projectionLayerViews;
        if (IsSessionVisible()) {
            if (RenderLayer(frameState.predictedDisplayTime, projectionLayerViews, layer)) {
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));
            }
        }

        XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO};
        frameEndInfo.displayTime = frameState.predictedDisplayTime;
        frameEndInfo.environmentBlendMode = m_environmentBlendMode;
        frameEndInfo.layerCount = (uint32_t)layers.size();
        frameEndInfo.layers = layers.data();
        CHECK_XRCMD(xrEndFrame(m_session, &frameEndInfo));
    }

    static std::string version() { return "1"; }

    inline std::string GetXrReferenceSpaceTypeString(XrReferenceSpaceType referenceSpaceType) {
        if (referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW)
            return "View";
        else if (referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
            return "Local";
        else if (referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE)
            return "Stage";
        return "Unknown";
    }

    inline std::string GetXrVersionString(uint32_t ver) {
        return Fmt("%d.%d.%d", XR_VERSION_MAJOR(ver), XR_VERSION_MINOR(ver), XR_VERSION_PATCH(ver));
    }

    XrSession m_session{XR_NULL_HANDLE};
    XrSystemId m_systemId{XR_NULL_SYSTEM_ID};
    XrInstance m_instance{XR_NULL_HANDLE};
    XrSpace m_appSpace;
    XrEventDataBuffer m_eventDataBuffer;
    std::vector<XrSpace> m_visualizedSpaces;
    std::vector<Swapchain> m_swapchains;
    std::vector<XrViewConfigurationView> m_configViews;
    std::vector<XrView> m_views;
    int64_t m_colorSwapchainFormat{-1};
    std::map<XrSwapchain, std::vector<XrSwapchainImageBaseHeader*>> m_swapchainImages;
    XrViewConfigurationType m_viewConfigType{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    // Application's current lifecycle state according to the runtime
    XrSessionState m_sessionState{XR_SESSION_STATE_UNKNOWN};
    XrEnvironmentBlendMode m_environmentBlendMode{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};

    // BGFX
    // ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    void initGraphicsDevice() {
        // Create the D3D11 device for the adapter associated with the system.
        XrGraphicsRequirementsD3D11KHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        CHECK_XRCMD(xrGetD3D11GraphicsRequirementsKHR(m_instance, m_systemId, &graphicsRequirements));

        // Init bgfx
        bgfx::renderFrame();
        bgfx::Init bgfxInit;
        bgfxInit.type = bgfx::RendererType::Direct3D11;
        bgfxInit.vendorId = BGFX_PCI_ID_NVIDIA;
        bgfxInit.deviceId = (uint16_t)graphicsRequirements.adapterLuid.LowPart;
        bgfxInit.resolution.width = 1000;
        bgfxInit.resolution.height = 1000;
        bgfxInit.resolution.reset = BGFX_RESET_VSYNC;
        bgfx::init(bgfxInit);

        // Print device id
        auto caps = bgfx::getCaps();
        std::cout << "BGFX initialized with DeviceID: " << caps->deviceId << "\n";

        // Enable debug text.
        // bgfx::setDebug(true);
    }

    ID3D11Device* getDeviceReference() {
        auto intern = bgfx::getInternalData();
        return (ID3D11Device*)intern->context;
    }

    int64_t selectColorSwapchainFormat(const std::vector<int64_t>& runtimeFormats) {
        // List of supported color swapchain formats, in priority order.
        constexpr DXGI_FORMAT SupportedColorSwapchainFormats[] = {
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        };

        auto swapchainFormatIt =
            std::find_first_of(std::begin(SupportedColorSwapchainFormats), std::end(SupportedColorSwapchainFormats),
                               runtimeFormats.begin(), runtimeFormats.end());
        if (swapchainFormatIt == std::end(SupportedColorSwapchainFormats)) {
            THROW("No runtime swapchain format supported for color swapchain");
        }

        return *swapchainFormatIt;
    }

    std::vector<XrSwapchainImageBaseHeader*> allocateSwapchainImageStructs(uint32_t capacity,
                                                                           const XrSwapchainCreateInfo& /*swapchainCreateInfo*/) {
        // Allocate and initialize the buffer of image structs (must be sequential in memory for xrEnumerateSwapchainImages).
        // Return back an array of pointers to each swapchain image struct so the consumer doesn't need to know the type/size.
        std::vector<XrSwapchainImageD3D11KHR> swapchainImageBuffer(capacity);
        std::vector<XrSwapchainImageBaseHeader*> swapchainImageBase;
        for (XrSwapchainImageD3D11KHR& image : swapchainImageBuffer) {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
            swapchainImageBase.push_back(reinterpret_cast<XrSwapchainImageBaseHeader*>(&image));
        }

        // Keep the buffer alive by moving it into the list of buffers.
        m_swapchainImageBuffers.push_back(std::move(swapchainImageBuffer));

        return swapchainImageBase;
    }

    void renderView(const XrCompositionLayerProjectionView& layerView, const XrSwapchainImageBaseHeader* swapchainImage,
                    int64_t swapchainFormat) {
        if (swapchainFormat) {
        
		}
		// Shared
        CHECK(layerView.subImage.imageArrayIndex == 0);  // Texture arrays not supported.
        ID3D11Texture2D* const colorTexture = reinterpret_cast<const XrSwapchainImageD3D11KHR*>(swapchainImage)->texture;

        // BGFX
        counter++;
        bgfx::ViewId view = 0;
        bgfx::setViewName(view, "standard view");
        bgfx::setViewRect(view, (uint16_t)layerView.subImage.imageRect.offset.x, (uint16_t)layerView.subImage.imageRect.offset.y,
                          (uint16_t)layerView.subImage.imageRect.extent.width,
                          (uint16_t)layerView.subImage.imageRect.extent.height);
        bgfx::setViewClear(view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, counter < 500 ? 0xff3030ff : 0xff30FFff, 1.0f, 0);
        std::cout << counter << "\n";

        auto frameId = (uintptr_t)colorTexture;
        if (textures.find(frameId) == textures.end()) {
            D3D11_TEXTURE2D_DESC colorDesc;
            colorTexture->GetDesc(&colorDesc);

            textures.insert(std::make_pair(
                frameId, bgfx::createTexture2D((uint16_t)1, (uint16_t)1, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT)));

            std::cout << "New frame added \n";
        }

        bgfx::overrideInternal(textures.at(frameId), (uintptr_t)colorTexture);
        if (framebuffers.find(frameId) != framebuffers.end()) {
            auto fb = framebuffers.at(frameId);
            // TODO: this is weird that I need to delete the framebuffer every frame and create a new one
            bgfx::destroy(fb);
        }

        framebuffers.erase(frameId);
        framebuffers.insert(std::make_pair(frameId, bgfx::createFrameBuffer(1, &textures.at(frameId))));

        /*  bgfx::overrideInternal(textures.at(frameId), (uintptr_t)colorTexture);

              framebuffers.erase(frameId);
              framebuffers.insert(std::make_pair(frameId, bgfx::createFrameBuffer(1, &textures.at(frameId))));*/

        bgfx::FrameBufferHandle frameBuffer = framebuffers.at(frameId);

        auto q = bx::Quaternion();
        q.x = layerView.pose.orientation.x;
        q.y = layerView.pose.orientation.y;
        q.z = layerView.pose.orientation.z;
        q.w = layerView.pose.orientation.w;

        // layerView.pose.orientation.
        const bx::Vec3 at = {0.0f, 0.0f, 1.0f};

        // float(-counter)

        const bx::Vec3 eye = {layerView.pose.position.x, layerView.pose.position.y, layerView.pose.position.z};

        auto lookAt = bx::add(eye, bx::mul(at, q));

        float m_width = (float)layerView.subImage.imageRect.extent.width;
        float m_height = (float)layerView.subImage.imageRect.extent.height;
        // Set view and projection matrix for view 0.
        {
            // float viewMatA[16];
            float viewMat[16];
            // bx::mtxLookAt(viewMat, eye, lookAt);
            bx::mtxQuatTranslation(viewMat, q, eye);

            // bx::mtxInverse(viewMat, viewMatA);

            float proj[16];
            bx::mtxProj(proj, 60.0f, float(m_width) / float(m_height), 0.1f, 1000.0f, bgfx::getCaps()->homogeneousDepth);

            XrMatrix4x4f projectionMatrix;
            XrMatrix4x4f_CreateProjectionFov(&projectionMatrix, GRAPHICS_D3D, layerView.fov, 0.05f, 100.0f);
            for (uint16_t j = 0; j < 16; j++) {
                proj[j] = projectionMatrix.m[j];
            }

            bgfx::setViewTransform(view, viewMat, proj);

            // Set view 0 default viewport.
            // bgfx::setViewRect(view, 0, 0, uint16_t(m_width), uint16_t(m_height));
        }

        bgfx::setViewFrameBuffer(view, frameBuffer);
        bgfx::touch(view);

        //m_pt = 2;
        //bgfx::IndexBufferHandle ibh = m_ibh[m_pt];
        //uint64_t state = 0 | (m_r ? BGFX_STATE_WRITE_R : 0) | (m_g ? BGFX_STATE_WRITE_G : 0) | (m_b ? BGFX_STATE_WRITE_B : 0) |
        //                 (m_a ? BGFX_STATE_WRITE_A : 0) | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CW |
        //                 BGFX_STATE_MSAA | s_ptState[m_pt];

        //// Submit 11x11 cubes.
        //for (uint32_t yy = 0; yy < 11; ++yy) {
        //    for (uint32_t xx = 0; xx < 11; ++xx) {
        //        float mtx[16];
        //        bx::mtxRotateXY(mtx, 0, 0);
        //        mtx[12] = -15.0f + float(xx) * 3.0f;
        //        mtx[13] = -15.0f + float(yy) * 3.0f;
        //        mtx[14] = 0.0f;

        //        // Set model matrix for rendering.
        //        bgfx::setTransform(mtx);

        //        // Set vertex and index buffer.
        //        bgfx::setVertexBuffer(0, m_vbh);
        //        bgfx::setIndexBuffer(ibh);

        //        // Set render states.
        //        bgfx::setState(state);

        //        // Submit primitive for rendering to view 0.
        //        bgfx::submit(0, m_program);
        //    }
        //}

        bgfx::frame();
	}

    XrGraphicsBindingD3D11KHR m_graphicsBinding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    std::list<std::vector<XrSwapchainImageD3D11KHR>> m_swapchainImageBuffers;

	std::unordered_map<uintptr_t, bgfx::FrameBufferHandle> framebuffers;
    std::unordered_map<uintptr_t, bgfx::TextureHandle> textures;
    uint16_t counter = 0;
};