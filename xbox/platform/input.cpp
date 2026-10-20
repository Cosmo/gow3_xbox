#include "input.h"
#include <windows.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <algorithm>
#include <cstdio>

static void Apartment() {
    thread_local bool initialized=false;
    if(!initialized) {
        try {winrt::init_apartment(winrt::apartment_type::multi_threaded);}
        catch(winrt::hresult_error const& e) {if(e.code()!=RPC_E_CHANGED_MODE) throw;}
        initialized=true;
    }
}
static int ReadGamepad(uint32_t* buttons,uint8_t axes[6]) {
    try {
        Apartment();
        auto pads=winrt::Windows::Gaming::Input::Gamepad::Gamepads();
        if(!pads.Size()) return 0;
        auto reading=pads.GetAt(0).GetCurrentReading();
        using B=winrt::Windows::Gaming::Input::GamepadButtons;
        struct Mapping {B host;uint32_t guest;};
        constexpr Mapping map[]={
            {B::A,0x4000},{B::B,0x2000},{B::X,0x8000},{B::Y,0x1000},
            {B::LeftShoulder,0x400},{B::RightShoulder,0x800},
            {B::LeftThumbstick,2},{B::RightThumbstick,4},{B::Menu,8},{B::View,0x80000000},
            {B::DPadUp,0x10},{B::DPadRight,0x20},{B::DPadDown,0x40},{B::DPadLeft,0x80}};
        *buttons=0;
        for(auto const& entry:map) if((reading.Buttons&entry.host)!=B::None) *buttons|=entry.guest;
        auto axis=[](double value){return static_cast<uint8_t>(std::clamp((value+1)*127.5,0.0,255.0)+0.5);};
        axes[0]=axis(reading.LeftThumbstickX); axes[1]=axis(-reading.LeftThumbstickY);
        axes[2]=axis(reading.RightThumbstickX); axes[3]=axis(-reading.RightThumbstickY);
        axes[4]=static_cast<uint8_t>(std::clamp(reading.LeftTrigger*255,0.0,255.0));
        axes[5]=static_cast<uint8_t>(std::clamp(reading.RightTrigger*255,0.0,255.0));
        if(axes[4]>30) *buttons|=0x100;
        if(axes[5]>30) *buttons|=0x200;
        return 1;
    } catch(winrt::hresult_error const& e) {
        static bool reported=false;
        if(!reported) {std::fprintf(stderr,"Xbox gamepad API: 0x%08x\n",static_cast<unsigned>(e.code()));reported=true;}
        return 0;
    }
}
extern "C" int gow3_uwp_gamepad(uint32_t* buttons,uint8_t axes[6]) {
    return ReadGamepad(buttons,axes);
}
extern "C" int gow3_uwp_dialog_gamepad(uint32_t* buttons,uint8_t axes[6]) {
    return ReadGamepad(buttons,axes);
}
extern "C" void gow3_uwp_vibration(uint8_t large,uint8_t small) {
    try {
        Apartment(); auto pads=winrt::Windows::Gaming::Input::Gamepad::Gamepads();
        if(pads.Size()) pads.GetAt(0).Vibration({large/255.0,small/255.0,0,0});
    } catch(winrt::hresult_error const&) {}
}
