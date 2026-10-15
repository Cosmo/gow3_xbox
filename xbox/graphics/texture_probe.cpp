#include "present_bridge.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/tile_manager.h"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace Gow3Uwp {
static bool CopyProbe(const Vulkan::Instance& instance,Vulkan::Scheduler& scheduler,vk::Format format,u32 block,u32 bytes) {
    const auto device=instance.GetDevice();
    const vk::ImageCreateInfo ci{
        .imageType=vk::ImageType::e2D,.format=format,.extent={68,52,1},.mipLevels=3,.arrayLayers=2,
        .samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
        .usage=vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode=vk::SharingMode::eExclusive};
    auto image=device.createImageUnique(ci).value;
    auto req=device.getImageMemoryRequirements(*image);
    u32 memory_type=0;
    while(!(req.memoryTypeBits&(1U<<memory_type))) ++memory_type;
    auto memory=device.allocateMemoryUnique(vk::MemoryAllocateInfo{.allocationSize=req.size,.memoryTypeIndex=memory_type}).value;
    if(device.bindImageMemory(*image,*memory,0)!=vk::Result::eSuccess) return false;
    std::vector<vk::BufferImageCopy> uploads,downloads;
    std::vector<u8> expected;
    u32 input_size=512,output_size=0;
    for(u32 mip=0;mip<3;++mip) {
        const u32 w=68>>mip,h=52>>mip;
        const u32 row=((w+block-1)/block)*bytes,rows=(h+block-1)/block;
        const u32 pitch=((80>>mip)+block-1)/block,padded_h=((64>>mip)+block-1)/block;
        uploads.push_back({input_size,pitch*block,padded_h*block,{vk::ImageAspectFlagBits::eColor,mip,0,2},{0,0,0},{w,h,1}});
        downloads.push_back({output_size,0,0,{vk::ImageAspectFlagBits::eColor,mip,0,2},{0,0,0},{w,h,1}});
        input_size+=pitch*padded_h*bytes*2;
        output_size+=row*rows*2;
    }
    VideoCore::Buffer input(instance,0,input_size+512,VideoCore::MemoryType::HostUncached);
    VideoCore::Buffer output(instance,0,output_size+512,VideoCore::MemoryType::HostCached);
    std::fill(input.mapped_data.begin(),input.mapped_data.end(),0xCD);
    for(u32 mip=0;mip<3;++mip) {
        const auto c=uploads[mip];
        const u32 row=((c.imageExtent.width+block-1)/block)*bytes,rows=(c.imageExtent.height+block-1)/block;
        const u32 pitch=c.bufferRowLength/block*bytes,stride=pitch*(c.bufferImageHeight/block);
        for(u32 layer=0;layer<2;++layer) for(u32 y=0;y<rows;++y) for(u32 x=0;x<row;++x) {
            const u8 value=u8(x*19+y*31+layer*83+mip*47);
            input.mapped_data[c.bufferOffset+layer*stride+y*pitch+x]=value;
            expected.push_back(value);
        }
    }
    input.Flush(0,input_size);
    scheduler.Record([&](vk::CommandBuffer cmd) {
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=*image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,3,0,2}};
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
        cmd.copyBufferToImage(input.Handle(),*image,vk::ImageLayout::eTransferDstOptimal,uploads);
        barrier.srcAccessMask=vk::AccessFlagBits2::eTransferWrite;
        barrier.dstAccessMask=vk::AccessFlagBits2::eTransferRead;
        barrier.oldLayout=vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout=vk::ImageLayout::eTransferSrcOptimal;
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
        cmd.copyImageToBuffer(*image,vk::ImageLayout::eTransferSrcOptimal,output.Handle(),downloads);
        Vulkan::Scheduler::RecordFullBarrier(cmd);
    });
    scheduler.Finish();
    output.Invalidate(0,output_size);
    u32 errors=0,first=0;
    for(u32 i=0;i<output_size;++i) if(output.mapped_data[i]!=expected[i]) { if(!errors) first=i; ++errors; }
    std::printf("Texture copy %s, padded rows/layers, 3 mips: %s (%u/%u bytes wrong; first %u expected %02x got %02x)\n",
        vk::to_string(format).c_str(),errors?"FAIL":"PASS",errors,output_size,first,expected[first],output.mapped_data[first]);
    image.reset();
    return errors==0;
}
int TextureProbe() {
    Vulkan::Instance instance(s32(-1),false);
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance,scheduler);
    VideoCore::StreamBuffer params(instance,scheduler,VideoCore::MemoryType::HostUncached,65536);
    VideoCore::TileManager tiles(instance,scheduler,runtime,params);
    unsigned failures=0;
    for(u32 bits : {8U,16U,32U,64U,128U}) {
        VideoCore::ImageInfo info{};
        info.props.is_tiled=1;
        info.num_bits=bits;
        info.tile_mode=AmdGpu::TileMode::Thin1DThin;
        info.array_mode=AmdGpu::ArrayMode::Array1DTiledThin1;
        info.resources={3,2};
        const u32 bytes=bits/8;
        u32 total=0;
        for(u32 mip=0;mip<3;++mip) {
            const u32 pitch=32>>mip,height=32>>mip;
            info.mips_layout[mip]={pitch*height*bytes*2,pitch,height,total};
            total+=info.mips_layout[mip].size;
        }
        info.guest_size=total;
        std::vector<u8> expected(total),tiled(total);
        for(u32 i=0;i<total;++i) expected[i]=u8((i*131+(i>>8)*17)^((i>>4)*29));
        for(u32 mip=0;mip<3;++mip) {
            const auto m=info.mips_layout[mip];
            for(u32 layer=0;layer<2;++layer) for(u32 y=0;y<m.height;++y) for(u32 x=0;x<m.pitch;++x) {
                u32 morton=0;
                for(u32 b=0;b<3;++b) morton|=((x>>b)&1)<<(2*b)|((y>>b)&1)<<(2*b+1);
                const u32 src=m.offset+(layer*m.pitch*m.height+y*m.pitch+x)*bytes;
                const u32 dst=m.offset+(layer*m.pitch*m.height+((y/8)*(m.pitch/8)+x/8)*64+morton)*bytes;
                std::copy_n(expected.data()+src,bytes,tiled.data()+dst);
            }
        }
        VideoCore::Buffer input(instance,0,total+512,VideoCore::MemoryType::HostUncached);
        VideoCore::Buffer output(instance,0,total,VideoCore::MemoryType::HostCached);
        std::copy(tiled.begin(),tiled.end(),input.mapped_data.begin()+512);
        input.Flush(512,total);
        auto [linear,offset]=tiles.DetileImage(&input,512,info);
        const vk::BufferCopy copy{offset,0,total};
        runtime.CopyBuffer(linear,&output,std::span(&copy,1));
        runtime.FlushBarriers();
        scheduler.Finish();
        output.Invalidate(0,total);
        u32 errors=0,first=0;
        for(u32 i=0;i<total;++i) if(output.mapped_data[i]!=expected[i]) { if(!errors) first=i; ++errors; }
        std::printf("Texture detile %u-bit, 3 mips, 2 layers, offset 512: %s (%u/%u bytes wrong; first %u expected %02x got %02x)\n",
            bits,errors?"FAIL":"PASS",errors,total,first,expected[first],output.mapped_data[first]);
        failures+=errors!=0;
    }
    failures+=!CopyProbe(instance,scheduler,vk::Format::eR8Unorm,1,1);
    failures+=!CopyProbe(instance,scheduler,vk::Format::eR8G8B8A8Unorm,1,4);
    failures+=!CopyProbe(instance,scheduler,vk::Format::eBc1RgbaUnormBlock,4,8);
    failures+=!CopyProbe(instance,scheduler,vk::Format::eBc3UnormBlock,4,16);
    return failures?1:0;
}
}
