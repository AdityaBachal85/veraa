#include "materialeditor.h"

#include <QScrollBar>

#include <QApplication>
#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>

#include "paramform.h"
#include "renderer.h"

namespace veraa {

namespace {
constexpr int kSlotSize = 96;
constexpr uint32_t kLevelPreviewId = 0xFFFFFFF0u;  // preview of a sub-material level

// Same pixmap for normal and selected states (no blue selection tint over the swatch).
QIcon slotIcon(const QImage& img) {
  QPixmap pm = QPixmap::fromImage(img.scaled(kSlotSize, kSlotSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  QIcon icon;
  icon.addPixmap(pm, QIcon::Normal);
  icon.addPixmap(pm, QIcon::Selected);
  return icon;
}

// Sample slots that can be dragged onto objects in a viewport.
class SlotList : public QListWidget {
 public:
  using QListWidget::QListWidget;

 protected:
  QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
    if (items.isEmpty()) return nullptr;
    auto* mime = new QMimeData;
    mime->setData(kMaterialMime, QByteArray::number(items.first()->data(Qt::UserRole).toUInt()));
    return mime;
  }
  QStringList mimeTypes() const override { return {kMaterialMime}; }
};
constexpr int kPreviewSize = 160;
constexpr int kPreviewSamples = 48;

QImage placeholder(int size) {
  QImage img(size, size, QImage::Format_RGB32);
  img.fill(QColor(0x3a, 0x3a, 0x3a));
  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing);
  for (int y = 0; y < size; y += 12)
    for (int x = (y / 12) % 2 * 12; x < size; x += 24) p.fillRect(x, y, 12, 12, QColor(0x46, 0x46, 0x46));
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0x80, 0x80, 0x80));
  p.drawEllipse(QRectF(size * 0.18, size * 0.14, size * 0.64, size * 0.64));
  return img;
}

QImage mapSwatch(const TexMap& map, const Scene& scene, int size) {
  std::vector<uint8_t> rgba = renderMapPreview(previewCopy(map, scene), nullptr, size);
  QImage img(rgba.data(), size, size, size * 4, QImage::Format_RGBA8888);
  return img.copy();
}

// Preset materials: index -> (label, builder).
struct Preset {
  const char* label;
  void (*build)(Material& m);
};

void glossy(Material& m, const Vec3& refl, float gloss) {
  m.params.setColor("reflect", refl);
  m.params.setNumber("reflectGlossiness", gloss);
}

// A metal whose reflectance comes from measured complex IOR data ("metalFresnel" choice).
void measuredMetal(Material& m, int metalFresnel, float gloss) {
  m.params.setColor("diffuse", Vec3(0.9f));
  glossy(m, Vec3(1.0f), gloss);
  m.params.setNumber("metalness", 1.0);
  m.params.setNumber("metalFresnel", metalFresnel);
}

