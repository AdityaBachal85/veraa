#include "createlayout.h"

#include <QObject>

#include "object.h"

namespace veraa {

namespace {

// One list of the reference layout. `sub` is the registry subcategory whose types it shows
// (empty: a list Veraa does not have yet). Entries:
//   "Box"                  the registered type of that subcategory with this display name
//   "Physical=TypeName"    a button labelled "Physical" creating TypeName
//   "?Path Follow"         a button for a type Veraa does not have yet
struct ListSpec {
  const char* name;
  const char* sub;
  std::vector<const char*> entries;
};

struct CategorySpec {
  const char* category;
  std::vector<ListSpec> lists;
};

// The reference shows V-Ray in some lists; Veraa's own renderer objects take that slot.
const std::vector<CategorySpec>& layoutTable() {
  static const std::vector<CategorySpec> table = {
      {"Geometry",
       {{"Standard Primitives", "Standard Primitives",
         {"Box", "Cone", "Sphere", "GeoSphere", "Cylinder", "Tube", "Torus", "Pyramid", "Teapot", "Plane", "TextPlus"}},
        {"Extended Primitives", "Extended Primitives",
         {"Hedra", "Torus Knot", "Chamfer Box", "ChamferCyl", "OilTank", "Capsule", "Spindle", "L-Ext", "Gengon", "C-Ext",
          "RingWave", "Hose", "Prism"}},
        {"Compound Objects", "Compound Objects",
         {"Scatter", "Conform", "Connect", "BlobMesh", "ShapeMerge", "Terrain", "Loft", "Boolean"}},
        {"Particle Systems", "Particle Systems", {"Spray", "Super Spray", "Snow"}},
        {"Patch Grids", "Patch Grids", {"Quad Patch", "Tri Patch"}},
        {"Body Objects", "", {}},
        {"Doors", "Doors", {"Pivot", "Sliding", "BiFold"}},
        {"NURBS Surfaces", "", {}},
        {"Windows", "Windows", {"Awning", "Casement", "Fixed", "Pivoted", "Projected", "Sliding"}},
        {"AEC Extended", "AEC Extended", {"Foliage", "Railing", "Wall"}},
        {"Point Cloud Objects", "", {}},
        {"Dynamics Objects", "", {}},
        {"Stairs", "Stairs", {"L-Type Stair", "Spiral Stair", "Straight Stair", "U-Type Stair"}},
        {"Veraa", "Veraa", {}},
        {"Alembic", "", {}},
        {"Fluids", "", {}}}},
      {"Shapes",
       {{"Splines", "Splines",
         {"Line", "Rectangle", "Circle", "Ellipse", "Arc", "Donut", "NGon", "Star", "Text", "Helix", "Egg", "Section",
          "Freehand"}},
        {"NURBS Curves", "NURBS Curves", {"Point Curve", "CV Curve"}},
        {"Compound Shapes", "", {}},
        {"Extended Splines", "Extended Splines", {"Walled Rectangle", "Channel", "Angle", "Tee", "Wide Flange"}},
        {"Creation Graph", "", {}}}},
      {"Lights",
       {{"Photometric", "Photometric", {"Target Light=VeraaIES", "Free Light=VeraaIES", "Sun Positioner"}},
        {"Standard", "Standard", {"Spot", "Omni", "Sun", "Plane Light", "Sphere Light"}},
        {"Veraa", "Veraa", {}}}},
      {"Cameras",
       {{"Standard", "Standard", {"Physical=VeraaPhysicalCamera", "Target=Camera", "Free=Camera"}},
        {"Veraa", "Veraa", {}}}},
      {"Helpers",
       {{"Standard", "Standard",
         {"Dummy", "?Container", "?Crowd", "?Delegate", "?ExposeTm", "?Grid", "Point", "Tape", "Protractor", "?Influence",
          "Arrow", "?Volume", "Compass"}},
        {"Atmospheric Apparatus", "", {}},
        {"Camera Match", "", {}},
        {"Assembly Heads", "", {}},
        {"Manipulators", "", {}},
        {"Particle Flow", "", {}},
        {"VRML97", "", {}},
        {"Veraa", "Veraa", {}}}},
      {"Space Warps",
       {{"Forces", "Forces",
         {"Push", "Motor", "Vortex", "Drag", "PBomb", "?Path Follow", "Gravity", "Wind", "?Displace", "?Motion Field"}},
        {"Deflectors", "Deflectors", {"Deflector", "SDeflector", "UDeflector"}},
        {"Geometric/Deformable", "Geometric/Deformable", {"Ripple", "Wave"}},
        {"Modifier-Based", "", {}},
        {"Particles & Dynamics", "", {}}}},
      {"Systems",
       {{"Standard", "Standard", {"Bones", "?Ring Array", "?Biped", "Sunlight=SunLight", "Daylight=SunPositioner"}},
        {"Veraa", "Veraa", {}}}},
  };
  return table;
}

std::string registrySub(const ObjectTypeInfo& t) { return t.subcategory.empty() ? std::string("Standard") : t.subcategory; }

}  // namespace

bool CreateList::available() const {
  for (const CreateButton& b : buttons)
    if (!b.type.isEmpty()) return true;
  return false;
}

const QStringList& createCategories() {
  static const QStringList cats = {"Geometry", "Shapes", "Lights", "Cameras", "Helpers", "Space Warps", "Systems"};
  return cats;
}

std::vector<CreateList> createLists(const QString& category) {
  const std::string cat = category.toStdString();
  std::vector<CreateList> out;
  std::vector<std::string> coveredSubs;
  for (const CategorySpec& c : layoutTable()) {
    if (cat != c.category) continue;
    for (const ListSpec& l : c.lists) {
      CreateList list;
      list.name = QObject::tr(l.name);
      std::vector<const ObjectTypeInfo*> registered;
      if (*l.sub) {
        coveredSubs.push_back(l.sub);
        for (const ObjectTypeInfo* t : objectTypesIn(cat))
          if (registrySub(*t) == l.sub) registered.push_back(t);
      }
      std::vector<const ObjectTypeInfo*> used;
      for (const char* entry : l.entries) {
        QString e = QString::fromUtf8(entry);
        if (e.startsWith('?')) {
          list.buttons.push_back({QObject::tr(entry + 1), QString()});
          continue;
        }
        int eq = e.indexOf('=');
        if (eq > 0) {
          const ObjectTypeInfo* t = findObjectType(e.mid(eq + 1).toStdString());
          list.buttons.push_back({QObject::tr(e.left(eq).toUtf8().constData()), t ? QString::fromStdString(t->type) : QString()});
          if (t) used.push_back(t);
          continue;
        }
        const ObjectTypeInfo* match = nullptr;
        for (const ObjectTypeInfo* t : registered)
          if (QString::fromStdString(t->displayName) == e || QString::fromStdString(t->type) == e) match = t;
        if (match) {
          list.buttons.push_back({QString::fromStdString(match->displayName), QString::fromStdString(match->type)});
          used.push_back(match);
        }
      }
      // Registered types the table does not name (new modules) follow in registry order.
      for (const ObjectTypeInfo* t : registered)
        if (std::find(used.begin(), used.end(), t) == used.end())
          list.buttons.push_back({QString::fromStdString(t->displayName), QString::fromStdString(t->type)});
      // Renderer slots only appear when the renderer registers something there.
      if (std::string(l.name) == "Veraa" && list.buttons.empty()) continue;
      out.push_back(std::move(list));
    }
  }
  // Registry subcategories the table does not know.
  for (const std::string& sub : objectSubcategories(cat)) {
    std::string name = sub.empty() ? std::string("Standard") : sub;
    if (std::find(coveredSubs.begin(), coveredSubs.end(), name) != coveredSubs.end()) continue;
    CreateList list;
    list.name = QString::fromStdString(name);
    for (const ObjectTypeInfo* t : objectTypesIn(cat))
      if (registrySub(*t) == name) list.buttons.push_back({QString::fromStdString(t->displayName), QString::fromStdString(t->type)});
    out.push_back(std::move(list));
  }
  return out;
}

}  // namespace veraa
