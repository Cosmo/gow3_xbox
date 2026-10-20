#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.UI.Popups.h>
#include <windows.h>
#include <unknwn.h>
#include <gamingdeviceinformation.h>
#include <fileapifromapp.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <io.h>
#include <fstream>
#include <map>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <utility>
#include "prepare_game.h"

extern "C" int gow3_probe_main(int argc,char **argv);
extern "C" FILE *gow3_uwp_fopen(const char *path,const char *mode);
extern "C" HANDLE gow3_uwp_find(const char *pattern,WIN32_FIND_DATAA *out);
extern "C" BOOL gow3_uwp_find_next(HANDLE handle,WIN32_FIND_DATAA *out);
static std::atomic<bool> finished{false};
static std::atomic<bool> game_mode{false};
static uint64_t memory_limit_override=0;
extern "C" void runtime_memory_start_paging();
extern "C" void gow3_uwp_pager_thread_init() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
}
extern "C" uint64_t gow3_uwp_app_memory_usage() {
    return winrt::Windows::System::MemoryManager::AppMemoryUsage();
}
extern "C" uint64_t gow3_uwp_app_memory_limit() {
    auto actual=winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
    return memory_limit_override ? std::min(memory_limit_override,actual) : actual;
}
static winrt::agile_ref<winrt::Windows::UI::Core::CoreWindow> core_window;
static std::atomic<bool> window_closed{false};
#ifdef GOW3_BUILD_GRAPHICS
#include "../graphics/present_bridge.h"
#include "input.h"
extern "C" IUnknown* gow3_uwp_core_window() {
    auto window=core_window.get();
    void* result=nullptr;
    winrt::copy_to_abi(window,result);
    return reinterpret_cast<IUnknown*>(result);
}
#endif

// The normal program exit would run static destructors and DLL teardown while the game's threads
// still run, and during gameplay that hung for minutes with the last frame on screen.
[[noreturn]] static void exit_now(const char* reason) {
    if(!finished.load()) std::printf("UWP lifecycle: exiting (%s)\n",reason);
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(),0);
    for(;;) Sleep(INFINITE);
}
// Closes the app the way closing its window does; ends the process if the window is still open after 3 s.
[[noreturn]] static void close_app(const char* reason) {
    core_window.get().Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High,[] {
        winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
    });
    for(int wait=0;wait<12&&!window_closed;++wait) Sleep(250);
    exit_now(window_closed ? "window closed" : reason);
}

// A system dialog for the player, which the controller operates: the index of the chosen button, -1 when it
// could not be shown. The last button is the one B (Escape) chooses.
static int ask(std::string const& text,std::vector<std::string> const& buttons) {
    std::printf("Player dialog: %s\n",text.c_str());
    auto choice=std::make_shared<std::promise<int>>();
    auto answer=choice->get_future();
    core_window.get().Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,[text,buttons,choice] {
        try {
            winrt::Windows::UI::Popups::MessageDialog dialog(winrt::to_hstring(text),L"God of War III");
            for(size_t i=0;i<buttons.size();++i)
                dialog.Commands().Append(winrt::Windows::UI::Popups::UICommand(winrt::to_hstring(buttons[i]),nullptr,winrt::box_value(int(i))));
            dialog.DefaultCommandIndex(0);
            dialog.CancelCommandIndex(uint32_t(buttons.size()-1));
            dialog.ShowAsync().Completed([choice](auto const& operation,winrt::Windows::Foundation::AsyncStatus status) {
                int index=-1;
                try {
                    if(status==winrt::Windows::Foundation::AsyncStatus::Completed)
                        if(auto command=operation.GetResults()) index=winrt::unbox_value<int>(command.Id());
                } catch(winrt::hresult_error const&) {}
                choice->set_value(index);
            });
        } catch(winrt::hresult_error const& error) {
            std::printf("Player dialog failed: 0x%08x\n",static_cast<unsigned>(error.code()));
            choice->set_value(-1);
        }
    });
    int index=answer.get();
    std::printf("Player choice: %d\n",index);
    return index;
}
// Players see the message and the app closes; developers (run.txt) find it in loader.log.
static void stop(std::string const& message,bool player) {
    std::printf("STOP: %s\n",message.c_str());
    if(!player) return;
    ask(message,{"OK"});
    close_app("closed after a message");
}

