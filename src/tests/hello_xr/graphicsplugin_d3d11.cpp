#include <bx/bx.h>
#include <bx/file.h>
#include <bx/sort.h>
#include "bgfx\bgfx.h"
#include "bgfx\platform.h"

#include "pch.h"
#include "common.h"
#include "geometry.h"
#include "graphicsplugin.h"

#include <unordered_map>

#ifdef XR_USE_GRAPHICS_API_D3D11

#include <common/xr_linear.h>
#include <DirectXColors.h>
#include <D3Dcompiler.h>

using namespace Microsoft::WRL;
using namespace DirectX;

namespace {
bx::AllocatorI* getDefaultAllocator() {
    BX_PRAGMA_DIAGNOSTIC_PUSH();
    BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4459);  // warning C4459: declaration of 's_allocator' hides global declaration
    BX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG_GCC("-Wshadow");
    static bx::DefaultAllocator s_allocator;
    return &s_allocator;
    BX_PRAGMA_DIAGNOSTIC_POP();
}
bx::AllocatorI* g_allocator = getDefaultAllocator();

typedef bx::StringT<&g_allocator> String;

class FileReader : public bx::FileReader {
    typedef bx::FileReader super;

   public:
    virtual bool open(const bx::FilePath& _filePath, bx::Error* _err) override {
        String filePath("");
        filePath.append(_filePath);
        return super::open(filePath.getPtr(), _err);
    }
};
struct PosColorVertex {
    float m_x;
    float m_y;
    float m_z;
    uint32_t m_abgr;

    static void init() {
        ms_decl.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .end();
    };

    static bgfx::VertexDecl ms_decl;
};

bgfx::VertexDecl PosColorVertex::ms_decl;

static PosColorVertex s_cubeVertices[] = {
    {-1.0f, 1.0f, 1.0f, 0xff000000},   {1.0f, 1.0f, 1.0f, 0xff0000ff},   {-1.0f, -1.0f, 1.0f, 0xff00ff00},
    {1.0f, -1.0f, 1.0f, 0xff00ffff},   {-1.0f, 1.0f, -1.0f, 0xffff0000}, {1.0f, 1.0f, -1.0f, 0xffff00ff},
    {-1.0f, -1.0f, -1.0f, 0xffffff00}, {1.0f, -1.0f, -1.0f, 0xffffffff},
};

static const uint16_t s_cubeTriList[] = {
    0, 1, 2,           // 0
    1, 3, 2, 4, 6, 5,  // 2
    5, 6, 7, 0, 2, 4,  // 4
    4, 2, 6, 1, 5, 3,  // 6
    5, 7, 3, 0, 4, 1,  // 8
    4, 5, 1, 2, 3, 6,  // 10
    6, 3, 7,
};

static const uint16_t s_cubeTriStrip[] = {
    0, 1, 2, 3, 7, 1, 5, 0, 4, 2, 6, 7, 4, 5,
};

static const uint16_t s_cubeLineList[] = {
    0, 1, 0, 2, 0, 4, 1, 3, 1, 5, 2, 3, 2, 6, 3, 7, 4, 5, 4, 6, 5, 7, 6, 7,
};

static const uint16_t s_cubeLineStrip[] = {
    0, 2, 3, 1, 5, 7, 6, 4, 0, 2, 6, 4, 5, 7, 3, 1, 0,
};

static const uint16_t s_cubePoints[] = {0, 1, 2, 3, 4, 5, 6, 7};

static const char* s_ptNames[]{
    "Triangle List", "Triangle Strip", "Lines", "Line Strip", "Points",
};

static const uint64_t s_ptState[]{
    UINT64_C(0), BGFX_STATE_PT_TRISTRIP, BGFX_STATE_PT_LINES, BGFX_STATE_PT_LINESTRIP, BGFX_STATE_PT_POINTS,
};
BX_STATIC_ASSERT(BX_COUNTOF(s_ptState) == BX_COUNTOF(s_ptNames));

struct ModelConstantBuffer {
    XMFLOAT4X4 Model;
};
struct ViewProjectionConstantBuffer {
    XMFLOAT4X4 ViewProjection;
};