const Preset kPresets[] = {
    {"Default (matte)", [](Material& m) { m.params.setColor("diffuse", Vec3(0.6f)); }},
    {"Glossy plastic",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.55f, 0.08f, 0.06f));
       glossy(m, Vec3(1.0f), 0.88f);
       m.params.setNumber("fresnelIor", 1.5);
     }},
    {"Clear glass",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.0f));
       glossy(m, Vec3(1.0f), 1.0f);
       m.params.setColor("refract", Vec3(1.0f));
       m.params.setNumber("ior", 1.52);
       m.params.setNumber("fresnelIor", 1.52);
     }},
    {"Frosted glass",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.0f));
       glossy(m, Vec3(1.0f), 0.8f);
       m.params.setColor("refract", Vec3(0.95f));
       m.params.setNumber("refractGlossiness", 0.75);
       m.params.setNumber("ior", 1.52);
     }},
    {"Diamond (dispersion)",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.0f));
       glossy(m, Vec3(1.0f), 1.0f);
       m.params.setColor("refract", Vec3(1.0f));
       m.params.setNumber("ior", 2.42);
       m.params.setNumber("lockIor", 1);
       m.params.setNumber("abbe", 55);
     }},
    {"Chrome",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.95f));
       glossy(m, Vec3(1.0f), 0.98f);
       m.params.setNumber("metalness", 1.0);
     }},
    {"Brushed aluminium",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.91f, 0.92f, 0.92f));
       glossy(m, Vec3(1.0f), 0.72f);
       m.params.setNumber("metalness", 1.0);
       m.params.setNumber("anisotropy", 0.7);
     }},
    {"Gold",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(1.0f, 0.77f, 0.34f));
       glossy(m, Vec3(1.0f), 0.9f);
       m.params.setNumber("metalness", 1.0);
     }},
    {"Copper",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.95f, 0.64f, 0.54f));
       glossy(m, Vec3(1.0f), 0.85f);
       m.params.setNumber("metalness", 1.0);
     }},
    // Measured metals: complex IOR reduced from spectral data (metalFresnel index = preset + 2).
    {"Gold (measured n, k)", [](Material& m) { measuredMetal(m, 2, 0.9f); }},
    {"Silver (measured n, k)", [](Material& m) { measuredMetal(m, 3, 0.95f); }},
    {"Copper (measured n, k)", [](Material& m) { measuredMetal(m, 4, 0.88f); }},
    {"Aluminium, brushed (measured n, k)",
     [](Material& m) {
       measuredMetal(m, 5, 0.75f);
       m.params.setNumber("anisotropy", 0.7);
     }},
    {"Chromium (measured n, k)", [](Material& m) { measuredMetal(m, 6, 0.97f); }},
    {"Iron, satin (measured n, k)", [](Material& m) { measuredMetal(m, 7, 0.7f); }},
    {"Brass (measured n, k)", [](Material& m) { measuredMetal(m, 11, 0.86f); }},
    {"Anodized titanium (thin film)",
     [](Material& m) {
       measuredMetal(m, 8, 0.9f);
       m.params.setNumber("thinFilm", 1);
       m.params.setNumber("thinFilmThickness", 280);
       m.params.setNumber("thinFilmIor", 2.3);  // titanium oxide
     }},
    {"Soap bubble (thin film)",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.0f));
       glossy(m, Vec3(1.0f), 1.0f);
       m.params.setColor("refract", Vec3(1.0f));
       m.params.setNumber("ior", 1.33);
       m.params.setNumber("lockIor", 1);
       m.params.setNumber("thinWalled", 1);
       m.params.setNumber("thinFilm", 1);
       m.params.setNumber("thinFilmThickness", 450);
       m.params.setNumber("thinFilmIor", 1.33);
       MapSlot s;
       s.map = createTexMap("Noise");
       s.map->params.setColor("color1", Vec3(0.25f));
       s.map->params.setColor("color2", Vec3(0.75f));
       s.map->params.setNumber("size", 30.0);
       m.maps["thinFilmThickness"] = std::move(s);  // map value scales the thickness
     }},
    {"Pearlescent coat (thin film)",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.82f, 0.8f, 0.78f));
       m.params.setNumber("coatAmount", 1.0);
       m.params.setNumber("coatGlossiness", 0.96);
       m.params.setNumber("thinFilm", 1);
       m.params.setNumber("thinFilmLayer", 1);
       m.params.setNumber("thinFilmThickness", 520);
       m.params.setNumber("thinFilmIor", 1.6);
     }},
    {"High-gloss lacquer",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.62f, 0.58f, 0.53f));
       glossy(m, Vec3(1.0f), 0.94f);
       m.params.setNumber("fresnelIor", 1.5);
     }},
    {"Rubber",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.04f));
       glossy(m, Vec3(0.5f), 0.45f);
     }},
    {"Ceramic (glazed)",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.85f, 0.85f, 0.82f));
       glossy(m, Vec3(1.0f), 0.95f);
       m.params.setNumber("fresnelIor", 1.55);
     }},
    {"Lacquered wood (coat)",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.35f, 0.18f, 0.08f));
       m.params.setNumber("coatAmount", 1.0);
       m.params.setNumber("coatGlossiness", 0.95);
       MapSlot s;
       s.map = createTexMap("Noise");
       s.map->params.setColor("color1", Vec3(0.28f, 0.13f, 0.05f));
       s.map->params.setColor("color2", Vec3(0.45f, 0.24f, 0.1f));
       s.map->params.setNumber("noiseType", 1);
       s.map->params.setNumber("tilingX", 0.15);
       s.map->params.setNumber("size", 4.0);
       m.maps["diffuse"] = std::move(s);
     }},
    {"Velvet (sheen)",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.18f, 0.02f, 0.06f));
       m.params.setColor("sheenColor", Vec3(1.0f, 0.6f, 0.75f));
       m.params.setNumber("sheenGlossiness", 0.6);
     }},
    {"Water",
     [](Material& m) {
       m.params.setColor("diffuse", Vec3(0.0f));
       glossy(m, Vec3(1.0f), 1.0f);
       m.params.setColor("refract", Vec3(1.0f));
       m.params.setNumber("ior", 1.333);
       m.params.setNumber("fresnelIor", 1.333);
       m.params.setColor("fogColor", Vec3(0.75f, 0.92f, 0.95f));
       m.params.setNumber("fogMultiplier", 0.05);
     }},
    {"Brick wall (Tiles map)",
     [](Material& m) {
       MapSlot s;
       s.map = createTexMap("Tiles");
       s.map->params.setNumber("tilesH", 4);
       s.map->params.setNumber("tilesV", 8);
       MapSlot b;
       b.map = std::make_unique<TexMap>(*s.map);
       b.map->params.setColor("tileColor", Vec3(1.0f));
       b.map->params.setColor("groutColor", Vec3(0.0f));
       m.maps["diffuse"] = std::move(s);
       m.maps["bump"] = std::move(b);
       m.params.setNumber("bumpAmount", 2.0);
       m.params.setNumber("roughness", 0.5);
     }},
    {"Car paint (red)", [](Material& m) { m.setMaterialType("VeraaCarPaintMtl"); }},
    {"Skin (subsurface)",
     [](Material& m) {
       m.setMaterialType("VeraaSSSMtl");
       m.params.setColor("sssColor", Vec3(0.85f, 0.55f, 0.45f));
       m.params.setNumber("scatterRadius", 0.8);
     }},
    {"Wax / marble (subsurface)",
     [](Material& m) {
       m.setMaterialType("VeraaSSSMtl");
       m.params.setColor("sssColor", Vec3(0.92f, 0.9f, 0.85f));
       m.params.setColor("radiusColor", Vec3(1.0f, 0.85f, 0.7f));
       m.params.setNumber("scatterRadius", 2.0);
       m.params.setNumber("specularGlossiness", 0.85);
     }},
    {"Leaf (two-sided)",
     [](Material& m) {
       m.setMaterialType("VeraaTwoSidedMtl");
       m.params.setColor("translucency", Vec3(0.45f, 0.55f, 0.25f));
       if (!m.subs.empty() && m.subs[0].material) {
         m.subs[0].material->params.setColor("diffuse", Vec3(0.12f, 0.3f, 0.06f));
         glossy(*m.subs[0].material, Vec3(0.4f), 0.6f);
       }
     }},
    {"Light emitter",
     [](Material& m) {
       m.setMaterialType("VeraaLightMtl");
       m.params.setColor("color", Vec3(1.0f, 0.95f, 0.85f));
       m.params.setNumber("multiplier", 5.0);
     }},
    {"Shadow catcher (matte)",
     [](Material& m) {
       m.setMaterialType("VeraaWrapperMtl");
       m.params.setNumber("matteSurface", 1);
       m.params.setNumber("alphaContribution", -1.0);
     }},
};

}  // namespace

