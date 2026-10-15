#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Gaming.Input.h>
#include <windows.h>
#include <unknwn.h>
#include <gamingdeviceinformation.h>
#include <fileapifromapp.h>
#include <cstdio>
#include <cstring>
#include <io.h>
#include <fstream>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

extern "C" int gow3_probe_main(int argc,char **argv);
extern "C" FILE *gow3_uwp_fopen(const char *path,const char *mode);
extern "C" HANDLE gow3_uwp_find(const char *pattern,WIN32_FIND_DATAA *out);
extern "C" BOOL gow3_uwp_find_next(HANDLE handle,WIN32_FIND_DATAA *out);
static std::atomic<bool> finished{false};
static std::atomic<bool> game_mode{false};
extern "C" uint64_t gow3_uwp_app_memory_usage() {
    return winrt::Windows::System::MemoryManager::AppMemoryUsage();
}
#ifdef GOW3_BUILD_GRAPHICS
#include "../graphics/present_bridge.h"
#include "../graphics/overlay.h"
static winrt::agile_ref<winrt::Windows::UI::Core::CoreWindow> core_window;
extern "C" IUnknown* gow3_uwp_core_window() {
    auto window=core_window.get();
    void* result=nullptr;
    winrt::copy_to_abi(window,result);
    return reinterpret_cast<IUnknown*>(result);
}
#endif