// Separate entrypoints for the vertex and pixel shader functions.
constexpr char ShaderHlsl[] = R"_(
    struct PSVertex {
        float4 Pos : SV_POSITION;
        float3 Color : COLOR0;
    };
    struct Vertex {
        float3 Pos : POSITION;
        float3 Color : COLOR0;
    };
    cbuffer ModelConstantBuffer : register(b0) {
        float4x4 Model;
    };
    cbuffer ViewProjectionConstantBuffer : register(b1) {
        float4x4 ViewProjection;
    };

    PSVertex MainVS(Vertex input) {
       PSVertex output;
       output.Pos = mul(mul(float4(input.Pos, 1), Model), ViewProjection);
       output.Color = input.Color;
       return output;
    }

    float4 MainPS(PSVertex input) : SV_TARGET {
        return float4(input.Color, 1);
    }
    )_";

XMMATRIX XM_CALLCONV LoadXrPose(const XrPosef& pose) {
    return XMMatrixAffineTransformation(DirectX::g_XMOne, DirectX::g_XMZero,
                                        XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(&pose.orientation)),
                                        XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&pose.position)));
}

XMMATRIX XM_CALLCONV LoadXrMatrix(const XrMatrix4x4f& matrix) {
    // XrMatrix4x4f has same memory layout as DirectX Math (Row-major,post-multiplied = column-major,pre-multiplied)
    return XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(&matrix));
}

ComPtr<ID3DBlob> CompileShader(const char* hlsl, const char* entrypoint, const char* shaderTarget) {
    ComPtr<ID3DBlob> compiled;
    ComPtr<ID3DBlob> errMsgs;
    DWORD flags = D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;

#ifdef _DEBUG
    flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entrypoint, shaderTarget, flags, 0,
                            compiled.GetAddressOf(), errMsgs.GetAddressOf());
    if (FAILED(hr)) {
        std::string errMsg((const char*)errMsgs->GetBufferPointer(), errMsgs->GetBufferSize());
        Log::Write(Log::Level::Error, Fmt("D3DCompile failed %X: %s", hr, errMsg.c_str()));
        THROW_HR(hr, "D3DCompile");
    }

    return compiled;
}

ComPtr<IDXGIAdapter1> GetAdapter(LUID adapterId) {
    // Create the DXGI factory.
    ComPtr<IDXGIFactory1> dxgiFactory;
    CHECK_HRCMD(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(dxgiFactory.ReleaseAndGetAddressOf())));

    for (UINT adapterIndex = 0;; adapterIndex++) {
        // EnumAdapters1 will fail with DXGI_ERROR_NOT_FOUND when there are no more adapters to enumerate.
        ComPtr<IDXGIAdapter1> dxgiAdapter;
        CHECK_HRCMD(dxgiFactory->EnumAdapters1(adapterIndex, dxgiAdapter.ReleaseAndGetAddressOf()));

        DXGI_ADAPTER_DESC1 adapterDesc;
        CHECK_HRCMD(dxgiAdapter->GetDesc1(&adapterDesc));
        if (memcmp(&adapterDesc.AdapterLuid, &adapterId, sizeof(adapterId)) == 0) {
            Log::Write(Log::Level::Verbose, Fmt("Using graphics adapter %ws", adapterDesc.Description));
            return dxgiAdapter;
        }
    }
}

void InitializeD3D11DeviceForAdapter(IDXGIAdapter1* adapter, const std::vector<D3D_FEATURE_LEVEL>& featureLevels,
                                     ID3D11Device** device, ID3D11DeviceContext** deviceContext) {
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#ifdef _DEBUG
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // Create the Direct3D 11 API device object and a corresponding context.
    const D3D_DRIVER_TYPE driverType = adapter == nullptr ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN;
    const HRESULT hr = D3D11CreateDevice(adapter, driverType, 0, creationFlags, featureLevels.data(), (UINT)featureLevels.size(),
                                         D3D11_SDK_VERSION, device, nullptr, deviceContext);
    if (FAILED(hr)) {
        // If the initialization fails, fall back to the WARP device.
        // For more information on WARP, see: http://go.microsoft.com/fwlink/?LinkId=286690
        CHECK_HRCMD(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, 0, creationFlags, featureLevels.data(),
                                      (UINT)featureLevels.size(), D3D11_SDK_VERSION, device, nullptr, deviceContext));
    }
}

struct D3D11GraphicsPlugin : public IGraphicsPlugin {
    static const bgfx::Memory* loadMem(bx::FileReaderI* _reader, const char* _filePath) {
        if (bx::open(_reader, _filePath)) {
            uint32_t size = (uint32_t)bx::getSize(_reader);
            const bgfx::Memory* mem = bgfx::alloc(size + 1);
            bx::read(_reader, mem->data, size);
            bx::close(_reader);
            mem->data[mem->size - 1] = '\0';
            return mem;
        }

        std::cout << "Failed to load " << _filePath << "\n";
        return NULL;
    }