// ---------------------------------------------------------------------------------------------

PreviewRenderer::PreviewRenderer(QObject* parent) : QObject(parent) {
  thread_ = std::thread([this] { run(); });
}

PreviewRenderer::~PreviewRenderer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
    jobs_.clear();
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void PreviewRenderer::request(uint32_t id, const Material& material, int size, int samples) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = jobs_.begin(); it != jobs_.end();)
      it = it->id == id ? jobs_.erase(it) : it + 1;
    jobs_.push_back(Job{id, material, size, samples});
  }
  cv_.notify_one();
}

void PreviewRenderer::run() {
  for (;;) {
    Job job{0, Material(), 0, 0};
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
      if (quit_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    std::vector<uint8_t> rgba = renderMaterialPreview(job.material, nullptr, job.size, job.samples);
    if (rgba.size() != static_cast<size_t>(job.size) * job.size * 4) continue;
    QImage img(rgba.data(), job.size, job.size, job.size * 4, QImage::Format_RGBA8888);
    QImage copy = img.copy();
    uint32_t id = job.id;
    QMetaObject::invokeMethod(this, [this, id, copy] { Q_EMIT ready(id, copy); }, Qt::QueuedConnection);
  }
}

// ---------------------------------------------------------------------------------------------

MaterialEditor::MaterialEditor(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
  setWindowTitle(tr("Material Editor - Veraa"));
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  resize(860, 720);

  auto* root = new QHBoxLayout(this);
  root->setContentsMargins(6, 6, 6, 6);
  auto* split = new QSplitter(Qt::Horizontal, this);
  root->addWidget(split);

  // Left: sample slots and material commands.
  auto* left = new QWidget(split);
  auto* lv = new QVBoxLayout(left);
  lv->setContentsMargins(0, 0, 0, 0);
  slots_ = new SlotList(left);
  slots_->setDragEnabled(true);
  slots_->setDragDropMode(QAbstractItemView::DragOnly);
  slots_->setToolTip(tr("Drag a material onto an object in a viewport to assign it"));
  slots_->setViewMode(QListView::IconMode);
  slots_->setIconSize(QSize(kSlotSize, kSlotSize));
  slots_->setGridSize(QSize(kSlotSize + 18, kSlotSize + 30));
  slots_->setResizeMode(QListView::Adjust);
  slots_->setMovement(QListView::Static);
  slots_->setWordWrap(true);
  slots_->setSpacing(4);
  lv->addWidget(slots_, 1);
  auto* row1 = new QHBoxLayout;
  auto* newBtn = new QPushButton(tr("New"), left);
  newBtn->setToolTip(tr("New material (Shift-click to choose the type)"));
  dup_ = new QPushButton(tr("Duplicate"), left);
  del_ = new QPushButton(tr("Delete"), left);
  row1->addWidget(newBtn);
  row1->addWidget(dup_);
  row1->addWidget(del_);
  lv->addLayout(row1);
  auto* row2 = new QHBoxLayout;
  assign_ = new QPushButton(tr("Assign to Selection"), left);
  pick_ = new QPushButton(tr("Pick from Object"), left);
  row2->addWidget(assign_);
  row2->addWidget(pick_);
  lv->addLayout(row2);
  auto* slate = new QPushButton(tr("Slate (Node) Editor..."), left);
  lv->addWidget(slate);

  // Right: preview, name, presets, navigation and the level editor.
  auto* right = new QWidget(split);
  auto* rv = new QVBoxLayout(right);
  rv->setContentsMargins(0, 0, 0, 0);
  auto* top = new QHBoxLayout;
  bigPreview_ = new QLabel(right);
  bigPreview_->setFixedSize(kPreviewSize, kPreviewSize);
  bigPreview_->setFrameShape(QFrame::Box);
  top->addWidget(bigPreview_);
  auto* info = new QVBoxLayout;
  name_ = new QLineEdit(right);
  presets_ = new QComboBox(right);
  presets_->addItem(tr("Apply preset..."));
  for (const auto& p : kPresets) presets_->addItem(tr(p.label));
  usage_ = new QLabel(right);
  usage_->setWordWrap(true);
  usage_->setStyleSheet("color: #a8a8a8;");
  info->addWidget(new QLabel(tr("Name:"), right));
  info->addWidget(name_);
  info->addWidget(presets_);
  info->addWidget(usage_);
  info->addStretch();
  top->addLayout(info, 1);
  rv->addLayout(top);
  auto* nav = new QHBoxLayout;
  parentBtn_ = new QPushButton(tr("Go to Parent"), right);
  parentBtn_->setToolTip(tr("Go up one level in the material tree"));
  breadcrumb_ = new QLabel(right);
  breadcrumb_->setWordWrap(true);
  breadcrumb_->setStyleSheet("color: #d8d8d8; font-weight: bold;");
  nav->addWidget(parentBtn_);
  nav->addWidget(breadcrumb_, 1);
  rv->addLayout(nav);
  auto* scroll = new QScrollArea(right);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  level_ = new LevelEditor(doc_);
  scroll->setWidget(level_);
  rv->addWidget(scroll, 1);
  split->setStretchFactor(0, 1);
  split->setStretchFactor(1, 2);
  split->setSizes({300, 560});

  previewTimer_.setSingleShot(true);
  previewTimer_.setInterval(250);
  connect(&previewTimer_, &QTimer::timeout, this, [this] {
    for (uint32_t id : pendingPreviews_) requestPreview(id);
    pendingPreviews_.clear();
    updateLevelPreview();
  });
  connect(&previews_, &PreviewRenderer::ready, this, [this](uint32_t id, const QImage& img) {
    images_[id] = img;
    for (int i = 0; i < slots_->count(); ++i)
      if (slots_->item(i)->data(Qt::UserRole).toUInt() == id) slots_->item(i)->setIcon(slotIcon(img));
    if ((id == currentId_ && path_.empty()) || id == kLevelPreviewId) bigPreview_->setPixmap(QPixmap::fromImage(img));
  });

  connect(slots_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* it) {
    if (!it) return;
    uint32_t id = it->data(Qt::UserRole).toUInt();
    if (id != currentId_) {
      currentId_ = id;
      path_.clear();
    }
    showCurrent();
  });
  connect(newBtn, &QPushButton::clicked, this, [this] {
    std::string type = "VeraaMtl";
    if (QApplication::keyboardModifiers() & Qt::ShiftModifier) {
      QString t = browseMaterialType(this);
      if (t.isEmpty()) return;
      type = t.toStdString();
    }
    doc_->beginEdit(tr("New Material"));
    Material* m = doc_->scene().createMaterial(doc_->scene().uniqueMaterialName("Material"));
    m->setMaterialType(type);
    uint32_t id = m->id;
    doc_->notifyMaterialsChanged();
    doc_->endEdit();
    selectMaterial(id);
  });
  connect(dup_, &QPushButton::clicked, this, [this] {
    Material* cur = current();
    if (!cur) return;
    doc_->beginEdit(tr("Duplicate Material"));
    auto copy = std::make_unique<Material>(*cur);
    copy->name = doc_->scene().uniqueMaterialName(cur->name + " copy");
    uint32_t id = doc_->scene().addMaterial(std::move(copy))->id;
    doc_->notifyMaterialsChanged();
    doc_->endEdit();
    selectMaterial(id);
  });
  connect(del_, &QPushButton::clicked, this, [this] {
    Material* cur = current();
    if (!cur) return;
    QVector<uint32_t> users;
    for (const auto& n : doc_->scene().nodes)
      if (n->materialId == cur->id) users.push_back(n->id);
    doc_->beginEdit(tr("Delete Material"));
    doc_->scene().removeMaterial(cur->id);
    currentId_ = 0;
    path_.clear();
    doc_->notifyMaterialsChanged();
    if (!users.isEmpty()) doc_->notifyNodesChanged(users, ChangeMaterial);
    doc_->endEdit();
  });
  connect(assign_, &QPushButton::clicked, this, [this] {
    if (Material* cur = current()) doc_->assignMaterialToSelection(cur->id);
    refreshUsage();
  });
  connect(pick_, &QPushButton::clicked, this, [this] {
    Node* n = doc_->firstSelected();
    if (n && n->materialId) selectMaterial(n->materialId);
  });
  connect(slate, &QPushButton::clicked, this, [this] { Q_EMIT slateRequested(currentId_); });
  connect(parentBtn_, &QPushButton::clicked, this, [this] {
    if (path_.empty()) return;
    MtlPath p = path_;
    p.pop_back();
    navigate(p);
  });
  connect(name_, &QLineEdit::editingFinished, this, [this] {
    MtlTarget t = resolveMtlPath(current(), path_);
    QString text = name_->text();
    if (text.isEmpty()) return;
    std::string* target = t.material ? &t.material->name : (t.map ? &t.map->name : nullptr);
    if (!target || *target == text.toStdString()) return;
    doc_->beginEdit(tr("Rename Material"));
    t = resolveMtlPath(current(), path_);
    if (t.material) t.material->name = text.toStdString();
    else if (t.map) t.map->name = text.toStdString();
    doc_->notifyMaterialsChanged();
    doc_->endEdit();
  });
  connect(presets_, &QComboBox::activated, this, [this](int i) {
    if (i > 0) applyPreset(i - 1);
    QSignalBlocker b(presets_);
    presets_->setCurrentIndex(0);
  });
  connect(level_, &LevelEditor::navigateRequested, this, [this](const MtlPath& p) { navigate(p); });
  connect(level_, &LevelEditor::edited, this, [this] {
    if (!pendingPreviews_.contains(currentId_)) pendingPreviews_.push_back(currentId_);
    previewTimer_.start();
  });

  auto onScene = [this] {
    // Material objects may have been replaced (undo/redo/load).
    rebuildSlots();
  };
  connect(doc_, &SceneDocument::materialsChanged, this, onScene);
  connect(doc_, &SceneDocument::structureChanged, this, onScene);
  connect(doc_, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>&, int flags) {
    if (flags & (ChangeMaterial | ChangeName)) refreshUsage();
  });
  connect(doc_, &SceneDocument::selectionChanged, this, [this] {
    assign_->setEnabled(current() && !doc_->selection().isEmpty());
    pick_->setEnabled(doc_->firstSelected() && doc_->firstSelected()->materialId);
  });
  connect(slots_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { requestVisiblePreviews(); });
  rebuildSlots();
}

