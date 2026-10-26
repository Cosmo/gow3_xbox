#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <set>
#include <cstdio>

namespace VideoCore {
// The logical DMA page table (one 8-byte entry per 64 KiB, read by shaders through a bounds-checked
// storage buffer) covers the first 128 GiB, not the 40-bit space: the full table was 128 MiB of GPU
// memory, which the Xbox counts against the app limit. The runtime places game memory upward from
// 64 GiB (find_free); God of War III's highest GPU-visible address was 69.4 GiB. A read beyond the
// table returns 0 (not resident); EnsureDenseResident reports a block beyond it once.
static constexpr u64 DmaTableBits=37;
void BufferCache::InitializeDenseBuffers() {
    block_shift=16;block_size=1u<<block_shift;
    blocks_per_arena_page=arena_page_size/block_size;
    blocks_per_arena_page_shift=arena_page_bits-block_shift;
    const u64 pages=u64{1}<<(DmaTableBits-block_shift);
    fault_manager=std::make_unique<FaultManager>(instance,scheduler,*this,block_shift,pages);
    bda_pagetable_buffer=std::make_unique<Buffer>(instance,0,pages*sizeof(u64),MemoryType::DeviceLocal,"Logical DMA Page Table");
    runtime.FillBuffer(bda_pagetable_buffer.get(),0,pages*sizeof(u64),0);
    null_buffer=std::make_unique<Buffer>(instance,0,64_KB,MemoryType::DeviceLocal,"Read-only zero binding");
    runtime.FillBuffer(null_buffer.get(),0,64_KB,0);
    runtime.FlushBarriers();
    scheduler.Finish();
    std::printf("UWP buffer cache: ordinary buffers, %llu MiB pages, %u logical DMA views\n",arena_page_size>>20,DmaViewCount);
}
std::array<vk::DescriptorBufferInfo,BufferCache::DmaViewCount> BufferCache::GetDmaViews() const {
    std::array<vk::DescriptorBufferInfo,DmaViewCount> result;
    for(u32 i=0;i<DmaViewCount;++i) {
        const auto& view=dense_views[i];
        result[i]=view.arena ? vk::DescriptorBufferInfo{view.arena->Handle(),view.offset,arena_page_size}
                            : vk::DescriptorBufferInfo{null_buffer->Handle(),0,null_buffer->SizeBytes()};
    }
    return result;
}
const Buffer* BufferCache::GetDenseArena(u64 first_block,u64 last_block) {
    u64 first_page=first_block>>blocks_per_arena_page_shift;
    u64 last_page=last_block>>blocks_per_arena_page_shift;
    ASSERT(last_page<address_space.size());
    auto existing=address_space[first_page];
    if(existing && existing->cpu_addr<=(first_block<<block_shift) &&
       existing->cpu_addr+existing->size_bytes>((last_block<<block_shift))) return existing;
    std::set<const Buffer*> previous;
    bool expanded;
    do {
        expanded=false;
        for(u64 page=first_page;page<=last_page;++page) if(auto old=address_space[page];old && previous.insert(old).second) {
            u64 old_first=old->cpu_addr>>arena_page_bits;
            u64 old_last=(old->cpu_addr+old->size_bytes-1)>>arena_page_bits;
            expanded|=old_first<first_page||old_last>last_page;
            first_page=std::min(first_page,old_first);last_page=std::max(last_page,old_last);
        }
    } while(expanded);
    auto arena=&arenas.emplace_back(instance,first_page<<arena_page_bits,
        (last_page-first_page+1)<<arena_page_bits,MemoryType::DeviceLocal);
    for(auto old:previous) {
        const vk::BufferCopy copy{0,old->cpu_addr-arena->cpu_addr,old->size_bytes};
        runtime.CopyBuffer(old,arena,std::span{&copy,1});
    }
    for(u64 page=first_page;page<=last_page;++page) {
        address_space[page]=arena;
        auto [entry,inserted]=dense_slots.emplace(page,u32(dense_slots.size()));
        ASSERT_MSG(entry->second<DmaViewCount,"Logical DMA view capacity reached (%u)",DmaViewCount);
        dense_views[entry->second]={arena,(page-first_page)<<arena_page_bits};
    }
    std::printf("UWP dense GPU arena: %#llx, %llu MiB, %zu live DMA views\n",arena->cpu_addr,arena->size_bytes>>20,dense_slots.size());
    return arena;
}
void BufferCache::EnsureDenseResident(const Buffer* arena,u64 first_block,u64 last_block) {
    const u64 table_blocks=bda_pagetable_buffer->SizeBytes()/sizeof(u64);
    if(last_block>=table_blocks) {
        static bool reported=false;
        if(!reported) {
            reported=true;
            std::printf("UWP buffer cache: GPU data at %#llx is beyond the logical DMA table (%llu GiB); shaders read it as zero\n",
                        static_cast<unsigned long long>(last_block<<block_shift),
                        static_cast<unsigned long long>((table_blocks<<block_shift)>>30));
        }
        if(first_block>=table_blocks) return;
        last_block=table_blocks-1;
    }
    IntervalList gaps;
    resident_ranges.ForEachGap(first_block,last_block+1,[&](u64 first,u64 end){gaps.Add({first,end});});
    for(auto const& range:gaps) {
        Backing backing;
        backing.start=range.start;backing.end=range.end;backing.memory=VK_NULL_HANDLE;
        backing.offset=(range.start<<block_shift)-arena->cpu_addr;
        resident_ranges.Add(backing);
        const u64 bytes=(range.end-range.start)*sizeof(u64);
        auto staging=staging_pool.Request(bytes,MemoryType::HostUncached);
        auto entries=reinterpret_cast<u64*>(staging.mapped);
        for(u64 block=range.start;block<range.end;++block) {
            const u64 address=block<<block_shift;
            const u32 slot=dense_slots.at(address>>arena_page_bits);
            *entries++=(u64(slot+1)<<32)|(address&(arena_page_size-1));
        }
        staging.Flush();
        const vk::BufferCopy copy{staging.offset,range.start*sizeof(u64),bytes};
        runtime.CopyBuffer(staging.buffer,bda_pagetable_buffer.get(),std::span{&copy,1});
    }
}
}