    static bgfx::ShaderHandle loadShader(bx::FileReaderI* _reader, const char* _name) {
        char filePath[512];

        const char* shaderPath = "???";

        switch (bgfx::getRendererType()) {
            case bgfx::RendererType::Noop:
            case bgfx::RendererType::Direct3D9:
                shaderPath = "shaders/dx9/";
                break;
            case bgfx::RendererType::Direct3D11:
            case bgfx::RendererType::Direct3D12:
                shaderPath = "shaders/dx11/";
                break;
            case bgfx::RendererType::Gnm:
                shaderPath = "shaders/pssl/";
                break;
            case bgfx::RendererType::Metal:
                shaderPath = "shaders/metal/";
                break;
            case bgfx::RendererType::Nvn:
                shaderPath = "shaders/nvn/";
                break;
            case bgfx::RendererType::OpenGL:
                shaderPath = "shaders/glsl/";
                break;
            case bgfx::RendererType::OpenGLES:
                shaderPath = "shaders/essl/";
                break;
            case bgfx::RendererType::Vulkan:
                shaderPath = "shaders/spirv/";
                break;

            case bgfx::RendererType::Count:
                BX_CHECK(false, "You should not be here!");
                break;
        }

        bx::strCopy(filePath, BX_COUNTOF(filePath),
                    "C:/Users/trbaron/workspace/OpenXR-SDK/build/win64/src/tests/hello_xr/Release/dx11/");
        bx::strCat(filePath, BX_COUNTOF(filePath), _name);
        bx::strCat(filePath, BX_COUNTOF(filePath), ".bin");

        bgfx::ShaderHandle handle = bgfx::createShader(loadMem(_reader, filePath));
        bgfx::setName(handle, _name);

        return handle;
    }

    D3D11GraphicsPlugin(const std::shared_ptr<Options>&, std::shared_ptr<IPlatformPlugin>){};

    std::vector<std::string> GetInstanceExtensions() const override { return {XR_KHR_D3D11_ENABLE_EXTENSION_NAME}; }

    void InitializeDevice(XrInstance instance, XrSystemId systemId) override {
        // Create the D3D11 device for the adapter associated with the system.
        XrGraphicsRequirementsD3D11KHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        CHECK_XRCMD(xrGetD3D11GraphicsRequirementsKHR(instance, systemId, &graphicsRequirements));

        // BGFX
        bgfx::renderFrame();
        bgfx::Init bgfxInit;
        bgfxInit.type = bgfx::RendererType::Direct3D11;
        //bgfxInit.vendorId = BGFX_PCI_ID_NONE;    // Auto select
        bgfxInit.vendorId = BGFX_PCI_ID_NVIDIA; 
        bgfxInit.deviceId = (uint16_t) graphicsRequirements.adapterLuid.LowPart;
        bgfxInit.resolution.width = 1000;
        bgfxInit.resolution.height = 1000;
        bgfxInit.resolution.reset = BGFX_RESET_VSYNC;
        bgfx::init(bgfxInit);

        std::cout << "BGFX INIT!!!"
                  << "\n";
        auto caps = bgfx::getCaps();
        std::cout << "DeviceID: " << caps->deviceId << "\n";

        // Enable debug text.
        bgfx::setDebug(true);

        // Set view 0 clear state.
        bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303030ff, 1.0f, 0);


        InitializeResources();

		auto intern = bgfx::getInternalData();
        
