// Copyright 2020-2023 The Defold Foundation
// Copyright 2014-2020 King
// Copyright 2009-2014 Ragnar Svensson, Christian Murray
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include <string.h>
#include <assert.h>

#include <d3d12.h>
#include <d3dx12.h>
#include <D3DCompiler.h>

#include <dxgi1_6.h>

#include <dmsdk/dlib/vmath.h>

#include <dlib/array.h>
#include <dlib/dstrings.h>
#include <dlib/log.h>
#include <dlib/math.h>
#include <dlib/profile.h>

#include <platform/platform_window.h>

#include <graphics/glfw/glfw_native.h>

#include "../graphics_private.h"
#include "../graphics_native.h"
#include "../graphics_adapter.h"

#include "graphics_dx12_private.h"


namespace dmGraphics
{
    static GraphicsAdapterFunctionTable DX12RegisterFunctionTable();
    static bool                         DX12IsSupported();
    static bool                         DX12Initialize(HContext _context);
    static const int8_t    g_dx12_adapter_priority = 0;
    static GraphicsAdapter g_dx12_adapter(ADAPTER_FAMILY_DIRECTX);
    static DX12Context*    g_DX12Context = 0x0;

    DM_REGISTER_GRAPHICS_ADAPTER(GraphicsAdapterDX12, &g_dx12_adapter, DX12IsSupported, DX12RegisterFunctionTable, g_dx12_adapter_priority);

    static int16_t CreateTextureSampler(DX12Context* context, TextureFilter minfilter, TextureFilter magfilter, TextureWrap uwrap, TextureWrap vwrap, uint8_t maxLod, float max_anisotropy);
    static void    FlushResourcesToDestroy(DX12FrameResource& current_frame_resource);

    #define CHECK_HR_ERROR(result) \
    { \
        if(g_DX12Context->m_VerifyGraphicsCalls && result != S_OK) { \
            dmLogError("DX Error (%s:%d) code: %d", __FILE__, __LINE__, HRESULT_CODE(result)); \
            assert(0); \
        } \
    }

    DX12Context::DX12Context(const ContextParams& params)
    {
        memset(this, 0, sizeof(*this));
        m_NumFramesInFlight       = MAX_FRAMES_IN_FLIGHT;
        m_DefaultTextureMinFilter = params.m_DefaultTextureMinFilter;
        m_DefaultTextureMagFilter = params.m_DefaultTextureMagFilter;
        m_VerifyGraphicsCalls     = params.m_VerifyGraphicsCalls;
        m_PrintDeviceInfo         = params.m_PrintDeviceInfo;
        m_Window                  = params.m_Window;
        m_Width                   = params.m_Width;
        m_Height                  = params.m_Height;
        m_UseValidationLayers     = params.m_UseValidationLayers;

        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_LUMINANCE;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_LUMINANCE_ALPHA;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_RGB;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_RGBA;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_RGB_16BPP;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_RGBA_16BPP;

        assert(dmPlatform::GetWindowStateParam(m_Window, dmPlatform::WINDOW_STATE_OPENED));
    }

    static HContext DX12NewContext(const ContextParams& params)
    {
        if (!g_DX12Context)
        {
            g_DX12Context = new DX12Context(params);

            if (DX12Initialize(g_DX12Context))
            {
                return g_DX12Context;
            }

            DeleteContext(g_DX12Context);
        }
        return 0x0;
    }

