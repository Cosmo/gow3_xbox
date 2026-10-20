// SPDX-License-Identifier: GPL-2.0-or-later
#include "overlay.h"
extern "C" void runtime_file_profile_report(void);
#include "video_core/renderer_vulkan/vk_instance.h"
#include <vk_mem_alloc.h>
#include <windows.h>
#include <winrt/Windows.System.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace Gow3Uwp::Overlay {
namespace {
using Clock=std::chrono::steady_clock;
const auto started=Clock::now();
std::array<float,120> frame_times{};
size_t frame_index=0,frame_count=0;
Clock::time_point last_frame{};
const bool metrics=[] { auto value=std::getenv("SPARTAN_UWP_METRICS");return value&&*value=='1'; }();
}
bool Visible() { return metrics; }
void Presented() {
    const auto now=Clock::now();
    if(last_frame!=Clock::time_point{}) {
        frame_times[frame_index]=std::chrono::duration<float,std::milli>(now-last_frame).count();
        frame_index=(frame_index+1)%frame_times.size();
        frame_count=std::min(frame_count+1,frame_times.size());
    }
    last_frame=now;
}
void Render(const Vulkan::Instance& instance) {
    if(!metrics) return;
    thread_local bool apartment=false;
    if(!apartment) {
        try {winrt::init_apartment(winrt::apartment_type::multi_threaded);}
        catch(winrt::hresult_error const&) {}
        apartment=true;
    }
    const auto now=Clock::now();
    static auto reported=Clock::time_point{},sampled=Clock::time_point{};
    if(now-reported<std::chrono::seconds(5)) return;
    static uint64_t cpu_time=0;
    uint64_t used=0,limit=0;
    double cpu=0;
    try {
        used=winrt::Windows::System::MemoryManager::AppMemoryUsage();
        limit=winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
    } catch(winrt::hresult_error const&) {}
    FILETIME created{},exited{},kernel{},user{};
    if(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) {
        auto ticks=[](FILETIME t) { return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime; };
        const auto total=ticks(kernel)+ticks(user);
        if(cpu_time&&sampled!=Clock::time_point{}) {
            const double wall=std::chrono::duration<double>(now-sampled).count()*1e7*
                std::max(1u,std::thread::hardware_concurrency());
            cpu=std::clamp(double(total-cpu_time)/wall*100,0.0,100.0);
        }
        cpu_time=total;
    }
    sampled=now;
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(instance.GetAllocator(),&stats);
    float sum=0;size_t count=0;
    for(;count<frame_count&&sum<1000;++count)
        sum+=frame_times[(frame_index+frame_times.size()-1-count)%frame_times.size()];
    const float average=count ? sum/count : 0;
    const float fps=average>0 ? 1000/average : 0;
    std::printf("UWP metrics: t=%lld fps=%.1f ms=%.1f cpu=%.1f ram=%llu limit=%llu gpu=%llu reserved=%llu MiB\n",
        std::chrono::duration_cast<std::chrono::seconds>(now-started).count(),fps,average,cpu,
        used>>20,limit>>20,stats.total.statistics.allocationBytes>>20,stats.total.statistics.blockBytes>>20);
    reported=now;
    runtime_file_profile_report();
}
}