Material* MaterialEditor::current() const { return doc_->scene().findMaterial(currentId_); }

void MaterialEditor::selectMaterial(uint32_t id) {
  for (int i = 0; i < slots_->count(); ++i)
    if (slots_->item(i)->data(Qt::UserRole).toUInt() == id) {
      slots_->setCurrentRow(i);
      return;
    }
}

void MaterialEditor::navigate(const MtlPath& path) {
  if (!resolveMtlPath(current(), path).valid()) return;
  path_ = path;
  showCurrent();
}

void MaterialEditor::rebuildSlots() {
  const Scene& scene = doc_->scene();
  // Keep the list when only names/params changed; rebuild if the set of materials changed.
  QList<uint32_t> ids;
  for (const auto& m : scene.materials) ids.push_back(m->id);
  QList<uint32_t> shown;
  for (int i = 0; i < slots_->count(); ++i) shown.push_back(slots_->item(i)->data(Qt::UserRole).toUInt());

  QSignalBlocker b(slots_);
  if (ids != shown) {
    slots_->clear();
    for (const auto& m : scene.materials) {
      auto* it = new QListWidgetItem(QString::fromStdString(m->name), slots_);
      it->setData(Qt::UserRole, m->id);
      QImage img = images_.value(m->id, placeholder(kSlotSize));
      it->setIcon(slotIcon(img));
      it->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
    }
    // Swatches render on demand for the slots in view (scenes may hold thousands of materials).
    requested_.clear();
    QMetaObject::invokeMethod(this, [this] { requestVisiblePreviews(); }, Qt::QueuedConnection);
  } else {
    for (int i = 0; i < slots_->count(); ++i)
      if (const Material* m = scene.findMaterial(ids[i])) slots_->item(i)->setText(QString::fromStdString(m->name));
  }
  if (!scene.findMaterial(currentId_)) {
    currentId_ = scene.materials.empty() ? 0 : scene.materials.front()->id;
    path_.clear();
  }
  for (int i = 0; i < slots_->count(); ++i)
    if (slots_->item(i)->data(Qt::UserRole).toUInt() == currentId_) slots_->setCurrentRow(i);
  showCurrent();
}

