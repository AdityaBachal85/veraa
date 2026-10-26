// veraa-render: renders a .veraa scene file to an image from the command line.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>

#include "io_formats.h"
#include "renderer.h"
#include "rendersettings.h"
#include "scene.h"

using namespace veraa;

namespace {

void usage() {
  std::printf(
      "Usage: veraa-render <scene.veraa> -o <out.png> [options]\n"
      "       (other scene formats such as .max, .fbx and .gltf are imported first)\n"
      "\n"
      "Output:\n"
      "  -o, --output FILE      image to write: .png .jpg .tif .bmp .tga (8-bit) or .exr .hdr (float)\n"
      "  --alpha                include the alpha channel (png, tif, exr)\n"
      "  --hdr FILE             also write the linear HDR image (.hdr or .exr)\n"
      "  --elements DIR         also write every enabled render element into DIR\n"
      "  --element NAME         enable a render element (repeatable): Lighting, GI, Specular,\n"
      "                         Reflection, Refraction, SSS, SelfIllumination, Atmosphere, Background,\n"
      "                         DiffuseFilter, Normals, ZDepth, ObjectID, MaterialID, WireColor, Alpha\n"
      "  --all-elements         enable every render element\n"
      "  --light-mix            render Light Mix groups (saved as LightMix_<group> with --elements)\n"
      "\n"
      "Sampling:\n"
      "  --preset NAME          quality preset: draft, medium, high, veryhigh or supreme (applied before\n"
      "                         the other options, which override it)\n"
      "  --width W --height H   image size (default: scene setting)\n"
      "  --samples N            maximum samples per pixel\n"
      "  --min-samples N        samples before adaptive sampling may stop a pixel\n"
      "  --noise T              noise threshold (0 = off)\n"
      "  --time SECONDS         stop after this many seconds\n"
      "  --bucket | --progressive  image sampler\n"
      "  --region X0,Y0,X1,Y1   render only this part of the image (fractions, top-left origin)\n"
      "\n"
      "Scene:\n"
      "  --camera NAME          camera node to render from\n"
      "  --frame F              scene time (frame) to render\n"
      "  --set KEY=VALUE        override any render setting (e.g. --set fogOn=1 --set ev100=14)\n"
      "\n"
      "Display:\n"
      "  --exposure EV          display exposure override\n"
      "  --denoise | --no-denoise\n"
      "  --quiet                no progress output\n");
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

// Applies KEY=VALUE to a parameter block (numbers, booleans, "r,g,b" colors and strings).
bool applySetting(ParamBlock& p, const std::string& kv, std::string& error) {
  size_t eq = kv.find('=');
  if (eq == std::string::npos) {
    error = "expected KEY=VALUE: " + kv;
    return false;
  }
  std::string key = kv.substr(0, eq), value = kv.substr(eq + 1);
  const ParamDef* d = p.def(key);
  if (!d) {
    error = "unknown render setting: " + key;
    return false;
  }
  switch (d->type) {
    case ParamType::Color: {
      Vec3 c;
      char c1, c2;
      std::istringstream in(value);
      if (!(in >> c.x >> c1 >> c.y >> c2 >> c.z)) {
        error = "expected r,g,b for " + key;
        return false;
      }
      p.setColor(key, c);
      return true;
    }
    case ParamType::String:
    case ParamType::File: p.setString(key, value); return true;
    default: {
      if (value == "true" || value == "on") value = "1";
      if (value == "false" || value == "off") value = "0";
      double v;
      if (!parseFloat(value.c_str(), v)) {
        error = "expected a number for " + key;
        return false;
      }
      p.setNumber(key, v);
      return true;
    }
  }
}

std::string extensionOf(const std::string& path) {
  std::string e = std::filesystem::path(path).extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

}  // namespace

int main(int argc, char** argv) {
  std::string scenePath, outPath, hdrPath, cameraName, elementsDir, preset;
  int width = 0, height = 0, samples = 0, minSamples = -1, sampler = -1;
  double timeLimit = -1.0, exposure = 0.0, noise = -1.0, frame = -1e30;
  bool exposureSet = false, quiet = false, alpha = false, allElements = false, lightMix = false;
  int denoise = -1;
  bool region = false;
  float box[4] = {0, 0, 1, 1};
  std::vector<std::string> settings, elements;

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
    } else if (a == "--preset") {
      preset = next("--preset");
      if (renderQualityPresetIndex(preset) < 0) {
        std::fprintf(stderr, "Unknown --preset %s (draft, medium, high, veryhigh, supreme)\n", preset.c_str());
        return 2;
      }
    } else if (a == "--width") {
      if (!parseInt(next("--width"), width) || width <= 0) { std::fprintf(stderr, "Invalid --width\n"); return 2; }
    } else if (a == "--height") {
      if (!parseInt(next("--height"), height) || height <= 0) { std::fprintf(stderr, "Invalid --height\n"); return 2; }
    } else if (a == "--samples") {
      if (!parseInt(next("--samples"), samples) || samples <= 0) { std::fprintf(stderr, "Invalid --samples\n"); return 2; }
    } else if (a == "--min-samples") {
      if (!parseInt(next("--min-samples"), minSamples) || minSamples <= 0) { std::fprintf(stderr, "Invalid --min-samples\n"); return 2; }
    } else if (a == "--noise") {
      if (!parseFloat(next("--noise"), noise) || noise < 0) { std::fprintf(stderr, "Invalid --noise\n"); return 2; }
    } else if (a == "--time") {
      if (!parseFloat(next("--time"), timeLimit) || timeLimit < 0) { std::fprintf(stderr, "Invalid --time\n"); return 2; }
    } else if (a == "--bucket") {
      sampler = 1;
    } else if (a == "--progressive") {
      sampler = 0;
    } else if (a == "--region") {
      std::string r = next("--region");
      char c1, c2, c3;
      std::istringstream in(r);
      if (!(in >> box[0] >> c1 >> box[1] >> c2 >> box[2] >> c3 >> box[3])) { std::fprintf(stderr, "Invalid --region\n"); return 2; }
      region = true;
    } else if (a == "--camera") {
      cameraName = next("--camera");
    } else if (a == "--frame") {
      if (!parseFloat(next("--frame"), frame)) { std::fprintf(stderr, "Invalid --frame\n"); return 2; }
    } else if (a == "--set") {
      settings.push_back(next("--set"));
    } else if (a == "--exposure") {
      if (!parseFloat(next("--exposure"), exposure)) { std::fprintf(stderr, "Invalid --exposure\n"); return 2; }
      exposureSet = true;
    } else if (a == "--no-denoise") {
      denoise = 0;
    } else if (a == "--denoise") {
      denoise = 1;
    } else if (a == "--hdr") {
      hdrPath = next("--hdr");
    } else if (a == "--alpha") {
      alpha = true;
    } else if (a == "--elements") {
      elementsDir = next("--elements");
    } else if (a == "--element") {
      elements.push_back(next("--element"));
    } else if (a == "--all-elements") {
      allElements = true;
    } else if (a == "--light-mix") {
      lightMix = true;
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
  std::string sceneExt = extensionOf(scenePath);
  bool imported = sceneExt != ".veraa" && sceneExt != ".json" && !sceneExt.empty();
  if (imported) {
    // Other formats (.max, .fbx, .gltf, ...) are imported into an empty scene.
    ImportResult res;
    if (!importScene(scene, scenePath, ImportOptions(), &res, &error)) {
      std::fprintf(stderr, "Error: %s\n", error.c_str());
      return 1;
    }
    if (!quiet)
      for (const auto& warn : res.warnings) std::printf("Import: %s\n", warn.c_str());
  } else if (!scene.load(scenePath, &error)) {
    std::fprintf(stderr, "Error: %s\n", error.c_str());
    return 1;
  }
  ParamBlock& rs = scene.renderSettings;
  if (!preset.empty()) applyRenderQualityPreset(rs, renderQualityPresetIndex(preset));
  for (const auto& kv : settings)
    if (!applySetting(rs, kv, error)) {
      std::fprintf(stderr, "Error: %s\n", error.c_str());
      return 2;
    }
  if (frame > -1e29) scene.time.currentFrame = frame;
  if (noise >= 0.0) rs.setNumber("noiseThreshold", noise);
  if (minSamples > 0) rs.setNumber("minSamples", minSamples);
  if (sampler >= 0) rs.setNumber("samplerType", sampler);
  for (const auto& e : renderElementInfos()) {
    bool on = allElements;
    for (const auto& name : elements) on = on || name == e.key;
    if (on) rs.setNumber(std::string("elem") + e.key, 1);
  }
  if (alpha) rs.setNumber("elemAlpha", 1);
  if (lightMix) rs.setNumber("lightMix", 1);

  Renderer renderer;
  int w = width > 0 ? width : rs.getInt("width");
  int h = height > 0 ? height : rs.getInt("height");
  if (width > 0 || height > 0) renderer.setResolution(w, h);
  if (samples > 0) renderer.setMaxSamples(samples);
  if (timeLimit >= 0.0) renderer.setTimeLimit(timeLimit);
  if (region) renderer.setRegion(true, box[0], box[1], box[2], box[3]);

  float aspect = static_cast<float>(w) / static_cast<float>(std::max(h, 1));
  std::string camName = cameraName.empty() ? rs.getString("camera") : cameraName;
  const Node* camNode = nullptr;
  if (!camName.empty()) {
    camNode = scene.findNodeByName(camName);
    if (!camNode || !camNode->camera()) {
      std::fprintf(stderr, "Error: camera \"%s\" not found in the scene\n", camName.c_str());
      return 1;
    }
    rs.setString("camera", camName);
  } else {
    for (const auto& n : scene.nodes)
      if (n->camera()) {
        camNode = n.get();
        break;
      }
  }
  // The view goes first: motion blur needs the shutter while the scene is built.
  renderer.setView(camNode ? viewFromCamera(*camNode, aspect) : defaultViewForScene(scene, aspect));
  if (!renderer.setScene(scene, &error)) {
    std::fprintf(stderr, "Error: %s\n", error.c_str());
    return 1;
  }
  for (const auto& warn : renderer.warnings()) std::fprintf(stderr, "Warning: %s\n", warn.c_str());

  if (!quiet)
    std::printf("Rendering %s (%dx%d, %s, %s sampler)\n", scenePath.c_str(), renderer.width(), renderer.height(),
                camNode ? ("camera " + camNode->name).c_str() : "default view", rs.getInt("samplerType") == 1 ? "bucket" : "progressive");

  renderer.start();
  int lastPrinted = -1;
  while (renderer.isRunning()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    RenderStats s = renderer.stats();
    if (!quiet && s.passes != lastPrinted) {
      std::printf("\r  %d / %d samples  %.1fs  %3.0f%% converged", s.passes, s.targetPasses, s.seconds, s.converged * 100.0f);
      std::fflush(stdout);
      lastPrinted = s.passes;
    }
  }
  RenderStats s = renderer.stats();
  if (!quiet)
    std::printf("\r  %d samples per pixel (max) in %.2fs, %.1f M samples/s, %.0f%% converged (%zu triangles)\n", s.passes,
                s.seconds, s.samplesPerSecond * 1e-6, s.converged * 100.0f, s.triangles);

  DisplaySettings display = displaySettingsFromScene(scene);
  if (exposureSet) display.exposureEV = static_cast<float>(exposure);
  if (denoise >= 0) display.denoise = denoise == 1;
  if (display.denoise && !Renderer::denoiserAvailable() && !quiet)
    std::printf("  Denoiser not available in this build; saving the raw image\n");

  auto t0 = std::chrono::steady_clock::now();
  std::vector<uint8_t> rgba;
  renderer.resolve(display, rgba);
  if (!quiet && display.denoise && Renderer::denoiserAvailable()) {
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  Denoised in %.0f ms\n", ms);
  }
  std::vector<float> finalHdr, alphaBuf;
  renderer.resolveFinalHdr(display, finalHdr);
  renderer.resolveAlpha(alphaBuf);
  if (!saveImage(outPath, renderer.width(), renderer.height(), rgba, &finalHdr, &alphaBuf, alpha, &error)) {
    std::fprintf(stderr, "Error: %s\n", error.c_str());
    return 1;
  }
  if (!hdrPath.empty()) {
    std::vector<float> rgb;
    renderer.resolveHdr(rgb);
    if (!saveImage(hdrPath, renderer.width(), renderer.height(), rgba, &rgb, &alphaBuf, alpha, &error)) {
      std::fprintf(stderr, "Error: %s\n", error.c_str());
      return 1;
    }
  }
  if (!elementsDir.empty()) {
    std::string ext = extensionOf(outPath) == ".exr" ? ".exr" : ".png";
    std::vector<std::string> saved;
    error.clear();
    saveRenderElements(renderer, display, elementsDir, "", {ext}, &saved, &error);
    if (!error.empty()) std::fprintf(stderr, "Error: %s\n", error.c_str());
    if (!quiet)
      for (const auto& path : saved) std::printf("  Element %s\n", path.c_str());
  }
  if (!quiet) std::printf("Saved %s\n", outPath.c_str());
  return 0;
}
