#pragma once
#include <memory>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Gow3Uwp {
class Presenter {
public:
    Presenter(vk::PhysicalDevice physical, vk::Device device, vk::Queue queue, u32 family,
              u32 width, u32 height);
    ~Presenter();
    vk::Image Image() const;
    vk::ImageView View() const;
    vk::Semaphore Acquired() const;
    vk::Semaphore Ready() const;
    bool Acquire();
    bool Present(bool vsync);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
int GraphicsProbe();
int TextureProbe();
void InitializeDriver();
}