// The player's dump: the folder \dumps\GodOfWar3 on a USB drive.
static std::string find_dump() {
    std::vector<std::string> roots;
    try {
        for(auto const& drive:winrt::Windows::Storage::KnownFolders::RemovableDevices().GetFoldersAsync().get()) {
            auto root=winrt::to_string(drive.Path());
            if(!root.empty()) roots.push_back(root);
        }
    } catch(winrt::hresult_error const& error) {
        std::printf("USB drive list failed: 0x%08x\n",static_cast<unsigned>(error.code()));
    }
    // The console names its first USB drive E:; it is tried when the list does not include it.
    if(std::none_of(roots.begin(),roots.end(),[](std::string const& root) { return _strnicmp(root.c_str(),"E:",2)==0; }))
        roots.push_back("E:\\");
    for(auto root:roots) {
        if(root.back()!='\\') root+='\\';
        std::string dump=root+"dumps\\GodOfWar3";
        FILE *probe=gow3_uwp_fopen((dump+"\\eboot.bin").c_str(),"rb");
        std::printf("Game dump %s: %s\n",dump.c_str(),probe ? "found" : "not found");
        if(probe) { std::fclose(probe); return dump; }
    }
    return {};
}

static bool read_file(std::string const& path,Gow3Prepare::Bytes& data) {
    FILE *file=gow3_uwp_fopen(path.c_str(),"rb");
    if(!file) return false;
    data.clear();
    std::vector<uint8_t> chunk(1<<20);
    for(size_t count;(count=std::fread(chunk.data(),1,chunk.size(),file))>0;) data.insert(data.end(),chunk.begin(),chunk.begin()+count);
    const bool ok=!std::ferror(file);
    std::fclose(file);
    return ok;
}
static bool write_file(std::string const& path,Gow3Prepare::Bytes const& data) {
    FILE *file=gow3_uwp_fopen(path.c_str(),"wb");
    if(!file) return false;
    const bool ok=std::fwrite(data.data(),1,data.size(),file)==data.size();
    return std::fclose(file)==0&&ok;
}

// The game files the launcher runs are made from the player's own dump and are never packaged: on the first
// start into LocalState\Prepared, and again when the dump, the patch file or their format changes. Players are
// asked first; developers (run.txt) get them without asking, and the prepare mode always makes them anew.
static bool prepare_game(std::string const& dump,std::string const& prepared,bool player,bool always) {
    Gow3Prepare::Dump files;
    const std::pair<const char*,Gow3Prepare::Bytes*> inputs[]={{"eboot.bin",&files.eboot},
        {"sce_module\\libc.prx",&files.libc},{"sce_module\\libSceFios2.prx",&files.fios},{"sce_sys\\param.sfo",&files.sfo}};
    for(auto [name,data]:inputs) {
        if(!read_file(dump+"\\"+name,*data)||data->empty()) {
            stop("The game dump in "+dump+" is incomplete: "+name+" is missing or empty.\n\nCopy the whole dump to the USB drive again.",player);
            return false;
        }
    }
    Gow3Prepare::Bytes patch_file;
    const std::string patch_path=winrt::to_string(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path())+
        "\\Patches\\CUSA01623.xml";
    if(!read_file(patch_path,patch_file)) { stop("This app is incomplete: its patch file is missing. Install the app again.",player); return false; }
    const std::string patches(patch_file.begin(),patch_file.end());
    if(auto problem=Gow3Prepare::CheckTitle(files.sfo);!problem.empty()) { stop("The game dump in "+dump+" "+problem,player); return false; }
    char marker[96];
    std::snprintf(marker,sizeof marker,"gow3-uwp-prepared " GOW3_PREPARED_FORMAT "\nsource %016llx\n",
                  static_cast<unsigned long long>(Gow3Prepare::Fingerprint(files,patches)));
    std::vector<std::string> outputs={"boot-linked.bin","content.bin","resolutions.txt"};
    for(auto name:Gow3Prepare::Resolutions) outputs.push_back(std::string("patches-")+name+".bin");
    if(!always) {
        Gow3Prepare::Bytes current;
        bool ready=read_file(prepared+"\\format.txt",current)&&std::string(current.begin(),current.end())==marker;
        for(auto const& name:outputs) {
            FILE *probe=ready ? gow3_uwp_fopen((prepared+"\\"+name).c_str(),"rb") : nullptr;
            ready=probe!=nullptr;
            if(probe) std::fclose(probe);
        }
        if(ready) { std::printf("Prepared game files: %s\n",prepared.c_str()); return true; }
    }
    if(player&&ask("Game found: "+dump+"\n\nPrepare the game files? This is needed once and takes a few seconds.",{"Prepare","Cancel"})!=0)
        close_app("game preparation declined");
    std::printf("Preparing game files from %s\n",dump.c_str());
    const auto started=std::chrono::steady_clock::now();
    Gow3Prepare::Files result;
    try { result=Gow3Prepare::Prepare(files,patches); }
    catch(std::exception const& error) { stop(std::string("Preparing the game files failed: ")+error.what(),player); return false; }
    DeleteFileFromAppW(winrt::to_hstring(prepared+"\\format.txt").c_str());
    // resolutions.txt: one "<resolution> <direct memory MiB>" line per patches-<resolution>.bin.
    std::string memory;
    std::vector<std::pair<std::string,Gow3Prepare::Bytes>> written;
    written.emplace_back("boot-linked.bin",std::move(result.boot));
    written.emplace_back("content.bin",std::move(result.content));
    for(auto& resolution:result.resolutions) {
        memory+=resolution.name+" "+std::to_string(resolution.dmem_mb)+"\n";
        written.emplace_back("patches-"+resolution.name+".bin",std::move(resolution.patches));
    }
    written.emplace_back("resolutions.txt",Gow3Prepare::Bytes(memory.begin(),memory.end()));
    for(auto const& [name,data]:written) {
        if(!write_file(prepared+"\\"+name,data)) { stop("Writing the prepared game file "+name+" failed.",player); return false; }
    }
    if(!write_file(prepared+"\\format.txt",Gow3Prepare::Bytes(marker,marker+std::strlen(marker)))) {
        stop("Writing the prepared game files failed.",player); return false;
    }
    std::printf("Prepared game files in %lld ms: %s\n",static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-started).count()),prepared.c_str());
    return true;
}

