#include "photometry.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace veraa {

// ---------------------------------------------------------------------------------------------
// Color temperature

namespace {

// Piecewise Gaussian fits of the CIE 1931 color matching functions (Wyman, Sloan & Shirley 2013).
float lobe(float l, float mu, float s1, float s2) {
  float t = (l - mu) / (l < mu ? s1 : s2);
  return std::exp(-0.5f * t * t);
}

Vec3 cieXyz(float l) {
  float x = 1.056f * lobe(l, 599.8f, 37.9f, 31.0f) + 0.362f * lobe(l, 442.0f, 16.0f, 26.7f) -
            0.065f * lobe(l, 501.1f, 20.4f, 26.2f);
  float y = 0.821f * lobe(l, 568.8f, 46.9f, 40.5f) + 0.286f * lobe(l, 530.9f, 16.3f, 31.1f);
  float z = 1.217f * lobe(l, 437.0f, 11.8f, 36.0f) + 0.681f * lobe(l, 459.0f, 26.0f, 13.8f);
  return {x, y, z};
}

Vec3 blackbodyLinearSrgb(float kelvin) {
  const double h = 6.62607015e-34, c = 2.99792458e8, k = 1.380649e-23;
  Vec3 xyz(0.0f);
  for (int nm = 380; nm <= 780; nm += 5) {
    double l = nm * 1e-9;
    double b = (2.0 * h * c * c) / (std::pow(l, 5.0) * (std::exp(h * c / (l * k * kelvin)) - 1.0));
    xyz += cieXyz(static_cast<float>(nm)) * static_cast<float>(b * 1e-13);
  }
  if (xyz.y <= 0.0f) return Vec3(0.0f);
  xyz /= xyz.y;
  return {3.2406f * xyz.x - 1.5372f * xyz.y - 0.4986f * xyz.z, -0.9689f * xyz.x + 1.8758f * xyz.y + 0.0415f * xyz.z,
          0.0557f * xyz.x - 0.2040f * xyz.y + 1.0570f * xyz.z};
}

}  // namespace

Vec3 kelvinToRgb(float kelvin) {
  kelvin = std::min(std::max(kelvin, 1000.0f), 40000.0f);
  static const Vec3 white = blackbodyLinearSrgb(6500.0f);
  Vec3 c = blackbodyLinearSrgb(kelvin);
  c = Vec3(c.x / white.x, c.y / white.y, c.z / white.z);
  c = Vec3(std::max(c.x, 0.0f), std::max(c.y, 0.0f), std::max(c.z, 0.0f));
  // Keep the luminance at 1 so temperature only changes the hue.
  float lum = luminance(c);
  return lum > 0.0f ? c / lum : Vec3(1.0f);
}

// ---------------------------------------------------------------------------------------------
// IES profiles

bool IesProfile::load(const std::string& path, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = "Cannot open IES file " + path;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return parse(ss.str(), error);
}

