#include "materialwidgets.h"

#include <array>
#include <map>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "paramform.h"

namespace veraa {

namespace {

QString pathKey(const MtlPath& path) {
  QString s;
  for (const auto& st : path)
    s += st.kind == MtlPathStep::Kind::Sub ? QString("/s%1").arg(st.sub) : QString("/m%1").arg(QString::fromStdString(st.key));
  return s;
}

QString prettyKey(const std::string& key) {
  QString out;
  for (size_t i = 0; i < key.size(); ++i) {
    QChar c = QChar::fromLatin1(key[i]);
    if (i == 0) out += c.toUpper();
    else if (c.isUpper()) out += QString(' ') + c.toLower();
    else out += c;
  }
  return out;
}

// Structure signature: rebuild the editor only when types, slots or sub-materials change.
QString signatureOf(const MtlTarget& t) {
  QString s;
  const MapSlots* slots = nullptr;
  if (t.material) {
    s += QString::fromStdString(t.material->materialType);
    slots = &t.material->maps;
    for (const auto& sub : t.material->subs)
      s += QString("|%1:%2:%3").arg(sub.material ? QString::fromStdString(sub.material->materialType) : "-").arg(sub.matId).arg(sub.enabled);
  } else if (t.map) {
    s += QString::fromStdString(t.map->type);
    slots = &t.map->maps;
  }
  if (slots)
    for (const auto& [k, slot] : *slots)
      s += QString("#%1=%2").arg(QString::fromStdString(k)).arg(slot.map ? QString::fromStdString(slot.map->type) : "-");
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Paths

MtlTarget resolveMtlPath(Material* root, const MtlPath& path) {
  MtlTarget t;
  t.material = root;
  if (!root) return MtlTarget();
  for (const auto& st : path) {
    if (st.kind == MtlPathStep::Kind::Sub) {
      if (!t.material || st.sub < 0 || st.sub >= static_cast<int>(t.material->subs.size())) return MtlTarget();
      Material* m = t.material->subs[st.sub].material.get();
      if (!m) return MtlTarget();
      t = MtlTarget();
      t.material = m;
    } else {
      MapSlots* slots = t.material ? &t.material->maps : (t.map ? &t.map->maps : nullptr);
      if (!slots) return MtlTarget();
      auto it = slots->find(st.key);
      if (it == slots->end() || !it->second.map) return MtlTarget();
      MtlTarget n;
      n.map = it->second.map.get();
      n.slot = &it->second;
      t = n;
    }
  }
  return t;
}

QString mapButtonText(const TexMap* map) {
  if (!map) return QObject::tr("None");
  QString type = QString::fromStdString(findTexMapType(map->type) ? findTexMapType(map->type)->displayName : map->type);
  if (map->type == "Bitmap" && !map->params.getString("file").empty())
    return QString("%1 (%2)").arg(type, QFileInfo(QString::fromStdString(map->params.getString("file"))).fileName());
  return type;
}

QString materialButtonText(const Material* m) {
  if (!m) return QObject::tr("None");
  return QString("%1 (%2)").arg(QString::fromStdString(m->name), QString::fromStdString(materialDisplayName(m->materialType)));
}

QString slotLabel(const ParamBlock* params, const std::string& key) {
  if (key == "bump") return QObject::tr("Bump");
  if (key == "displacement") return QObject::tr("Displacement");
  if (key == "coatBump") return QObject::tr("Coat bump");
  if (params)
    if (const ParamDef* d = params->def(key)) {
      QString group = QString::fromStdString(d->group);
      QString label = QString::fromStdString(d->label);
      // "Glossiness" alone is ambiguous between reflection and refraction.
      if (label == "Glossiness" || label == "IOR") return group + " " + label.toLower();
      return label;
    }
  return prettyKey(key);
}

QString describeMtlPath(const Material* root, const MtlPath& path) {
  if (!root) return QString();
  QStringList parts{QString::fromStdString(root->name)};
  MtlTarget t = resolveMtlPath(const_cast<Material*>(root), MtlPath());
  MtlPath partial;
  for (const auto& st : path) {
    const ParamBlock* params = t.material ? &t.material->params : (t.map ? &t.map->params : nullptr);
    partial.push_back(st);
    MtlTarget next = resolveMtlPath(const_cast<Material*>(root), partial);
    if (st.kind == MtlPathStep::Kind::Sub) {
      QString label = t.material && st.sub < static_cast<int>(t.material->subs.size())
                          ? QString::fromStdString(t.material->subs[st.sub].label)
                          : QString("Sub %1").arg(st.sub + 1);
      parts << label;
    } else {
      parts << QString("%1: %2").arg(slotLabel(params, st.key), mapButtonText(next.map));
    }
    t = next;
  }
  return parts.join("  >  ");
}

MaterialClipboard& materialClipboard() {
  static MaterialClipboard clip;
  return clip;
}

namespace {
void resolveFiles(ParamBlock& p, const Scene& scene) {
  for (const auto& d : p.defs())
    if (d.type == ParamType::File) {
      const std::string& f = p.getString(d.key);
      if (!f.empty() && f[0] != ':') p.setString(d.key, scene.resolvePath(f));
    }
}
void resolveMapFiles(TexMap& m, const Scene& scene) {
  resolveFiles(m.params, scene);
  for (auto& [k, s] : m.maps)
    if (s.map) resolveMapFiles(*s.map, scene);
}
void resolveMaterialFiles(Material& m, const Scene& scene) {
  resolveFiles(m.params, scene);
  for (auto& [k, s] : m.maps)
    if (s.map) resolveMapFiles(*s.map, scene);
  for (auto& s : m.subs)
    if (s.material) resolveMaterialFiles(*s.material, scene);
}
}  // namespace

Material previewCopy(const Material& m, const Scene& scene) {
  Material c = m;
  c.upgradeLegacyMaps();
  resolveMaterialFiles(c, scene);
  return c;
}

TexMap previewCopy(const TexMap& m, const Scene& scene) {
  TexMap c = m;
  resolveMapFiles(c, scene);
  return c;
}

// ---------------------------------------------------------------------------------------------
// Browsers

namespace {

QString browse(QWidget* parent, const QString& title, const std::vector<std::array<std::string, 4>>& items,
               const QString& current) {
  QDialog dlg(parent);
  dlg.setWindowTitle(title);
  dlg.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  dlg.resize(380, 480);
  auto* v = new QVBoxLayout(&dlg);
  auto* filter = new QLineEdit(&dlg);
  filter->setPlaceholderText(QObject::tr("Search by name..."));
  v->addWidget(filter);
  auto* tree = new QTreeWidget(&dlg);
  tree->setHeaderHidden(true);
  v->addWidget(tree, 1);
  auto* desc = new QLabel(&dlg);
  desc->setWordWrap(true);
  desc->setMinimumHeight(36);
  desc->setStyleSheet("color: #a8a8a8;");
  v->addWidget(desc);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  v->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  std::map<std::string, QTreeWidgetItem*> groups;
  for (const auto& it : items) {  // type, display name, category, description
    QTreeWidgetItem*& g = groups[it[2]];
    if (!g) {
      g = new QTreeWidgetItem(tree, {QString::fromStdString(it[2])});
      g->setFlags(Qt::ItemIsEnabled);
      g->setExpanded(true);
    }
    auto* item = new QTreeWidgetItem(g, {QString::fromStdString(it[1])});
    item->setData(0, Qt::UserRole, QString::fromStdString(it[0]));
    item->setData(0, Qt::UserRole + 1, QString::fromStdString(it[3]));
    if (QString::fromStdString(it[0]) == current) tree->setCurrentItem(item);
  }
  QObject::connect(tree, &QTreeWidget::currentItemChanged, &dlg, [desc](QTreeWidgetItem* cur) {
    desc->setText(cur ? cur->data(0, Qt::UserRole + 1).toString() : QString());
  });
  QObject::connect(tree, &QTreeWidget::itemDoubleClicked, &dlg, [&dlg](QTreeWidgetItem* it) {
    if (it && !it->data(0, Qt::UserRole).toString().isEmpty()) dlg.accept();
  });
  QObject::connect(filter, &QLineEdit::textChanged, &dlg, [tree](const QString& text) {
    for (int g = 0; g < tree->topLevelItemCount(); ++g) {
      QTreeWidgetItem* grp = tree->topLevelItem(g);
      bool any = false;
      for (int i = 0; i < grp->childCount(); ++i) {
        bool show = text.isEmpty() || grp->child(i)->text(0).contains(text, Qt::CaseInsensitive);
        grp->child(i)->setHidden(!show);
        any = any || show;
      }
      grp->setHidden(!any);
    }
  });
  if (dlg.exec() != QDialog::Accepted || !tree->currentItem()) return QString();
  return tree->currentItem()->data(0, Qt::UserRole).toString();
}

}  // namespace

QString browseMapType(QWidget* parent, const QString& current) {
  std::vector<std::array<std::string, 4>> items;
  for (const auto& t : texMapTypes()) items.push_back({t.type, t.displayName, t.category, ""});
  return browse(parent, QObject::tr("Map Browser"), items, current);
}

QString browseMaterialType(QWidget* parent, const QString& current) {
  std::vector<std::array<std::string, 4>> items;
  for (const auto& t : materialTypes()) items.push_back({t.type, t.displayName, t.category, t.description});
  return browse(parent, QObject::tr("Material Browser"), items, current);
}

// ---------------------------------------------------------------------------------------------
// LevelEditor

LevelEditor::LevelEditor(SceneDocument* doc, QWidget* parent) : QWidget(parent), doc_(doc) {
  layout_ = new QVBoxLayout(this);
  layout_->setContentsMargins(0, 0, 0, 0);
}

MtlTarget LevelEditor::target() const { return resolveMtlPath(doc_->scene().findMaterial(materialId_), path_); }

void LevelEditor::setTarget(uint32_t materialId, const MtlPath& path) {
  materialId_ = materialId;
  path_ = path;
  builtFor_ = nullptr;
  rebuild();
}

void LevelEditor::sync() {
  MtlTarget t = target();
  if (!t.valid()) {
    // The level disappeared (undo, deleted map): fall back to the root.
    if (!path_.empty()) {
      path_.clear();
      Q_EMIT navigateRequested(path_);
    }
    rebuild();
    return;
  }
  const void* obj = t.material ? static_cast<const void*>(t.material) : static_cast<const void*>(t.map);
  if (obj != builtFor_ || property("signature").toString() != signatureOf(t)) {
    rebuild();
  } else if (form_) {
    form_->refresh();
  }
}

MapSlots* LevelEditor::slotsOf(const MtlTarget& t) const {
  return t.material ? &t.material->maps : (t.map ? &t.map->maps : nullptr);
}

void LevelEditor::structural(const QString& label, const std::function<void(MtlTarget&)>& fn) {
  doc_->commitSoftEdit();
  doc_->beginEdit(label);
  MtlTarget t = target();
  if (t.valid()) fn(t);
  doc_->notifyMaterialsChanged();
  doc_->endEdit();
  sync();
  Q_EMIT edited();
}

void LevelEditor::assignMap(const std::string& key, std::unique_ptr<TexMap> map) {
  structural(tr("Assign Map"), [&](MtlTarget& t) {
    MapSlots* slots = slotsOf(t);
    if (!slots) return;
    MapSlot& s = (*slots)[key];
    s.map = std::move(map);
    s.enabled = true;
  });
}

void LevelEditor::rebuild() {
  if (content_) {
    content_->hide();
    content_->deleteLater();
  }
  content_ = new QWidget(this);
  form_ = nullptr;
  auto* v = new QVBoxLayout(content_);
  v->setContentsMargins(0, 0, 0, 0);
  layout_->addWidget(content_);
  MtlTarget t = target();
  builtFor_ = t.material ? static_cast<const void*>(t.material) : static_cast<const void*>(t.map);
  setProperty("signature", signatureOf(t));
  if (!t.valid()) {
    v->addStretch();
    return;
  }

  // ---- Type ----
  auto* head = new QHBoxLayout;
  if (t.material) {
    head->addWidget(new QLabel(tr("Material type:"), content_));
    auto* typeBtn = new QPushButton(QString::fromStdString(materialDisplayName(t.material->materialType)), content_);
    typeBtn->setToolTip(tr("Change the material type (shared parameters are kept)"));
    head->addWidget(typeBtn, 1);
    connect(typeBtn, &QPushButton::clicked, this, [this] {
      MtlTarget cur = target();
      if (!cur.material) return;
      QString type = browseMaterialType(this, QString::fromStdString(cur.material->materialType));
      if (type.isEmpty() || type.toStdString() == cur.material->materialType) return;
      structural(tr("Change Material Type"), [&](MtlTarget& tt) {
        if (tt.material) tt.material->setMaterialType(type.toStdString());
      });
    });
  } else {
    head->addWidget(new QLabel(tr("Map type:"), content_));
    auto* typeBtn = new QPushButton(mapButtonText(t.map), content_);
    typeBtn->setToolTip(tr("Replace this map with another map type"));
    head->addWidget(typeBtn, 1);
    connect(typeBtn, &QPushButton::clicked, this, [this] {
      MtlTarget cur = target();
      if (!cur.map) return;
      QString type = browseMapType(this, QString::fromStdString(cur.map->type));
      if (type.isEmpty() || type.toStdString() == cur.map->type) return;
      structural(tr("Replace Map"), [&](MtlTarget& tt) {
        if (tt.slot) tt.slot->map = createTexMap(type.toStdString());
      });
    });
  }
  v->addLayout(head);
  if (t.map && t.slot) {
    auto* slotRow = new QHBoxLayout;
    auto* on = new QCheckBox(tr("Enabled"), content_);
    on->setChecked(t.slot->enabled);
    auto* amount = new QDoubleSpinBox(content_);
    amount->setRange(0, 100);
    amount->setSuffix(" %");
    amount->setValue(t.slot->amount * 100.0);
    installSpinnerScrub(amount);
    slotRow->addWidget(on);
    slotRow->addWidget(new QLabel(tr("Amount:"), content_));
    slotRow->addWidget(amount);
    slotRow->addStretch();
    v->addLayout(slotRow);
    connect(on, &QCheckBox::toggled, this, [this](bool b) {
      structural(tr("Toggle Map"), [&](MtlTarget& tt) {
        if (tt.slot) tt.slot->enabled = b;
      });
    });
    connect(amount, &QDoubleSpinBox::valueChanged, this, [this](double val) {
      doc_->softEdit(tr("Map Amount"), QString("mtl/%1%2/amount").arg(materialId_).arg(pathKey(path_)));
      MtlTarget tt = target();
      if (tt.slot) tt.slot->amount = static_cast<float>(val / 100.0);
      doc_->notifyMaterialsChanged();
      Q_EMIT edited();
    });
  }

  // ---- Parameters ----
  ParamBlock* params = t.material ? &t.material->params : &t.map->params;
  QStringList exclude;
  for (const auto& d : params->defs())
    if (d.group == "Legacy") exclude << QString::fromStdString(d.key);
  form_ = new ParamForm(content_);
  QString file = doc_->filePath();
  form_->browseDir = file.isEmpty() ? QString() : QFileInfo(file).absolutePath();
  form_->setBlock(params, exclude);
  form_->beforeChange = [this](const std::string& key) {
    doc_->softEdit(tr("Material Parameter"),
                   QString("mtl/%1%2/%3").arg(materialId_).arg(pathKey(path_)).arg(QString::fromStdString(key)));
  };
  form_->afterChange = [this](const std::string&) {
    doc_->notifyMaterialsChanged();
    Q_EMIT edited();
  };
  v->addWidget(form_);

  if (t.material && !t.material->subs.empty()) buildSubs(v, t.material);
  buildMaps(v, t);
  v->addStretch();
}

void LevelEditor::buildMaps(QVBoxLayout* into, const MtlTarget& t) {
  const std::vector<std::string>& keys = t.material ? t.material->mappableKeys() : t.map->mappableKeys();
  if (keys.empty()) return;
  const ParamBlock* params = t.material ? &t.material->params : &t.map->params;
  MapSlots* slots = slotsOf(t);
  auto* roll = new Rollout(tr("Maps"), content_);
  auto* grid = new QWidget(roll->body());
  auto* gv = new QVBoxLayout(grid);
  gv->setContentsMargins(0, 0, 0, 0);
  gv->setSpacing(2);
  for (const auto& key : keys) {
    auto* row = new QHBoxLayout;
    auto it = slots->find(key);
    const MapSlot* slot = it != slots->end() ? &it->second : nullptr;
    auto* on = new QCheckBox(grid);
    on->setChecked(!slot || slot->enabled);
    on->setEnabled(slot && slot->map);
    auto* label = new QLabel(slotLabel(params, key), grid);
    label->setMinimumWidth(130);
    auto* amount = new QDoubleSpinBox(grid);
    amount->setRange(0, 100);
    amount->setDecimals(0);
    amount->setSuffix(" %");
    amount->setValue(slot ? slot->amount * 100.0 : 100.0);
    amount->setEnabled(slot && slot->map);
    amount->setMaximumWidth(72);
    installSpinnerScrub(amount);
    auto* btn = new QPushButton(mapButtonText(slot ? slot->map.get() : nullptr), grid);
    btn->setToolTip(slot && slot->map ? tr("Click to edit the map; right-click for more") : tr("Click to choose a map"));
    btn->setContextMenuPolicy(Qt::CustomContextMenu);
    row->addWidget(on);
    row->addWidget(label);
    row->addWidget(amount);
    row->addWidget(btn, 1);
    gv->addLayout(row);
    std::string k = key;
    connect(on, &QCheckBox::toggled, this, [this, k](bool b) {
      structural(tr("Toggle Map"), [&](MtlTarget& tt) {
        MapSlots* s = slotsOf(tt);
        if (s && s->count(k)) (*s)[k].enabled = b;
      });
    });
    connect(amount, &QDoubleSpinBox::valueChanged, this, [this, k](double val) {
      doc_->softEdit(tr("Map Amount"), QString("mtl/%1%2/amt/%3").arg(materialId_).arg(pathKey(path_)).arg(QString::fromStdString(k)));
      MtlTarget tt = target();
      MapSlots* s = slotsOf(tt);
      if (s && s->count(k)) (*s)[k].amount = static_cast<float>(val / 100.0);
      doc_->notifyMaterialsChanged();
      Q_EMIT edited();
    });
    connect(btn, &QPushButton::clicked, this, [this, k] {
      MtlTarget tt = target();
      MapSlots* s = slotsOf(tt);
      bool has = s && s->count(k) && (*s)[k].map;
      if (!has) {
        QString type = browseMapType(this);
        if (type.isEmpty()) return;
        auto map = createTexMap(type.toStdString());
        if (type == "Bitmap") {
          QString start = doc_->filePath().isEmpty() ? QString() : QFileInfo(doc_->filePath()).absolutePath();
          QString f = QFileDialog::getOpenFileName(this, tr("Select Bitmap Image File"), start,
                                                   tr("Images (*.png *.jpg *.jpeg *.bmp *.tga *.tif *.tiff *.hdr *.exr);;All files (*)"));
          if (!f.isEmpty()) {
            map->params.setString("file", f.toStdString());
            map->name = QFileInfo(f).fileName().toStdString();
          }
        }
        assignMap(k, std::move(map));
      }
      MtlPath p = path_;
      p.push_back({MtlPathStep::Kind::Map, 0, k});
      Q_EMIT navigateRequested(p);
    });
    connect(btn, &QPushButton::customContextMenuRequested, this, [this, k, btn](const QPoint& pos) {
      QMenu menu(this);
      MtlTarget tt = target();
      MapSlots* s = slotsOf(tt);
      bool has = s && s->count(k) && (*s)[k].map;
      QAction* clear = menu.addAction(tr("Clear"));
      clear->setEnabled(has);
      QAction* copy = menu.addAction(tr("Copy"));
      copy->setEnabled(has);
      QAction* paste = menu.addAction(tr("Paste"));
      paste->setEnabled(materialClipboard().map != nullptr);
      QAction* replace = menu.addAction(tr("Choose Map..."));
      QAction* chosen = menu.exec(btn->mapToGlobal(pos));
      if (chosen == clear) {
        structural(tr("Clear Map"), [&](MtlTarget& t2) {
          if (MapSlots* s2 = slotsOf(t2)) s2->erase(k);
        });
      } else if (chosen == copy && has) {
        materialClipboard().map = std::make_unique<TexMap>(*(*s)[k].map);
      } else if (chosen == paste && materialClipboard().map) {
        assignMap(k, std::make_unique<TexMap>(*materialClipboard().map));
      } else if (chosen == replace) {
        QString type = browseMapType(this);
        if (!type.isEmpty()) assignMap(k, createTexMap(type.toStdString()));
      }
    });
  }
  roll->form()->addRow(grid);
  into->addWidget(roll);
}

void LevelEditor::buildSubs(QVBoxLayout* into, Material* m) {
  bool multi = m->materialType == "MultiSubObject";
  auto* roll = new Rollout(multi ? tr("Multi/Sub-Object") : tr("Sub-materials"), content_);
  auto* grid = new QWidget(roll->body());
  auto* gv = new QVBoxLayout(grid);
  gv->setContentsMargins(0, 0, 0, 0);
  gv->setSpacing(2);
  for (int i = 0; i < static_cast<int>(m->subs.size()); ++i) {
    const SubMaterialSlot& s = m->subs[i];
    auto* row = new QHBoxLayout;
    auto* on = new QCheckBox(grid);
    on->setChecked(s.enabled);
    row->addWidget(on);
    if (multi) {
      auto* id = new QSpinBox(grid);
      id->setRange(1, 65535);
      id->setValue(s.matId);
      id->setToolTip(tr("Material ID"));
      id->setMaximumWidth(64);
      row->addWidget(id);
      connect(id, &QSpinBox::valueChanged, this, [this, i](int val) {
        doc_->softEdit(tr("Material ID"), QString("mtl/%1%2/id%3").arg(materialId_).arg(pathKey(path_)).arg(i));
        MtlTarget tt = target();
        if (tt.material && i < static_cast<int>(tt.material->subs.size())) tt.material->subs[i].matId = val;
        doc_->notifyMaterialsChanged();
        Q_EMIT edited();
      });
    }
    auto* label = new QLabel(QString::fromStdString(s.label), grid);
    label->setMinimumWidth(multi ? 60 : 110);
    row->addWidget(label);
    auto* btn = new QPushButton(materialButtonText(s.material.get()), grid);
    btn->setContextMenuPolicy(Qt::CustomContextMenu);
    row->addWidget(btn, 1);
    gv->addLayout(row);
    connect(on, &QCheckBox::toggled, this, [this, i](bool b) {
      structural(tr("Toggle Sub-material"), [&](MtlTarget& tt) {
        if (tt.material && i < static_cast<int>(tt.material->subs.size())) tt.material->subs[i].enabled = b;
      });
    });
    connect(btn, &QPushButton::clicked, this, [this, i] {
      MtlTarget tt = target();
      if (!tt.material || i >= static_cast<int>(tt.material->subs.size())) return;
      if (!tt.material->subs[i].material) {
        structural(tr("New Sub-material"), [&](MtlTarget& t2) {
          auto mtl = std::make_unique<Material>();
          mtl->name = t2.material->subs[i].label;
          t2.material->subs[i].material = std::move(mtl);
        });
      }
      MtlPath p = path_;
      p.push_back({MtlPathStep::Kind::Sub, i, {}});
      Q_EMIT navigateRequested(p);
    });
    connect(btn, &QPushButton::customContextMenuRequested, this, [this, i, btn](const QPoint& pos) {
      QMenu menu(this);
      MtlTarget tt = target();
      bool has = tt.material && i < static_cast<int>(tt.material->subs.size()) && tt.material->subs[i].material;
      QAction* clear = menu.addAction(tr("Clear"));
      clear->setEnabled(has);
      QAction* copy = menu.addAction(tr("Copy"));
      copy->setEnabled(has);
      QAction* paste = menu.addAction(tr("Paste"));
      paste->setEnabled(materialClipboard().material != nullptr);
      QAction* fresh = menu.addAction(tr("New Material of Type..."));
      QAction* chosen = menu.exec(btn->mapToGlobal(pos));
      if (chosen == clear) {
        structural(tr("Clear Sub-material"), [&](MtlTarget& t2) { t2.material->subs[i].material.reset(); });
      } else if (chosen == copy && has) {
        materialClipboard().material = std::make_unique<Material>(*tt.material->subs[i].material);
      } else if (chosen == paste && materialClipboard().material) {
        structural(tr("Paste Sub-material"), [&](MtlTarget& t2) {
          t2.material->subs[i].material = std::make_unique<Material>(*materialClipboard().material);
        });
      } else if (chosen == fresh) {
        QString type = browseMaterialType(this);
        if (!type.isEmpty())
          structural(tr("New Sub-material"), [&](MtlTarget& t2) {
            auto mtl = std::make_unique<Material>(type.toStdString());
            mtl->name = t2.material->subs[i].label;
            t2.material->subs[i].material = std::move(mtl);
          });
      }
    });
  }
  if (multi) {
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"), grid);
    auto* del = new QPushButton(tr("Delete Last"), grid);
    del->setEnabled(m->subs.size() > 1);
    row->addWidget(add);
    row->addWidget(del);
    row->addStretch();
    gv->addLayout(row);
    connect(add, &QPushButton::clicked, this, [this] {
      structural(tr("Add Sub-material"), [&](MtlTarget& t2) {
        int next = 1;
        for (const auto& s : t2.material->subs) next = std::max(next, s.matId + 1);
        SubMaterialSlot s;
        s.matId = next;
        s.label = "ID " + std::to_string(next);
        s.material = std::make_unique<Material>();
        s.material->name = "Sub " + std::to_string(next);
        t2.material->subs.push_back(std::move(s));
      });
    });
    connect(del, &QPushButton::clicked, this, [this] {
      structural(tr("Delete Sub-material"), [&](MtlTarget& t2) {
        if (t2.material->subs.size() > 1) t2.material->subs.pop_back();
      });
    });
  }
  roll->form()->addRow(grid);
  into->addWidget(roll);
}

}  // namespace veraa
