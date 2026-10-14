#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Gaming.Input.h>
#include <windows.h>
#include <gamingdeviceinformation.h>
#include <cstdio>
#include <cstring>
#include <io.h>
#include <fstream>
#include <string>
#include <thread>
#include <atomic>

extern "C" int gow3_probe_main(int argc,char **argv);
extern "C" FILE *gow3_uwp_fopen(const char *path,const char *mode);
static std::atomic<bool> finished{false};

static void loader() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto local=winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
    auto log=std::wstring(local)+L"\\loader.log";
    FILE *stream=nullptr;
    _wfreopen_s(&stream,log.c_str(),L"w",stdout);
    auto error_log=std::wstring(local)+L"\\errors.log";
    _wfreopen_s(&stream,error_log.c_str(),L"w",stderr);
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
    if(mode!="boot"&&mode!="imports") {
        std::puts("READY: upload run.txt (boot or imports on line 1, dump folder on line 2), then relaunch."); finished=true; return;
    }
    if(dump.empty()) { std::puts("STOP: dump folder missing from run.txt"); finished=true; return; }
    std::string dump_eboot=dump+"\\eboot.bin";
    FILE *dump_file=gow3_uwp_fopen(dump_eboot.c_str(),"rb");
    std::printf("External dump eboot.bin readable: %s\n",dump_file ? "yes" : "no (only packaged-code initialization can be tested)");
    if(dump_file) std::fclose(dump_file);
    std::string package=winrt::to_string(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
    std::string boot=package+"\\Prepared\\boot-linked.bin";
    std::string content=package+"\\Prepared\\content.bin";
    std::string user=winrt::to_string(local)+"\\user";
    CreateDirectoryW((std::wstring(local)+L"\\user").c_str(),nullptr);
    std::puts("Running the existing loader and CPU runtime; graphics/media imports remain unresolved.");
    char *args[]={const_cast<char *>("gow3-uwp"),boot.data(),const_cast<char *>("--cpu-only"),
                  const_cast<char *>("--app0"),dump.data(),const_cast<char *>("--user"),user.data(),
                  const_cast<char *>("--content-profile"),content.data(),const_cast<char *>("--check-imports")};
    int result=gow3_probe_main(mode=="imports" ? 10 : 9,args);
    std::printf("Loader returned %d\n",result); finished=true;
}

struct View : winrt::implements<View,winrt::Windows::ApplicationModel::Core::IFrameworkView> {
    winrt::Windows::UI::Core::CoreWindow window{nullptr};
    bool closed=false;
    void Initialize(winrt::Windows::ApplicationModel::Core::CoreApplicationView const& app) {
        app.Activated([this](auto const&,auto const&) { window.Activate(); });
    }
    void SetWindow(winrt::Windows::UI::Core::CoreWindow const& value) {
        window=value; window.Closed([this](auto const&,auto const&) { closed=true; });
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
        std::thread([] { Sleep(30000); if(!finished.load()) { std::fputs("STOP: 30 second milestone watchdog\n",stderr); std::fflush(nullptr); ExitProcess(124); } }).detach();
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
