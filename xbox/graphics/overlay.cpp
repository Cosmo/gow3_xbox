// SPDX-License-Identifier: GPL-2.0-or-later
#include "overlay.h"
extern "C" void runtime_file_profile_report(void);
#include "overlay_chord.h"
#include "imgui.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include <vk_mem_alloc.h>
#include <windows.h>
#include <winrt/Windows.System.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace Gow3Uwp::Overlay {
namespace {
using Clock=std::chrono::steady_clock;
std::atomic<bool> shown{true};
const auto started=Clock::now();
std::array<float,120> frame_times{};
size_t frame_index=0,frame_count=0;
Clock::time_point last_frame{};
}
bool Visible() { return shown.load(); }
void Toggle() {
    shown=!shown.load();
    std::printf("UWP performance overlay: %s\n",shown.load() ? "shown" : "hidden");
}
bool ControllerChord(bool view,bool menu,double left_trigger,double right_trigger) {
    thread_local OverlayChord chord;
    const auto result=chord.Update(view,menu,left_trigger,right_trigger);
    if(result.toggle) Toggle();
    return result.consume;
}
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
    static const bool metrics=[] { auto value=std::getenv("GOW3_UWP_METRICS");return value&&*value=='1'; }();
    if(!Visible()&&!metrics) return;
    thread_local bool apartment=false;
    if(!apartment) {
        try {winrt::init_apartment(winrt::apartment_type::multi_threaded);}
        catch(winrt::hresult_error const&) {}
        apartment=true;
    }
    const auto now=Clock::now();
    static auto sampled=Clock::time_point{};
    static uint64_t used=0,limit=0,cpu_time=0;
    static uint64_t gpu_allocated=0,gpu_reserved=0;
    static double cpu=0;
    if(now-sampled>=std::chrono::milliseconds(250)) {
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
        gpu_allocated=stats.total.statistics.allocationBytes;
        gpu_reserved=stats.total.statistics.blockBytes;
    }
    float sum=0;size_t count=0;
    for(;count<frame_count&&sum<1000;++count)
        sum+=frame_times[(frame_index+frame_times.size()-1-count)%frame_times.size()];
    const float average=count ? sum/count : 0;
    const float fps=average>0 ? 1000/average : 0;
    static auto reported=Clock::time_point{};
    if(metrics&&now-reported>=std::chrono::seconds(5)) {
        std::printf("UWP metrics: t=%lld fps=%.1f ms=%.1f cpu=%.1f ram=%llu limit=%llu gpu=%llu reserved=%llu MiB\n",
            std::chrono::duration_cast<std::chrono::seconds>(now-started).count(),fps,average,cpu,
            used>>20,limit>>20,gpu_allocated>>20,gpu_reserved>>20);
        reported=now;
        runtime_file_profile_report();
    }
    if(!Visible()) return;
    const float scale=std::max(ImGui::GetIO().DisplaySize.y/1080.0f,0.75f);
    ImGui::SetNextWindowPos({24*scale,24*scale},ImGuiCond_Always);
    ImGui::SetNextWindowSize({300*scale,0},ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(31/255.0f,31/255.0f,31/255.0f,0.85f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{12*scale,12*scale});
    ImGui::Begin("UWP performance",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|
        ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted("God of War III Remastered");
    const auto seconds=std::chrono::duration_cast<std::chrono::seconds>(now-started).count();
    ImGui::TextDisabled("Running  %02lld:%02lld",seconds/60,seconds%60);
    ImGui::Text("%.0f fps",fps);ImGui::SameLine();ImGui::TextDisabled("%.1f ms",average);
    std::array<float,120> graph{};
    for(size_t i=0;i<frame_count;++i)
        graph[graph.size()-frame_count+i]=frame_times[(frame_index+frame_times.size()-frame_count+i)%frame_times.size()];
    ImGui::PushStyleColor(ImGuiCol_PlotLines,ImVec4(124/255.0f,194/255.0f,77/255.0f,1));
    ImGui::PlotLines("##frames",graph.data(),int(graph.size()),0,nullptr,0,50,{0,48*scale});
    ImGui::PopStyleColor();
    if(ImGui::BeginTable("System",2,ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextDisabled("System");ImGui::TableNextColumn();
        ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted("CPU");ImGui::TableNextColumn();ImGui::Text("%.0f %%",cpu);
        ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted("RAM");ImGui::TableNextColumn();
        if(limit<(uint64_t(64)<<30)) ImGui::Text("%llu / %llu MiB",used>>20,limit>>20);
        else ImGui::Text("%llu MiB",used>>20);
        ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted("GPU allocated");
        ImGui::TableNextColumn();ImGui::Text("%llu MiB",gpu_allocated>>20);
        ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted("GPU reserved");
        ImGui::TableNextColumn();ImGui::Text("%llu MiB",gpu_reserved>>20);
        ImGui::EndTable();
    }
    ImGui::End();ImGui::PopStyleVar(2);ImGui::PopStyleColor();
}
}