static void loader() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto local=winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
    auto log=std::wstring(local)+L"\\loader.log";
    _wfreopen(log.c_str(),L"w",stdout);
    auto error_log=std::wstring(local)+L"\\errors.log";
    _wfreopen(error_log.c_str(),L"w",stderr);
    auto driver_log=std::wstring(local)+L"\\dozen.log";
    HANDLE driver_stream=CreateFile2FromAppW(driver_log.c_str(),GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE,CREATE_ALWAYS,nullptr);
    if(driver_stream!=INVALID_HANDLE_VALUE) SetStdHandle(STD_ERROR_HANDLE,driver_stream);
    setvbuf(stdout,nullptr,_IONBF,0); setvbuf(stderr,nullptr,_IONBF,0);
    std::printf("GOW3 UWP loader milestone 1\n");
    GAMING_DEVICE_MODEL_INFORMATION model{};
    GetGamingDeviceModelInformation(&model);
    std::printf("Device model: 0x%08x; app budget: %llu MiB; controllers: %u\n",model.deviceId,
                static_cast<unsigned long long>(winrt::Windows::System::MemoryManager::AppMemoryUsageLimit()>>20),
                winrt::Windows::Gaming::Input::Gamepad::Gamepads().Size());
    void *code=VirtualAllocFromApp(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    DWORD old{};
    if(!code) { std::printf("STOP: executable smoke allocation failed (%lu)\n",GetLastError()); finished=true; return; }
    const unsigned char instructions[]={0xb8,0x2a,0,0,0,0xc3};
    std::memcpy(code,instructions,sizeof(instructions));
    if(!VirtualProtectFromApp(code,4096,PAGE_EXECUTE_READ,&old)) {
        std::printf("STOP: executable smoke protection failed (%lu)\n",GetLastError()); VirtualFree(code,0,MEM_RELEASE); finished=true; return;
    }
    FlushInstructionCache(GetCurrentProcess(),code,4096);
    using NativeFunction=int (__attribute__((sysv_abi)) *)();
    int smoke=reinterpret_cast<NativeFunction>(code)();
    VirtualFree(code,0,MEM_RELEASE);
    std::printf("Executable System V ABI smoke: %s (%d)\n",smoke==42 ? "PASS" : "FAIL",smoke);
    if(smoke!=42) { finished=true; return; }
    std::ifstream settings(std::wstring(local)+L"\\run.txt");
    std::string mode,dump;
    std::getline(settings,mode); std::getline(settings,dump);
    if(!mode.empty()&&mode.back()=='\r') mode.pop_back();
    if(!dump.empty()&&dump.back()=='\r') dump.pop_back();
    std::ifstream debug_settings(std::wstring(local)+L"\\graphics-debug.txt");
    for(std::string line;std::getline(debug_settings,line);) {
        if(!line.empty()&&line.back()=='\r') line.pop_back();
        auto separator=line.find('=');
        if(separator==std::string::npos) continue;
        auto key=line.substr(0,separator),value=line.substr(separator+1);
        if(key=="DZN_DEBUG"||key=="DZN_MAX_SHADER_MODEL"||key=="DZN_MAX_D3D12_OPTIONS"||
           key=="GOW3_DUMP_SHADERS"||key=="GOW3_UWP_CAPTURE"||key=="GOW3_NO_MSAA"||
           key=="GOW3_UWP_TEXTURE_DUMP"||key=="GOW3_UWP_RENDER_RES"||key=="GOW3_DRAW_PIPE"||
           key=="GOW3_UWP_TEXTURE_ADDRESSES"||key=="GOW3_DEFERRED_READBACK"||
           key=="GOW3_STALE_READBACK"||key=="GOW3_ASYNC_SHADERS"||key=="GOW3_PIPELINE_CACHE"||
           key=="GOW3_UWP_METRICS"||key=="GOW3_VK_RECORD_THREAD") {
            _putenv_s(key.c_str(),value.c_str());
            SetEnvironmentVariableW(winrt::to_hstring(key).c_str(),winrt::to_hstring(value).c_str());
        }
    }
    game_mode=(mode=="boot");
    std::string user=winrt::to_string(local)+"\\user";
    CreateDirectoryW((std::wstring(local)+L"\\user").c_str(),nullptr);
    _putenv_s("GOW3_GPU_USER_DIR",user.c_str());
#ifdef GOW3_BUILD_GRAPHICS
    if(mode=="graphics") { int result=Gow3Uwp::GraphicsProbe(); std::printf("Graphics probe returned %d\n",result); finished=true; return; }
#ifdef GOW3_LINK_RENDERER
    if(mode=="texture") { int result=Gow3Uwp::TextureProbe(); std::printf("Texture probe returned %d\n",result); finished=true; return; }
#endif
#endif
    if(mode=="memory") {
        char *args[]={const_cast<char *>("gow3-uwp"),const_cast<char *>("--memory-selftest")};
        int result=gow3_probe_main(2,args);
        std::printf("Memory contract self-test returned %d\n",result); finished=true; return;
    }
    if(mode!="boot"&&mode!="imports"&&mode!="storage") {
        std::puts("READY: upload run.txt (boot or imports on line 1, dump folder on line 2), then relaunch."); finished=true; return;
    }
    if(dump.empty()) { std::puts("STOP: dump folder missing from run.txt"); finished=true; return; }
    if(mode=="storage") {
        std::string paths[]={dump.substr(0,3),dump.substr(0,dump.find_last_of("\\/")),dump};
        for(auto const& path:paths) {
            WIN32_FIND_DATAA entry{};
            HANDLE search=gow3_uwp_find((path+"\\*").c_str(),&entry);
            if(search==INVALID_HANDLE_VALUE) { std::printf("List %s: error %lu\n",path.c_str(),GetLastError()); continue; }
            std::printf("List %s:\n",path.c_str());
            unsigned count=0;
            do { std::printf("  %s%s\n",entry.cFileName,entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY ? "/" : ""); }
            while(++count<24&&gow3_uwp_find_next(search,&entry));
            FindClose(search);
        }
    }
    std::string dump_eboot=dump+"\\eboot.bin";
    try {
        auto file=winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(winrt::to_hstring(dump_eboot)).get();
        std::printf("Game file broker lookup: PASS (%s)\n",winrt::to_string(file.Name()).c_str());
    } catch(winrt::hresult_error const& error) {
        std::printf("Game file broker lookup: 0x%08x\n",static_cast<unsigned>(error.code()));
    }
    FILE *dump_file=gow3_uwp_fopen(dump_eboot.c_str(),"rb");
    DWORD dump_error=dump_file ? 0 : GetLastError();
    std::printf("External dump eboot.bin readable: %s\n",dump_file ? "yes" : "no (only packaged-code initialization can be tested)");
    if(!dump_file) std::printf("External dump open error: %lu\n",dump_error);
    if(dump_file) std::fclose(dump_file);
    if(mode=="storage") {
        auto name=winrt::to_hstring(dump_eboot);
        HANDLE strict=CreateFile2FromAppW(name.c_str(),GENERIC_READ,FILE_SHARE_READ,OPEN_EXISTING,nullptr);
        DWORD strict_error=strict==INVALID_HANDLE_VALUE ? GetLastError() : 0;
        std::printf("Native read-only open, default parameters: %s (error %lu)\n",strict==INVALID_HANDLE_VALUE ? "failed" : "PASS",strict_error);
        if(strict!=INVALID_HANDLE_VALUE) CloseHandle(strict);
        try {
            auto file=winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(name).get();
            std::printf("WinRT file lookup: PASS (%s)\n",winrt::to_string(file.Name()).c_str());
        } catch(winrt::hresult_error const& error) {
            std::printf("WinRT file lookup: 0x%08x %s\n",static_cast<unsigned>(error.code()),winrt::to_string(error.message()).c_str());
        }
        finished=true; return;
    }
    std::string package=winrt::to_string(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
    std::string boot=package+"\\Prepared\\boot-linked.bin";
    std::string content=package+"\\Prepared\\content.bin";
    _putenv_s("GOW3_CONFIG",(user+"\\graphics.cfg").c_str());
    _putenv_s("GOW3_PAD_FILE",(winrt::to_string(local)+"\\controls.txt").c_str());
    _putenv_s("GOW3_CAPTURE_TRIGGER",(winrt::to_string(local)+"\\capture.txt").c_str());
    _putenv_s("GOW3_TOGGLE_FILE",(winrt::to_string(local)+"\\toggles.txt").c_str());
    _putenv_s("GOW3_UPSCALER","off");
    _putenv_s("GOW3_READBACK_LINEAR","1");
    if(!std::getenv("GOW3_NO_MSAA")) _putenv_s("GOW3_NO_MSAA","1");
    if(!std::getenv("GOW3_DRAW_PIPE")) _putenv_s("GOW3_DRAW_PIPE","1");
    if(!std::getenv("GOW3_VK_RECORD_THREAD")) _putenv_s("GOW3_VK_RECORD_THREAD","0");
    _putenv_s("GOW3_COPY_THREADS","0");
    _putenv_s("GOW3_PREP_WORKERS","0");
    if(!std::getenv("GOW3_ASYNC_SHADERS")) _putenv_s("GOW3_ASYNC_SHADERS","0");
    if(!std::getenv("GOW3_PIPELINE_CACHE")) _putenv_s("GOW3_PIPELINE_CACHE","0");
    const char* render_res=std::getenv("GOW3_UWP_RENDER_RES");
    const bool use_720p=render_res&&std::strcmp(render_res,"720p")==0;
    std::string patches=package+"\\Prepared\\patches-"+(use_720p?"720p":"native")+".bin";
    _putenv_s("GOW3_DMEM_MB","7104");
    _putenv_s("GOW3_ENGINE_FPS","60");
    _putenv_s("GOW3_VBLANK_HZ","60");
    std::printf("UWP graphics patches: %s, texture arena fix, 7104 MiB virtual direct pool, 60 Hz engine\n",use_720p?"720p":"native");
    std::printf("UWP renderer: draw pipe %s, record thread %s, async shaders %s, pipeline cache %s\n",
        std::getenv("GOW3_DRAW_PIPE"),std::getenv("GOW3_VK_RECORD_THREAD"),
        std::getenv("GOW3_ASYNC_SHADERS"),std::getenv("GOW3_PIPELINE_CACHE"));
    std::puts("Running the existing loader and runtime with native UWP audio output.");
    std::vector<std::string> arguments={"gow3-uwp",boot};
#ifdef GOW3_LINK_RENDERER
    if(mode!="boot") arguments.push_back("--cpu-only");
#else
    arguments.push_back("--cpu-only");
#endif
    arguments.insert(arguments.end(),{"--app0",dump,"--user",user,"--content-profile",content,"--patches",patches});
    if(mode=="imports") arguments.push_back("--check-imports");
    std::vector<char*> args;
    for(auto& argument:arguments) args.push_back(argument.data());
    int result=gow3_probe_main(static_cast<int>(args.size()),args.data());
    std::printf("Loader returned %d\n",result); finished=true;
}

struct View : winrt::implements<View,winrt::Windows::ApplicationModel::Core::IFrameworkView> {
    winrt::Windows::UI::Core::CoreWindow window{nullptr};
    bool closed=false;
    void Initialize(winrt::Windows::ApplicationModel::Core::CoreApplicationView const& app) {
        app.Activated([this](auto const&,auto const&) { window.Activate(); });
        winrt::Windows::ApplicationModel::Core::CoreApplication::Suspending([](auto const&,auto const&) {
            std::printf("UWP lifecycle: suspending; memory=%llu MiB, limit=%llu MiB\n",
                winrt::Windows::System::MemoryManager::AppMemoryUsage()>>20,
                winrt::Windows::System::MemoryManager::AppMemoryUsageLimit()>>20);
            std::fflush(stdout);
        });
        winrt::Windows::ApplicationModel::Core::CoreApplication::Resuming([](auto const&,auto const&) {
            std::puts("UWP lifecycle: resuming"); std::fflush(stdout);
        });
    }
    void SetWindow(winrt::Windows::UI::Core::CoreWindow const& value) {
        window=value; window.Closed([this](auto const&,auto const&) { closed=true; });
        winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested(
            [](auto const&,winrt::Windows::UI::Core::BackRequestedEventArgs const& event) {
                event.Handled(true);
            });
        window.VisibilityChanged([](auto const&,auto const& event) {
            std::printf("UWP lifecycle: visible=%u\n",unsigned(event.Visible())); std::fflush(stdout);
        });
#ifdef GOW3_LINK_RENDERER
        window.KeyDown([](auto const&,auto const& event) {
            if(event.VirtualKey()==winrt::Windows::System::VirtualKey::F3&&!event.KeyStatus().WasKeyDown) {
                event.Handled(true);Gow3Uwp::Overlay::Toggle();
            }
        });
#endif
#ifdef GOW3_BUILD_GRAPHICS
        core_window=winrt::agile_ref(value);
#endif
    }
    void Load(winrt::hstring const&) {}
    void Run() {
        std::thread([] {
            try { loader(); }
            catch(winrt::hresult_error const& error) { std::printf("STOP: WinRT error 0x%08x\n",static_cast<unsigned>(error.code())); finished=true; }
            catch(std::exception const& error) { std::printf("STOP: host error: %s\n",error.what()); finished=true; }
            std::fflush(nullptr);
            std::fclose(stdout); std::fclose(stderr);
        }).detach();
        std::thread([] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            for(unsigned sample=0;sample<24&&!finished.load();++sample) {
                Sleep(5000);
                std::printf("UWP startup memory: %llu/%llu MiB, level=%u\n",
                    winrt::Windows::System::MemoryManager::AppMemoryUsage()>>20,
                    winrt::Windows::System::MemoryManager::AppMemoryUsageLimit()>>20,
                    unsigned(winrt::Windows::System::MemoryManager::AppMemoryUsageLevel()));
                std::fflush(stdout);
                if(sample==5&&!finished.load()&&!game_mode.load()) {
                    std::fputs("STOP: 30 second milestone watchdog\n",stderr); std::fflush(nullptr); ExitProcess(124);
                }
            }
        }).detach();
        while(!closed) window.Dispatcher().ProcessEvents(winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessOneAndAllPending);
    }
    void Uninitialize() {}
};
struct ViewSource : winrt::implements<ViewSource,winrt::Windows::ApplicationModel::Core::IFrameworkViewSource> {
    winrt::Windows::ApplicationModel::Core::IFrameworkView CreateView() { return winrt::make<View>(); }
};
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    winrt::init_apartment();
    winrt::Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<ViewSource>());
    return 0;
}
