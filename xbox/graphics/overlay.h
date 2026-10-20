#pragma once
namespace Vulkan {class Instance;}

// The Xbox app's performance log: with SPARTAN_UWP_METRICS=1 a "UWP metrics" line every 5 s (FPS, frame time,
// CPU, the app's memory and its budget, GPU memory). The on-screen overlay is SpartanOverlay's.
namespace Gow3Uwp::Overlay {
void Presented();
// Once per overlay frame (SpartanOverlay::Render).
void Render(const Vulkan::Instance& instance);
// Whether the log needs the overlay to render every frame.
bool Visible();
}