bool IesProfile::parse(const std::string& text, std::string* error) {
  *this = IesProfile();
  auto fail = [&](const std::string& msg) {
    if (error) *error = "Invalid IES file: " + msg;
    *this = IesProfile();
    return false;
  };
  size_t pos = text.find("TILT=");
  if (pos == std::string::npos) return fail("missing TILT line");
  size_t eol = text.find_first_of("\r\n", pos);
  std::string tilt = text.substr(pos + 5, eol == std::string::npos ? std::string::npos : eol - pos - 5);
  std::string rest = eol == std::string::npos ? std::string() : text.substr(eol);
  for (char& ch : rest)
    if (ch == ',') ch = ' ';
  std::istringstream in(rest);
  auto num = [&](double& v) { return static_cast<bool>(in >> v); };
  double v = 0.0;
  if (tilt.rfind("INCLUDE", 0) == 0) {
    // Lamp-to-luminaire geometry, pair count, angles and multiplying factors: not needed.
    double geometry = 0, pairs = 0;
    if (!num(geometry) || !num(pairs) || pairs < 0 || pairs > 1e5) return fail("bad TILT data");
    for (int i = 0; i < static_cast<int>(pairs) * 2; ++i)
      if (!num(v)) return fail("bad TILT data");
  }
  double lamps, lumens, mult, nv, nh, photType, unitsType, w, l, hgt, ballast, future, watts;
  if (!num(lamps) || !num(lumens) || !num(mult) || !num(nv) || !num(nh) || !num(photType) || !num(unitsType) ||
      !num(w) || !num(l) || !num(hgt) || !num(ballast) || !num(future) || !num(watts))
    return fail("bad header");
  if (nv < 1 || nh < 1 || nv > 10000 || nh > 10000) return fail("bad angle counts");
  int NV = static_cast<int>(nv), NH = static_cast<int>(nh);
  vAngles.resize(NV);
  hAngles.resize(NH);
  for (auto& a : vAngles) {
    if (!num(v)) return fail("missing vertical angles");
    a = static_cast<float>(v);
  }
  for (auto& a : hAngles) {
    if (!num(v)) return fail("missing horizontal angles");
    a = static_cast<float>(v);
  }
  candela.resize(static_cast<size_t>(NV) * NH);
  float scale = static_cast<float>(mult * (ballast > 0.0 ? ballast : 1.0));
  for (auto& c : candela) {
    if (!num(v)) return fail("missing candela values");
    c = std::max(0.0f, static_cast<float>(v) * scale);
  }
  if (!std::is_sorted(vAngles.begin(), vAngles.end()) || !std::is_sorted(hAngles.begin(), hAngles.end()))
    return fail("angles must increase");
  ratedLumens = static_cast<float>(lumens * lamps);
  float toMeters = unitsType == 1 ? 0.3048f : 1.0f;
  width = static_cast<float>(std::fabs(w)) * toMeters;
  length = static_cast<float>(std::fabs(l)) * toMeters;
  height = static_cast<float>(std::fabs(hgt)) * toMeters;
  maxCandela = *std::max_element(candela.begin(), candela.end());
  // Type A/B photometry (rare, mostly automotive/floodlights) is read as type C.
  return true;
}

namespace {

// Interpolation position of x in a sorted array: index i and fraction t with a[i] <= x <= a[i+1].
bool bracket(const std::vector<float>& a, float x, int& i, float& t) {
  int n = static_cast<int>(a.size());
  if (n == 1) {
    i = 0;
    t = 0.0f;
    return true;
  }
  if (x < a.front() - 1e-4f || x > a.back() + 1e-4f) return false;
  auto it = std::upper_bound(a.begin(), a.end(), x);
  i = std::min(std::max(static_cast<int>(it - a.begin()) - 1, 0), n - 2);
  float d = a[i + 1] - a[i];
  t = d > 0.0f ? std::min(std::max((x - a[i]) / d, 0.0f), 1.0f) : 0.0f;
  return true;
}

}  // namespace

float IesProfile::intensity(float vDeg, float hDeg) const {
  if (!valid()) return 0.0f;
  int NV = static_cast<int>(vAngles.size()), NH = static_cast<int>(hAngles.size());
  vDeg = std::min(std::max(vDeg, 0.0f), 180.0f);
  // Fold the horizontal angle into the range the file covers (symmetries of type C photometry).
  float h = std::fmod(hDeg, 360.0f);
  if (h < 0.0f) h += 360.0f;
  float last = hAngles.back();
  if (NH == 1) {
    h = hAngles[0];
  } else if (last <= 90.0f + 1e-3f) {
    if (h > 270.0f) h = 360.0f - h;
    else if (h > 180.0f) h = h - 180.0f;
    else if (h > 90.0f) h = 180.0f - h;
  } else if (last <= 180.0f + 1e-3f) {
    if (h > 180.0f) h = 360.0f - h;
  }
  int vi, hi;
  float vt, ht;
  if (NV > 1) {
    if (!bracket(vAngles, vDeg, vi, vt)) return 0.0f;
  } else {
    vi = 0;
    vt = 0.0f;
  }
  if (NH > 1) {
    if (!bracket(hAngles, h, hi, ht)) {
      // Full 360° data that stops short of 360: wrap between the last and first angle.
      hi = NH - 1;
      ht = 0.0f;
    }
  } else {
    hi = 0;
    ht = 0.0f;
  }
  auto at = [&](int hh, int vv) {
    hh = std::min(hh, NH - 1);
    vv = std::min(vv, NV - 1);
    return candela[static_cast<size_t>(hh) * NV + vv];
  };
  float c00 = at(hi, vi), c01 = at(hi, vi + 1), c10 = at(hi + 1, vi), c11 = at(hi + 1, vi + 1);
  float a = c00 + (c01 - c00) * vt, b = c10 + (c11 - c10) * vt;
  return a + (b - a) * ht;
}