void MaterialEditor::showCurrent() {
  Material* m = current();
  if (m && m->upgradeLegacyMaps()) doc_->setModified(true);  // older scenes: file maps become Bitmap maps
  if (!resolveMtlPath(m, path_).valid()) path_.clear();
  MtlTarget t = resolveMtlPath(m, path_);
  dup_->setEnabled(m);
  del_->setEnabled(m);
  presets_->setEnabled(m && path_.empty());
  name_->setEnabled(m);
  parentBtn_->setEnabled(!path_.empty());
  assign_->setEnabled(m && !doc_->selection().isEmpty());
  pick_->setEnabled(doc_->firstSelected() && doc_->firstSelected()->materialId);
  {
    QSignalBlocker b(name_);
    name_->setText(t.material ? QString::fromStdString(t.material->name)
                              : (t.map ? QString::fromStdString(t.map->name) : QString()));
  }
  breadcrumb_->setText(m ? describeMtlPath(m, path_) : QString());
  if (level_->materialId() != currentId_ || level_->path() != path_) level_->setTarget(m ? currentId_ : 0, path_);
  else level_->sync();
  updateLevelPreview();
  refreshUsage();
}

void MaterialEditor::updateLevelPreview() {
  Material* m = current();
  MtlTarget t = resolveMtlPath(m, path_);
  if (!m) {
    bigPreview_->setPixmap(QPixmap::fromImage(placeholder(kPreviewSize)));
    return;
  }
  if (path_.empty()) {
    bigPreview_->setPixmap(QPixmap::fromImage(images_.value(m->id, placeholder(kPreviewSize)).scaled(kPreviewSize, kPreviewSize)));
  } else if (t.map) {
    bigPreview_->setPixmap(QPixmap::fromImage(mapSwatch(*t.map, doc_->scene(), kPreviewSize)));
  } else if (t.material) {
    previews_.request(kLevelPreviewId, previewCopy(*t.material, doc_->scene()), kPreviewSize, kPreviewSamples);
  }
}

