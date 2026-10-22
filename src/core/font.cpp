// TrueType font discovery and text outlines for Text shapes.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "shapes.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <stb/stb_truetype.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace veraa {

namespace fs = std::filesystem;

namespace {

std::mutex g_fontMutex;

std::vector<std::string>& searchPaths() {
  static std::vector<std::string> paths = [] {
    std::vector<std::string> p = {"assets/fonts", "../assets/fonts", "../../assets/fonts", "../../../assets/fonts"};
#ifdef VERAA_SOURCE_DIR
    p.push_back(std::string(VERAA_SOURCE_DIR) + "/assets/fonts");
#endif
    return p;
  }();
  return paths;
}

std::vector<std::string> systemFontDirs() {
  std::vector<std::string> dirs;
#ifdef _WIN32
  if (const char* windir = std::getenv("WINDIR")) dirs.push_back(std::string(windir) + "/Fonts");
  dirs.push_back("C:/Windows/Fonts");
#elif defined(__APPLE__)
  dirs = {"/System/Library/Fonts", "/Library/Fonts"};
#else
  dirs = {"/usr/share/fonts/truetype", "/usr/local/share/fonts"};
#endif
  return dirs;
}

bool isFontFile(const fs::path& p) {
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == ".ttf";
}

// name -> file path; bundled fonts take precedence over system fonts with the same name.
std::map<std::string, std::string> scanFonts(std::vector<std::string>* order) {
  std::map<std::string, std::string> found;
  auto scan = [&](const std::string& dir, int depth) {
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(dir), ec)) return;
    std::vector<fs::path> files;
    if (depth > 1) {
      for (auto it = fs::recursive_directory_iterator(fs::u8path(dir), ec); it != fs::recursive_directory_iterator();
           it.increment(ec)) {
        if (ec) break;
        if (it.depth() >= depth) it.disable_recursion_pending();
        if (it->is_regular_file(ec) && isFontFile(it->path())) files.push_back(it->path());
      }
    } else {
      for (auto it = fs::directory_iterator(fs::u8path(dir), ec); it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && isFontFile(it->path())) files.push_back(it->path());
      }
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      std::string name = f.stem().u8string();
      if (found.emplace(name, f.u8string()).second && order) order->push_back(name);
    }
  };
  for (const auto& d : searchPaths()) scan(d, 1);
  for (const auto& d : systemFontDirs()) scan(d, 3);
  return found;
}

struct LoadedFont {
  std::vector<unsigned char> data;
  stbtt_fontinfo info{};
  bool ok = false;
};

LoadedFont* loadFont(const std::string& name) {
  static std::map<std::string, std::unique_ptr<LoadedFont>> cache;
  auto it = cache.find(name);
  if (it != cache.end()) return it->second->ok ? it->second.get() : nullptr;
  auto font = std::make_unique<LoadedFont>();
  std::map<std::string, std::string> fonts = scanFonts(nullptr);
  auto f = fonts.find(name);
  if (f == fonts.end()) f = fonts.find(defaultFontName());
  if (f == fonts.end() && !fonts.empty()) f = fonts.begin();
  if (f != fonts.end()) {
    std::ifstream in(fs::u8path(f->second), std::ios::binary);
    font->data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (!font->data.empty()) {
      int offset = stbtt_GetFontOffsetForIndex(font->data.data(), 0);
      font->ok = offset >= 0 && stbtt_InitFont(&font->info, font->data.data(), offset) != 0;
    }
  }
  LoadedFont* raw = font.get();
  cache[name] = std::move(font);
  return raw->ok ? raw : nullptr;
}

std::vector<uint32_t> decodeUtf8(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    int extra = 0;
    if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
    else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
    else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
    ++i;
    for (int k = 0; k < extra && i < s.size(); ++k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    out.push_back(cp);
  }
  return out;
}

