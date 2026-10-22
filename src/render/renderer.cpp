// TEMPORARY STUB so the app can link while the real path tracer is written.
#include "renderer.h"

namespace veraa {

struct Renderer::Impl {
  int width = 640, height = 360;
};

RenderView viewFromCamera(const Node&, float) { return RenderView(); }
RenderView defaultViewForScene(const Scene&, float) { return RenderView(); }
DisplaySettings displaySettingsFromScene(const Scene&) { return DisplaySettings(); }
Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
bool Renderer::setScene(const Scene&, std::string*) { return true; }
std::vector<std::string> Renderer::warnings() const { return {}; }
void Renderer::setView(const RenderView&) {}
void Renderer::setResolution(int w, int h) { if (w > 0) impl_->width = w; if (h > 0) impl_->height = h; }
void Renderer::setMaxSamples(int) {}
void Renderer::setTimeLimit(double) {}
void Renderer::start() {}
void Renderer::stop() {}
bool Renderer::isRunning() const { return false; }
void Renderer::renderBlocking() {}
RenderStats Renderer::stats() const { return RenderStats(); }
int Renderer::width() const { return impl_->width; }
int Renderer::height() const { return impl_->height; }
void Renderer::resolve(const DisplaySettings&, std::vector<uint8_t>& rgba) const {
  rgba.assign(static_cast<size_t>(impl_->width) * impl_->height * 4, 64);
}
void Renderer::resolveHdr(std::vector<float>& rgb) const { rgb.assign(static_cast<size_t>(impl_->width) * impl_->height * 3, 0.25f); }
bool Renderer::denoiserAvailable() { return false; }
std::vector<uint8_t> renderMaterialPreview(const Material&, const Scene*, int size, int) {
  return std::vector<uint8_t>(static_cast<size_t>(size) * size * 4, 128);
}
bool writePng(const std::string&, int, int, const std::vector<uint8_t>&) { return false; }
bool writeHdr(const std::string&, int, int, const std::vector<float>&) { return false; }

}  // namespace veraa
