#include "common/types.h"
#include "present_bridge.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <array>
#include <cstring>

extern "C" IUnknown* gow3_uwp_core_window();
using Microsoft::WRL::ComPtr;

namespace Gow3Uwp {
static void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::fprintf(stderr,"%s: HRESULT 0x%08lx\n",operation,static_cast<unsigned long>(result));
        throw std::runtime_error(operation);
    }
}
static void Check(vk::Result result, const char* operation) {
    if (result != vk::Result::eSuccess) {
        std::fprintf(stderr,"%s: Vulkan %d\n",operation,static_cast<int>(result));
        throw std::runtime_error(operation);
    }
}
void InitializeDriver() {
    static HMODULE driver = LoadPackagedLibrary(L"vulkan_dzn.dll",0);
    if (!driver) throw std::runtime_error("Cannot load packaged Dozen driver");
    auto proc=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(driver,"vk_icdGetInstanceProcAddr"));
    if (!proc) throw std::runtime_error("Dozen ICD entry point missing");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(proc);
}
struct Presenter::Impl {
    vk::Device device;
    vk::Queue queue;
    u32 family;
    vk::UniqueCommandPool pool;
    vk::CommandBuffer cmd;
    vk::UniqueFence vk_fence;
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView view;
    vk::UniqueSemaphore acquired,ready;
    ComPtr<ID3D12Device> native;
    ComPtr<ID3D12CommandQueue> native_queue;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12Resource> source;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event=nullptr;
    UINT64 tick=0;
    bool first=true;
    void WaitNative() {
        if (fence->GetCompletedValue() >= tick) return;
        Check(fence->SetEventOnCompletion(tick,event),"Native fence event");
        if (WaitForSingleObject(event,10000)!=WAIT_OBJECT_0) throw std::runtime_error("Native presentation timed out");
    }
    void Submit(std::span<const vk::Semaphore> waits,std::span<const vk::Semaphore> signals) {
        std::array<vk::PipelineStageFlags,1> stages{vk::PipelineStageFlagBits::eAllCommands};
        Check(device.resetFences(*vk_fence),"Reset bridge fence");
        vk::SubmitInfo submit{
            .waitSemaphoreCount=static_cast<u32>(waits.size()),.pWaitSemaphores=waits.data(),
            .pWaitDstStageMask=stages.data(),.commandBufferCount=1,.pCommandBuffers=&cmd,
            .signalSemaphoreCount=static_cast<u32>(signals.size()),.pSignalSemaphores=signals.data(),
        };
        Check(queue.submit(submit,*vk_fence),"Submit presentation bridge");
        Check(device.waitForFences(*vk_fence,true,10'000'000'000ULL),"Wait presentation bridge");
    }
    ~Impl() {
        if(native_queue && fence) {
            native_queue->Signal(fence.Get(),++tick);
            if(event) { try { WaitNative(); } catch(...) {} }
        }
        if(device) {
            device.waitIdle();
            device.destroyImageView(view);
            device.destroyImage(image);
            device.freeMemory(memory);
        }
        if(event) CloseHandle(event);
    }
};
Presenter::Presenter(vk::PhysicalDevice physical,vk::Device device,vk::Queue queue,u32 family,u32 width,u32 height)
    : impl(std::make_unique<Impl>()) {
    auto& p=*impl; p.device=device; p.queue=queue; p.family=family;
    auto ids=physical.getProperties2<vk::PhysicalDeviceProperties2,vk::PhysicalDeviceIDProperties>();
    auto const& id=ids.get<vk::PhysicalDeviceIDProperties>();
    if(!id.deviceLUIDValid) throw std::runtime_error("Dozen adapter has no valid LUID");
    LUID luid{}; std::memcpy(&luid,id.deviceLUID.data(),sizeof(luid));
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"Create DXGI factory");
    ComPtr<IDXGIAdapter1> adapter;
    Check(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter)),"Find Dozen adapter");
    Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&p.native)),"Create presentation device");
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    Check(p.native->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&p.native_queue)),"Create presentation queue");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width=width; desc.Height=height; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD; desc.Scaling=DXGI_SCALING_STRETCH;
    ComPtr<IUnknown> window; window.Attach(gow3_uwp_core_window());
    ComPtr<IDXGISwapChain1> chain;
    Check(factory->CreateSwapChainForCoreWindow(p.native_queue.Get(),window.Get(),&desc,nullptr,&chain),"Create CoreWindow swapchain");
    Check(chain.As(&p.swapchain),"Query CoreWindow swapchain");
    Check(p.native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&p.allocator)),"Create copy allocator");
    Check(p.native->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,p.allocator.Get(),nullptr,IID_PPV_ARGS(&p.list)),"Create copy list");
    Check(p.list->Close(),"Close copy list");
    Check(p.native->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&p.fence)),"Create copy fence");
    p.event=CreateEventExW(nullptr,nullptr,0,EVENT_ALL_ACCESS);
    if(!p.event) throw std::runtime_error("Create presentation event");
    const vk::ExternalMemoryImageCreateInfo external{.handleTypes=vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource};
    vk::ImageCreateInfo image_info{.pNext=&external,.imageType=vk::ImageType::e2D,
        .format=vk::Format::eR8G8B8A8Unorm,.extent={width,height,1},.mipLevels=1,.arrayLayers=1,
        .samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
        .usage=vk::ImageUsageFlagBits::eColorAttachment|vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode=vk::SharingMode::eExclusive,.initialLayout=vk::ImageLayout::eUndefined};
    auto image_result=device.createImage(image_info); Check(image_result.result,"Create presentation image"); p.image=image_result.value;
    auto req=device.getImageMemoryRequirements(p.image);
    auto props=physical.getMemoryProperties(); u32 type=props.memoryTypeCount;
    for(u32 i=0;i<props.memoryTypeCount;++i) if((req.memoryTypeBits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&vk::MemoryPropertyFlagBits::eDeviceLocal)) { type=i; break; }
    if(type==props.memoryTypeCount) throw std::runtime_error("No presentation memory type");
    vk::MemoryDedicatedAllocateInfo dedicated{.image=p.image};
    vk::ExportMemoryAllocateInfo export_info{.pNext=&dedicated,.handleTypes=vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource};
    auto allocation=device.allocateMemory({.pNext=&export_info,.allocationSize=req.size,.memoryTypeIndex=type});
    Check(allocation.result,"Allocate shared presentation image"); p.memory=allocation.value;
    Check(device.bindImageMemory(p.image,p.memory,0),"Bind presentation image");
    auto exported=device.getMemoryWin32HandleKHR({.memory=p.memory,.handleType=vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource});
    Check(exported.result,"Export presentation image");
    HRESULT opened=p.native->OpenSharedHandle(exported.value,IID_PPV_ARGS(&p.source));
    CloseHandle(exported.value); Check(opened,"Open presentation image in D3D12");
    auto view_result=device.createImageView({.image=p.image,.viewType=vk::ImageViewType::e2D,
        .format=vk::Format::eR8G8B8A8Unorm,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}});
    Check(view_result.result,"Create presentation view"); p.view=view_result.value;
    auto pool_result=device.createCommandPoolUnique({.flags=vk::CommandPoolCreateFlagBits::eResetCommandBuffer,.queueFamilyIndex=family});
    Check(pool_result.result,"Create bridge command pool"); p.pool=std::move(pool_result.value);
    auto commands=device.allocateCommandBuffers({.commandPool=*p.pool,.level=vk::CommandBufferLevel::ePrimary,.commandBufferCount=1});
    Check(commands.result,"Allocate bridge command"); p.cmd=commands.value[0];
    auto fence_result=device.createFenceUnique({}); Check(fence_result.result,"Create bridge fence"); p.vk_fence=std::move(fence_result.value);
    auto acquired=device.createSemaphoreUnique({}); Check(acquired.result,"Create acquire semaphore"); p.acquired=std::move(acquired.value);
    auto ready=device.createSemaphoreUnique({}); Check(ready.result,"Create ready semaphore"); p.ready=std::move(ready.value);
    std::printf("CoreWindow presentation bridge: %ux%u, shared image %llu KiB\n",width,height,static_cast<unsigned long long>(req.size>>10));
}
Presenter::~Presenter()=default;
vk::Image Presenter::Image() const {return impl->image;}
vk::ImageView Presenter::View() const {return impl->view;}
vk::Semaphore Presenter::Acquired() const {return *impl->acquired;}
vk::Semaphore Presenter::Ready() const {return *impl->ready;}
bool Presenter::Acquire() {
    auto& p=*impl; p.WaitNative();
    Check(p.cmd.reset({}),"Reset acquire command");
    Check(p.cmd.begin({.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit}),"Begin acquire command");
    if(!p.first) p.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eAllCommands,{}, {}, {},
        vk::ImageMemoryBarrier{.dstAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite,
        .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,.dstQueueFamilyIndex=p.family,
        .image=p.image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}});
    Check(p.cmd.end(),"End acquire command");
    std::array signals{*p.acquired}; p.Submit({},signals); p.first=false; return true;
}
bool Presenter::Present(bool vsync) {
    auto& p=*impl;
    Check(p.cmd.reset({}),"Reset release command");
    Check(p.cmd.begin({.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit}),"Begin release command");
    p.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eAllCommands,{}, {}, {},
        vk::ImageMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eMemoryWrite,
        .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex=p.family,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,
        .image=p.image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}});
    Check(p.cmd.end(),"End release command");
    std::array waits{*p.ready}; p.Submit(waits,{});
    Check(p.allocator->Reset(),"Reset native copy allocator");
    Check(p.list->Reset(p.allocator.Get(),nullptr),"Reset native copy command");
    ComPtr<ID3D12Resource> dest;
    Check(p.swapchain->GetBuffer(p.swapchain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&dest)),"Get CoreWindow backbuffer");
    std::array<D3D12_RESOURCE_BARRIER,2> barriers{};
    for(auto& barrier:barriers) {barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;}
    barriers[0].Transition={p.source.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition={dest.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST};
    p.list->ResourceBarrier(2,barriers.data()); p.list->CopyResource(dest.Get(),p.source.Get());
    for(auto& barrier:barriers) std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
    p.list->ResourceBarrier(2,barriers.data()); Check(p.list->Close(),"Close native copy");
    ID3D12CommandList* lists[]={p.list.Get()}; p.native_queue->ExecuteCommandLists(1,lists);
    Check(p.swapchain->Present(vsync ? 1 : 0,0),"Present CoreWindow frame");
    Check(p.native_queue->Signal(p.fence.Get(),++p.tick),"Signal native frame"); p.WaitNative();
    if(p.tick==1) std::puts("CoreWindow first frame presented");
    return true;
}
}