void MaterialEditor::refreshUsage() {
  Material* m = current();
  if (!m) {
    usage_->setText(doc_->scene().materials.empty() ? tr("No materials yet. Click New to create one.") : QString());
    return;
  }
  QStringList users;
  for (const auto& n : doc_->scene().nodes)
    if (n->materialId == m->id) users << QString::fromStdString(n->name);
  usage_->setText(QString("%1\n").arg(QString::fromStdString(materialDisplayName(m->materialType))) +
                  (users.isEmpty() ? tr("Not assigned to any object.")
                                   : tr("Used by: %1").arg(users.mid(0, 12).join(", ") +
                                                           (users.size() > 12 ? QString(", ...") : QString()))));
}

void MaterialEditor::requestVisiblePreviews() {
  if (!isVisible()) return;
  QRect view = slots_->viewport()->rect();
  for (int i = 0; i < slots_->count(); ++i) {
    QListWidgetItem* it = slots_->item(i);
    uint32_t id = it->data(Qt::UserRole).toUInt();
    if (requested_.contains(id) || !slots_->visualItemRect(it).intersects(view)) continue;
    requestPreview(id);
  }
}

void MaterialEditor::showEvent(QShowEvent* e) {
  QDialog::showEvent(e);
  requestVisiblePreviews();
}

void MaterialEditor::requestPreview(uint32_t id) {
  const Material* m = doc_->scene().findMaterial(id);
  if (!m) return;
  requested_.insert(id);
  previews_.request(id, previewCopy(*m, doc_->scene()), kPreviewSize, kPreviewSamples);
}

void MaterialEditor::applyPreset(int index) {
  Material* m = current();
  if (!m || index < 0 || index >= static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]))) return;
  doc_->beginEdit(tr("Material Preset"));
  std::string name = m->name;
  uint32_t id = m->id;
  Material fresh;
  kPresets[index].build(fresh);
  fresh.name = name;
  fresh.id = id;
  *m = std::move(fresh);
  path_.clear();
  doc_->notifyMaterialsChanged();
  doc_->endEdit();
  showCurrent();
  requestPreview(m->id);
}

}  // namespace veraa