// Appends the glyph outline (font units) at pen position (px, py) scaled by scale.
void appendGlyph(const stbtt_fontinfo& info, uint32_t cp, float px, float py, float scale, float shear,
                 BezierShape& out) {
  stbtt_vertex* verts = nullptr;
  int n = stbtt_GetCodepointShape(&info, static_cast<int>(cp), &verts);
  auto P = [&](float x, float y) {
    float X = px + x * scale, Y = py + y * scale;
    return Vec3(X + Y * shear, Y, 0.0f);
  };
  Spline cur;
  auto finish = [&] {
    if (cur.knots.size() >= 2) {
      if (length2(cur.knots.front().p - cur.knots.back().p) < 1e-10f) {
        cur.knots.front().in = cur.knots.back().in;
        cur.knots.pop_back();
      }
      if (cur.knots.size() >= 2) {
        cur.closed = true;
        out.splines.push_back(cur);
      }
    }
    cur = Spline();
  };
  for (int i = 0; i < n; ++i) {
    const stbtt_vertex& v = verts[i];
    Vec3 p = P(v.x, v.y);
    if (v.type == STBTT_vmove) {
      finish();
      cur.knots.push_back(Knot{p, p, p, KnotType::BezierCorner});
    } else if (cur.knots.empty()) {
      cur.knots.push_back(Knot{p, p, p, KnotType::BezierCorner});
    } else if (v.type == STBTT_vline) {
      cur.knots.back().out = cur.knots.back().p;
      cur.knots.push_back(Knot{p, p, p, KnotType::BezierCorner});
    } else if (v.type == STBTT_vcurve) {
      Vec3 p0 = cur.knots.back().p, q = P(v.cx, v.cy);
      cur.knots.back().out = p0 + (q - p0) * (2.0f / 3.0f);
      cur.knots.push_back(Knot{p, p + (q - p) * (2.0f / 3.0f), p, KnotType::BezierCorner});
    } else if (v.type == STBTT_vcubic) {
      cur.knots.back().out = P(v.cx, v.cy);
      cur.knots.push_back(Knot{p, P(v.cx1, v.cy1), p, KnotType::BezierCorner});
    }
  }
  finish();
  if (verts) stbtt_FreeShape(&info, verts);
}

}  // namespace

void addFontSearchPath(const std::string& dir) {
  std::lock_guard<std::mutex> lock(g_fontMutex);
  auto& p = searchPaths();
  if (std::find(p.begin(), p.end(), dir) == p.end()) p.insert(p.begin(), dir);
}

std::vector<std::string> availableFonts() {
  std::lock_guard<std::mutex> lock(g_fontMutex);
  std::vector<std::string> order;
  scanFonts(&order);
  return order;
}

std::string defaultFontName() { return "DejaVuSans"; }

BezierShape textToShape(const std::string& utf8, const std::string& fontName, float size, float kerning, float leading,
                        int align, bool italic, bool underline) {
  std::lock_guard<std::mutex> lock(g_fontMutex);
  BezierShape out;
  LoadedFont* font = loadFont(fontName);
  if (!font || size <= 0.0f) return out;
  const stbtt_fontinfo& info = font->info;
  float scale = stbtt_ScaleForPixelHeight(&info, size);
  int ascent, descent, gap;
  stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
  float lineHeight = (ascent - descent + gap) * scale + leading;
  float shear = italic ? 0.21f : 0.0f;

  // Split into lines and measure them.
  std::vector<std::vector<uint32_t>> lines(1);
  for (uint32_t cp : decodeUtf8(utf8)) {
    if (cp == '\r') continue;
    if (cp == '\n') lines.emplace_back();
    else lines.back().push_back(cp);
  }
  auto advance = [&](uint32_t cp, uint32_t next) {
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&info, static_cast<int>(cp), &adv, &lsb);
    float a = adv * scale;
    if (next) a += stbtt_GetCodepointKernAdvance(&info, static_cast<int>(cp), static_cast<int>(next)) * scale;
    return a;
  };
  std::vector<float> widths;
  float maxWidth = 0.0f;
  for (const auto& line : lines) {
    float w = 0.0f;
    for (size_t i = 0; i < line.size(); ++i) {
      w += advance(line[i], i + 1 < line.size() ? line[i + 1] : 0);
      if (i + 1 < line.size()) w += kerning;
    }
    widths.push_back(w);
    maxWidth = std::max(maxWidth, w);
  }
  for (size_t li = 0; li < lines.size(); ++li) {
    const auto& line = lines[li];
    float x = 0.0f, extra = kerning;
    if (align == 1) x = (maxWidth - widths[li]) * 0.5f;
    else if (align == 2) x = maxWidth - widths[li];
    else if (align == 3 && li + 1 < lines.size() && line.size() > 1)
      extra += (maxWidth - widths[li]) / static_cast<float>(line.size() - 1);
    float y = -static_cast<float>(li) * lineHeight;
    float x0 = x;
    for (size_t i = 0; i < line.size(); ++i) {
      appendGlyph(info, line[i], x, y, scale, shear, out);
      x += advance(line[i], i + 1 < line.size() ? line[i + 1] : 0);
      if (i + 1 < line.size()) x += extra;
    }
    if (underline && x > x0) {
      float uy = y - size * 0.12f, th = size * 0.05f;
      out.splines.push_back(
          makePolylineSpline({{x0 + uy * shear, uy - th}, {x + uy * shear, uy - th}, {x + uy * shear, uy}, {x0 + uy * shear, uy}},
                             true));
    }
  }
  // Pivot at the center of the text block, like Max.
  Aabb b = out.bounds();
  if (b.valid()) {
    Vec3 c = b.center();
    out.transform(Mat4::translate(Vec3(-c.x, -c.y, 0.0f)));
  }
  return out;
}

}  // namespace veraa
