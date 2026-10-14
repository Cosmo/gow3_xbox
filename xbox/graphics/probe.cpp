#include "common/types.h"
#include "present_bridge.h"
#include <cstdio>
#include <array>
#include <stdexcept>
#ifndef GOW3_LINK_RENDERER
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#endif

namespace Gow3Uwp {
struct ProbeDisplay {
    vk::UniqueInstance instance;
    vk::UniqueDevice device;
    std::unique_ptr<Presenter> presenter;
};
static std::unique_ptr<ProbeDisplay> display;
static void Require(vk::Result r,const char* operation) {
    if(r!=vk::Result::eSuccess) throw std::runtime_error(operation);
}
int GraphicsProbe() {
    InitializeDriver();
    vk::ApplicationInfo app{.pApplicationName="GoW3 UWP",.apiVersion=VK_API_VERSION_1_2};
    auto instance_result=vk::createInstanceUnique({.pApplicationInfo=&app});
    Require(instance_result.result,"Create probe Vulkan instance"); auto instance=std::move(instance_result.value);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*instance);
    auto devices=instance->enumeratePhysicalDevices(); Require(devices.result,"Enumerate Dozen devices");
    if(devices.value.empty()) throw std::runtime_error("No Dozen device");
    auto physical=devices.value.front(); auto properties=physical.getProperties();
    std::printf("Dozen device: %s, Vulkan %u.%u\n",properties.deviceName.data(),VK_VERSION_MAJOR(properties.apiVersion),VK_VERSION_MINOR(properties.apiVersion));
    auto features=physical.getFeatures2<vk::PhysicalDeviceFeatures2,vk::PhysicalDeviceVulkan12Features,
        vk::PhysicalDeviceDynamicRenderingFeaturesKHR,vk::PhysicalDeviceSynchronization2FeaturesKHR>();
    auto const& f12=features.get<vk::PhysicalDeviceVulkan12Features>();
    std::printf("Driver features: bufferDeviceAddress=%u sparseBinding=%u robustBufferAccess=%u dynamicRendering=%u synchronization2=%u\n",
        f12.bufferDeviceAddress,features.get().features.sparseBinding,features.get().features.robustBufferAccess,
        features.get<vk::PhysicalDeviceDynamicRenderingFeaturesKHR>().dynamicRendering,
        features.get<vk::PhysicalDeviceSynchronization2FeaturesKHR>().synchronization2);
    auto extensions=physical.enumerateDeviceExtensionProperties(); Require(extensions.result,"List driver extensions");
    for(auto const& ext:extensions.value) std::printf("Vulkan extension: %s\n",ext.extensionName.data());
    u32 family=0; auto queues=physical.getQueueFamilyProperties();
    while(family<queues.size() && !(queues[family].queueFlags&vk::QueueFlagBits::eGraphics)) ++family;
    if(family==queues.size()) throw std::runtime_error("No Dozen graphics queue");
    float priority=1; vk::DeviceQueueCreateInfo queue_info{.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    std::array extensions_enabled{"VK_KHR_external_memory_win32"};
    auto device_result=physical.createDeviceUnique({.queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info,
        .enabledExtensionCount=static_cast<u32>(extensions_enabled.size()),.ppEnabledExtensionNames=extensions_enabled.data()});
    Require(device_result.result,"Create Dozen probe device"); auto device=std::move(device_result.value);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*device); auto queue=device->getQueue(family,0);
    auto retained_presenter=std::make_unique<Presenter>(physical,*device,queue,family,1280,720);
    auto& presenter=*retained_presenter;
    auto pool_result=device->createCommandPoolUnique({.queueFamilyIndex=family}); Require(pool_result.result,"Create probe command pool");
    auto pool=std::move(pool_result.value);
    auto commands=device->allocateCommandBuffers({.commandPool=*pool,.level=vk::CommandBufferLevel::ePrimary,.commandBufferCount=1});
    Require(commands.result,"Allocate probe command"); auto cmd=commands.value.front();
    presenter.Acquire(); Require(cmd.begin({}),"Begin probe frame");
    vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,0,1,0,1};
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,vk::PipelineStageFlagBits::eTransfer,{}, {}, {},
        vk::ImageMemoryBarrier{.dstAccessMask=vk::AccessFlagBits::eTransferWrite,.oldLayout=vk::ImageLayout::eUndefined,
        .newLayout=vk::ImageLayout::eTransferDstOptimal,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=presenter.Image(),.subresourceRange=range});
    cmd.clearColorImage(presenter.Image(),vk::ImageLayout::eTransferDstOptimal,
        vk::ClearColorValue{std::array<float,4>{0.05f,0.25f,0.65f,1.0f}},range);
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eAllCommands,{}, {}, {},
        vk::ImageMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eTransferWrite,.oldLayout=vk::ImageLayout::eTransferDstOptimal,
        .newLayout=vk::ImageLayout::eGeneral,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=presenter.Image(),.subresourceRange=range});
    Require(cmd.end(),"End probe frame");
    auto acquired=presenter.Acquired(); auto ready=presenter.Ready();
    vk::PipelineStageFlags stage=vk::PipelineStageFlagBits::eTransfer;
    Require(queue.submit(vk::SubmitInfo{.waitSemaphoreCount=1,.pWaitSemaphores=&acquired,.pWaitDstStageMask=&stage,
        .commandBufferCount=1,.pCommandBuffers=&cmd,.signalSemaphoreCount=1,.pSignalSemaphores=&ready}),"Submit probe frame");
    presenter.Present(true); Require(device->waitIdle(),"Wait probe frame");
    std::puts("Graphics probe: Vulkan clear -> shared D3D12 image -> CoreWindow PRESENTED");
    display=std::make_unique<ProbeDisplay>();
    display->instance=std::move(instance);
    display->device=std::move(device);
    display->presenter=std::move(retained_presenter);
    return 0;
}
}