float IesProfile::totalFlux() const {
  if (!valid()) return 0.0f;
  const int nT = 360, nP = 360;
  double sum = 0.0;
  double dT = kPi / nT, dP = 2.0 * kPi / nP;
  for (int i = 0; i < nT; ++i) {
    double t = (i + 0.5) * dT;
    double row = 0.0;
    for (int j = 0; j < nP; ++j) row += intensity(static_cast<float>(t * 180.0 / kPi), static_cast<float>((j + 0.5) * 360.0 / nP));
    sum += row * std::sin(t) * dT * dP;
  }
  return static_cast<float>(sum);
}

std::string makeIesText(const std::vector<float>& vAngles, const std::vector<float>& candela,
                        const std::string& description) {
  std::ostringstream o;
  o << "IESNA:LM-63-2002\r\n[TEST] " << description << "\r\n[MANUFAC] Veraa sample\r\nTILT=NONE\r\n";
  o << "1 -1 1 " << vAngles.size() << " 1 1 2 0 0 0\r\n1 1 0\r\n";
  for (size_t i = 0; i < vAngles.size(); ++i) o << vAngles[i] << (i + 1 < vAngles.size() ? " " : "\r\n");
  o << "0\r\n";
  for (size_t i = 0; i < candela.size(); ++i) o << candela[i] << (i + 1 < candela.size() ? " " : "\r\n");
  return o.str();
}

// ---------------------------------------------------------------------------------------------
// Solar position (NOAA General Solar Position spreadsheet formulas)

namespace {
double rad(double d) { return d * kPi / 180.0; }
double deg(double r) { return r * 180.0 / kPi; }
double wrap360(double a) {
  a = std::fmod(a, 360.0);
  return a < 0.0 ? a + 360.0 : a;
}
}  // namespace

