#include "common/types.h"
#include "present_bridge.h"
#include <cstdio>
#include <array>
#include <stdexcept>
#ifdef GOW3_LINK_RENDERER
#include <vk_mem_alloc.h>
#endif
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
    std::array extensions_enabled{"VK_KHR_external_memory_win32","VK_KHR_dynamic_rendering"};
    vk::PhysicalDeviceDynamicRenderingFeaturesKHR rendering_features{.dynamicRendering=VK_TRUE};
    auto device_result=physical.createDeviceUnique({.pNext=&rendering_features,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info,
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
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eColorAttachmentOutput,{}, {}, {},
        vk::ImageMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eTransferWrite,.oldLayout=vk::ImageLayout::eTransferDstOptimal,
        .newLayout=vk::ImageLayout::eColorAttachmentOptimal,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=presenter.Image(),.subresourceRange=range});
    vk::RenderingAttachmentInfo attachment{.imageView=presenter.View(),.imageLayout=vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp=vk::AttachmentLoadOp::eClear,.storeOp=vk::AttachmentStoreOp::eStore,
        .clearValue=vk::ClearValue{vk::ClearColorValue{std::array<float,4>{0.05f,0.25f,0.65f,1.0f}}}};
    std::puts("Graphics probe: begin dynamic rendering with a real color attachment");
    cmd.beginRendering(vk::RenderingInfo{.renderArea={{0,0},{1280,720}},.layerCount=1,
        .colorAttachmentCount=1,.pColorAttachments=&attachment});
    cmd.endRendering();
#ifdef GOW3_LINK_RENDERER
    VmaVulkanFunctions functions{.vkGetInstanceProcAddr=VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr=VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr};
    VmaAllocatorCreateInfo allocator_info{.physicalDevice=physical,.device=*device,.pVulkanFunctions=&functions,
        .instance=*instance,.vulkanApiVersion=VK_API_VERSION_1_2};
    VmaAllocator allocator{};
    Require(vk::Result(vmaCreateAllocator(&allocator_info,&allocator)),"Create probe VMA allocator");
    VkImageCreateInfo image_info{.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.extent={1280,720,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo allocation_info{.flags=VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage image{}; VmaAllocation allocation{};
    Require(vk::Result(vmaCreateImage(allocator,&image_info,&allocation_info,&image,&allocation,nullptr)),"Allocate probe VMA attachment");
    auto image_view=device->createImageViewUnique({.image=image,.viewType=vk::ImageViewType::e2D,
        .format=vk::Format::eR8G8B8A8Unorm,.subresourceRange=range});
    Require(image_view.result,"Create VMA attachment view");
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,vk::PipelineStageFlagBits::eColorAttachmentOutput,{}, {}, {},
        vk::ImageMemoryBarrier{.dstAccessMask=vk::AccessFlagBits::eColorAttachmentWrite,.oldLayout=vk::ImageLayout::eUndefined,
        .newLayout=vk::ImageLayout::eColorAttachmentOptimal,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange=range});
    attachment.imageView=*image_view.value;
    attachment.clearValue=vk::ClearValue{vk::ClearColorValue{std::array<float,4>{0.1f,0.4f,0.1f,1.0f}}};
    std::puts("Graphics probe: begin dynamic rendering with a VMA mutable color attachment");
    cmd.beginRendering(vk::RenderingInfo{.renderArea={{0,0},{1280,720}},.layerCount=1,
        .colorAttachmentCount=1,.pColorAttachments=&attachment});
    cmd.endRendering();
    const std::array copy_barriers{
        vk::ImageMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eColorAttachmentWrite,.dstAccessMask=vk::AccessFlagBits::eTransferRead,
            .oldLayout=vk::ImageLayout::eColorAttachmentOptimal,.newLayout=vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange=range},
        vk::ImageMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eColorAttachmentWrite,.dstAccessMask=vk::AccessFlagBits::eTransferWrite,
            .oldLayout=vk::ImageLayout::eColorAttachmentOptimal,.newLayout=vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=presenter.Image(),.subresourceRange=range}};
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,vk::PipelineStageFlagBits::eTransfer,{}, {}, {},copy_barriers);
    cmd.copyImage(image,vk::ImageLayout::eTransferSrcOptimal,presenter.Image(),vk::ImageLayout::eTransferDstOptimal,
        vk::ImageCopy{.srcSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
            .dstSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.extent={1280,720,1}});
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eAllCommands,{}, {}, {},
        vk::ImageMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eTransferWrite,.oldLayout=vk::ImageLayout::eTransferDstOptimal,
#else
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,vk::PipelineStageFlagBits::eAllCommands,{}, {}, {},
        vk::ImageMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eColorAttachmentWrite,.oldLayout=vk::ImageLayout::eColorAttachmentOptimal,
#endif
        .newLayout=vk::ImageLayout::eGeneral,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=presenter.Image(),.subresourceRange=range});
    Require(cmd.end(),"End probe frame");
    auto acquired=presenter.Acquired(); auto ready=presenter.Ready();
    vk::PipelineStageFlags stage=vk::PipelineStageFlagBits::eTransfer;
    Require(queue.submit(vk::SubmitInfo{.waitSemaphoreCount=1,.pWaitSemaphores=&acquired,.pWaitDstStageMask=&stage,
        .commandBufferCount=1,.pCommandBuffers=&cmd,.signalSemaphoreCount=1,.pSignalSemaphores=&ready}),"Submit probe frame");
    presenter.Present(true); Require(device->waitIdle(),"Wait probe frame");
#ifdef GOW3_LINK_RENDERER
    image_view.value.reset();
    vmaDestroyImage(allocator,image,allocation);
    vmaDestroyAllocator(allocator);
    std::puts("Graphics probe: VMA color attachment rendered and copied to CoreWindow");
#endif
    std::puts("Graphics probe: Vulkan clear -> shared D3D12 image -> CoreWindow PRESENTED");
    display=std::make_unique<ProbeDisplay>();
    display->instance=std::move(instance);
    display->device=std::move(device);
    display->presenter=std::move(retained_presenter);
    return 0;
}
}
