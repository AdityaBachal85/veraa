// veraa-render: renders a .veraa scene file to an image from the command line.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "renderer.h"
#include "scene.h"

using namespace veraa;

namespace {

void usage() {
  std::printf(
      "Usage: veraa-render <scene.veraa> -o <out.png> [options]\n"
      "\n"
      "Options:\n"
      "  -o, --output FILE   PNG file to write (required)\n"
      "  --width W           image width (default: scene setting)\n"
      "  --height H          image height (default: scene setting)\n"
      "  --samples N         samples per pixel (default: scene setting)\n"
      "  --time SECONDS      stop after this many seconds\n"
      "  --camera NAME       camera node to render from\n"
      "  --exposure EV       exposure override\n"
      "  --no-denoise        skip the denoiser\n"
      "  --hdr FILE          also write the linear HDR image (.hdr)\n"
      "  --quiet             no progress output\n");
}

bool parseInt(const char* s, int& out) {
  char* end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (!end || *end != '\0') return false;
  out = static_cast<int>(v);
  return true;
}

bool parseFloat(const char* s, double& out) {
  char* end = nullptr;
  double v = std::strtod(s, &end);
  if (!end || *end != '\0') return false;
  out = v;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string scenePath, outPath, hdrPath, cameraName;
  int width = 0, height = 0, samples = 0;
  double timeLimit = -1.0, exposure = 0.0;
  bool exposureSet = false, noDenoise = false, quiet = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for %s\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (a == "-o" || a == "--output") {
      outPath = next("--output");
    } else if (a == "--width") {
      if (!parseInt(next("--width"), width) || width <= 0) { std::fprintf(stderr, "Invalid --width\n"); return 2; }
    } else if (a == "--height") {
      if (!parseInt(next("--height"), height) || height <= 0) { std::fprintf(stderr, "Invalid --height\n"); return 2; }
    } else if (a == "--samples") {
      if (!parseInt(next("--samples"), samples) || samples <= 0) { std::fprintf(stderr, "Invalid --samples\n"); return 2; }
    } else if (a == "--time") {
      if (!parseFloat(next("--time"), timeLimit) || timeLimit < 0) { std::fprintf(stderr, "Invalid --time\n"); return 2; }
    } else if (a == "--camera") {
      cameraName = next("--camera");
    } else if (a == "--exposure") {
      if (!parseFloat(next("--exposure"), exposure)) { std::fprintf(stderr, "Invalid --exposure\n"); return 2; }
      exposureSet = true;
    } else if (a == "--no-denoise") {
      noDenoise = true;
    } else if (a == "--hdr") {
      hdrPath = next("--hdr");
    } else if (a == "--quiet" || a == "-q") {
      quiet = true;
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "Unknown option %s\n", a.c_str());
      usage();
      return 2;
    } else if (scenePath.empty()) {
      scenePath = a;
    } else {
      std::fprintf(stderr, "Unexpected argument %s\n", a.c_str());
      return 2;
    }
  }
  if (scenePath.empty() || outPath.empty()) {
    usage();
    return 2;
  }

  Scene scene;
  std::string error;
  if (!scene.load(scenePath, &error)) {
    std::fprintf(stderr, "Error: %s\n", error.c_str());
    return 1;
  }

  Renderer renderer;
  if (width > 0 || height > 0) {
    int w = width > 0 ? width : scene.renderSettings.getInt("width");
    int h = height > 0 ? height : scene.renderSettings.getInt("height");
    renderer.setResolution(w, h);
  }
  if (samples > 0) renderer.setMaxSamples(samples);
  if (timeLimit >= 0.0) renderer.setTimeLimit(timeLimit);

  if (!renderer.setScene(scene, &error)) {
    std::fprintf(stderr, "Error: %s\n", error.c_str());
    return 1;
  }
  for (const auto& w : renderer.warnings()) std::fprintf(stderr, "Warning: %s\n", w.c_str());

  int w = renderer.width(), h = renderer.height();
  float aspect = static_cast<float>(w) / static_cast<float>(h);
  std::string camName = cameraName.empty() ? scene.renderSettings.getString("camera") : cameraName;
  const Node* camNode = nullptr;
  if (!camName.empty()) {
    camNode = scene.findNodeByName(camName);
    if (!camNode || !camNode->camera()) {
      std::fprintf(stderr, "Error: camera \"%s\" not found in the scene\n", camName.c_str());
      return 1;
    }
  } else {
    for (const auto& n : scene.nodes)
      if (n->camera()) {
        camNode = n.get();
        break;
      }
  }
  renderer.setView(camNode ? viewFromCamera(*camNode, aspect) : defaultViewForScene(scene, aspect));

  if (!quiet)
    std::printf("Rendering %s (%dx%d, %s)\n", scenePath.c_str(), w, h,
                camNode ? ("camera " + camNode->name).c_str() : "default view");

  renderer.start();
  int lastPrinted = -1;
  while (renderer.isRunning()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    RenderStats s = renderer.stats();
    if (!quiet && s.passes != lastPrinted) {
      std::printf("\r  %d / %d samples  %.1fs", s.passes, s.targetPasses, s.seconds);
      std::fflush(stdout);
      lastPrinted = s.passes;
    }
  }
  RenderStats s = renderer.stats();
  if (!quiet)
    std::printf("\r  %d samples per pixel in %.2fs (%zu triangles)\n", s.passes, s.seconds, s.triangles);

  DisplaySettings display = displaySettingsFromScene(scene);
  if (exposureSet) display.exposureEV = static_cast<float>(exposure);
  if (noDenoise) display.denoise = false;
  if (display.denoise && !Renderer::denoiserAvailable() && !quiet)
    std::printf("  Denoiser not available in this build; saving the raw image\n");

  auto t0 = std::chrono::steady_clock::now();
  std::vector<uint8_t> rgba;
  renderer.resolve(display, rgba);
  if (!quiet && display.denoise && Renderer::denoiserAvailable()) {
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  Denoised in %.0f ms\n", ms);
  }
  if (!writePng(outPath, renderer.width(), renderer.height(), rgba)) {
    std::fprintf(stderr, "Error: cannot write %s\n", outPath.c_str());
    return 1;
  }
  if (!hdrPath.empty()) {
    std::vector<float> rgb;
    renderer.resolveHdr(rgb);
    if (!writeHdr(hdrPath, renderer.width(), renderer.height(), rgb)) {
      std::fprintf(stderr, "Error: cannot write %s\n", hdrPath.c_str());
      return 1;
    }
  }
  if (!quiet) std::printf("Saved %s\n", outPath.c_str());
  return 0;
}
