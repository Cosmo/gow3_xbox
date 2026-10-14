#pragma once
namespace Vulkan {class Instance;}

namespace Gow3Uwp::Overlay {
void Presented();
void Render(const Vulkan::Instance& instance);
bool Visible();
void Toggle();
bool ControllerChord(bool view, bool menu, double left_trigger, double right_trigger);
}