SolarPosition solarPosition(const SolarInput& in) {
  double tz = in.timezone + (in.daylightSaving ? 1.0 : 0.0);
  double utcHours = in.hour - tz;
  // Julian day (Meeus), Gregorian calendar.
  int y = in.year, m = in.month;
  if (m <= 2) {
    y -= 1;
    m += 12;
  }
  int A = y / 100;
  int B = 2 - A + A / 4;
  double jd = std::floor(365.25 * (y + 4716)) + std::floor(30.6001 * (m + 1)) + in.day + B - 1524.5 + utcHours / 24.0;
  double jc = (jd - 2451545.0) / 36525.0;

  double L0 = wrap360(280.46646 + jc * (36000.76983 + jc * 0.0003032));
  double M = 357.52911 + jc * (35999.05029 - 0.0001537 * jc);
  double e = 0.016708634 - jc * (0.000042037 + 0.0000001267 * jc);
  double C = std::sin(rad(M)) * (1.914602 - jc * (0.004817 + 0.000014 * jc)) + std::sin(rad(2 * M)) * (0.019993 - 0.000101 * jc) +
             std::sin(rad(3 * M)) * 0.000289;
  double trueLong = L0 + C;
  double omega = 125.04 - 1934.136 * jc;
  double appLong = trueLong - 0.00569 - 0.00478 * std::sin(rad(omega));
  double eps0 = 23.0 + (26.0 + (21.448 - jc * (46.815 + jc * (0.00059 - jc * 0.001813))) / 60.0) / 60.0;
  double eps = eps0 + 0.00256 * std::cos(rad(omega));
  double decl = deg(std::asin(std::sin(rad(eps)) * std::sin(rad(appLong))));
  double yv = std::tan(rad(eps / 2.0));
  yv *= yv;
  double eqTime = 4.0 * deg(yv * std::sin(2 * rad(L0)) - 2 * e * std::sin(rad(M)) +
                            4 * e * yv * std::sin(rad(M)) * std::cos(2 * rad(L0)) - 0.5 * yv * yv * std::sin(4 * rad(L0)) -
                            1.25 * e * e * std::sin(2 * rad(M)));
  double localMinutes = in.hour * 60.0;
  double tst = std::fmod(localMinutes + eqTime + 4.0 * in.longitude - 60.0 * tz, 1440.0);
  if (tst < 0.0) tst += 1440.0;
  double ha = tst / 4.0 < 0.0 ? tst / 4.0 + 180.0 : tst / 4.0 - 180.0;
  double lat = rad(in.latitude), dr = rad(decl);
  double cosZ = std::sin(lat) * std::sin(dr) + std::cos(lat) * std::cos(dr) * std::cos(rad(ha));
  cosZ = std::min(std::max(cosZ, -1.0), 1.0);
  double zen = deg(std::acos(cosZ));
  double az;
  double denom = std::cos(lat) * std::sin(rad(zen));
  if (std::fabs(denom) < 1e-9) {
    az = 180.0;
  } else {
    double c = (std::sin(lat) * std::cos(rad(zen)) - std::sin(dr)) / denom;
    c = std::min(std::max(c, -1.0), 1.0);
    double a = deg(std::acos(c));
    az = ha > 0.0 ? wrap360(a + 180.0) : wrap360(540.0 - a);
  }
  SolarPosition p;
  p.azimuthDeg = static_cast<float>(az);
  p.elevationDeg = static_cast<float>(90.0 - zen);
  p.declinationDeg = static_cast<float>(decl);
  p.equationOfTimeMin = static_cast<float>(eqTime);
  return p;
}

Vec3 solarDirection(float azimuthDeg, float elevationDeg, float northDeg) {
  float n = radians(northDeg);
  Vec3 north(-std::sin(n), std::cos(n), 0.0f);
  Vec3 east(std::cos(n), std::sin(n), 0.0f);
  float az = radians(azimuthDeg), el = radians(elevationDeg);
  return normalize(east * (std::sin(az) * std::cos(el)) + north * (std::cos(az) * std::cos(el)) +
                   Vec3(0, 0, std::sin(el)));
}

// ---------------------------------------------------------------------------------------------
// Units and exposure

float luminanceFromUnits(LightUnits units, float value, float areaM2, int sides) {
  float a = std::max(areaM2, 1e-12f) * static_cast<float>(std::max(sides, 1));
  switch (units) {
    case LightUnits::Image:
    case LightUnits::Luminance: return value;
    case LightUnits::Lumens: return value / (kPi * a);
    case LightUnits::Watts: return value * kLumensPerWatt / (kPi * a);
    case LightUnits::Radiance: return value * kLumensPerWatt;
  }
  return value;
}

float ev100(float fNumber, float shutterSeconds, float iso) {
  fNumber = std::max(fNumber, 0.1f);
  shutterSeconds = std::max(shutterSeconds, 1e-6f);
  iso = std::max(iso, 1.0f);
  return std::log2(fNumber * fNumber / shutterSeconds) - std::log2(iso / 100.0f);
}

float exposureScale(float ev) { return 1.0f / (1.2f * std::pow(2.0f, ev)); }

}  // namespace veraa