    static IDXGIAdapter1* CreateDeviceAdapter(IDXGIFactory4* dxgiFactory)
    {
        IDXGIAdapter1* adapter = 0;
        int adapterIndex = 0;

        // find first hardware gpu that supports d3d 12
        while (dxgiFactory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                adapterIndex++;
                continue;
            }

            // we want a device that is compatible with direct3d 12 (feature level 11 or higher)
            HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), NULL);
            if (SUCCEEDED(hr))
            {
                break;
            }

            adapterIndex++;
        }

        return adapter;
    }

    static IDXGIFactory4* CreateDXGIFactory()
    {
        IDXGIFactory4* factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr))
        {
            return 0;
        }
        return factory;
    }

    static bool DX12IsSupported()
    {
        IDXGIAdapter1* adapter = CreateDeviceAdapter(CreateDXGIFactory());
        if (adapter)
        {
            adapter->Release();
            return true;
        }
        return false;
    }

    static void DX12DeleteContext(HContext _context)
    {
        assert(_context);
        if (g_DX12Context)
        {
            DX12Context* context = (DX12Context*) _context;

            for (uint8_t i=0; i < DM_ARRAY_SIZE(context->m_FrameResources); i++)
            {
                FlushResourcesToDestroy(context->m_FrameResources[i]);
            }

            delete (DX12Context*) context;
            g_DX12Context = 0x0;
        }
    }

    static void CreateRootSignature(DX12Context* context, CD3DX12_ROOT_SIGNATURE_DESC* desc, DX12ShaderProgram* program)
    {
        ID3DBlob* signature;
        ID3DBlob* error;
        HRESULT hr = D3D12SerializeRootSignature(desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
        CHECK_HR_ERROR(hr);

        hr = context->m_Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&program->m_RootSignature));
        CHECK_HR_ERROR(hr);
    }

    static void SetupMainRenderTarget(DX12Context* context, DXGI_SAMPLE_DESC sample_desc)
    {
        // Initialize the dummy rendertarget for the main framebuffer
        // The m_Framebuffer construct will be rotated sequentially
        // with the framebuffer objects created per swap chain.
        DX12RenderTarget* rt = GetAssetFromContainer<DX12RenderTarget>(context->m_AssetHandleContainer, context->m_MainRenderTarget);
        assert(rt == 0x0);

        rt               = new DX12RenderTarget();
        rt->m_Id         = DM_RENDERTARGET_BACKBUFFER_ID;
        rt->m_Format     = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt->m_SampleDesc = sample_desc;

        context->m_MainRenderTarget    = StoreAssetInContainer(context->m_AssetHandleContainer, rt, ASSET_TYPE_RENDER_TARGET);
        context->m_CurrentRenderTarget = context->m_MainRenderTarget;

        // rt->m_Handle.m_RenderPass  = context->m_MainRenderPass;
        // rt->m_Handle.m_Framebuffer = context->m_MainFrameBuffers[0];
        // rt->m_Extent               = context->m_SwapChain->m_ImageExtent;
        // rt->m_ColorAttachmentCount = 1;
    }

    void DX12ScratchBuffer::Initialize(DX12Context* context, uint32_t frame_index)
    {
        m_FrameIndex = frame_index;

        // Initialize constant buffer heap
        {
            uint32_t pool_block_count = MAX_BLOCK_SIZE / BLOCK_STEP_SIZE;
            m_MemoryPools.SetCapacity(pool_block_count);
            m_MemoryPools.SetSize(pool_block_count);

            for (int i = 0; i < m_MemoryPools.Size(); ++i)
            {
                m_MemoryPools[i].m_BlockSize        = (i+1) * BLOCK_STEP_SIZE;
                m_MemoryPools[i].m_DescriptorCursor = 0;
                m_MemoryPools[i].m_MemoryCursor     = 0;

                D3D12_DESCRIPTOR_HEAP_DESC heap_Desc = {};
                heap_Desc.NumDescriptors             = DESCRIPTORS_PER_POOL;
                heap_Desc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                heap_Desc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

                HRESULT hr = context->m_Device->CreateDescriptorHeap(&heap_Desc, IID_PPV_ARGS(&m_MemoryPools[i].m_DescriptorHeap));
                CHECK_HR_ERROR(hr);

                const uint32_t memory_heap_alignment = 1024 * 64;
                const uint32_t memory_heap_size      = memory_heap_alignment; // TODO: Some other memory metric here

                hr = context->m_Device->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), // this heap will be used to upload the constant buffer data
                    D3D12_HEAP_FLAG_NONE,                             // no flags
                    &CD3DX12_RESOURCE_DESC::Buffer(memory_heap_size), // size of the resource heap. Must be a multiple of 64KB for single-textures and constant buffers
                    D3D12_RESOURCE_STATE_GENERIC_READ,                // will be data that is read from so we keep it in the generic read state
                    NULL,                                             // we do not have use an optimized clear value for constant buffers
                    IID_PPV_ARGS(&m_MemoryPools[i].m_MemoryHeap));
                CHECK_HR_ERROR(hr);

                for (int j = 0; j < DESCRIPTORS_PER_POOL; ++j)
                {
                    D3D12_CONSTANT_BUFFER_VIEW_DESC view_desc = {};
                    view_desc.BufferLocation = m_MemoryPools[i].m_MemoryHeap->GetGPUVirtualAddress() + i * m_MemoryPools[i].m_BlockSize;
                    view_desc.SizeInBytes    = m_MemoryPools[i].m_BlockSize;

                    CD3DX12_CPU_DESCRIPTOR_HANDLE view_handle(m_MemoryPools[i].m_DescriptorHeap->GetCPUDescriptorHandleForHeapStart(), i, context->m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
                    context->m_Device->CreateConstantBufferView(&view_desc, view_handle);
                }

                hr = m_MemoryPools[i].m_MemoryHeap->Map(0, 0, &m_MemoryPools[i].m_MappedDataPtr);
                CHECK_HR_ERROR(hr);
            }
        }
    }

    void* DX12ScratchBuffer::AllocateConstantBuffer(DX12Context* context, uint32_t buffer_index, uint32_t non_aligned_byte_size)
    {
        assert(non_aligned_byte_size < MAX_BLOCK_SIZE);
        uint32_t pool_index     = non_aligned_byte_size / BLOCK_STEP_SIZE;
        uint32_t memory_cursor  = m_MemoryPools[pool_index].m_MemoryCursor;
        uint8_t* base_ptr       = ((uint8_t*) m_MemoryPools[pool_index].m_MappedDataPtr) + memory_cursor;

        context->m_CommandList->SetGraphicsRootConstantBufferView(buffer_index, m_MemoryPools[0].m_MemoryHeap->GetGPUVirtualAddress() + memory_cursor);

        m_MemoryPools[pool_index].m_MemoryCursor += m_MemoryPools[pool_index].m_BlockSize;
        m_MemoryPools[pool_index].m_DescriptorCursor++;

    #if 0
        dmLogInfo("AllocateConstantBuffer: ptr: %p, frame: %d, pool: %d, descriptor: %d, offset: %d", base_ptr, m_FrameIndex, pool_index, m_MemoryPools[pool_index].m_DescriptorCursor, cursor);
    #endif

        return (void*) base_ptr;
    }

    void DX12ScratchBuffer::AllocateTexture2D(DX12Context* context, DX12Texture* texture, uint32_t texture_index, const DX12TextureSampler& sampler, uint32_t sampler_index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view_desc = {};
        view_desc.Format                          = texture->m_ResourceDesc.Format;
        view_desc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2D;
        view_desc.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view_desc.Texture2D.MipLevels             = texture->m_MipMapCount;

        uint32_t desc_size   = context->m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        uint32_t desc_offset = desc_size * m_MemoryPools[0].m_DescriptorCursor;

        CD3DX12_CPU_DESCRIPTOR_HANDLE  view_desc_handle(
            m_MemoryPools[0].m_DescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
            m_MemoryPools[0].m_DescriptorCursor,
            desc_size);

        context->m_Device->CreateShaderResourceView(texture->m_Resource, &view_desc, view_desc_handle);
        m_MemoryPools[0].m_DescriptorCursor++;

        CD3DX12_GPU_DESCRIPTOR_HANDLE handle_sampler(context->m_SamplerPool.m_DescriptorHeap->GetGPUDescriptorHandleForHeapStart(), sampler.m_DescriptorOffset);
        CD3DX12_GPU_DESCRIPTOR_HANDLE handle_texture(m_MemoryPools[0].m_DescriptorHeap->GetGPUDescriptorHandleForHeapStart(), desc_offset);

        context->m_CommandList->SetGraphicsRootDescriptorTable(texture_index, handle_texture);
        context->m_CommandList->SetGraphicsRootDescriptorTable(sampler_index, handle_sampler);
    }

    void DX12ScratchBuffer::Reset(DX12Context* context)
    {
        for (int i = 0; i < m_MemoryPools.Size(); ++i)
        {
            m_MemoryPools[i].m_DescriptorCursor = 0;
            m_MemoryPools[i].m_MemoryCursor     = 0;
        }
    }

    // Can we bind this at start of frame?
    void DX12ScratchBuffer::Bind(DX12Context* context)
    {
        // TODO: multiple heaps needs to be bound here
        ID3D12DescriptorHeap* heaps[] = {
            m_MemoryPools[0].m_DescriptorHeap
        };
        context->m_CommandList->SetDescriptorHeaps(DM_ARRAY_SIZE(heaps), heaps);
    }

    static bool DX12Initialize(HContext _context)
    {
        DX12Context* context = (DX12Context*) _context;

        HRESULT hr = S_OK;

        // This needs to be created before the device
        // if (context->m_UseValidationLayers)
        if (true)
        {
            hr = D3D12GetDebugInterface(IID_PPV_ARGS(&context->m_DebugInterface));
            CHECK_HR_ERROR(hr);

            context->m_DebugInterface->EnableDebugLayer(); // TODO: Release
        }

        IDXGIFactory4* factory = CreateDXGIFactory();
        IDXGIAdapter1* adapter = CreateDeviceAdapter(factory);

        hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&context->m_Device));
        CHECK_HR_ERROR(hr);

        D3D12_COMMAND_QUEUE_DESC cmd_queue_desc = {};
        hr = context->m_Device->CreateCommandQueue(&cmd_queue_desc, IID_PPV_ARGS(&context->m_CommandQueue));
        CHECK_HR_ERROR(hr);

        // Create swapchain
        DXGI_MODE_DESC back_buffer_desc = {};
        back_buffer_desc.Width          = context->m_Width;
        back_buffer_desc.Height         = context->m_Height;
        back_buffer_desc.Format         = DXGI_FORMAT_R8G8B8A8_UNORM;

        DXGI_SAMPLE_DESC sample_desc = {};
        sample_desc.Count            = 1;

        DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
        swap_chain_desc.BufferCount          = MAX_FRAMEBUFFERS;
        swap_chain_desc.BufferDesc           = back_buffer_desc;
        swap_chain_desc.BufferUsage          = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_chain_desc.SwapEffect           = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swap_chain_desc.OutputWindow         = glfwGetWindowsHWND();
        swap_chain_desc.SampleDesc           = sample_desc;
        swap_chain_desc.Windowed             = true;

        IDXGISwapChain* swap_chain_tmp = 0;
        factory->CreateSwapChain(context->m_CommandQueue, &swap_chain_desc, &swap_chain_tmp);
        context->m_SwapChain = static_cast<IDXGISwapChain3*>(swap_chain_tmp);

        // frameIndex = swapChain->GetCurrentBackBufferIndex();

        ////// MOVE THIS?
        D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_desc = {};
        sampler_heap_desc.NumDescriptors             = 128; // TODO, I don't know if this is a good value, the sampler pool should be fully dynamic I think
        sampler_heap_desc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        sampler_heap_desc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        hr = context->m_Device->CreateDescriptorHeap(&sampler_heap_desc, IID_PPV_ARGS(&context->m_SamplerPool.m_DescriptorHeap));
        CHECK_HR_ERROR(hr);

        // this heap is a render target view heap
        D3D12_DESCRIPTOR_HEAP_DESC rt_view_heap_desc = {};
        rt_view_heap_desc.NumDescriptors             = MAX_FRAMEBUFFERS;
        rt_view_heap_desc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rt_view_heap_desc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        hr = context->m_Device->CreateDescriptorHeap(&rt_view_heap_desc, IID_PPV_ARGS(&context->m_RtvDescriptorHeap));
        CHECK_HR_ERROR(hr);

        context->m_RtvDescriptorSize = context->m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        // get a handle to the first descriptor in the descriptor heap. a handle is basically a pointer,
        // but we cannot literally use it like a c++ pointer.
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv_handle(context->m_RtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

        for (int i = 0; i < MAX_FRAMEBUFFERS; i++)
        {
            // first we get the n'th buffer in the swap chain and store it in the n'th
            // position of our ID3D12Resource array
            hr = context->m_SwapChain->GetBuffer(i, IID_PPV_ARGS(&context->m_FrameResources[i].m_RenderTarget.m_Resource));
            CHECK_HR_ERROR(hr);

            // the we "create" a render target view which binds the swap chain buffer (ID3D12Resource[n]) to the rtv handle
            context->m_Device->CreateRenderTargetView(context->m_FrameResources[i].m_RenderTarget.m_Resource, NULL, rtv_handle);

            // we increment the rtv handle by the rtv descriptor size we got above
            rtv_handle.Offset(1, context->m_RtvDescriptorSize);

            hr = context->m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&context->m_FrameResources[i].m_CommandAllocator));
            CHECK_HR_ERROR(hr);

            // Create the frame fence that will be signaled when we can render to this frame
            hr = context->m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context->m_FrameResources[i].m_Fence));
            CHECK_HR_ERROR(hr);

            context->m_FrameResources[i].m_FenceValue = RENDER_CONTEXT_STATE_FREE;
            context->m_FrameResources[i].m_ScratchBuffer.Initialize(context, i);
        }


        context->m_FenceEvent = CreateEvent(NULL, false, false, NULL);
        if (!context->m_FenceEvent)
        {
            dmLogFatal("Unable to create fence event");
            return false;
        }

        // command buffer / command list
        // TODO: We should create one of these for every thread we have that are recording commands
        hr = context->m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, context->m_FrameResources[0].m_CommandAllocator, NULL, IID_PPV_ARGS(&context->m_CommandList));
        CHECK_HR_ERROR(hr);

        context->m_CommandList->Close();

        SetupMainRenderTarget(context, sample_desc);

        context->m_PipelineState = GetDefaultPipelineState();

        CreateTextureSampler(context, TEXTURE_FILTER_LINEAR, TEXTURE_FILTER_LINEAR, TEXTURE_WRAP_REPEAT, TEXTURE_WRAP_REPEAT, 1, 1.0f);

        if (context->m_PrintDeviceInfo)
        {
            dmLogInfo("Device: DirectX 12");
        }
        return true;
    }

    static void DX12Finalize()
    {

    }

    static void DX12CloseWindow(HContext _context)
    {
        DX12Context* context = (DX12Context*) _context;

        if (dmPlatform::GetWindowStateParam(context->m_Window, dmPlatform::WINDOW_STATE_OPENED))
        {
        }
    }

    static void DX12RunApplicationLoop(void* user_data, WindowStepMethod step_method, WindowIsRunning is_running)
    {
    }

    static dmPlatform::HWindow DX12GetWindow(HContext _context)
    {
        DX12Context* context = (DX12Context*) _context;
        return context->m_Window;
    }

    static uint32_t DX12GetDisplayDpi(HContext context)
    {
        assert(context);
        return 0;
    }

    static uint32_t DX12GetWidth(HContext _context)
    {
        DX12Context* context = (DX12Context*) _context;
        return context->m_Width;
    }

    static uint32_t DX12GetHeight(HContext _context)
    {
        DX12Context* context = (DX12Context*) _context;
        return context->m_Height;
    }

    /*
    void VulkanSetWindowSize(HContext _context, uint32_t width, uint32_t height)
    {
        VulkanContext* context = (VulkanContext*) _context;

        if (dmPlatform::GetWindowStateParam(context->m_Window, dmPlatform::WINDOW_STATE_OPENED))
        {
            context->m_Width  = width;
            context->m_Height = height;

            dmPlatform::SetWindowSize(context->m_Window, width, height);

            context->m_WindowWidth  = dmPlatform::GetWindowWidth(context->m_Window);
            context->m_WindowHeight = dmPlatform::GetWindowHeight(context->m_Window);

            SwapChainChanged(g_VulkanContext, &context->m_WindowWidth, &context->m_WindowHeight, 0, 0);
        }
    }

    /*
    HRESULT ResizeBuffers(
  UINT        BufferCount,
  UINT        Width,
  UINT        Height,
  DXGI_FORMAT NewFormat,
  UINT        SwapChainFlags
);
*/
    /*
    void VulkanResizeWindow(HContext _context, uint32_t width, uint32_t height)
    {
        VulkanContext* context = (VulkanContext*) _context;
        if (dmPlatform::GetWindowStateParam(context->m_Window, dmPlatform::WINDOW_STATE_OPENED))
        {
            VulkanSetWindowSize(_context, width, height);
        }
    }
    */

    static void DX12SetWindowSize(HContext _context, uint32_t width, uint32_t height)
    {
        assert(_context);
        DX12Context* context = (DX12Context*) _context;
        if (dmPlatform::GetWindowStateParam(context->m_Window, dmPlatform::WINDOW_STATE_OPENED))
        {
            dmPlatform::SetWindowSize(context->m_Window, width, height);
        }
    }

    static void DX12ResizeWindow(HContext _context, uint32_t width, uint32_t height)
    {
        assert(_context);
        DX12Context* context = (DX12Context*) _context;
        if (dmPlatform::GetWindowStateParam(context->m_Window, dmPlatform::WINDOW_STATE_OPENED))
        {
            dmPlatform::SetWindowSize(context->m_Window, width, height);
        }
    }

    static void DX12GetDefaultTextureFilters(HContext _context, TextureFilter& out_min_filter, TextureFilter& out_mag_filter)
    {
        DX12Context* context = (DX12Context*) _context;
        out_min_filter = context->m_DefaultTextureMinFilter;
        out_mag_filter = context->m_DefaultTextureMagFilter;
    }

    static void DX12Clear(HContext _context, uint32_t flags, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha, float depth, uint32_t stencil)
    {
        DX12Context* context = (DX12Context*) _context;

        const float r = ((float)red)/255.0f;
        const float g = ((float)green)/255.0f;
        const float b = ((float)blue)/255.0f;
        const float a = ((float)alpha)/255.0f;
        const float cc[] = { r, g, b, a };
        context->m_CommandList->ClearRenderTargetView(context->m_RtvHandle, cc, 0, NULL);
    }

    static void SyncronizeFrame(DX12Context* context)
    {
        // swap the current rtv buffer index so we draw on the correct buffer
        context->m_CurrentFrameIndex = context->m_SwapChain->GetCurrentBackBufferIndex();

        DX12FrameResource& current_frame_resource = context->m_FrameResources[context->m_CurrentFrameIndex];

        // if the current fence value is still less than "fenceValue", then we know the GPU has not finished executing
        // the command queue since it has not reached the "commandQueue->Signal(fence, fenceValue)" command

        HRESULT hr = S_OK;

        if (current_frame_resource.m_Fence->GetCompletedValue() < current_frame_resource.m_FenceValue)
        {
            // we have the fence create an event which is signaled once the fence's current value is "fenceValue"
            hr = current_frame_resource.m_Fence->SetEventOnCompletion(current_frame_resource.m_FenceValue, context->m_FenceEvent);
            CHECK_HR_ERROR(hr);

            // We will wait until the fence has triggered the event that it's current value has reached "fenceValue". once it's value
            // has reached "fenceValue", we know the command queue has finished executing
            WaitForSingleObject(context->m_FenceEvent, INFINITE);
        }

        // increment fenceValue for next frame
        current_frame_resource.m_FenceValue++;
    }

    static bool EndRenderPass(DX12Context* context)
    {
        DX12RenderTarget* current_rt = GetAssetFromContainer<DX12RenderTarget>(context->m_AssetHandleContainer, context->m_CurrentRenderTarget);

        if (!current_rt->m_IsBound)
        {
            return false;
        }

        if (current_rt->m_Id == DM_RENDERTARGET_BACKBUFFER_ID)
        {
            context->m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(current_rt->m_Resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
        }

        current_rt->m_IsBound = 0;
        return true;
    }

    static void BeginRenderPass(DX12Context* context, HRenderTarget render_target)
    {
        DX12RenderTarget* current_rt = GetAssetFromContainer<DX12RenderTarget>(context->m_AssetHandleContainer, context->m_CurrentRenderTarget);
        DX12RenderTarget* rt         = GetAssetFromContainer<DX12RenderTarget>(context->m_AssetHandleContainer, render_target);

        if (current_rt->m_Id == rt->m_Id &&
            current_rt->m_IsBound)
        {
            return;
        }

        if (current_rt->m_IsBound)
        {
            EndRenderPass(context);
        }

        if (current_rt->m_Id == DM_RENDERTARGET_BACKBUFFER_ID)
        {
            context->m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(current_rt->m_Resource, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));
        }

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(context->m_RtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), context->m_CurrentFrameIndex, context->m_RtvDescriptorSize);
        context->m_RtvHandle = rtvHandle;
        context->m_CommandList->OMSetRenderTargets(1, &context->m_RtvHandle, false, NULL);

        rt->m_IsBound = 1;

        context->m_CurrentRenderTarget = render_target;
    }

    template <typename T>
    static void DestroyResourceDeferred(DX12FrameResource& current_frame_resource, T* resource) 
    {
        if (resource == 0x0 || resource->m_Resource == 0x0)
        {
            return;
        }

        if (current_frame_resource.m_ResourcesToDestroy.Full())
        {
            current_frame_resource.m_ResourcesToDestroy.OffsetCapacity(8);
        }

        current_frame_resource.m_ResourcesToDestroy.Push(resource->m_Resource);
        resource->m_Destroyed = 1;
        resource->m_Resource = 0;
    }

    static void FlushResourcesToDestroy(DX12FrameResource& current_frame_resource)
    {
        if (current_frame_resource.m_ResourcesToDestroy.Size() > 0)
        {
            for (uint32_t i = 0; i < current_frame_resource.m_ResourcesToDestroy.Size(); ++i)
            {
                current_frame_resource.m_ResourcesToDestroy[i]->Release();
            }
            current_frame_resource.m_ResourcesToDestroy.SetSize(0);
        }
    }

    static void DX12BeginFrame(HContext _context)
    {
        DX12Context* context = (DX12Context*) _context;
        SyncronizeFrame(context);

        DX12FrameResource& current_frame_resource = context->m_FrameResources[context->m_CurrentFrameIndex];

        HRESULT hr = current_frame_resource.m_CommandAllocator->Reset();
        CHECK_HR_ERROR(hr);

        DX12RenderTarget* rt = GetAssetFromContainer<DX12RenderTarget>(context->m_AssetHandleContainer, context->m_MainRenderTarget);
        rt->m_Resource = current_frame_resource.m_RenderTarget.m_Resource;

        FlushResourcesToDestroy(current_frame_resource);

        // Enter "record" mode
        hr = context->m_CommandList->Reset(current_frame_resource.m_CommandAllocator, NULL); // Second argument is a pipeline object (TODO)
        CHECK_HR_ERROR(hr);

        current_frame_resource.m_ScratchBuffer.Reset(context);

        context->m_FrameBegun = 1;

        ID3D12DescriptorHeap* heaps[] = {
            context->m_SamplerPool.m_DescriptorHeap,
            current_frame_resource.m_ScratchBuffer.m_MemoryPools[0].m_DescriptorHeap
        };

        context->m_CommandList->SetDescriptorHeaps(DM_ARRAY_SIZE(heaps), heaps);

        BeginRenderPass(context, context->m_MainRenderTarget);
    }

    static void DX12Flip(HContext _context)
    {
        DX12Context* context = (DX12Context*) _context;
        EndRenderPass(context);

        DX12FrameResource& current_frame_resource = context->m_FrameResources[context->m_CurrentFrameIndex];

        // Close the command list for recording
        HRESULT hr = context->m_CommandList->Close();

        // create an array of command lists (only one command list here)
        ID3D12CommandList* execute_cmd_lists[] = { context->m_CommandList };

        // execute the array of command lists
        context->m_CommandQueue->ExecuteCommandLists(DM_ARRAY_SIZE(execute_cmd_lists), execute_cmd_lists);

        hr = context->m_CommandQueue->Signal(current_frame_resource.m_Fence, current_frame_resource.m_FenceValue);
        CHECK_HR_ERROR(hr);

        hr = context->m_SwapChain->Present(0, 0);
        CHECK_HR_ERROR(hr);

        context->m_FrameBegun = 0;
    }

    static inline uint32_t GetPitchFromMipMap(uint32_t pitch, uint8_t mipmap)
    {
        for (uint32_t i = 0; i < mipmap; ++i)
        {
            pitch /= 2;
        }
        return pitch;
    }

    static void CopyTextureData(const TextureParams& params, TextureFormat format_dst, TextureFormat format_src, D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout, uint32_t* num_rows, uint32_t array_count, uint32_t mipmap_count, uint32_t* slice_row_pitch, uint8_t* pixels, uint8_t* upload_data)
    {
        uint8_t bpp_dst = GetTextureFormatBitsPerPixel(format_dst) / 8;
        uint8_t bpp_src = GetTextureFormatBitsPerPixel(format_src) / 8;

        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& subResourceLayout = layout; // layouts[subResourceIndex];

        for (uint64_t array = 0; array < array_count; array++)
        {
            for (uint64_t mipmap = 0; mipmap < mipmap_count; mipmap++)
            {
                const uint64_t subResourceIndex = mipmap + (array * mipmap_count);
         
                //const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& subResourceLayout = layouts[subResourceIndex];
                const uint64_t subResourceHeight                            = num_rows[subResourceIndex];
                const uint64_t subResourcePitch                             = DM_ALIGN(subResourceLayout.Footprint.RowPitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
                const uint64_t subResourceDepth                             = subResourceLayout.Footprint.Depth;
                uint8_t* destinationSubResourceMemory                       = upload_data + subResourceLayout.Offset;

                uint64_t row_pitch = (uint64_t) slice_row_pitch[mipmap];
         
                for (uint64_t slice = 0; slice < subResourceDepth; slice++)
                {
                    // Todo: This isn't right
                    const uint8_t* sourceSubResourceMemory = pixels; // subImage->pixels;

                    if (params.m_SubUpdate)
                    {
                        for(int y = params.m_Y; y < (params.m_Y + params.m_Height); ++y)
                        {
                            uint8_t* dest_row = destinationSubResourceMemory + subResourcePitch * y;
                            uint8_t* dest_pixel_start = dest_row + bpp_src * params.m_X;
                            memcpy(dest_pixel_start, pixels, bpp_src * params.m_Width);
                            pixels += bpp_src * params.m_Width;
                        }
                    }
                    else
                    {
                        for (uint64_t height = 0; height < subResourceHeight; height++)
                        {
                            memcpy(destinationSubResourceMemory, sourceSubResourceMemory, dmMath::Min(subResourcePitch, row_pitch));
                            destinationSubResourceMemory += subResourcePitch;
                            sourceSubResourceMemory      += row_pitch;
                        }
                    }
                }
            }
        }
    }

    static void TextureBufferUploadHelper(DX12Context* context, DX12Texture* texture, TextureFormat format_dst, TextureFormat format_src, const TextureParams& params, uint8_t* pixels)
    {
        uint64_t slice_upload_size = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16] = { 0 };

        uint8_t bpp_dst            = GetTextureFormatBitsPerPixel(format_dst) / 8;
        uint32_t texture_pitch     = texture->m_Width * bpp_dst;
        uint32_t mipmap_pitch      = GetPitchFromMipMap(texture_pitch, params.m_MipMap);

        uint32_t num_rows[16];
        uint64_t row_size_in_bytes[16];
        context->m_Device->GetCopyableFootprints(&texture->m_ResourceDesc, params.m_MipMap, 1, 0, fp, num_rows, row_size_in_bytes, &slice_upload_size);

        // create upload heap
        // upload heaps are used to upload data to the GPU. CPU can write to it, GPU can read from it
        // We will upload the vertex buffer using this heap to the default heap
        D3D12_RESOURCE_DESC upload_desc = {};
        upload_desc.Dimension           = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_desc.Alignment           = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        upload_desc.Width               = slice_upload_size;
        upload_desc.Height              = 1;
        upload_desc.DepthOrArraySize    = 1;
        upload_desc.MipLevels           = 1;
        upload_desc.Format              = DXGI_FORMAT_UNKNOWN;
        upload_desc.Layout              = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        upload_desc.SampleDesc.Count    = 1;

        ID3D12Resource* upload_heap;
        HRESULT hr = context->m_Device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &upload_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            NULL,
            IID_PPV_ARGS(&upload_heap));
        CHECK_HR_ERROR(hr);

        uint8_t* upload_data = NULL;
        hr = upload_heap->Map(0, NULL, (void**) &upload_data);
        CHECK_HR_ERROR(hr);

        CopyTextureData(params, format_dst, format_src, fp[0], num_rows, 1, 1, &mipmap_pitch, pixels, upload_data);

        if (!context->m_FrameBegun)
        {
            hr = context->m_CommandList->Reset(context->m_FrameResources[0].m_CommandAllocator, NULL); // Second argument is a pipeline object (TODO)
            CHECK_HR_ERROR(hr);
        }

        if (texture->m_ResourceStates[params.m_MipMap] != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            context->m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(texture->m_Resource, texture->m_ResourceStates[params.m_MipMap], D3D12_RESOURCE_STATE_COPY_DEST, (UINT) params.m_MipMap));
            texture->m_ResourceStates[params.m_MipMap] = D3D12_RESOURCE_STATE_COPY_DEST;
        }

        D3D12_TEXTURE_COPY_LOCATION copy_dst = {};
        copy_dst.pResource        = texture->m_Resource;
        copy_dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        copy_dst.SubresourceIndex = params.m_MipMap;

        D3D12_TEXTURE_COPY_LOCATION copy_src = {};
        copy_src.pResource        = upload_heap;
        copy_src.Type             = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        copy_src.PlacedFootprint  = fp[0];

        D3D12_BOX box = {};
        box.top    = params.m_Y;
        box.left   = params.m_X;
        box.bottom = params.m_Y + params.m_Height;
        box.right  = params.m_X + params.m_Width;
        box.front  = 0;
        box.back   = 1;

        // The box acts like a clip box, which indicates where from the source we should take the data from
        context->m_CommandList->CopyTextureRegion(&copy_dst, params.m_X, params.m_Y, 0, &copy_src, &box);

        if (texture->m_ResourceStates[params.m_MipMap] != D3D12_RESOURCE_STATE_GENERIC_READ)
        {
            context->m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(texture->m_Resource, texture->m_ResourceStates[params.m_MipMap], D3D12_RESOURCE_STATE_GENERIC_READ, (UINT) params.m_MipMap));
            texture->m_ResourceStates[params.m_MipMap] = D3D12_RESOURCE_STATE_GENERIC_READ;
        }

        if (!context->m_FrameBegun)
        {
            context->m_CommandList->Close(); // THis might be wrong!
            ID3D12CommandList* execute_cmd_lists[] = { context->m_CommandList };
            context->m_CommandQueue->ExecuteCommandLists(DM_ARRAY_SIZE(execute_cmd_lists), execute_cmd_lists);
        }
    }

    static void CreateDeviceBuffer(DX12Context* context, DX12DeviceBuffer* device_buffer, uint32_t size)
    {
        assert(device_buffer->m_Resource == 0x0);

        // create default heap
        // default heap is memory on the GPU. Only the GPU has access to this memory
        // To get data into this heap, we will have to upload the data using
        // an upload heap
        HRESULT hr = context->m_Device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), // a default heap
            D3D12_HEAP_FLAG_NONE,                              // no flags
            &CD3DX12_RESOURCE_DESC::Buffer(size),              // resource description for a buffer
            D3D12_RESOURCE_STATE_COMMON,                       // we will start this heap in the copy destination state since we will copy data from the upload heap to this heap
            NULL,                                              // optimized clear value must be null for this type of resource. used for render targets and depth/stencil buffers
            IID_PPV_ARGS(&device_buffer->m_Resource));
        CHECK_HR_ERROR(hr);

        device_buffer->m_Resource->SetName(L"Vertex Buffer Resource Heap");
    }

    static void DeviceBufferUploadHelper(DX12Context* context, DX12DeviceBuffer* device_buffer, const void* data, uint32_t data_size)
    {
        if (data == 0 || data_size == 0)
            return;

        if (device_buffer->m_Destroyed || device_buffer->m_Resource == 0x0)
        {
            CreateDeviceBuffer(context, device_buffer, data_size);
        }

        // create upload heap
        // upload heaps are used to upload data to the GPU. CPU can write to it, GPU can read from it
        // We will upload the vertex buffer using this heap to the default heap
        ID3D12Resource* upload_heap;
        HRESULT hr = context->m_Device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),   // upload heap
            D3D12_HEAP_FLAG_NONE,                               // no flags
            &CD3DX12_RESOURCE_DESC::Buffer(data_size),          // resource description for a buffer
            D3D12_RESOURCE_STATE_GENERIC_READ,                  // GPU will read from this buffer and copy its contents to the default heap
            NULL,
            IID_PPV_ARGS(&upload_heap));
        CHECK_HR_ERROR(hr);

        upload_heap->SetName(L"Vertex Buffer Upload Resource Heap");

        // store vertex buffer in upload heap
        D3D12_SUBRESOURCE_DATA vx_data = {};
        vx_data.pData      = data; // pointer to our vertex array
        vx_data.RowPitch   = data_size; // size of all our vertex data
        vx_data.SlicePitch = data_size; // also the size of our vertex data

        if (!context->m_FrameBegun)
        {
            hr = context->m_CommandList->Reset(context->m_FrameResources[0].m_CommandAllocator, NULL); // Second argument is a pipeline object (TODO)
            CHECK_HR_ERROR(hr);
        }

        UpdateSubresources(context->m_CommandList, device_buffer->m_Resource, upload_heap, 0, 0, 1, &vx_data);

        // transition the vertex buffer data from copy destination state to vertex buffer state
        context->m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(device_buffer->m_Resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));

        if (!context->m_FrameBegun)
        {
            context->m_CommandList->Close(); // THis might be wrong!
            ID3D12CommandList* execute_cmd_lists[] = { context->m_CommandList };
            context->m_CommandQueue->ExecuteCommandLists(DM_ARRAY_SIZE(execute_cmd_lists), execute_cmd_lists);
        }

        device_buffer->m_DataSize = data_size;
    }

    static HVertexBuffer DX12NewVertexBuffer(HContext _context, uint32_t size, const void* data, BufferUsage buffer_usage)
    {
        DX12Context* context        = (DX12Context*) _context;
        DX12VertexBuffer* vx_buffer = new DX12VertexBuffer();

        if (size > 0)
        {
            DeviceBufferUploadHelper(context, &vx_buffer->m_DeviceBuffer, data, size);
        }

        return (HVertexBuffer) vx_buffer;
    }

    static void DX12DeleteVertexBuffer(HVertexBuffer buffer)
    {
        DX12VertexBuffer* buffer_ptr = (DX12VertexBuffer*) buffer;
        DestroyResourceDeferred(g_DX12Context->m_FrameResources[g_DX12Context->m_CurrentFrameIndex], &buffer_ptr->m_DeviceBuffer);
    }

    static void DX12SetVertexBufferData(HVertexBuffer _buffer, uint32_t size, const void* data, BufferUsage buffer_usage)
    {
        DM_PROFILE(__FUNCTION__);

        if (size == 0)
        {
            return;
        }

        DX12VertexBuffer* vx_buffer = (DX12VertexBuffer*) _buffer;
        //if (size != vx_buffer->m_DeviceBuffer.m_DataSize)
        {
            DestroyResourceDeferred(g_DX12Context->m_FrameResources[g_DX12Context->m_CurrentFrameIndex], &vx_buffer->m_DeviceBuffer);
        }

        DeviceBufferUploadHelper(g_DX12Context, &vx_buffer->m_DeviceBuffer, data, size);
    }

    static void DX12SetVertexBufferSubData(HVertexBuffer buffer, uint32_t offset, uint32_t size, const void* data)
    {
        assert(0);
    }

    static uint32_t DX12GetMaxElementsVertices(HContext context)
    {
        return 65536;
    }

    static HIndexBuffer DX12NewIndexBuffer(HContext _context, uint32_t size, const void* data, BufferUsage buffer_usage)
    {
        DX12Context* context       = (DX12Context*) _context;
        DX12IndexBuffer* ix_buffer = new DX12IndexBuffer();

        if (size > 0)
        {
            DeviceBufferUploadHelper(context, &ix_buffer->m_DeviceBuffer, data, size);
        }

        return (HIndexBuffer) ix_buffer;
    }

    static void DX12DeleteIndexBuffer(HIndexBuffer buffer)
    {
        DX12IndexBuffer* buffer_ptr = (DX12IndexBuffer*) buffer;
        DestroyResourceDeferred(g_DX12Context->m_FrameResources[g_DX12Context->m_CurrentFrameIndex], &buffer_ptr->m_DeviceBuffer);
    }

    static void DX12SetIndexBufferData(HIndexBuffer buffer, uint32_t size, const void* data, BufferUsage buffer_usage)
    {
        DM_PROFILE(__FUNCTION__);

        if (size == 0)
        {
            return;
        }

        DX12IndexBuffer* ix_buffer = (DX12IndexBuffer*) buffer;
        //if (ix_buffer->m_DeviceBuffer.m_Resource == 0x0)
        //{
        //    CreateDeviceBuffer(g_DX12Context, &ix_buffer->m_DeviceBuffer, size);
        //}

        DestroyResourceDeferred(g_DX12Context->m_FrameResources[g_DX12Context->m_CurrentFrameIndex], &ix_buffer->m_DeviceBuffer);

        DeviceBufferUploadHelper(g_DX12Context, &ix_buffer->m_DeviceBuffer, data, size);
    }

    static void DX12SetIndexBufferSubData(HIndexBuffer buffer, uint32_t offset, uint32_t size, const void* data)
    {
        assert(0);
    }

    static bool DX12IsIndexBufferFormatSupported(HContext context, IndexBufferFormat format)
    {
        return true;
    }

    static uint32_t DX12GetMaxElementsIndices(HContext context)
    {
        return 65536;
    }

    static HVertexDeclaration DX12NewVertexDeclaration(HContext context, HVertexStreamDeclaration stream_declaration)
    {
        VertexDeclaration* vd = new VertexDeclaration;
        memset(vd, 0, sizeof(VertexDeclaration));

        vd->m_Stride = 0;
        for (uint32_t i=0; i<stream_declaration->m_StreamCount; i++)
        {
            vd->m_Streams[i].m_NameHash  = stream_declaration->m_Streams[i].m_NameHash;
            vd->m_Streams[i].m_Location  = -1;
            vd->m_Streams[i].m_Size      = stream_declaration->m_Streams[i].m_Size;
            vd->m_Streams[i].m_Type      = stream_declaration->m_Streams[i].m_Type;
            vd->m_Streams[i].m_Normalize = stream_declaration->m_Streams[i].m_Normalize;
            vd->m_Streams[i].m_Offset    = vd->m_Stride;

            vd->m_Stride += stream_declaration->m_Streams[i].m_Size * GetTypeSize(stream_declaration->m_Streams[i].m_Type);
        }
        vd->m_StreamCount = stream_declaration->m_StreamCount;

        return vd;
    }

    static HVertexDeclaration DX12NewVertexDeclarationStride(HContext context, HVertexStreamDeclaration stream_declaration, uint32_t stride)
    {
        HVertexDeclaration vd = DX12NewVertexDeclaration(context, stream_declaration);
        vd->m_Stride = stride;
        return vd;
    }

    static void DX12EnableVertexBuffer(HContext _context, HVertexBuffer vertex_buffer, uint32_t binding_index)
    {
        DX12Context* context                          = (DX12Context*) _context;
        context->m_CurrentVertexBuffer[binding_index] = (DX12VertexBuffer*) vertex_buffer;
    }

    static void DX12DisableVertexBuffer(HContext _context, HVertexBuffer vertex_buffer)
    {
        DX12Context* context = (DX12Context*) _context;
        for (int i = 0; i < MAX_VERTEX_BUFFERS; ++i)
        {
            if (context->m_CurrentVertexBuffer[i] == (DX12VertexBuffer*) vertex_buffer)
                context->m_CurrentVertexBuffer[i] = 0;
        }
    }

    static void DX12EnableVertexDeclaration(HContext _context, HVertexDeclaration vertex_declaration, uint32_t binding_index, HProgram program)
    {
        DX12Context* context            = (DX12Context*) _context;
        DX12ShaderProgram* program_ptr  = (DX12ShaderProgram*) program;
        DX12ShaderModule* vertex_shader = program_ptr->m_VertexModule;

        context->m_MainVertexDeclaration[binding_index]                = {};
        context->m_MainVertexDeclaration[binding_index].m_Stride       = vertex_declaration->m_Stride;
        context->m_MainVertexDeclaration[binding_index].m_StepFunction = vertex_declaration->m_StepFunction;
        context->m_MainVertexDeclaration[binding_index].m_PipelineHash = vertex_declaration->m_PipelineHash;

        context->m_CurrentVertexDeclaration[binding_index]             = &context->m_MainVertexDeclaration[binding_index];

        uint32_t stream_ix = 0;
        uint32_t num_inputs = vertex_shader->m_ShaderMeta.m_Inputs.Size();

        for (int i = 0; i < vertex_declaration->m_StreamCount; ++i)
        {
            for (int j = 0; j < num_inputs; ++j)
            {
                ShaderResourceBinding& input = vertex_shader->m_ShaderMeta.m_Inputs[j];

                if (input.m_NameHash == vertex_declaration->m_Streams[i].m_NameHash)
                {
                    VertexDeclaration::Stream& stream = context->m_MainVertexDeclaration[binding_index].m_Streams[stream_ix];
                    stream.m_NameHash  = input.m_NameHash;
                    stream.m_Location  = input.m_Binding;
                    stream.m_Type      = vertex_declaration->m_Streams[i].m_Type;
                    stream.m_Offset    = vertex_declaration->m_Streams[i].m_Offset;
                    stream.m_Size      = vertex_declaration->m_Streams[i].m_Size;
                    stream.m_Normalize = vertex_declaration->m_Streams[i].m_Normalize;
                    stream_ix++;

                    context->m_MainVertexDeclaration[binding_index].m_StreamCount++;
                    break;
                }
            }
        }
    }

    static void DX12DisableVertexDeclaration(HContext _context, HVertexDeclaration vertex_declaration)
    {
        DX12Context* context = (DX12Context*) _context;
        for (int i = 0; i < MAX_VERTEX_BUFFERS; ++i)
        {
            if (context->m_CurrentVertexDeclaration[i] == vertex_declaration)
                context->m_CurrentVertexDeclaration[i] = 0;
        }
    }

    static inline D3D_PRIMITIVE_TOPOLOGY GetPrimitiveTopology(PrimitiveType prim_type)
    {
        switch(prim_type)
        {
            case PRIMITIVE_LINES:          return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            case PRIMITIVE_TRIANGLES:      return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            case PRIMITIVE_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            default:break;
        }
        return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }

    static inline DXGI_FORMAT GetDXGIFormat(Type type, uint16_t size, bool normalized)
    {
        /*
        // undefined formats:
        // DXGI_FORMAT_R8G8B8_SNORM
        // DXGI_FORMAT_R8G8B8_SINT
        // DXGI_FORMAT_R8G8B8_UNORM
        // DXGI_FORMAT_R8G8B8_UINT
        // DXGI_FORMAT_R16G16B16_SNORM
        // DXGI_FORMAT_R16G16B16_SINT
        // DXGI_FORMAT_R16G16B16_UNORM
        // DXGI_FORMAT_R16G16B16_UINT
        */
        if (type == TYPE_FLOAT)
        {
            switch(size)
            {
                case 1: return DXGI_FORMAT_R32_FLOAT;
                case 2: return DXGI_FORMAT_R32G32_FLOAT;
                case 3: return DXGI_FORMAT_R32G32B32_FLOAT;
                case 4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
                default:break;
            }
        }
        else if (type == TYPE_INT)
        {
            switch(size)
            {
                case 1: return DXGI_FORMAT_R32_SINT;
                case 2: return DXGI_FORMAT_R32G32_SINT;
                case 3: return DXGI_FORMAT_R32G32B32_SINT;
                case 4: return DXGI_FORMAT_R32G32B32A32_SINT;
                default:break;
            }
        }
        else if (type == TYPE_UNSIGNED_INT)
        {
            switch(size)
            {
                case 1: return DXGI_FORMAT_R32_UINT;
                case 2: return DXGI_FORMAT_R32G32_UINT;
                case 3: return DXGI_FORMAT_R32G32B32_UINT;
                case 4: return DXGI_FORMAT_R32G32B32A32_UINT;
                default:break;
            }
        }
        else if (type == TYPE_BYTE)
        {
            switch(size)
            {
                case 1: return normalized ? DXGI_FORMAT_R8_SNORM : DXGI_FORMAT_R8_SINT;
                case 2: return normalized ? DXGI_FORMAT_R8G8_SNORM : DXGI_FORMAT_R8G8_SINT;
                // case 3: return normalized ? DXGI_FORMAT_R8G8B8_SNORM : DXGI_FORMAT_R8G8B8_SINT;
                case 4: return normalized ? DXGI_FORMAT_R8G8B8A8_SNORM : DXGI_FORMAT_R8G8B8A8_SINT;
                default:break;
            }
        }
        else if (type == TYPE_UNSIGNED_BYTE)
        {
            switch(size)
            {
                case 1: return normalized ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8_UINT;
                case 2: return normalized ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8G8_UINT;
                // case 3: return normalized ? DXGI_FORMAT_R8G8B8_UNORM : DXGI_FORMAT_R8G8B8_UINT;
                case 4: return normalized ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UINT;
                default:break;
            }
        }
        else if (type == TYPE_SHORT)
        {
            switch(size)
            {
                case 1: return normalized ? DXGI_FORMAT_R16_SNORM : DXGI_FORMAT_R16_SINT;
                case 2: return normalized ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16_SINT;
                //case 3: return normalized ? DXGI_FORMAT_R16G16B16_SNORM : DXGI_FORMAT_R16G16B16_SINT;
                case 4: return normalized ? DXGI_FORMAT_R16G16B16A16_SNORM : DXGI_FORMAT_R16G16B16A16_SINT;
                default:break;
            }
        }
        else if (type == TYPE_UNSIGNED_SHORT)
        {
            switch(size)
            {
                case 1: return normalized ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R16_UINT;
                case 2: return normalized ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16G16_UINT;
                //case 3: return normalized ? DXGI_FORMAT_R16G16B16_UNORM : DXGI_FORMAT_R16G16B16_UINT;
                case 4: return normalized ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R16G16B16A16_UINT;
                default:break;
            }
        }
        else if (type == TYPE_FLOAT_MAT4)
        {
            return DXGI_FORMAT_R32_FLOAT;
        }
        else if (type == TYPE_FLOAT_VEC4)
        {
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        }

        assert(0 && "Unable to deduce type from dmGraphics::Type");
        return DXGI_FORMAT_UNKNOWN;
    }

    static inline D3D12_CULL_MODE GetCullMode(const PipelineState& state)
    {
        if (state.m_CullFaceEnabled)
        {
            if (state.m_CullFaceType == FACE_TYPE_BACK)
                return D3D12_CULL_MODE_BACK;
            else if (state.m_CullFaceType == FACE_TYPE_FRONT)
                return D3D12_CULL_MODE_FRONT;
            // FRONT_AND_BACK not supported
        }
        return D3D12_CULL_MODE_NONE;
    }

    static void CreatePipeline(DX12Context* context, DX12RenderTarget* rt, DX12Pipeline* pipeline)
    {
        D3D12_SHADER_BYTECODE vs_byte_code = {};
        vs_byte_code.BytecodeLength        = context->m_CurrentProgram->m_VertexModule->m_ShaderBlob->GetBufferSize();
        vs_byte_code.pShaderBytecode       = context->m_CurrentProgram->m_VertexModule->m_ShaderBlob->GetBufferPointer();

        D3D12_SHADER_BYTECODE fs_byte_code = {};
        fs_byte_code.BytecodeLength        = context->m_CurrentProgram->m_FragmentModule->m_ShaderBlob->GetBufferSize();
        fs_byte_code.pShaderBytecode       = context->m_CurrentProgram->m_FragmentModule->m_ShaderBlob->GetBufferPointer();

        uint32_t stream_count = 0;
        D3D12_INPUT_ELEMENT_DESC input_layout[MAX_VERTEX_STREAM_COUNT] = {};

        for (int i = 0; i < MAX_VERTEX_BUFFERS; ++i)
        {
            if (context->m_CurrentVertexDeclaration[i])
            {
                for (int j = 0; j < context->m_CurrentVertexDeclaration[i]->m_StreamCount; ++j)
                {
                    VertexDeclaration::Stream& stream = context->m_CurrentVertexDeclaration[i]->m_Streams[j];
                    D3D12_INPUT_ELEMENT_DESC& desc    = input_layout[stream_count];

                    desc.SemanticName         = "TEXCOORD";
                    desc.SemanticIndex        = stream.m_Location;
                    desc.Format               = GetDXGIFormat(stream.m_Type, stream.m_Size, stream.m_Normalize);
                    desc.InputSlot            = i;
                    desc.AlignedByteOffset    = stream.m_Offset;
                    desc.InputSlotClass       = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                    desc.InstanceDataStepRate = 0;

                    stream_count++;
                }
            }
        }

        D3D12_INPUT_LAYOUT_DESC inputLayoutDesc = {};
        inputLayoutDesc.NumElements             = stream_count;
        inputLayoutDesc.pInputElementDescs      = input_layout;

        CD3DX12_RASTERIZER_DESC rasterizerState = CD3DX12_RASTERIZER_DESC(
            D3D12_FILL_MODE_SOLID,
            GetCullMode(context->m_PipelineState),
            true,                                       // FrontCounterClockwise
            D3D12_DEFAULT_DEPTH_BIAS,
            D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
            D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
            true,                                       // DepthClipEnable
            false,                                      // MultisampleEnable
            false,                                      // AntialiasedLineEnable: TODO
            0,                                          // forcedSampleCount
            D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF); // conservativeRaster

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {}; // a structure to define a pso
        psoDesc.InputLayout           = inputLayoutDesc; // the structure describing our input layout
        psoDesc.pRootSignature        = context->m_CurrentProgram->m_RootSignature;
        psoDesc.VS                    = vs_byte_code;
        psoDesc.PS                    = fs_byte_code;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; // SHould we support points?
        psoDesc.RTVFormats[0]         = rt->m_Format;
        psoDesc.SampleDesc            = rt->m_SampleDesc; // must be the same sample description as the swapchain and depth/stencil buffer
        psoDesc.SampleMask            = UINT_MAX; // TODO: sample mask has to do with multi-sampling. 0xffffffff means point sampling is done
        psoDesc.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT); // TODO? rasterizerState;
        psoDesc.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT); // TODO
        psoDesc.NumRenderTargets      = 1; // TODO

        HRESULT hr = context->m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pipeline));
        CHECK_HR_ERROR(hr);
    }

    static DX12Pipeline* GetOrCreatePipeline(DX12Context* context, DX12RenderTarget* current_rt)
    {
        HashState64 pipeline_hash_state;
        dmHashInit64(&pipeline_hash_state, false);
        //dmHashUpdateBuffer64(&pipeline_hash_state, &context->m_CurrentProgram->m_Hash, sizeof(context->m_CurrentProgram->m_Hash));
        dmHashUpdateBuffer64(&pipeline_hash_state, &context->m_PipelineState,                   sizeof(context->m_PipelineState));
        dmHashUpdateBuffer64(&pipeline_hash_state, &current_rt->m_Id,                           sizeof(current_rt->m_Id));
        dmHashUpdateBuffer64(&pipeline_hash_state, &context->m_CurrentProgram->m_RootSignature, sizeof(context->m_CurrentProgram->m_RootSignature));
        // dmHashUpdateBuffer64(&pipeline_hash_state, &vk_sample_count,  sizeof(vk_sample_count));

        // for (int i = 0; i < vertexDeclarationCount; ++i)
        // {
        //     dmHashUpdateBuffer64(&pipeline_hash_state, &context->m_CurrentVertexDeclaration[i]->m_PipelineHash, sizeof(context->m_CurrentVertexDeclaration[i]->m_PipelineHash));
        //     dmHashUpdateBuffer64(&pipeline_hash_state, &context->m_CurrentVertexDeclaration[i]->m_StepFunction, sizeof(context->m_CurrentVertexDeclaration[i]->m_StepFunction));
        // }

        dmhash_t pipeline_hash = dmHashFinal64(&pipeline_hash_state);
        DX12Pipeline* cached_pipeline = context->m_PipelineCache.Get(pipeline_hash);

        if (!cached_pipeline)
        {
            if (context->m_PipelineCache.Full())
            {
                context->m_PipelineCache.SetCapacity(32, context->m_PipelineCache.Capacity() + 4);
            }

            context->m_PipelineCache.Put(pipeline_hash, {});
            cached_pipeline = context->m_PipelineCache.Get(pipeline_hash);
            CreatePipeline(context, current_rt, cached_pipeline);

            dmLogDebug("Created new DX12 Pipeline with hash %llu", (unsigned long long) pipeline_hash);
        }

        return cached_pipeline;
    }

    static inline void SetViewportAndScissorHelper(DX12Context* context, int32_t x, int32_t y, int32_t width, int32_t height)
    {
        D3D12_VIEWPORT viewport;
        D3D12_RECT scissor;

        viewport.TopLeftX = (float) x;
        viewport.TopLeftY = (float) y;
        viewport.Width    = (float) width;
        viewport.Height   = (float) height;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        scissor.left   = (float) x;
        scissor.top    = (float) y;
        scissor.right  = (float) width;
        scissor.bottom = (float) height;

        context->m_CommandList->RSSetViewports(1, &viewport);
        context->m_CommandList->RSSetScissorRects(1, &scissor);
    }

    static void CommitUniforms(DX12Context* context, DX12FrameResource& frame_resources)
    {
        DX12ShaderProgram* program = context->m_CurrentProgram;

        uint32_t texture_unit_start = program->m_UniformBufferCount;

        for (int set = 0; set < program->m_MaxSet; ++set)
        {
            for (int binding = 0; binding < program->m_MaxBinding; ++binding)
            {
                ProgramResourceBinding& pgm_res = program->m_ResourceBindings[set][binding];

                if (pgm_res.m_Res == 0x0)
                    continue;

                switch(pgm_res.m_Res->m_BindingFamily)
                {
                    case ShaderResourceBinding::BINDING_FAMILY_TEXTURE:
                    {
                        DX12Texture* texture              = GetAssetFromContainer<DX12Texture>(context->m_AssetHandleContainer, context->m_CurrentTextures[pgm_res.m_TextureUnit]);
                        const DX12TextureSampler& sampler = context->m_TextureSamplers[texture->m_TextureSamplerIndex];
                        uint32_t ix = texture_unit_start + pgm_res.m_TextureUnit * 2;
                        frame_resources.m_ScratchBuffer.AllocateTexture2D(context, texture, ix, sampler, ix + 1);
                    } break;
                    case ShaderResourceBinding::BINDING_FAMILY_STORAGE_BUFFER:
                    {
                        assert(0);
                    } break;
                    case ShaderResourceBinding::BINDING_FAMILY_UNIFORM_BUFFER:
                    {
                        const uint32_t uniform_size_nonalign = pgm_res.m_Res->m_BlockSize;
                        void* gpu_mapped_memory = frame_resources.m_ScratchBuffer.AllocateConstantBuffer(context, pgm_res.m_Res->m_Binding, uniform_size_nonalign);
                        memcpy(gpu_mapped_memory, &program->m_UniformData[pgm_res.m_DataOffset], uniform_size_nonalign);
                    } break;
                    case ShaderResourceBinding::BINDING_FAMILY_GENERIC:
                    default: continue;
                }
            }
        }
    }

    static void DrawSetup(DX12Context* context, PrimitiveType prim_type)
    {
        assert(context->m_CurrentProgram);

        DX12FrameResource& frame_resources = context->m_FrameResources[context->m_CurrentFrameIndex];

        DX12RenderTarget* current_rt = GetAssetFromContainer<DX12RenderTarget>(context->m_AssetHandleContainer, context->m_CurrentRenderTarget);

        D3D12_VERTEX_BUFFER_VIEW vx_buffer_views[MAX_VERTEX_BUFFERS];
        uint32_t num_vx_buffers = 0;

        for (int i = 0; i < MAX_VERTEX_BUFFERS; ++i)
        {
            if (context->m_CurrentVertexBuffer[i] && context->m_CurrentVertexDeclaration[i])
            {
                vx_buffer_views[num_vx_buffers].BufferLocation = context->m_CurrentVertexBuffer[i]->m_DeviceBuffer.m_Resource->GetGPUVirtualAddress();
                vx_buffer_views[num_vx_buffers].SizeInBytes    = context->m_CurrentVertexBuffer[i]->m_DeviceBuffer.m_DataSize;
                vx_buffer_views[num_vx_buffers].StrideInBytes  = context->m_CurrentVertexDeclaration[i]->m_Stride;
                num_vx_buffers++;
            }
        }

        // Update the viewport
        if (context->m_ViewportChanged)
        {
            DX12Viewport& vp = context->m_CurrentViewport;
            SetViewportAndScissorHelper(context, vp.m_X, vp.m_Y, vp.m_W, vp.m_H);
            context->m_ViewportChanged = 0;
        }

        DX12Pipeline* pipeline = GetOrCreatePipeline(context, current_rt);
        context->m_CommandList->SetGraphicsRootSignature(context->m_CurrentProgram->m_RootSignature);
        context->m_CommandList->SetPipelineState(*pipeline);
        context->m_CommandList->IASetPrimitiveTopology(GetPrimitiveTopology(prim_type));
        context->m_CommandList->IASetVertexBuffers(0, num_vx_buffers, vx_buffer_views); // set the vertex buffer (using the vertex buffer view)

        // frame_resources.m_ScratchBuffer.Bind(context);
        CommitUniforms(context, frame_resources);
    }

    static void DX12DrawElements(HContext _context, PrimitiveType prim_type, uint32_t first, uint32_t count, Type type, HIndexBuffer index_buffer)
    {
        DX12Context* context = (DX12Context*) _context;
        DrawSetup(context, prim_type);

        DX12IndexBuffer* ix_buffer   = (DX12IndexBuffer*) index_buffer;
        D3D12_INDEX_BUFFER_VIEW view = {};
        view.BufferLocation          = ix_buffer->m_DeviceBuffer.m_Resource->GetGPUVirtualAddress();
        view.SizeInBytes             = ix_buffer->m_DeviceBuffer.m_DataSize;
        view.Format                  = type == dmGraphics::TYPE_UNSIGNED_SHORT ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        uint32_t index_offset        = first / (type == TYPE_UNSIGNED_SHORT ? 2 : 4);

        context->m_CommandList->IASetIndexBuffer(&view);
        context->m_CommandList->DrawIndexedInstanced(count, 1, index_offset, 0, 0); // draw first quad
    }

    static void DX12Draw(HContext _context, PrimitiveType prim_type, uint32_t first, uint32_t count)
    {
        DX12Context* context = (DX12Context*) _context;
        DrawSetup(context, prim_type);

        context->m_CommandList->DrawInstanced(count, 1, first, 0);
    }

    static HComputeProgram DX12NewComputeProgram(HContext context, ShaderDesc::Shader* ddf)
    {
        return 0;
    }

    static HProgram DX12NewProgramFromCompute(HContext context, HComputeProgram compute_program)
    {
        return (HProgram) 0;
    }

    static void DX12DeleteComputeProgram(HComputeProgram prog)
    {
    }

    static bool DX12ReloadProgramCompute(HContext context, HProgram program, HComputeProgram compute_program)
    {
        return true;
    }

    static bool DX12ReloadComputeProgram(HComputeProgram prog, ShaderDesc::Shader* ddf)
    {
        return true;
    }

    static D3D12_SHADER_VISIBILITY GetShaderVisibilityFromStage(uint8_t stage_flag)
    {
        if (stage_flag & SHADER_STAGE_FLAG_VERTEX && stage_flag & SHADER_STAGE_FLAG_FRAGMENT)
        {
            return D3D12_SHADER_VISIBILITY_ALL;
        }
        else if (stage_flag & SHADER_STAGE_FLAG_VERTEX)
        {
            return D3D12_SHADER_VISIBILITY_VERTEX;
        }
        else if (stage_flag & SHADER_STAGE_FLAG_FRAGMENT)
        {
            return D3D12_SHADER_VISIBILITY_PIXEL;
        }
        else if (stage_flag & SHADER_STAGE_FLAG_COMPUTE)
        {
            return D3D12_SHADER_VISIBILITY_ALL;
        }
        return D3D12_SHADER_VISIBILITY_ALL;
    }

    struct ResourceBindingDesc
    {
        uint16_t m_Binding;
        uint8_t  m_Taken;
    };

    static void FillProgramResourceBindings(
        DX12ShaderProgram*               program,
        dmArray<ShaderResourceBinding>&  resources,
        dmArray<ShaderResourceTypeInfo>& stage_type_infos,
        ResourceBindingDesc              bindings[MAX_SET_COUNT][MAX_BINDINGS_PER_SET_COUNT],
        uint32_t                         ubo_alignment,
        uint32_t                         ssbo_alignment,
        ShaderStageFlag                  stage_flag,
        ProgramResourceBindingsInfo&     info)
    {
        for (int i = 0; i < resources.Size(); ++i)
        {
            ShaderResourceBinding& res   = resources[i];
            ResourceBindingDesc& binding = bindings[res.m_Set][res.m_Binding];
            ProgramResourceBinding& program_resource_binding = program->m_ResourceBindings[res.m_Set][res.m_Binding];

            if (!binding.m_Taken)
            {
                binding.m_Binding = res.m_Binding;
                binding.m_Taken   = 1;

                program_resource_binding.m_Res         = &res;
                program_resource_binding.m_TypeInfos   = &stage_type_infos;
                program_resource_binding.m_StageFlags |= (int) stage_flag;

                switch(res.m_BindingFamily)
                {
                    case ShaderResourceBinding::BINDING_FAMILY_TEXTURE:
                        program_resource_binding.m_TextureUnit = info.m_TextureCount;
                        info.m_TextureCount++;
                        info.m_TotalUniformCount++;
                        break;
                    case ShaderResourceBinding::BINDING_FAMILY_STORAGE_BUFFER:
                        program_resource_binding.m_StorageBufferUnit = info.m_StorageBufferCount;
                        info.m_StorageBufferCount++;
                        info.m_TotalUniformCount++;

                    #if 0
                        dmLogInfo("SSBO: name=%s, set=%d, binding=%d, ssbo-unit=%d", res.m_Name, res.m_Set, res.m_Binding, program_resource_binding.m_StorageBufferUnit);
                    #endif

                        break;
                    case ShaderResourceBinding::BINDING_FAMILY_UNIFORM_BUFFER:
                    {
                        assert(res.m_Type.m_UseTypeIndex);
                        const ShaderResourceTypeInfo& type_info       = stage_type_infos[res.m_Type.m_TypeIndex];
                        program_resource_binding.m_DataOffset         = info.m_UniformDataSize;
                        program_resource_binding.m_DynamicOffsetIndex = info.m_UniformBufferCount;

                        info.m_UniformBufferCount++;
                        info.m_UniformDataSize        += res.m_BlockSize;
                        info.m_UniformDataSizeAligned += DM_ALIGN(res.m_BlockSize, ubo_alignment);
                        info.m_TotalUniformCount      += type_info.m_Members.Size();
                    }
                    break;
                    case ShaderResourceBinding::BINDING_FAMILY_GENERIC:
                    default:break;
                }

                info.m_MaxSet     = dmMath::Max(info.m_MaxSet, (uint32_t) (res.m_Set + 1));
                info.m_MaxBinding = dmMath::Max(info.m_MaxBinding, (uint32_t) (res.m_Binding + 1));

            #if 1
                dmLogInfo("    name=%s, set=%d, binding=%d, data_offset=%d, texture_unit=%d", res.m_Name, res.m_Set, res.m_Binding, program_resource_binding.m_DataOffset, program_resource_binding.m_TextureUnit);
            #endif
            }
        }
    }

    static void FillProgramResourceBindings(
        DX12ShaderProgram*           program,
        DX12ShaderModule*            module,
        ResourceBindingDesc          bindings[MAX_SET_COUNT][MAX_BINDINGS_PER_SET_COUNT],
        uint32_t                     ubo_alignment,
        uint32_t                     ssbo_alignment,
        ShaderStageFlag              stage_flag,
        ProgramResourceBindingsInfo& info)
    {
        if (program && module)
        {
            FillProgramResourceBindings(program, module->m_ShaderMeta.m_UniformBuffers, module->m_ShaderMeta.m_TypeInfos, bindings, ubo_alignment, ssbo_alignment, stage_flag, info);
            FillProgramResourceBindings(program, module->m_ShaderMeta.m_StorageBuffers, module->m_ShaderMeta.m_TypeInfos, bindings, ubo_alignment, ssbo_alignment, stage_flag, info);
            FillProgramResourceBindings(program, module->m_ShaderMeta.m_Textures, module->m_ShaderMeta.m_TypeInfos, bindings, ubo_alignment, ssbo_alignment, stage_flag, info);
        }
    }

    static void CreateProgramResourceBindings(DX12ShaderProgram* program, DX12ShaderModule* vertex_module, DX12ShaderModule* fragment_module, DX12ShaderModule* compute_module)
    {
        ResourceBindingDesc bindings[MAX_SET_COUNT][MAX_BINDINGS_PER_SET_COUNT] = {};

        uint32_t ubo_alignment = UNIFORM_BUFFERS_ALIGNMENT;
        uint32_t ssbo_alignment = 0; // TODO

        ProgramResourceBindingsInfo binding_info = {};
        FillProgramResourceBindings(program, vertex_module, bindings, ubo_alignment, ssbo_alignment, SHADER_STAGE_FLAG_VERTEX, binding_info);
        FillProgramResourceBindings(program, fragment_module, bindings, ubo_alignment, ssbo_alignment, SHADER_STAGE_FLAG_FRAGMENT, binding_info);
        FillProgramResourceBindings(program, compute_module, bindings, ubo_alignment, ssbo_alignment, SHADER_STAGE_FLAG_COMPUTE, binding_info);

        program->m_UniformData = new uint8_t[binding_info.m_UniformDataSize];
        memset(program->m_UniformData, 0, binding_info.m_UniformDataSize);

        program->m_UniformDataSizeAligned = binding_info.m_UniformDataSizeAligned;
        program->m_UniformBufferCount     = binding_info.m_UniformBufferCount;
        program->m_StorageBufferCount     = binding_info.m_StorageBufferCount;
        program->m_TextureSamplerCount    = binding_info.m_TextureCount;
        program->m_TotalUniformCount      = binding_info.m_TotalUniformCount;
        program->m_TotalResourcesCount    = binding_info.m_UniformBufferCount + binding_info.m_TextureCount + binding_info.m_StorageBufferCount; // num actual descriptors
        program->m_MaxSet                 = binding_info.m_MaxSet;
        program->m_MaxBinding             = binding_info.m_MaxBinding;
    }

    static HProgram DX12NewProgram(HContext context, HVertexProgram vertex_program, HFragmentProgram fragment_program)
    {
        DX12ShaderProgram* program = new DX12ShaderProgram();
        program->m_VertexModule    = (DX12ShaderModule*) vertex_program;
        program->m_FragmentModule  = (DX12ShaderModule*) fragment_program;
        program->m_ComputeModule   = 0;

        dmLogInfo("New program");

        CreateProgramResourceBindings(program, program->m_VertexModule, program->m_FragmentModule, 0);

        dmArray<CD3DX12_ROOT_PARAMETER > root_parameter_descs;
        root_parameter_descs.SetCapacity(program->m_UniformBufferCount + program->m_TextureSamplerCount * 2);
        root_parameter_descs.SetSize(root_parameter_descs.Capacity());

        uint32_t texture_unit_start = program->m_UniformBufferCount;
        uint32_t texture_ix         = 0;
        uint32_t ubo_ix             = 0;

        for (int set = 0; set < program->m_MaxSet; ++set)
        {
            for (int binding = 0; binding < program->m_MaxBinding; ++binding)
            {
                ProgramResourceBinding& pgm_res = program->m_ResourceBindings[set][binding];

                if (pgm_res.m_Res == 0x0)
                    continue;
                switch(pgm_res.m_Res->m_BindingFamily)
                {
                    case ShaderResourceBinding::BINDING_FAMILY_TEXTURE:
                    {
                        uint32_t ix = texture_unit_start + pgm_res.m_TextureUnit * 2;

                        CD3DX12_DESCRIPTOR_RANGE texture_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
                        CD3DX12_DESCRIPTOR_RANGE sampler_range(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0);

                        root_parameter_descs[ix + 0].InitAsDescriptorTable(1, &texture_range, GetShaderVisibilityFromStage(pgm_res.m_StageFlags));
                        root_parameter_descs[ix + 1].InitAsDescriptorTable(1, &sampler_range, GetShaderVisibilityFromStage(pgm_res.m_StageFlags));

                        texture_ix++;
                    } break;
                    case ShaderResourceBinding::BINDING_FAMILY_STORAGE_BUFFER:
                    {
                        // TODO
                        assert(0);
                    } break;
                    case ShaderResourceBinding::BINDING_FAMILY_UNIFORM_BUFFER:
                    {
                        root_parameter_descs[ubo_ix].InitAsConstantBufferView(pgm_res.m_Res->m_Binding, 0, GetShaderVisibilityFromStage(pgm_res.m_StageFlags));
                        ubo_ix++;
                    } break;
                    case ShaderResourceBinding::BINDING_FAMILY_GENERIC:
                    default: continue;
                }
            }
        }

        CD3DX12_ROOT_SIGNATURE_DESC root_signature_desc;
        root_signature_desc.Init(
            root_parameter_descs.Size(),
            root_parameter_descs.Begin(),
            // No static samplers
            0, 0,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED |
            // we can deny more shader stages here for better performance
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

        CreateRootSignature((DX12Context*) context, &root_signature_desc, program);

        return (HProgram) program;
    }

    static void DX12DeleteProgram(HContext context, HProgram program)
    {
    }

    static HRESULT CreateShaderModule(DX12Context* context, const char* target, void* data, uint32_t data_size, DX12ShaderModule* shader)
    {
        ID3DBlob* error_blob;
        uint32_t flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;

        HRESULT hr = D3DCompile(data, data_size, NULL, NULL, NULL, "main", target, flags, 0, &shader->m_ShaderBlob, &error_blob);
        if (FAILED(hr))
        {
            dmLogError("%s", error_blob->GetBufferPointer());
            return hr;
        }
        return S_OK;
    }

    static HVertexProgram DX12NewVertexProgram(HContext _context, ShaderDesc::Shader* ddf)
    {
        DX12Context* context = (DX12Context*) _context;
        DX12ShaderModule* shader = new DX12ShaderModule;
        memset(shader, 0, sizeof(*shader));

        HRESULT hr = CreateShaderModule(context, "vs_5_0", ddf->m_Source.m_Data, ddf->m_Source.m_Count, shader);
        CHECK_HR_ERROR(hr);

        CreateShaderMeta(ddf, &shader->m_ShaderMeta);
        return (HVertexProgram) shader;
    }

    static HFragmentProgram DX12NewFragmentProgram(HContext _context, ShaderDesc::Shader* ddf)
    {
        DX12Context* context = (DX12Context*) _context;
        DX12ShaderModule* shader = new DX12ShaderModule;
        memset(shader, 0, sizeof(*shader));

        HRESULT hr = CreateShaderModule(context, "ps_5_0", ddf->m_Source.m_Data, ddf->m_Source.m_Count, shader);
        CHECK_HR_ERROR(hr);

        CreateShaderMeta(ddf, &shader->m_ShaderMeta);
        return (HVertexProgram) shader;
    }

    static bool DX12ReloadVertexProgram(HVertexProgram prog, ShaderDesc::Shader* ddf)
    {
        return 0;
    }

    static bool DX12ReloadFragmentProgram(HFragmentProgram prog, ShaderDesc::Shader* ddf)
    {
        return 0;
    }

    static void DX12DeleteVertexProgram(HVertexProgram program)
    {
    }

    static void DX12DeleteFragmentProgram(HFragmentProgram program)
    {
    }

    static ShaderDesc::Language DX12GetProgramLanguage(HProgram program)
    {
        return ShaderDesc::LANGUAGE_HLSL;
    }

    static ShaderDesc::Language DX12GetShaderProgramLanguage(HContext context, ShaderDesc::ShaderClass shader_class)
    {
        return ShaderDesc::LANGUAGE_HLSL;
    }

    static void DX12EnableProgram(HContext context, HProgram program)
    {
        ((DX12Context*) context)->m_CurrentProgram = (DX12ShaderProgram*) program;
    }

    static void DX12DisableProgram(HContext context)
    {
        ((DX12Context*) context)->m_CurrentProgram = 0;
    }

    static bool DX12ReloadProgramGraphics(HContext context, HProgram program, HVertexProgram vert_program, HFragmentProgram frag_program)
    {
        return true;
    }

    static uint32_t DX12GetAttributeCount(HProgram prog)
    {
        DX12ShaderProgram* program_ptr = (DX12ShaderProgram*) prog;
        return program_ptr->m_VertexModule->m_ShaderMeta.m_Inputs.Size();
    }

    static void DX12GetAttribute(HProgram prog, uint32_t index, dmhash_t* name_hash, Type* type, uint32_t* element_count, uint32_t* num_values, int32_t* location)
    {
        DX12ShaderProgram* program_ptr = (DX12ShaderProgram*) prog;
        assert(index < program_ptr->m_VertexModule->m_ShaderMeta.m_Inputs.Size());
        ShaderResourceBinding& attr = program_ptr->m_VertexModule->m_ShaderMeta.m_Inputs[index];

        *name_hash     = attr.m_NameHash;
        *type          = ShaderDataTypeToGraphicsType(attr.m_Type.m_ShaderType);
        *num_values    = 1;
        *location      = attr.m_Binding;
        *element_count = GetShaderTypeSize(attr.m_Type.m_ShaderType) / sizeof(float);
    }

    static uint32_t DX12GetUniformCount(HProgram prog)
    {
        DX12ShaderProgram* program_ptr = (DX12ShaderProgram*) prog;
        return program_ptr->m_TotalUniformCount;
    }

    // MOVE TO graphics.cpp
    static uint32_t GetUniformName(const ProgramResourceBinding bindings[MAX_SET_COUNT][MAX_BINDINGS_PER_SET_COUNT], uint8_t max_set, uint8_t max_binding, uint32_t index, char* buffer, uint32_t buffer_size, Type* type, int32_t* size)
    {
        uint32_t search_index = 0;
        for (int set = 0; set < max_set; ++set)
        {
            for (int binding = 0; binding < max_binding; ++binding)
            {
                const ProgramResourceBinding& pgm_res = bindings[set][binding];

                if (pgm_res.m_Res == 0x0)
                    continue;

                if (pgm_res.m_Res->m_BindingFamily == ShaderResourceBinding::BINDING_FAMILY_TEXTURE ||
                    pgm_res.m_Res->m_BindingFamily == ShaderResourceBinding::BINDING_FAMILY_STORAGE_BUFFER)
                {
                    if (search_index == index)
                    {
                        ShaderResourceBinding* res = pgm_res.m_Res;
                        *type = ShaderDataTypeToGraphicsType(res->m_Type.m_ShaderType);
                        *size = 1;
                        return (uint32_t)dmStrlCpy(buffer, res->m_Name, buffer_size);
                    }
                    search_index++;
                }
                else if (pgm_res.m_Res->m_BindingFamily == ShaderResourceBinding::BINDING_FAMILY_UNIFORM_BUFFER)
                {
                    // TODO: Generic type lookup is not supported yet!
                    // We can only support one level of indirection here right now
                    assert(pgm_res.m_Res->m_Type.m_UseTypeIndex);
                    const dmArray<ShaderResourceTypeInfo>& type_infos = *pgm_res.m_TypeInfos;
                    const ShaderResourceTypeInfo& type_info = type_infos[pgm_res.m_Res->m_Type.m_TypeIndex];

                    const uint32_t num_members = type_info.m_Members.Size();
                    for (int i = 0; i < num_members; ++i)
                    {
                        if (search_index == index)
                        {
                            const ShaderResourceMember& member = type_info.m_Members[i];
                            *type = ShaderDataTypeToGraphicsType(member.m_Type.m_ShaderType);
                            *size = dmMath::Max((uint32_t) 1, member.m_ElementCount);
                            return (uint32_t)dmStrlCpy(buffer, member.m_Name, buffer_size);
                        }
                        search_index++;
                    }
                }
            }
        }
        return 0;
    }

    static HUniformLocation GetUniformLocation(const ProgramResourceBinding bindings[MAX_SET_COUNT][MAX_BINDINGS_PER_SET_COUNT], uint8_t max_set, uint8_t max_binding, dmhash_t name_hash)
    {
        for (int set = 0; set < max_set; ++set)
        {
            for (int binding = 0; binding < max_binding; ++binding)
            {
                const ProgramResourceBinding& pgm_res = bindings[set][binding];

                if (pgm_res.m_Res == 0x0)
                    continue;

                if (pgm_res.m_Res->m_NameHash == name_hash)
                {
                    return set | binding << 16;
                }
                else if (pgm_res.m_Res->m_Type.m_UseTypeIndex)
                {
                    // TODO: Generic type lookup is not supported yet!
                    // We can only support one level of indirection here right now
                    const dmArray<ShaderResourceTypeInfo>& type_infos = *pgm_res.m_TypeInfos;
                    const ShaderResourceTypeInfo& type_info = type_infos[pgm_res.m_Res->m_Type.m_TypeIndex];

                    const uint32_t num_members = type_info.m_Members.Size();
                    for (int i = 0; i < num_members; ++i)
                    {
                        const ShaderResourceMember& member = type_info.m_Members[i];

                        if (member.m_NameHash == name_hash)
                        {
                            return set | binding << 16 | ((uint64_t) i) << 32;
                        }
                    }
                }
            }
        }

        return INVALID_UNIFORM_LOCATION;
    }

    static uint32_t DX12GetUniformName(HProgram prog, uint32_t index, char* buffer, uint32_t buffer_size, Type* type, int32_t* size)
    {
        assert(prog);
        DX12ShaderProgram* program = (DX12ShaderProgram*) prog;
        return GetUniformName(program->m_ResourceBindings, program->m_MaxSet, program->m_MaxBinding, index, buffer, buffer_size, type, size);
    }

    static HUniformLocation DX12GetUniformLocation(HProgram prog, const char* name)
    {
        assert(prog);
        DX12ShaderProgram* program = (DX12ShaderProgram*) prog;
        dmhash_t name_hash = dmHashString64(name);

        return GetUniformLocation(program->m_ResourceBindings, program->m_MaxSet, program->m_MaxBinding, name_hash);
    }

    static inline void WriteConstantData(uint32_t offset, uint8_t* uniform_data_ptr, uint8_t* data_ptr, uint32_t data_size)
    {
        memcpy(&uniform_data_ptr[offset], data_ptr, data_size);
    }

    static void DX12SetConstantV4(HContext _context, const dmVMath::Vector4* data, int count, HUniformLocation base_location)
    {
        DX12Context* context = (DX12Context*) _context;
        assert(context->m_CurrentProgram);
        assert(base_location != INVALID_UNIFORM_LOCATION);

        DX12ShaderProgram* program = (DX12ShaderProgram*) context->m_CurrentProgram;
        uint32_t set               = UNIFORM_LOCATION_GET_VS(base_location);
        uint32_t binding           = UNIFORM_LOCATION_GET_VS_MEMBER(base_location);
        uint32_t member            = UNIFORM_LOCATION_GET_FS(base_location);
        assert(!(set == UNIFORM_LOCATION_MAX && binding == UNIFORM_LOCATION_MAX));

        ProgramResourceBinding& pgm_res          = program->m_ResourceBindings[set][binding];
        const dmArray<ShaderResourceTypeInfo>& type_infos = *pgm_res.m_TypeInfos;
        const ShaderResourceTypeInfo&           type_info = type_infos[pgm_res.m_Res->m_Type.m_TypeIndex];

        uint32_t offset = pgm_res.m_DataOffset + type_info.m_Members[member].m_Offset;
        WriteConstantData(offset, program->m_UniformData, (uint8_t*) data, sizeof(dmVMath::Vector4) * count);
    }

    static void DX12SetConstantM4(HContext _context, const dmVMath::Vector4* data, int count, HUniformLocation base_location)
    {
        DX12Context* context = (DX12Context*) _context;
        assert(context->m_CurrentProgram);
        assert(base_location != INVALID_UNIFORM_LOCATION);

        DX12ShaderProgram* program = (DX12ShaderProgram*) context->m_CurrentProgram;
        uint32_t set               = UNIFORM_LOCATION_GET_VS(base_location);
        uint32_t binding           = UNIFORM_LOCATION_GET_VS_MEMBER(base_location);
        uint32_t member            = UNIFORM_LOCATION_GET_FS(base_location);
        assert(!(set == UNIFORM_LOCATION_MAX && binding == UNIFORM_LOCATION_MAX));

        ProgramResourceBinding& pgm_res          = program->m_ResourceBindings[set][binding];
        const dmArray<ShaderResourceTypeInfo>& type_infos = *pgm_res.m_TypeInfos;
        const ShaderResourceTypeInfo&           type_info = type_infos[pgm_res.m_Res->m_Type.m_TypeIndex];

        uint32_t offset = pgm_res.m_DataOffset + type_info.m_Members[member].m_Offset;
        WriteConstantData(offset, program->m_UniformData, (uint8_t*) data, sizeof(dmVMath::Vector4) * 4 * count);
     }

    static void DX12SetSampler(HContext _context, HUniformLocation location, int32_t unit)
    {
        DX12Context* context = (DX12Context*) _context;
        assert(context->m_CurrentProgram);
        assert(location != INVALID_UNIFORM_LOCATION);

        DX12ShaderProgram* program = (DX12ShaderProgram*) context->m_CurrentProgram;
        uint32_t set         = UNIFORM_LOCATION_GET_VS(location);
        uint32_t binding     = UNIFORM_LOCATION_GET_VS_MEMBER(location);
        assert(!(set == UNIFORM_LOCATION_MAX && binding == UNIFORM_LOCATION_MAX));

        // TODO: Compute shaders does not have samplers, but does support texture storage
        //       which is not the same thing.
        assert(program->m_ComputeModule == 0x0);

        assert(program->m_ResourceBindings[set][binding].m_Res);
        program->m_ResourceBindings[set][binding].m_TextureUnit = unit;
    }


    static HRenderTarget DX12NewRenderTarget(HContext _context, uint32_t buffer_type_flags, const RenderTargetCreationParams params)
    {
        return 0;
    }

    static void DX12DeleteRenderTarget(HRenderTarget render_target)
    {
    }

    static void DX12SetRenderTarget(HContext _context, HRenderTarget render_target, uint32_t transient_buffer_types)
    {
    }

    static HTexture DX12GetRenderTargetTexture(HRenderTarget render_target, BufferType buffer_type)
    {
        return 0;
    }

    static void DX12GetRenderTargetSize(HRenderTarget render_target, BufferType buffer_type, uint32_t& width, uint32_t& height)
    {
    }

    static void DX12SetRenderTargetSize(HRenderTarget render_target, uint32_t width, uint32_t height)
    {
    }

    static bool DX12IsTextureFormatSupported(HContext _context, TextureFormat format)
    {
        DX12Context* context = (DX12Context*) _context;
        return (context->m_TextureFormatSupport & (1 << format)) != 0;
    }

    static uint32_t DX12GetMaxTextureSize(HContext context)
    {
        return 1024;
    }

    static HTexture DX12NewTexture(HContext _context, const TextureCreationParams& params)
    {
        DX12Context* context = (DX12Context*) _context;
        DX12Texture* tex = new DX12Texture;
        memset(tex, 0, sizeof(DX12Texture));

        tex->m_Type             = params.m_Type;
        tex->m_Width            = params.m_Width;
        tex->m_Height           = params.m_Height;
        tex->m_Depth            = params.m_Depth;
        tex->m_MipMapCount      = params.m_MipMapCount;

        // tex->m_UsageFlags  = GetVulkanUsageFromHints(params.m_UsageHintBits);

        if (params.m_OriginalWidth == 0)
        {
            tex->m_OriginalWidth  = params.m_Width;
            tex->m_OriginalHeight = params.m_Height;
        }
        else
        {
            tex->m_OriginalWidth  = params.m_OriginalWidth;
            tex->m_OriginalHeight = params.m_OriginalHeight;
        }
        return StoreAssetInContainer(context->m_AssetHandleContainer, tex, ASSET_TYPE_TEXTURE);
    }

    static void DX12DeleteTexture(HTexture texture)
    {
    }

    static HandleResult DX12GetTextureHandle(HTexture texture, void** out_handle)
    {

        return HANDLE_RESULT_OK;
    }

    static float GetMaxAnisotrophyClamped(float max_anisotropy_requested)
    {
        return dmMath::Min(max_anisotropy_requested, 32.0f); // TODO: What's the max limit here?
    }

    static int16_t GetTextureSamplerIndex(DX12Context* context, TextureFilter minfilter, TextureFilter magfilter, TextureWrap uwrap, TextureWrap vwrap, uint8_t maxLod, float max_anisotropy)
    {
        if (minfilter == TEXTURE_FILTER_DEFAULT)
            minfilter = context->m_DefaultTextureMinFilter;
        if (magfilter == TEXTURE_FILTER_DEFAULT)
            magfilter = context->m_DefaultTextureMagFilter;

        for (uint32_t i=0; i < context->m_TextureSamplers.Size(); i++)
        {
            const DX12TextureSampler& sampler = context->m_TextureSamplers[i];
            if (sampler.m_MagFilter     == magfilter &&
                sampler.m_MinFilter     == minfilter &&
                sampler.m_AddressModeU  == uwrap     &&
                sampler.m_AddressModeV  == vwrap     &&
                sampler.m_MaxLod        == maxLod    &&
                sampler.m_MaxAnisotropy == max_anisotropy)
            {
                return (uint8_t) i;
            }
        }

        return -1;
    }

    static int16_t CreateTextureSampler(DX12Context* context, TextureFilter minfilter, TextureFilter magfilter, TextureWrap uwrap, TextureWrap vwrap, uint8_t maxLod, float max_anisotropy)
    {
        DX12TextureSampler new_sampler  = {};
        new_sampler.m_MinFilter     = minfilter;
        new_sampler.m_MagFilter     = magfilter;
        new_sampler.m_AddressModeU  = uwrap;
        new_sampler.m_AddressModeV  = vwrap;
        new_sampler.m_MaxLod        = maxLod;
        new_sampler.m_MaxAnisotropy = max_anisotropy;

        uint32_t sampler_index = context->m_TextureSamplers.Size();
        if (context->m_TextureSamplers.Full())
        {
            context->m_TextureSamplers.OffsetCapacity(1);
        }

        D3D12_SAMPLER_DESC desc  = {};
        desc.Filter              = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU            = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        desc.AddressV            = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        desc.AddressW            = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        desc.MinLOD              = 0;
        desc.MaxLOD              = D3D12_FLOAT32_MAX;
        desc.MipLODBias          = 0.0f;
        desc.MaxAnisotropy       = 1;
        desc.ComparisonFunc      = D3D12_COMPARISON_FUNC_ALWAYS;

        CD3DX12_CPU_DESCRIPTOR_HANDLE  desc_handle(context->m_SamplerPool.m_DescriptorHeap->GetCPUDescriptorHandleForHeapStart(), sampler_index, context->m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER));
        context->m_Device->CreateSampler(&desc, desc_handle);
        context->m_SamplerPool.m_DescriptorCursor++;

        new_sampler.m_DescriptorOffset = sampler_index * context->m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

        /*
        // D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        // desc.Format              = texture->m_ResourceDesc.Format;
        // desc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
        // desc.Texture2D.MipLevels = texture->m_MipMapCount;
        // mDevice->CreateShaderResourceView(texture->m_Resource, &desc, m_SamplerPool.m_DescriptorHeap->GetCPUDescriptorHandleForHeapStart());
        // cpuDesc.ptr += i * renderer->device->lpVtbl->GetDescriptorHandleIncrementSize ( renderer->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV );

        // g_DX12Context->m_SamplerPool
        */

        context->m_TextureSamplers.Push(new_sampler);
        return (int16_t) sampler_index;
    }

    static void DX12SetTextureParamsInternal(DX12Context* context, DX12Texture* texture, TextureFilter minfilter, TextureFilter magfilter, TextureWrap uwrap, TextureWrap vwrap, float max_anisotropy)
    {
        const DX12TextureSampler& sampler = context->m_TextureSamplers[texture->m_TextureSamplerIndex];
        float anisotropy_clamped = GetMaxAnisotrophyClamped(max_anisotropy);

        if (sampler.m_MinFilter     != minfilter              ||
            sampler.m_MagFilter     != magfilter              ||
            sampler.m_AddressModeU  != uwrap                  ||
            sampler.m_AddressModeV  != vwrap                  ||
            sampler.m_MaxLod        != texture->m_MipMapCount ||
            sampler.m_MaxAnisotropy != anisotropy_clamped)
        {
            int16_t sampler_index = GetTextureSamplerIndex(context, minfilter, magfilter, uwrap, vwrap, texture->m_MipMapCount, anisotropy_clamped);
            if (sampler_index < 0)
            {
                sampler_index = CreateTextureSampler(context, minfilter, magfilter, uwrap, vwrap, texture->m_MipMapCount, anisotropy_clamped);
            }
            texture->m_TextureSamplerIndex = sampler_index;
        }
    }

    static void DX12SetTextureParams(HTexture texture, TextureFilter minfilter, TextureFilter magfilter, TextureWrap uwrap, TextureWrap vwrap, float max_anisotropy)
    {
        DX12Texture* tex = GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture);
        DX12SetTextureParamsInternal(g_DX12Context, tex, minfilter, magfilter, uwrap, vwrap, max_anisotropy);
    }

    /*
    case TEXTURE_FORMAT_LUMINANCE:          return VK_FORMAT_R8_UNORM;
    case TEXTURE_FORMAT_LUMINANCE_ALPHA:    return VK_FORMAT_R8G8_UNORM;
    case TEXTURE_FORMAT_RGB:                return VK_FORMAT_R8G8B8_UNORM;
    case TEXTURE_FORMAT_RGBA:               return VK_FORMAT_R8G8B8A8_UNORM;
    case TEXTURE_FORMAT_RGB_16BPP:          return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case TEXTURE_FORMAT_RGBA_16BPP:         return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case TEXTURE_FORMAT_DEPTH:              return VK_FORMAT_UNDEFINED;
    case TEXTURE_FORMAT_STENCIL:            return VK_FORMAT_UNDEFINED;
    case TEXTURE_FORMAT_RGB_PVRTC_2BPPV1:   return VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG;
    case TEXTURE_FORMAT_RGB_PVRTC_4BPPV1:   return VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG;
    case dTEXTURE_FORMAT_RGBA_PVRTC_2BPPV1:  return VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG;
    case TEXTURE_FORMAT_RGBA_PVRTC_4BPPV1:  return VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG;
    case TEXTURE_FORMAT_RGB_ETC1:           return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    case TEXTURE_FORMAT_RGBA_ETC2:          return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
    case TEXTURE_FORMAT_RGBA_ASTC_4x4:      return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case TEXTURE_FORMAT_RGB_BC1:            return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case TEXTURE_FORMAT_RGBA_BC3:           return VK_FORMAT_BC3_UNORM_BLOCK;
    case TEXTURE_FORMAT_RGBA_BC7:           return VK_FORMAT_BC7_UNORM_BLOCK;
    case TEXTURE_FORMAT_R_BC4:              return VK_FORMAT_BC4_UNORM_BLOCK;
    case TEXTURE_FORMAT_RG_BC5:             return VK_FORMAT_BC5_UNORM_BLOCK;
    case TEXTURE_FORMAT_RGB16F:             return VK_FORMAT_R16G16B16_SFLOAT;
    case TEXTURE_FORMAT_RGB32F:             return VK_FORMAT_R32G32B32_SFLOAT;
    case TEXTURE_FORMAT_RGBA16F:            return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TEXTURE_FORMAT_RGBA32F:            return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TEXTURE_FORMAT_R16F:               return VK_FORMAT_R16_SFLOAT;
    case TEXTURE_FORMAT_RG16F:              return VK_FORMAT_R16G16_SFLOAT;
    case TEXTURE_FORMAT_R32F:               return VK_FORMAT_R32_SFLOAT;
    case TEXTURE_FORMAT_RG32F:              return VK_FORMAT_R32G32_SFLOAT;
    case TEXTURE_FORMAT_RGBA32UI:           return VK_FORMAT_R32G32B32A32_UINT;
    case TEXTURE_FORMAT_BGRA8U:             return VK_FORMAT_B8G8R8A8_UNORM;
    case TEXTURE_FORMAT_R32UI:              return VK_FORMAT_R32_UINT;
    default:                                return VK_FORMAT_UNDEFINED;
        */

    static DXGI_FORMAT GetDXGIFormatFromTextureFormat(TextureFormat format)
    {
        switch(format)
        {
            case TEXTURE_FORMAT_LUMINANCE:         return DXGI_FORMAT_R8_UNORM;
            case TEXTURE_FORMAT_LUMINANCE_ALPHA:   return DXGI_FORMAT_R8G8_UNORM;
            case TEXTURE_FORMAT_RGB:               return DXGI_FORMAT_UNKNOWN; // Unsupported?
            case TEXTURE_FORMAT_RGBA:              return DXGI_FORMAT_R8G8B8A8_UNORM;
            case TEXTURE_FORMAT_RGB_16BPP:         return DXGI_FORMAT_UNKNOWN; // Unsupported
            case TEXTURE_FORMAT_RGBA_16BPP:        return DXGI_FORMAT_R16G16B16A16_UNORM;
            /*
            TEXTURE_FORMAT_DEPTH:             return ;
            TEXTURE_FORMAT_STENCIL:           return ;
            TEXTURE_FORMAT_RGB_PVRTC_2BPPV1:  return ;
            TEXTURE_FORMAT_RGB_PVRTC_4BPPV1:  return ;
            TEXTURE_FORMAT_RGBA_PVRTC_2BPPV1: return ;
            TEXTURE_FORMAT_RGBA_PVRTC_4BPPV1: return ;
            TEXTURE_FORMAT_RGB_ETC1:          return ;
            TEXTURE_FORMAT_R_ETC2:            return ;
            TEXTURE_FORMAT_RG_ETC2:           return ;
            TEXTURE_FORMAT_RGBA_ETC2:         return ;
            TEXTURE_FORMAT_RGBA_ASTC_4x4:     return ;
            TEXTURE_FORMAT_RGB_BC1: return ;
            TEXTURE_FORMAT_RGBA_BC3: return ;
            TEXTURE_FORMAT_R_BC4: return ;
            TEXTURE_FORMAT_RG_BC5: return ;
            TEXTURE_FORMAT_RGBA_BC7: return ;
            // Floating point texture formats
            TEXTURE_FORMAT_RGB16F: return ;
            TEXTURE_FORMAT_RGB32F: return ;
            TEXTURE_FORMAT_RGBA16F: return ;
            TEXTURE_FORMAT_RGBA32F: return ;
            TEXTURE_FORMAT_R16F: return ;
            TEXTURE_FORMAT_RG16F: return ;
            TEXTURE_FORMAT_R32F: return ;
            TEXTURE_FORMAT_RG32F: return ;
            // Internal formats (not exposed via script APIs)
            TEXTURE_FORMAT_RGBA32UI: return ;
            TEXTURE_FORMAT_BGRA8U: return ;
            TEXTURE_FORMAT_R32UI: return ;
            */
        }

        assert(0);

        return (DXGI_FORMAT) -1;
    }

    static void DX12SetTexture(HTexture texture, const TextureParams& params)
    {
        // Same as graphics_opengl.cpp
        switch (params.m_Format)
        {
            case TEXTURE_FORMAT_DEPTH:
            case TEXTURE_FORMAT_STENCIL:
                dmLogError("Unable to upload texture data, unsupported type (%s).", TextureFormatToString(params.m_Format));
                return;
            default:break;
        }

        DX12Texture* tex            = GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture);
        TextureFormat format_orig   = params.m_Format;
        TextureFormat format_actual = params.m_Format;
        void* tex_data_ptr          = (void*) params.m_Data;
        uint32_t tex_layer_count    = tex->m_Depth;
        uint32_t tex_data_size      = params.m_DataSize;
        DXGI_FORMAT dxgi_format     = GetDXGIFormatFromTextureFormat(format_orig);

        if (tex->m_MipMapCount == 1 && params.m_MipMap > 0)
        {
            return;
        }

        tex->m_MipMapCount = dmMath::Max(tex->m_MipMapCount, (uint16_t)(params.m_MipMap+1));

        // Note: There's no 8 bit RGB format, we have to expand this to four channels
        // TODO: Can we use R11G11B10 somehow?
        if (format_orig == TEXTURE_FORMAT_RGB)
        {
            format_actual = TEXTURE_FORMAT_RGBA;
            dxgi_format   = GetDXGIFormatFromTextureFormat(format_actual);

            uint32_t data_pixel_count = params.m_Width * params.m_Height * tex_layer_count;
            uint8_t bpp_new           = 32;
            uint8_t* data_new         = new uint8_t[data_pixel_count * bpp_new];

            RepackRGBToRGBA(data_pixel_count, (uint8_t*) tex_data_ptr, data_new);
            tex_data_ptr  = data_new;
        }

        if (!tex->m_Resource)
        {
            D3D12_RESOURCE_DESC desc = {};
            desc.Format              = dxgi_format;
            desc.Width               = params.m_Width;
            desc.Height              = params.m_Height;
            desc.Flags               = D3D12_RESOURCE_FLAG_NONE;
            desc.DepthOrArraySize    = dmMath::Max(1U, (uint32_t) params.m_Depth);
            desc.MipLevels           = tex->m_MipMapCount;
            desc.SampleDesc.Count    = 1;
            desc.SampleDesc.Quality  = 0;
            desc.Dimension           = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Layout              = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Alignment           = 0;

            CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);

            HRESULT hr = g_DX12Context->m_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&tex->m_Resource));
            CHECK_HR_ERROR(hr);

            for (int i = 0; i < tex->m_MipMapCount; ++i)
            {
                tex->m_ResourceStates[i] = D3D12_RESOURCE_STATE_COPY_DEST;
            }

            tex->m_ResourceDesc = desc;
        }

        TextureBufferUploadHelper(g_DX12Context, tex, format_actual, format_orig, params, (uint8_t*) tex_data_ptr);

        DX12SetTextureParamsInternal(g_DX12Context, tex, params.m_MinFilter, params.m_MagFilter, params.m_UWrap, params.m_VWrap, 1.0f);

        if (format_orig == TEXTURE_FORMAT_RGB)
        {
            delete[] (uint8_t*)tex_data_ptr;
        }
    }

    static uint32_t DX12GetTextureResourceSize(HTexture texture)
    {
        return 0;
    }

    static uint16_t DX12GetTextureWidth(HTexture texture)
    {
        return GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture)->m_Width;
    }

    static uint16_t DX12GetTextureHeight(HTexture texture)
    {
        return GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture)->m_Height;
    }

    static uint16_t DX12GetOriginalTextureWidth(HTexture texture)
    {
        return GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture)->m_OriginalWidth;
    }

    static uint16_t DX12GetOriginalTextureHeight(HTexture texture)
    {
        return GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture)->m_OriginalHeight;
    }

    static void DX12EnableTexture(HContext _context, uint32_t unit, uint8_t value_index, HTexture texture)
    {
        assert(unit < DM_MAX_TEXTURE_UNITS);
        g_DX12Context->m_CurrentTextures[unit] = texture;
    }

    static void DX12DisableTexture(HContext context, uint32_t unit, HTexture texture)
    {
        assert(unit < DM_MAX_TEXTURE_UNITS);
        g_DX12Context->m_CurrentTextures[unit] = 0x0;
    }

    static void DX12ReadPixels(HContext context, void* buffer, uint32_t buffer_size)
    {
    }

    static void DX12SetViewport(HContext _context, int32_t x, int32_t y, int32_t width, int32_t height)
    {
        DX12Context* context = (DX12Context*) _context;

        DX12Viewport& viewport = context->m_CurrentViewport;
        viewport.m_X = (uint16_t) x;
        viewport.m_Y = (uint16_t) y;
        viewport.m_W = (uint16_t) width;
        viewport.m_H = (uint16_t) height;
        context->m_ViewportChanged = 1;
    }

    static void DX12EnableState(HContext context, State state)
    {
        SetPipelineStateValue(g_DX12Context->m_PipelineState, state, 1);
    }

    static void DX12DisableState(HContext context, State state)
    {
        SetPipelineStateValue(g_DX12Context->m_PipelineState, state, 0);
    }

    static void DX12SetBlendFunc(HContext _context, BlendFactor source_factor, BlendFactor destinaton_factor)
    {
        g_DX12Context->m_PipelineState.m_BlendSrcFactor = source_factor;
        g_DX12Context->m_PipelineState.m_BlendDstFactor = destinaton_factor;
    }

    static void DX12SetColorMask(HContext context, bool red, bool green, bool blue, bool alpha)
    {
        assert(context);
        uint8_t write_mask = red   ? DM_GRAPHICS_STATE_WRITE_R : 0;
        write_mask        |= green ? DM_GRAPHICS_STATE_WRITE_G : 0;
        write_mask        |= blue  ? DM_GRAPHICS_STATE_WRITE_B : 0;
        write_mask        |= alpha ? DM_GRAPHICS_STATE_WRITE_A : 0;

        g_DX12Context->m_PipelineState.m_WriteColorMask = write_mask;
    }

    static void DX12SetDepthMask(HContext context, bool mask)
    {
        g_DX12Context->m_PipelineState.m_WriteDepth = mask;
    }

    static void DX12SetDepthFunc(HContext context, CompareFunc func)
    {
        g_DX12Context->m_PipelineState.m_DepthTestFunc = func;
    }

    static void DX12SetScissor(HContext _context, int32_t x, int32_t y, int32_t width, int32_t height)
    {
    }

    static void DX12SetStencilMask(HContext context, uint32_t mask)
    {
        g_DX12Context->m_PipelineState.m_StencilWriteMask = mask;
    }

    static void DX12SetStencilFunc(HContext _context, CompareFunc func, uint32_t ref, uint32_t mask)
    {
        g_DX12Context->m_PipelineState.m_StencilFrontTestFunc = (uint8_t) func;
        g_DX12Context->m_PipelineState.m_StencilBackTestFunc  = (uint8_t) func;
        g_DX12Context->m_PipelineState.m_StencilReference     = (uint8_t) ref;
        g_DX12Context->m_PipelineState.m_StencilCompareMask   = (uint8_t) mask;
    }

    static void DX12SetStencilOp(HContext _context, StencilOp sfail, StencilOp dpfail, StencilOp dppass)
    {
        g_DX12Context->m_PipelineState.m_StencilFrontOpFail      = sfail;
        g_DX12Context->m_PipelineState.m_StencilFrontOpDepthFail = dpfail;
        g_DX12Context->m_PipelineState.m_StencilFrontOpPass      = dppass;
        g_DX12Context->m_PipelineState.m_StencilBackOpFail       = sfail;
        g_DX12Context->m_PipelineState.m_StencilBackOpDepthFail  = dpfail;
        g_DX12Context->m_PipelineState.m_StencilBackOpPass       = dppass;
    }

    static void DX12SetStencilFuncSeparate(HContext _context, FaceType face_type, CompareFunc func, uint32_t ref, uint32_t mask)
    {
        if (face_type == FACE_TYPE_BACK)
        {
            g_DX12Context->m_PipelineState.m_StencilBackTestFunc  = (uint8_t) func;
        }
        else
        {
            g_DX12Context->m_PipelineState.m_StencilFrontTestFunc = (uint8_t) func;
        }
        g_DX12Context->m_PipelineState.m_StencilReference     = (uint8_t) ref;
        g_DX12Context->m_PipelineState.m_StencilCompareMask   = (uint8_t) mask;
    }

    static void DX12SetStencilOpSeparate(HContext _context, FaceType face_type, StencilOp sfail, StencilOp dpfail, StencilOp dppass)
    {
        if (face_type == FACE_TYPE_BACK)
        {
            g_DX12Context->m_PipelineState.m_StencilBackOpFail       = sfail;
            g_DX12Context->m_PipelineState.m_StencilBackOpDepthFail  = dpfail;
            g_DX12Context->m_PipelineState.m_StencilBackOpPass       = dppass;
        }
        else
        {
            g_DX12Context->m_PipelineState.m_StencilFrontOpFail      = sfail;
            g_DX12Context->m_PipelineState.m_StencilFrontOpDepthFail = dpfail;
            g_DX12Context->m_PipelineState.m_StencilFrontOpPass      = dppass;
        }
    }

    static void DX12SetFaceWinding(HContext context, FaceWinding face_winding)
    {
        // TODO: Add this to the DX12 pipeline handle aswell, for now it's a NOP
    }

    static void DX12SetCullFace(HContext context, FaceType face_type)
    {
        g_DX12Context->m_PipelineState.m_CullFaceType = face_type;
        g_DX12Context->m_CullFaceChanged              = true;
    }

    static void DX12SetPolygonOffset(HContext context, float factor, float units)
    {
        // TODO: Add this to the DX12 pipeline handle aswell, for now it's a NOP
    }

    static PipelineState DX12GetPipelineState(HContext context)
    {
        return ((DX12Context*) context)->m_PipelineState;
    }

    static void DX12SetTextureAsync(HTexture texture, const TextureParams& params, SetTextureAsyncCallback callback, void* user_data)
    {
        SetTexture(texture, params);
        if (callback)
        {
            callback(texture, user_data);
        }
    }

    static uint32_t DX12GetTextureStatusFlags(HTexture texture)
    {
        return TEXTURE_STATUS_OK;
    }

    static bool DX12IsExtensionSupported(HContext context, const char* extension)
    {
        return true;
    }

    static TextureType DX12GetTextureType(HTexture texture)
    {
        return GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture)->m_Type;
    }

    static uint32_t DX12GetNumSupportedExtensions(HContext context)
    {
        return 0;
    }

    static const char* DX12GetSupportedExtension(HContext context, uint32_t index)
    {
        return "";
    }

    static uint8_t DX12GetNumTextureHandles(HTexture texture)
    {
        return 1;
    }

    static bool DX12IsContextFeatureSupported(HContext context, ContextFeature feature)
    {
        return true;
    }

    static uint16_t DX12GetTextureDepth(HTexture texture)
    {
        return GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture)->m_Depth;
    }

    static uint8_t DX12GetTextureMipmapCount(HTexture texture)
    {
        return GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture)->m_MipMapCount;
    }

    static bool DX12IsAssetHandleValid(HContext _context, HAssetHandle asset_handle)
    {
        assert(_context);
        if (asset_handle == 0)
        {
            return false;
        }
        DX12Context* context = (DX12Context*) _context;
        AssetType type       = GetAssetType(asset_handle);
        if (type == ASSET_TYPE_TEXTURE)
        {
            return GetAssetFromContainer<DX12Texture>(context->m_AssetHandleContainer, asset_handle) != 0;
        }
        else if (type == ASSET_TYPE_RENDER_TARGET)
        {
            return GetAssetFromContainer<DX12RenderTarget>(context->m_AssetHandleContainer, asset_handle) != 0;
        }
        return false;
    }

    static GraphicsAdapterFunctionTable DX12RegisterFunctionTable()
    {
        GraphicsAdapterFunctionTable fn_table = {};
        DM_REGISTER_GRAPHICS_FUNCTION_TABLE(fn_table, DX12);
        return fn_table;
    }
}

