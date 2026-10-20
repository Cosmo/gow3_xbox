// Runs the actual UWP window pump with a fake controller and save-dialog consumer.
#include "sdl_window.h"
#include <SDL3/SDL.h>
#include <cstdint>
#include <cstdio>
#include <windows.h>
#include "../xbox/platform/input.h"

static uint32_t buttons;
static bool active;
static int selected,result=-1;
extern "C" uint32_t runtime_pad_dialog_buttons() { return buttons; }
namespace SpartanOverlay {
bool CapturesInput() { return active; }
bool HandleEvent(const SDL_Event& event) {
    if(!active) return false;
    if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        if(event.gbutton.button==SDL_GAMEPAD_BUTTON_DPAD_DOWN) selected=(selected+1)%3;
        if(event.gbutton.button==SDL_GAMEPAD_BUTTON_DPAD_UP) selected=(selected+2)%3;
        if(event.gbutton.button==SDL_GAMEPAD_BUTTON_SOUTH) { result=selected; active=false; }
        if(event.gbutton.button==SDL_GAMEPAD_BUTTON_EAST) { result=-2; active=false; }
    } else if(event.type==SDL_EVENT_KEY_DOWN) {
        if(event.key.key==SDLK_RETURN) { result=selected; active=false; }
        if(event.key.key==SDLK_ESCAPE) { result=-2; active=false; }
    }
    return true;
}
}
#define CHECK(condition) do { if(!(condition)) { std::printf("FAIL line %d: %s\n",__LINE__,#condition); return 1; } } while(0)
int main() {
    Frontend::WindowSDL window(1280,720,"dialog input regression");
    // An A held before the dialog opened must not accept it.
    buttons=0x4000; window.PollEvents(); active=true; window.PollEvents();
    CHECK(active&&result==-1);
    buttons=0; window.PollEvents(); buttons=0x40; window.PollEvents();
    CHECK(selected==1); window.PollEvents(); CHECK(selected==1);
    buttons=0; window.PollEvents(); buttons=0x10; window.PollEvents();
    CHECK(selected==0);
    buttons=0; window.PollEvents(); buttons=0x4000; window.PollEvents();
    CHECK(!active&&result==0);
    // Holding that A cannot accept a subsequent dialog either.
    active=true; result=-1; window.PollEvents(); CHECK(active&&result==-1);
    buttons=0; window.PollEvents(); buttons=0x2000; window.PollEvents();
    CHECK(!active&&result==-2);
    active=true; result=-1;
    CHECK(!gow3_uwp_dialog_key(VK_RETURN,1,1)&&active);
    CHECK(gow3_uwp_dialog_key(VK_RETURN,1,0)&&!active&&result==0);
    active=true; CHECK(gow3_uwp_dialog_key(VK_ESCAPE,1,0)&&!active&&result==-2);
    CHECK(!gow3_uwp_dialog_key(VK_RETURN,1,0));
    std::puts("PASS: UWP save-dialog navigation, confirm/cancel, held-button suppression and keyboard fallback");
    return 0;
}
