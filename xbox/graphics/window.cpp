#include "sdl_window.h"
#include "spartanport_overlay.h"
#include "../platform/input.h"
#include <SDL3/SDL.h>
#include <windows.h>
#include <chrono>
#include <thread>

extern "C" uint32_t runtime_pad_dialog_buttons(void);
extern "C" int gow3_uwp_dialog_key(uint32_t key,int down,int repeat) {
    // Insert opens the menu as on the PC; the other keys only work while it or a dialog is open.
    if((key!=VK_INSERT&&!SpartanOverlay::CapturesInput()) || (down&&repeat)) return 0;
    SDL_Event event{};
    event.type=down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    switch(key) {
    case VK_INSERT: event.key.key=SDLK_INSERT; break;
    case VK_LEFT: event.key.key=SDLK_LEFT; break;
    case VK_RIGHT: event.key.key=SDLK_RIGHT; break;
    case VK_RETURN: event.key.key=SDLK_RETURN; break;
    case VK_SPACE: event.key.key=SDLK_SPACE; break;
    case VK_ESCAPE: event.key.key=SDLK_ESCAPE; break;
    case VK_BACK: event.key.key=SDLK_BACKSPACE; break;
    case VK_UP: event.key.key=SDLK_UP; break;
    case VK_DOWN: event.key.key=SDLK_DOWN; break;
    case 'W': event.key.key=SDLK_W; break;
    case 'S': event.key.key=SDLK_S; break;
    default: return 0;
    }
    return SpartanOverlay::HandleEvent(event);
}

namespace Frontend {
// The Xbox has no mouse to lock in the window.
void ToggleMousePause() {}
WindowSDL::WindowSDL(s32 w, s32 h, const char* title) : width(w), height(h), base_title(title) {}
WindowSDL::~WindowSDL() = default;
bool WindowSDL::PollEvents() {
    // The controller reaches the in-game menu and the dialogs here, as SDL gamepad events do on the PC.
    // Only changes are sent: a held confirm cannot accept a newly opened dialog.
    static thread_local uint32_t previous=0;
    const uint32_t buttons=runtime_pad_dialog_buttons();
    const uint32_t changed=buttons^previous;
    previous=buttons;
    const auto send=[&](uint32_t guest,Uint8 host) {
        SDL_Event event{};
        event.type=buttons&guest ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        event.gbutton.button=host;
        SpartanOverlay::HandleEvent(event);
    };
    // R3 + left trigger opens and closes the menu, so both always reach it.
    if(changed&0x100) {
        SDL_Event event{}; event.type=SDL_EVENT_GAMEPAD_AXIS_MOTION;
        event.gaxis.axis=SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
        event.gaxis.value=buttons&0x100 ? SDL_JOYSTICK_AXIS_MAX : 0;
        SpartanOverlay::HandleEvent(event);
    }
    if(changed&4) send(4,SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    if(SpartanOverlay::CapturesInput()) {
        struct Mapping {uint32_t guest;Uint8 host;};
        constexpr Mapping map[]={
            {0x4000,SDL_GAMEPAD_BUTTON_SOUTH},{0x2000,SDL_GAMEPAD_BUTTON_EAST},
            {0x8000,SDL_GAMEPAD_BUTTON_WEST},{0x1000,SDL_GAMEPAD_BUTTON_NORTH},
            {0x10,SDL_GAMEPAD_BUTTON_DPAD_UP},{0x40,SDL_GAMEPAD_BUTTON_DPAD_DOWN},
            {0x80,SDL_GAMEPAD_BUTTON_DPAD_LEFT},{0x20,SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
            {0x400,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},{0x800,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
            {0x8,SDL_GAMEPAD_BUTTON_START},{0x80000000u,SDL_GAMEPAD_BUTTON_BACK}};
        for(auto const& entry:map) if(changed&entry.guest) {
            send(entry.guest,entry.host);
            // One press closes at most one modal dialog.
            if(!SpartanOverlay::CapturesInput()) break;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return IsOpen();
}
void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock(text_mutex);
    text = initial;
    text_prompt = prompt;
    text_state = 2;
}
int WindowSDL::PollTextInput(std::string& result) {
    std::scoped_lock lock(text_mutex);
    result = text;
    return text_state;
}
}
