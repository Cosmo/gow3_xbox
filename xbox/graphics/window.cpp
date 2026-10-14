#include "sdl_window.h"
#include <chrono>
#include <thread>

namespace Frontend {
WindowSDL::WindowSDL(s32 w, s32 h, const char* title) : width(w), height(h), base_title(title) {}
WindowSDL::~WindowSDL() = default;
bool WindowSDL::PollEvents() {
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