		m_graphicsBinding.device = (ID3D11Device*)intern->context;
    }

    void InitializeResources() {
        // BGFX
        // Create vertex stream declaration.
        PosColorVertex::init();

        // Create static vertex buffer.
        m_vbh = bgfx::createVertexBuffer(
            // Static data can be passed with bgfx::makeRef
            bgfx::makeRef(s_cubeVertices, sizeof(s_cubeVertices)), PosColorVertex::ms_decl);

        // Create static index buffer for triangle list rendering.
        m_ibh[0] = bgfx::createIndexBuffer(
            // Static data can be passed with bgfx::makeRef
            bgfx::makeRef(s_cubeTriList, sizeof(s_cubeTriList)));

        // Create static index buffer for triangle strip rendering.
        m_ibh[1] = bgfx::createIndexBuffer(
            // Static data can be passed with bgfx::makeRef
            bgfx::makeRef(s_cubeTriStrip, sizeof(s_cubeTriStrip)));

        // Create static index buffer for line list rendering.
        m_ibh[2] = bgfx::createIndexBuffer(
            // Static data can be passed with bgfx::makeRef
            bgfx::makeRef(s_cubeLineList, sizeof(s_cubeLineList)));

        // Create static index buffer for line strip rendering.
        m_ibh[3] = bgfx::createIndexBuffer(
            // Static data can be passed with bgfx::makeRef
            bgfx::makeRef(s_cubeLineStrip, sizeof(s_cubeLineStrip)));

        // Create static index buffer for point list rendering.
        m_ibh[4] = bgfx::createIndexBuffer(
            // Static data can be passed with bgfx::makeRef
            bgfx::makeRef(s_cubePoints, sizeof(s_cubePoints)));

        // Create program from shaders.
        s_fileReader = BX_NEW(getDefaultAllocator(), FileReader);
        auto _fsName = "fs_cubes";
        auto _vsName = "vs_cubes";
        bgfx::ShaderHandle vsh = loadShader(s_fileReader, _vsName);
        bgfx::ShaderHandle fsh = BGFX_INVALID_HANDLE;
        if (NULL != _fsName) {
            fsh = loadShader(s_fileReader, _fsName);
        }

        m_program = bgfx::createProgram(vsh, fsh, true /* destroy shaders when program is destroyed */);
        // m_program = loadProgram("vs_cubes", "fs_cubes");

        // m_timeOffset = bx::getHPCounter();

    }

    int64_t SelectColorSwapchainFormat(const std::vector<int64_t>& runtimeFormats) const override {
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

    const XrBaseInStructure* GetGraphicsBinding() const override {
        return reinterpret_cast<const XrBaseInStructure*>(&m_graphicsBinding);
    }

    std::vector<XrSwapchainImageBaseHeader*> AllocateSwapchainImageStructs(
        uint32_t capacity, const XrSwapchainCreateInfo& /*swapchainCreateInfo*/) override {
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

    //ComPtr<ID3D11DepthStencilView> GetDepthStencilView(ID3D11Texture2D* colorTexture) {
    //    // If a depth-stencil view has already been created for this back-buffer, use it.
    //    auto depthBufferIt = m_colorToDepthMap.find(colorTexture);
    //    if (depthBufferIt != m_colorToDepthMap.end()) {
    //        return depthBufferIt->second;
    //    }

    //    // This back-buffer has no cooresponding depth-stencil texture, so create one with matching dimensions.
    //    D3D11_TEXTURE2D_DESC colorDesc;
    //    colorTexture->GetDesc(&colorDesc);

    //    D3D11_TEXTURE2D_DESC depthDesc{};
    //    depthDesc.Width = colorDesc.Width;
    //    depthDesc.Height = colorDesc.Height;
    //    depthDesc.ArraySize = colorDesc.ArraySize;
    //    depthDesc.MipLevels = 1;
    //    depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    //    depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
    //    depthDesc.SampleDesc.Count = 1;
    //    ComPtr<ID3D11Texture2D> depthTexture;
    //    CHECK_HRCMD(m_device->CreateTexture2D(&depthDesc, nullptr, depthTexture.ReleaseAndGetAddressOf()));

    //    // Create and cache the depth stencil view.
    //    ComPtr<ID3D11DepthStencilView> depthStencilView;
    //    CD3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc(D3D11_DSV_DIMENSION_TEXTURE2D, DXGI_FORMAT_D32_FLOAT);
    //    CHECK_HRCMD(m_device->CreateDepthStencilView(depthTexture.Get(), &depthStencilViewDesc, depthStencilView.GetAddressOf()));
    //    depthBufferIt = m_colorToDepthMap.insert(std::make_pair(colorTexture, depthStencilView)).first;

    //    return depthStencilView;
    //}
    uint16_t counter = 0;

    void RenderView(const XrCompositionLayerProjectionView& layerView, const XrSwapchainImageBaseHeader* swapchainImage,
                    int64_t swapchainFormat, const std::vector<Cube>& cubes) override {
        auto x = swapchainFormat || cubes.size();
        if (x) {
            //std::cout << "tmp\n";
		}

        // Shared
        CHECK(layerView.subImage.imageArrayIndex == 0);  // Texture arrays not supported.
        ID3D11Texture2D* const colorTexture = reinterpret_cast<const XrSwapchainImageD3D11KHR*>(swapchainImage)->texture;

        // BGFX
        counter++;
        bgfx::ViewId view = 0;
        bgfx::setViewName(view, "standard view");
        bgfx::setViewRect(view, (uint16_t)layerView.subImage.imageRect.offset.x, (uint16_t)layerView.subImage.imageRect.offset.y, (uint16_t)layerView.subImage.imageRect.extent.width, (uint16_t)layerView.subImage.imageRect.extent.height);
        bgfx::setViewClear(view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, counter < 500 ? 0xff3030ff : 0xff30FFff, 1.0f, 0);
        std::cout <<  counter << "\n";

        
        auto frameId = (uintptr_t)colorTexture;
        if (textures.find(frameId) == textures.end()) {
            D3D11_TEXTURE2D_DESC colorDesc;
            colorTexture->GetDesc(&colorDesc);

			textures.insert(std::make_pair(
                frameId,
                bgfx::createTexture2D((uint16_t)1, (uint16_t)1, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT)));	
		
			

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

		
		//layerView.pose.orientation.
		const bx::Vec3 at = {0.0f, 0.0f, 1.0f};

		//float(-counter)
          
         const bx::Vec3 eye = {layerView.pose.position.x, layerView.pose.position.y, layerView.pose.position.z};

		 auto lookAt =  bx::add(eye, bx::mul(at, q));

		float m_width = (float)layerView.subImage.imageRect.extent.width;
        float m_height = (float)layerView.subImage.imageRect.extent.height;
        // Set view and projection matrix for view 0.
        {
            //float viewMatA[16];
            float viewMat[16];
            //bx::mtxLookAt(viewMat, eye, lookAt);
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
            //bgfx::setViewRect(view, 0, 0, uint16_t(m_width), uint16_t(m_height));
        }

        bgfx::setViewFrameBuffer(view, frameBuffer);
        bgfx::touch(view);

		m_pt = 2;
		bgfx::IndexBufferHandle ibh = m_ibh[m_pt];
		uint64_t state = 0
			| (m_r ? BGFX_STATE_WRITE_R : 0)
			| (m_g ? BGFX_STATE_WRITE_G : 0)
			| (m_b ? BGFX_STATE_WRITE_B : 0)
			| (m_a ? BGFX_STATE_WRITE_A : 0)
			| BGFX_STATE_WRITE_Z
			| BGFX_STATE_DEPTH_TEST_LESS
			| BGFX_STATE_CULL_CW
			| BGFX_STATE_MSAA
			| s_ptState[m_pt]
			;

		// Submit 11x11 cubes.
		for (uint32_t yy = 0; yy < 11; ++yy)
		{
			for (uint32_t xx = 0; xx < 11; ++xx)
			{
				float mtx[16];
				bx::mtxRotateXY(mtx, 0, 0);
				mtx[12] = -15.0f + float(xx)*3.0f;
				mtx[13] = -15.0f + float(yy)*3.0f;
				mtx[14] = 0.0f;

				// Set model matrix for rendering.
				bgfx::setTransform(mtx);

				// Set vertex and index buffer.
				bgfx::setVertexBuffer(0, m_vbh);
				bgfx::setIndexBuffer(ibh);

				// Set render states.
				bgfx::setState(state);

				// Submit primitive for rendering to view 0.
				bgfx::submit(0, m_program);
			}
		}

        bgfx::frame();
		
        
    }


   private:
    // BGFX
    bx::FileReaderI* s_fileReader = NULL;

    bgfx::VertexBufferHandle m_vbh;
    bgfx::IndexBufferHandle m_ibh[BX_COUNTOF(s_ptState)];
    bgfx::ProgramHandle m_program;
    int64_t m_timeOffset;
    int32_t m_pt;

	std::unordered_map<uintptr_t, bgfx::FrameBufferHandle> framebuffers; 
	std::unordered_map<uintptr_t, bgfx::TextureHandle> textures; 

    bool m_r;
    bool m_g;
    bool m_b;
    bool m_a;

    // Shared
    std::list<std::vector<XrSwapchainImageD3D11KHR>> m_swapchainImageBuffers;

    // DX
    XrGraphicsBindingD3D11KHR m_graphicsBinding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};

    // Map color buffer to associated depth buffer. This map is populated on demand.
    std::map<ID3D11Texture2D*, ComPtr<ID3D11DepthStencilView>> m_colorToDepthMap;
};
}  // namespace

std::shared_ptr<IGraphicsPlugin> CreateGraphicsPlugin_D3D11(const std::shared_ptr<Options>& options,
                                                            std::shared_ptr<IPlatformPlugin> platformPlugin) {
    return std::make_shared<D3D11GraphicsPlugin>(options, platformPlugin);
}

#endif
