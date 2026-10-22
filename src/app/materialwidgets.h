// Editing of material trees: a path addresses a sub-material or a texture map inside a scene
// material; LevelEditor edits one level (parameters, map slots, sub-materials) with undo.
#pragma once

#include <QDialog>
#include <QImage>
#include <QWidget>

#include <memory>
#include <string>
#include <vector>

#include "document.h"

class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace veraa {

class ParamForm;

struct MtlPathStep {
  enum class Kind { Sub, Map };
  Kind kind = Kind::Sub;
  int sub = 0;      // sub-material slot index (Kind::Sub)
  std::string key;  // map slot key (Kind::Map)
  bool operator==(const MtlPathStep& o) const { return kind == o.kind && sub == o.sub && key == o.key; }
};
using MtlPath = std::vector<MtlPathStep>;

// The material or map a path points to (one of the two is set; both null when invalid).
struct MtlTarget {
  Material* material = nullptr;
  TexMap* map = nullptr;
  // The slot holding the map (for amount/enable) when map is set.
  MapSlot* slot = nullptr;
  bool valid() const { return material || map; }
};

MtlTarget resolveMtlPath(Material* root, const MtlPath& path);
// "Wood > Coat 1 > Diffuse: Bitmap".
QString describeMtlPath(const Material* root, const MtlPath& path);
QString mapButtonText(const TexMap* map);
QString materialButtonText(const Material* m);
// Short labels for map slot keys (diffuse -> "Diffuse", reflectGlossiness -> "Reflection glossiness").
QString slotLabel(const ParamBlock* params, const std::string& key);

// Material / map browser. Returns the chosen type, or an empty string when cancelled.
QString browseMapType(QWidget* parent, const QString& current = QString());
QString browseMaterialType(QWidget* parent, const QString& current = QString());

// Clipboard shared by the material editors.
struct MaterialClipboard {
  std::unique_ptr<TexMap> map;
  std::unique_ptr<Material> material;
};
MaterialClipboard& materialClipboard();

// Prepares a material copy for previews: resolves file paths against the scene folder.
Material previewCopy(const Material& m, const Scene& scene);
TexMap previewCopy(const TexMap& m, const Scene& scene);

class LevelEditor : public QWidget {
  Q_OBJECT
 public:
  LevelEditor(SceneDocument* doc, QWidget* parent = nullptr);

  // Edits the level at path inside scene material materialId (0 clears the editor).
  void setTarget(uint32_t materialId, const MtlPath& path);
  uint32_t materialId() const { return materialId_; }
  const MtlPath& path() const { return path_; }
  MtlTarget target() const;
  // Re-resolves the target after the scene changed (undo, load); rebuilds when needed.
  void sync();

 Q_SIGNALS:
  void navigateRequested(const MtlPath& path);
  void edited();  // any value or structure change (previews)

 private:
  void rebuild();
  void buildMaps(QVBoxLayout* into, const MtlTarget& t);
  void buildSubs(QVBoxLayout* into, Material* m);
  void structural(const QString& label, const std::function<void(MtlTarget&)>& fn);
  void assignMap(const std::string& key, std::unique_ptr<TexMap> map);
  MapSlots* slotsOf(const MtlTarget& t) const;

  SceneDocument* doc_;
  uint32_t materialId_ = 0;
  MtlPath path_;
  const void* builtFor_ = nullptr;  // target object the widgets were built for
  QVBoxLayout* layout_;
  QWidget* content_ = nullptr;
  ParamForm* form_ = nullptr;
};

}  // namespace veraa
