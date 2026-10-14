#pragma once

namespace Gow3Uwp::Overlay {
void Presented();
void Render();
bool Visible();
void Toggle();
bool ControllerChord(bool view, bool menu, double left_trigger, double right_trigger);
}