static void loader() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto local=winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
    {
        std::ifstream limit_file(std::wstring(local)+L"\\memory_limit_mb.txt");
        uint64_t mb=0;
        if(limit_file>>mb && mb>=2048 && mb<=65536) memory_limit_override=mb<<20;
    }
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
    if(memory_limit_override) std::printf("UWP memory: debug budget %llu MiB (OS limit unchanged)\n",gow3_uwp_app_memory_limit()>>20);
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
    // Players have no run.txt: the game runs from their dump on the USB drive. Developers pick a test mode
    // (line 1) and the dump folder (line 2) in it.
    std::ifstream settings(std::wstring(local)+L"\\run.txt");
    const bool player=!settings.is_open();
    std::string mode=player ? "boot" : "",dump;
    if(!player) { std::getline(settings,mode); std::getline(settings,dump); }
    if(!mode.empty()&&mode.back()=='\r') mode.pop_back();
    if(!dump.empty()&&dump.back()=='\r') dump.pop_back();
    std::ifstream debug_settings(std::wstring(local)+L"\\graphics-debug.txt");
    for(std::string line;std::getline(debug_settings,line);) {
        if(!line.empty()&&line.back()=='\r') line.pop_back();
        auto separator=line.find('=');
        if(separator==std::string::npos) continue;
        auto key=line.substr(0,separator),value=line.substr(separator+1);
        // Settings files from before the SpartanPort rename name the same settings GOW3_*.
        if(key.rfind("GOW3_",0)==0) key="SPARTAN_"+key.substr(5);
        if(key=="DZN_DEBUG"||key=="DZN_MAX_SHADER_MODEL"||key=="DZN_MAX_D3D12_OPTIONS"||
           key=="SPARTAN_DUMP_SHADERS"||key=="SPARTAN_UWP_CAPTURE"||key=="SPARTAN_NO_MSAA"||
           key=="SPARTAN_UWP_TEXTURE_DUMP"||key=="SPARTAN_UWP_RENDER_RES"||key=="SPARTAN_DRAW_PIPE"||
           key=="SPARTAN_UWP_TEXTURE_ADDRESSES"||key=="SPARTAN_DEFERRED_READBACK"||
           key=="SPARTAN_STALE_READBACK"||key=="SPARTAN_ASYNC_SHADERS"||key=="SPARTAN_PIPELINE_CACHE"||
           key=="SPARTAN_UWP_METRICS"||key=="SPARTAN_VK_RECORD_THREAD"||key=="SPARTAN_GC_BUDGET_MB"||key=="SPARTAN_UWP_SWAP"||
           key=="SPARTAN_PRECOMPILE_AUTORUN"||key=="SPARTAN_FRAME_STATS"||key=="SPARTAN_IO_STATS"||
           key=="SPARTAN_BACKGROUND_WARMUP"||key=="SPARTAN_PARALLEL_WARMUP"||
           key=="SPARTAN_PIPELINE_BINARIES"||key=="SPARTAN_FRAMES_AHEAD"||key=="SPARTAN_STREAM_MEMORY"||
           key=="SPARTAN_RECORDER_TIMINGS"||key=="SPARTAN_BAKED_PIPELINES"||
           key=="SPARTAN_PERF_DIAG") {
            _putenv_s(key.c_str(),value.c_str());
            SetEnvironmentVariableW(winrt::to_hstring(key).c_str(),winrt::to_hstring(value).c_str());
        }
    }
    game_mode=(mode=="boot");
    _putenv_s("SPARTAN_UWP_LAUNCHER",mode=="boot" ? "1" : "0");
    if(player) {
        dump=find_dump();
        if(dump.empty()) {
            stop("No game dump found.\n\nCopy your God of War III Remastered dump (the folder with eboot.bin) to a USB drive "
                 "as \\dumps\\GodOfWar3, connect the drive and start the app again.",true);
            finished=true; return;
        }
    }
    _putenv_s("SPARTAN_PRECOMPILE_DUMP",dump.c_str());
    std::string user=winrt::to_string(local)+"\\user";
    CreateDirectoryW((std::wstring(local)+L"\\user").c_str(),nullptr);
    CreateDirectoryW((std::wstring(local)+L"\\Prepared").c_str(),nullptr);
    _putenv_s("SPARTAN_GPU_USER_DIR",user.c_str());
    _putenv_s("SPARTAN_UWP_SWAP_FILE",(winrt::to_string(local)+"\\gow3-swap.bin").c_str());
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
    if(mode!="boot"&&mode!="imports"&&mode!="storage"&&mode!="prepare") {
        std::puts("READY: upload run.txt (boot, imports, storage or prepare on line 1, dump folder on line 2), then relaunch."); finished=true; return;
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
    std::string prepared=winrt::to_string(local)+"\\Prepared";
    if(!prepare_game(dump,prepared,player,mode=="prepare")||mode=="prepare") { finished=true; return; }
    std::string boot=prepared+"\\boot-linked.bin";
    std::string content=prepared+"\\content.bin";
    _putenv_s("SPARTAN_CONFIG",(user+"\\graphics.cfg").c_str());
    _putenv_s("SPARTAN_PAD_FILE",(winrt::to_string(local)+"\\controls.txt").c_str());
    _putenv_s("SPARTAN_CAPTURE_TRIGGER",(winrt::to_string(local)+"\\capture.txt").c_str());
    _putenv_s("SPARTAN_TOGGLE_FILE",(winrt::to_string(local)+"\\toggles.txt").c_str());
    _putenv_s("SPARTAN_UPSCALER","off");
    _putenv_s("SPARTAN_READBACK_LINEAR","1");
    if(!std::getenv("SPARTAN_NO_MSAA")) _putenv_s("SPARTAN_NO_MSAA","1");
    if(!std::getenv("SPARTAN_DRAW_PIPE")) _putenv_s("SPARTAN_DRAW_PIPE","1");
    if(!std::getenv("SPARTAN_VK_RECORD_THREAD")) _putenv_s("SPARTAN_VK_RECORD_THREAD","1");
    _putenv_s("SPARTAN_COPY_THREADS","0");
    _putenv_s("SPARTAN_PREP_WORKERS","0");
    // Finish saved-pipeline loading before play; reserve background compilation for new effects.
    // Compiler concurrency and retained IR pools are bounded separately in the UWP renderer.
    const std::pair<const char*,const char*> performance_defaults[]={
        {"SPARTAN_PIPELINE_CACHE","1"},{"SPARTAN_BACKGROUND_WARMUP","0"},{"SPARTAN_PARALLEL_WARMUP","1"},
        {"SPARTAN_PIPELINE_BINARIES","1"},{"SPARTAN_STREAM_MEMORY","cached"}
    };
    for(auto [key,value]:performance_defaults) if(!std::getenv(key)) _putenv_s(key,value);
    // The in-game menu's settings, as upstream's launcher reads them before a start: the render resolution chooses
    // the patches, and the settings the renderer reads as variables are passed on (graphics-debug.txt still wins).
    std::map<std::string,std::string> menu;
    {
        std::ifstream file(winrt::to_hstring(user+"\\graphics.cfg").c_str());
        for(std::string line;std::getline(file,line);) {
            if(!line.empty()&&line.back()=='\r') line.pop_back();
            const auto separator=line.find('=');
            if(separator!=std::string::npos&&line[0]!='#') menu[line.substr(0,separator)]=line.substr(separator+1);
        }
    }
    const auto setting=[&](const char* key,const char* fallback) {
        const auto found=menu.find(key);
        return found==menu.end() ? std::string(fallback) : found->second;
    };
    struct Passed {const char* variable; const char* key; const char* fallback;};
    const Passed passed[]={{"SPARTAN_ASYNC_SHADERS","async_shaders","1"},
        {"SPARTAN_DEFERRED_READBACK","deferred_readback","1"},{"SPARTAN_STALE_READBACK","stale_readback","1"}};
    for(auto const& entry:passed) if(!std::getenv(entry.variable)) _putenv_s(entry.variable,setting(entry.key,entry.fallback).c_str());
    _putenv_s("SPARTAN_GLYPHS","xbox");  // the menu names the Xbox controller's buttons
    std::map<std::string,std::string> direct_memory;  // resolutions.txt: the prepared resolutions and their memory
    {
        std::ifstream file(winrt::to_hstring(prepared+"\\resolutions.txt").c_str());
        for(std::string name,megabytes;file>>name>>megabytes;) direct_memory[name]=megabytes;
    }
    std::string resolution=setting("render_resolution","720p");
    if(const char* forced=std::getenv("SPARTAN_UWP_RENDER_RES")) resolution=forced;
    if(!direct_memory.count(resolution)) {
        std::printf("UWP graphics: no patches prepared for %s, 720p instead\n",resolution.c_str());
        resolution="720p";
    }
    std::string patches=prepared+"\\patches-"+resolution+".bin";
    _putenv_s("SPARTAN_DMEM_MB",direct_memory[resolution].c_str());
    if(mode=="boot") runtime_memory_start_paging();
    _putenv_s("SPARTAN_ENGINE_FPS","60");
    _putenv_s("SPARTAN_VBLANK_HZ","60");
    std::printf("UWP graphics patches: %s, %s MiB virtual direct pool, 60 Hz engine\n",resolution.c_str(),
        direct_memory[resolution].c_str());
    std::printf("UWP renderer: draw pipe %s, record thread %s, async shaders %s, pipeline cache %s\n",
        std::getenv("SPARTAN_DRAW_PIPE"),std::getenv("SPARTAN_VK_RECORD_THREAD"),
        std::getenv("SPARTAN_ASYNC_SHADERS"),std::getenv("SPARTAN_PIPELINE_CACHE"));
    std::printf("UWP performance: background warm-up %s, parallel warm-up %s, pipeline binaries %s, stream memory %s, "
        "readbacks deferred %s stale %s\n",
        std::getenv("SPARTAN_BACKGROUND_WARMUP"),std::getenv("SPARTAN_PARALLEL_WARMUP"),
        std::getenv("SPARTAN_PIPELINE_BINARIES"),std::getenv("SPARTAN_STREAM_MEMORY"),
        std::getenv("SPARTAN_DEFERRED_READBACK"),std::getenv("SPARTAN_STALE_READBACK"));
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
        window=value; window.Closed([](auto const&,auto const&) {
            if(!finished.load()) { std::puts("UWP lifecycle: window closed"); std::fflush(stdout); }
            window_closed=true;
        });
        winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested(
            [](auto const&,winrt::Windows::UI::Core::BackRequestedEventArgs const& event) {
                event.Handled(true);
            });
        window.VisibilityChanged([](auto const&,auto const& event) {
            std::printf("UWP lifecycle: visible=%u\n",unsigned(event.Visible())); std::fflush(stdout);
        });
#ifdef GOW3_LINK_RENDERER
        window.KeyDown([](auto const&,auto const& event) {
            if(gow3_uwp_dialog_key(uint32_t(event.VirtualKey()),1,event.KeyStatus().WasKeyDown)) event.Handled(true);
        });
        window.KeyUp([](auto const&,auto const& event) {
            if(gow3_uwp_dialog_key(uint32_t(event.VirtualKey()),0,0)) event.Handled(true);
        });
#endif
        core_window=winrt::agile_ref(value);
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
        // Scripts request a normal close through LocalState\quit.txt, the same path as closing the window,
        // instead of terminating the process (which leaves the window or the last frame on screen).
        std::thread([quit=std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())+L"\\quit.txt"] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            DeleteFileFromAppW(quit.c_str());
            while(!GetFileAttributesExFromAppW(quit.c_str(),GetFileExInfoStandard,&attributes)) Sleep(250);
            DeleteFileFromAppW(quit.c_str());
            if(!finished.load()) { std::puts("UWP lifecycle: quit requested"); std::fflush(stdout); }
            close_app("quit requested, window still open");
        }).detach();
        while(!window_closed) window.Dispatcher().ProcessEvents(winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessOneAndAllPending);
        exit_now("window closed");
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
