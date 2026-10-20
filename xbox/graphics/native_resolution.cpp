#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "spartanport_settings.h"

namespace Vulkan {
TemporalUpscaler::TemporalUpscaler(const Instance& i, Scheduler& s, VideoCore::TextureCache& t,
                                 Runtime& r, CameraMotion& c, SceneTargets& targets)
    : instance(i), scheduler(s), texture_cache(t), runtime(r), camera_motion(c), scene_targets(targets) {
    SpartanSettings::ConfigureUpscalerSupport(false, false);
}
TemporalUpscaler::~TemporalUpscaler() = default;
void TemporalUpscaler::OnSceneColor(VideoCore::ImageId) {}
void TemporalUpscaler::OnBlendedSceneDraw() {}
void TemporalUpscaler::OnSceneComposite() {}
void TemporalUpscaler::OnDispatch(u64) {}
bool TemporalUpscaler::OnFrameStart() { return false; }
bool TemporalUpscaler::RasterScaling() const { return false; }
float TemporalUpscaler::SceneMipBias() const { return 0; }
void TemporalUpscaler::OnDraw(u64, VideoCore::ImageId, VideoCore::ImageId, bool) {}
void TemporalUpscaler::OnColorTarget(VideoCore::ImageId) {}
bool TemporalUpscaler::RedirectColor(VideoCore::ImageId, const VideoCore::ImageViewInfo&, Target&) { return false; }
bool TemporalUpscaler::RedirectDepth(VideoCore::ImageId, Target&) { return false; }
bool TemporalUpscaler::RedirectSampled(VideoCore::ImageId, const VideoCore::ImageViewInfo&,
                                     vk::ImageView&, vk::ImageLayout&) { return false; }
bool TemporalUpscaler::DisplayOverride(VAddr, Display&) { return false; }
struct Fsr4Upscaler::Impl {};
Fsr4Upscaler::Fsr4Upscaler(const Instance&, Scheduler&) {}
Fsr4Upscaler::~Fsr4Upscaler() = default;
bool Fsr4Upscaler::Record(const Frame&) { return false; }
const char* Fsr4Upscaler::Problem() const noexcept { return "UWP currently uses native resolution"; }
bool Fsr4Upscaler::Fatal() const noexcept { return true; }
}
