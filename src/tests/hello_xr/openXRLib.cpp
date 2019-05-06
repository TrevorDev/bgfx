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


class OpenXRLib {
   public:
    OpenXRLib() {}

    void init(const std::vector<std::string> platformExtensions = {},
              const std::vector<std::string> graphicsExtensions = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME},
              XrBaseInStructure* createInstanceExtension = nullptr,
              XrFormFactor formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
              //XrViewConfigurationType configurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
              //XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
              const char* xrApplicationName = "XR App"
	) {

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

	

	void initializeSession() {
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_session == XR_NULL_HANDLE);

		// Create session
        {
            Log::Write(Log::Level::Verbose, Fmt("Creating session..."));

            XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};

			auto intern = bgfx::getInternalData();

            createInfo.next = (ID3D11Device*)intern->context;
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

        XrReferenceSpaceType visualizedSpaces[] = {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,  XR_REFERENCE_SPACE_TYPE_STAGE};
        
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
                           Fmt("Failed to create one of the reference spaces with error %d for visualization",  res));
            }
        }

		// TODO is this neeeded?
		{
            XrReferenceSpaceCreateInfo referenceSpaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            XrPosef t{};
            t.orientation.w = 1;
            referenceSpaceCreateInfo.poseInReferenceSpace = t;
            referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            CHECK_XRCMD(xrCreateReferenceSpace(m_session, &referenceSpaceCreateInfo, &m_appSpace));
        }
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
    std::vector<XrSpace> m_visualizedSpaces;



	void initDevice() {
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
};