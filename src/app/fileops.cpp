// File menu: New (all / keep objects / keep hierarchy), Reset, Open, Open Recent, View Image File,
// Save variants, Archive, Import / Merge / Replace, Export, XRef, Project folder, Summary Info,
// File Properties and auto backup.
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

#include "commandpanel.h"
#include "customize.h"
#include "dialogs.h"
#include "icons.h"
#include "interfacedata.h"
#include "io_formats.h"
#include "mainwindow.h"
#include "miniz/miniz.h"
#include "project.h"
#include "scripting.h"
#include "units.h"
#include "viewportpanel.h"

namespace veraa {

namespace {

QSettings appSettings() { return QSettings("Veraa", "Veraa"); }

QString importFilter() {
  return QObject::tr(
      "All supported (*.veraa *.obj *.fbx *.gltf *.glb *.stl *.ply *.3ds);;"
      "Veraa scenes (*.veraa);;Wavefront OBJ (*.obj);;FBX (*.fbx);;glTF 2.0 (*.gltf *.glb);;STL (*.stl);;"
      "PLY (*.ply);;3D Studio (*.3ds);;All files (*)");
}

QString exportFilter() {
  return QObject::tr(
      "FBX (*.fbx);;glTF 2.0 binary (*.glb);;glTF 2.0 (*.gltf);;Wavefront OBJ (*.obj);;STL (*.stl);;PLY (*.ply)");
}

// Every texture and file path a scene refers to: (param block, key).
std::vector<std::pair<ParamBlock*, std::string>> fileReferences(Scene& s) {
  std::vector<std::pair<ParamBlock*, std::string>> out;
  for (auto& m : s.materials)
    for (const ParamDef& d : m->params.defs())
      if (d.type == ParamType::File && !m->params.getString(d.key).empty()) out.push_back({&m->params, d.key});
  for (const ParamDef& d : s.renderSettings.defs())
    if (d.type == ParamType::File && !s.renderSettings.getString(d.key).empty()) out.push_back({&s.renderSettings, d.key});
  return out;
}

class ImportOptionsDialog : public QDialog {
 public:
  ImportOptionsDialog(const QString& ext, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("%1 Import Options").arg(ext.toUpper()));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    auto* f = new QFormLayout(this);
    units_ = new QCheckBox(tr("Convert file units to scene units"), this);
    units_->setChecked(true);
    axes_ = new QCheckBox(ext == "stl" || ext == "ply" ? tr("File is Y-up (convert to Z-up)") : tr("Convert Y-up files to Z-up"), this);
    axes_->setChecked(ext != "stl" && ext != "ply" && ext != "3ds");
    scale_ = new QDoubleSpinBox(this);
    scale_->setRange(0.0001, 100000);
    scale_->setDecimals(4);
    scale_->setValue(1.0);
    scale_->setToolTip(tr("Extra scale applied after unit conversion"));
    materials_ = new QCheckBox(tr("Import materials"), this);
    materials_->setChecked(true);
    lights_ = new QCheckBox(tr("Import lights"), this);
    lights_->setChecked(true);
    cameras_ = new QCheckBox(tr("Import cameras"), this);
    cameras_->setChecked(true);
    weld_ = new QCheckBox(tr("Weld coincident vertices"), this);
    weld_->setChecked(true);
    if (ext == "fbx" || ext == "gltf" || ext == "glb") f->addRow(units_);
    f->addRow(axes_);
    f->addRow(tr("Scale:"), scale_);
    if (ext != "stl" && ext != "ply") f->addRow(materials_);
    if (ext == "fbx" || ext == "gltf" || ext == "glb") {
      f->addRow(lights_);
      f->addRow(cameras_);
    }
    if (ext == "stl" || ext == "gltf" || ext == "glb" || ext == "3ds") f->addRow(weld_);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    f->addRow(box);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    ext_ = ext;
  }
  ImportOptions options() const {
    ImportOptions o;
    o.convertUnits = units_->isChecked();
    o.convertAxes = ext_ == "stl" || ext_ == "ply" ? false : axes_->isChecked();
    o.yUpOverride = (ext_ == "stl" || ext_ == "ply") && axes_->isChecked();
    o.scale = static_cast<float>(scale_->value());
    o.importMaterials = materials_->isChecked();
    o.importLights = lights_->isChecked();
    o.importCameras = cameras_->isChecked();
    o.weld = weld_->isChecked();
    return o;
  }

 private:
  QString ext_;
  QCheckBox *units_, *axes_, *materials_, *lights_, *cameras_, *weld_;
  QDoubleSpinBox* scale_;
};

}  // namespace

void MainWindow::createFileMenu(QMenu* file) {
  QMenu* newMenu = file->addMenu(icons::newFile(), tr("&New"));
  newMenu->addAction(newAct_);
  QAction* keep = cmd("file.newKeepObjects", tr("New: Keep &Objects"));
  connect(keep, &QAction::triggered, this, [this] { newSceneKeep(1); });
  QAction* keepH = cmd("file.newKeepHierarchy", tr("New: Keep Objects and &Hierarchy"));
  connect(keepH, &QAction::triggered, this, [this] { newSceneKeep(2); });
  newMenu->addAction(keep);
  newMenu->addAction(keepH);
  QAction* reset = cmd("file.reset", tr("&Reset"));
  connect(reset, &QAction::triggered, this, &MainWindow::resetScene);
  file->addAction(reset);
  file->addSeparator();
  file->addAction(openAct_);
  recentMenu_ = file->addMenu(tr("Open &Recent"));
  QAction* viewImage = cmd("file.viewImage", tr("&View Image File..."));
  connect(viewImage, &QAction::triggered, this, &MainWindow::viewImageFile);
  file->addAction(viewImage);
  file->addSeparator();
  file->addAction(saveAct_);
  file->addAction(saveAsAct_);
  QAction* copyAs = cmd("file.saveCopyAs", tr("Save &Copy As..."));
  connect(copyAs, &QAction::triggered, this, [this] { saveCopyAs(); });
  file->addAction(copyAs);
  QAction* saveSel = cmd("file.saveSelected", tr("Save Selec&ted..."));
  connect(saveSel, &QAction::triggered, this, [this] { saveSelected(); });
  file->addAction(saveSel);
  QAction* archive = cmd("file.archive", tr("&Archive..."));
  connect(archive, &QAction::triggered, this, [this] { archiveScene(); });
  file->addAction(archive);
  file->addSeparator();
  QMenu* imp = file->addMenu(icons::import(), tr("&Import"));
  imp->addAction(importAct_);
  QAction* merge = cmd("file.merge", tr("&Merge..."));
  connect(merge, &QAction::triggered, this, [this] { mergeFile(false); });
  QAction* replace = cmd("file.replace", tr("&Replace..."));
  connect(replace, &QAction::triggered, this, [this] { mergeFile(true); });
  QAction* importObjAct = cmd("file.importObj", tr("Import &OBJ..."));
  connect(importObjAct, &QAction::triggered, this, &MainWindow::importObjFile);
  imp->addAction(merge);
  imp->addAction(replace);
  imp->addAction(importObjAct);
  QMenu* exp = file->addMenu(tr("&Export"));
  QAction* exportAll = cmd("file.export", tr("&Export..."));
  connect(exportAll, &QAction::triggered, this, [this] { exportFile(false); });
  QAction* exportSel = cmd("file.exportSelected", tr("Export &Selected..."));
  connect(exportSel, &QAction::triggered, this, [this] { exportFile(true); });
  exp->addAction(exportAll);
  exp->addAction(exportSel);
  file->addSeparator();
  QMenu* ref = file->addMenu(tr("Re&ference"));
  QAction* xrefObjects = cmd("file.xrefObjects", tr("XRef &Objects..."));
  connect(xrefObjects, &QAction::triggered, this, [this] {
    QString f = QFileDialog::getOpenFileName(this, tr("XRef Objects"), projectSubfolder("scenes"), tr("Veraa scenes (*.veraa)"));
    if (f.isEmpty()) return;
    Scene tmp;
    std::string err;
    if (!tmp.load(f.toStdString(), &err)) {
      QMessageBox::warning(this, tr("XRef Objects"), QString::fromStdString(err));
      return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("XRef Objects: %1").arg(QFileInfo(f).fileName()));
    auto* v = new QVBoxLayout(&dlg);
    auto* list = new QListWidget(&dlg);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const auto& n : tmp.nodes) list->addItem(QString::fromStdString(n->name));
    list->selectAll();
    v->addWidget(list);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    QStringList names;
    for (QListWidgetItem* it : list->selectedItems()) names << it->text();
    QString merr;
    QVector<uint32_t> ids = mergeScene(f, names, true, &merr);
    if (ids.isEmpty() && !merr.isEmpty()) QMessageBox::warning(this, tr("XRef Objects"), merr);
  });
  QAction* xrefSceneAct = cmd("file.xrefScene", tr("XRef &Scene..."));
  connect(xrefSceneAct, &QAction::triggered, this, &MainWindow::xrefScene);
  ref->addAction(xrefObjects);
  ref->addAction(xrefSceneAct);
  QAction* reload = cmd("file.reloadXRefs", tr("&Update XRefs"));
  connect(reload, &QAction::triggered, this, &MainWindow::reloadXRefs);
  ref->addAction(reload);
  QMenu* project = file->addMenu(tr("&Project"));
  QAction* createProject = cmd("file.projectCreate", tr("&Create Default Project..."));
  connect(createProject, &QAction::triggered, this, [this] { setProjectFolder(true); });
  QAction* setProject = cmd("file.projectSet", tr("&Set Project Folder..."));
  connect(setProject, &QAction::triggered, this, [this] { setProjectFolder(false); });
  project->addAction(createProject);
  project->addAction(setProject);
  project->addSeparator();
  QAction* projectPaths = new QAction(tr("Configure Project Paths..."), this);
  connect(projectPaths, &QAction::triggered, this, [this] {
    PathsDialog dlg(this, false);
    dlg.exec();
  });
  project->addAction(projectPaths);
  file->addSeparator();
  QAction* summary = cmd("file.summaryInfo", tr("Summary &Info..."));
  connect(summary, &QAction::triggered, this, &MainWindow::showSummaryInfo);
  file->addAction(summary);
  QAction* props = cmd("file.fileProperties", tr("File &Properties..."));
  connect(props, &QAction::triggered, this, &MainWindow::showFileProperties);
  file->addAction(props);
  file->addSeparator();
  QAction* prefs = new QAction(tr("Pre&ferences..."), this);
  connect(prefs, &QAction::triggered, this, [this] {
    PreferencesDialog dlg(this);
    dlg.exec();
  });
  file->addAction(prefs);
  file->addSeparator();
  file->addAction(exitAct_);
}

// ---------------------------------------------------------------------------------------------

void MainWindow::newSceneKeep(int mode) {
  if (mode == 0) {
    if (!maybeSave()) return;
    doc_->newScene();
    isolated_ = false;
    isolatedHidden_.clear();
    setPrompt(tr("New scene"));
    return;
  }
  // Keep objects (and optionally their links); animation and the file name are dropped.
  json kept = doc_->scene().toJson();
  if (kept.contains("extensions") && kept["extensions"].is_object()) kept["extensions"].erase("animation");
  if (!maybeSave()) return;
  doc_->newScene();
  std::string err;
  doc_->scene().fromJson(kept, &err);
  doc_->scene().filePath.clear();
  if (mode == 1) {
    std::vector<uint32_t> ids;
    for (const auto& n : doc_->scene().nodes) ids.push_back(n->id);
    for (uint32_t id : ids) doc_->scene().setParent(id, 0);
  }
  doc_->notifyStructureChanged();
  doc_->notifyMaterialsChanged();
  doc_->notifyRenderSettingsChanged();
  doc_->setModified(false);
  setPrompt(mode == 1 ? tr("New scene keeping the objects") : tr("New scene keeping the objects and hierarchy"));
}

void MainWindow::resetScene() {
  if (QMessageBox::question(this, tr("Reset"), tr("Do you really want to reset?")) != QMessageBox::Yes) return;
  if (!maybeSave()) return;
  doc_->newScene();
  isolated_ = false;
  isolatedHidden_.clear();
  state_->setViewportTool(nullptr);
  state_->setCreateType({});
  state_->setTool(TransformTool::Select);
  viewports_->setViewLayout(9);
  const ViewType types[4] = {ViewType::Top, ViewType::Front, ViewType::Left, ViewType::Perspective};
  for (int i = 0; i < viewports_->count(); ++i) {
    Viewport* vp = viewports_->viewport(i);
    vp->camera() = ViewCamera();
    vp->camera().type = types[i];
    vp->setShading(i == 3 ? ShadingMode::Shaded : ShadingMode::Wireframe);
    vp->setEdgedFaces(false);
    vp->setSafeFrame(false);
    vp->setBackground(ViewportBackground());
  }
  viewports_->setActiveIndex(3);
  viewports_->zoomExtentsAll(false);
  setPrompt(tr("Scene reset"));
}

bool MainWindow::saveCopyAs() {
  QString start = doc_->filePath().isEmpty() ? QDir(projectSubfolder("scenes")).filePath(tr("untitled_copy.veraa"))
                                             : QFileInfo(doc_->filePath()).absolutePath() + "/" +
                                                   QFileInfo(doc_->filePath()).completeBaseName() + "_copy.veraa";
  QString f = QFileDialog::getSaveFileName(this, tr("Save Copy As"), start, tr("Veraa scenes (*.veraa)"));
  if (f.isEmpty()) return false;
  if (QFileInfo(f).suffix().isEmpty()) f += ".veraa";
  doc_->commitSoftEdit();
  std::string keep = doc_->scene().filePath, err;
  bool ok = doc_->scene().save(f.toStdString(), &err);
  doc_->scene().filePath = keep;
  if (!ok) QMessageBox::warning(this, tr("Save Copy As"), QString::fromStdString(err));
  else setPrompt(tr("Saved a copy as %1").arg(QFileInfo(f).fileName()));
  return ok;
}

bool MainWindow::saveSelected() {
  if (doc_->selection().isEmpty()) {
    setPrompt(tr("Select the objects to save first"));
    return false;
  }
  QString f = QFileDialog::getSaveFileName(this, tr("Save Selected"), QDir(projectSubfolder("scenes")).filePath(tr("selected.veraa")),
                                           tr("Veraa scenes (*.veraa)"));
  if (f.isEmpty()) return false;
  if (QFileInfo(f).suffix().isEmpty()) f += ".veraa";
  doc_->commitSoftEdit();
  Scene tmp;
  std::string err;
  tmp.fromJson(doc_->scene().toJson(), &err);
  std::vector<uint32_t> drop;
  for (const auto& n : tmp.nodes)
    if (!doc_->isSelected(n->id)) drop.push_back(n->id);
  for (uint32_t id : drop) tmp.removeNode(id);
  if (!tmp.save(f.toStdString(), &err)) {
    QMessageBox::warning(this, tr("Save Selected"), QString::fromStdString(err));
    return false;
  }
  setPrompt(tr("Saved %1 object(s) to %2").arg(tmp.nodes.size()).arg(QFileInfo(f).fileName()));
  return true;
}

bool MainWindow::archiveScene() {
  QString base = doc_->filePath().isEmpty() ? tr("untitled") : QFileInfo(doc_->filePath()).completeBaseName();
  QString f = QFileDialog::getSaveFileName(this, tr("Archive"), QDir(projectSubfolder("scenes")).filePath(base + ".zip"),
                                           tr("Zip archives (*.zip)"));
  if (f.isEmpty()) return false;
  if (QFileInfo(f).suffix().isEmpty()) f += ".zip";
  doc_->commitSoftEdit();
  // Copy the scene, point its maps at "maps/<file>" and zip everything together.
  Scene tmp;
  std::string err;
  tmp.fromJson(doc_->scene().toJson(), &err);
  tmp.filePath = doc_->scene().filePath;
  std::map<std::string, std::string> files;  // absolute source -> name in the archive
  QStringList missing;
  for (auto& [block, key] : fileReferences(tmp)) {
    std::string abs = tmp.resolvePath(block->getString(key));
    if (!QFileInfo::exists(QString::fromStdString(abs))) {
      missing << QString::fromStdString(abs);
      continue;
    }
    std::string name = "maps/" + QFileInfo(QString::fromStdString(abs)).fileName().toStdString();
    files[abs] = name;
    block->setString(key, name);
  }
  std::string text = tmp.toJson().dump(1);
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof(zip));
  QByteArray zipPath = QDir::toNativeSeparators(f).toUtf8();
  if (!mz_zip_writer_init_file(&zip, zipPath.constData(), 0)) {
    QMessageBox::warning(this, tr("Archive"), tr("Cannot create %1").arg(f));
    return false;
  }
  bool ok = mz_zip_writer_add_mem(&zip, (base + ".veraa").toUtf8().constData(), text.data(), text.size(),
                                  MZ_DEFAULT_COMPRESSION);
  for (const auto& [abs, name] : files) {
    QFile in(QString::fromStdString(abs));
    if (!in.open(QIODevice::ReadOnly)) continue;
    QByteArray bytes = in.readAll();
    ok &= mz_zip_writer_add_mem(&zip, name.c_str(), bytes.constData(), static_cast<size_t>(bytes.size()),
                                MZ_DEFAULT_COMPRESSION) != 0;
  }
  ok &= mz_zip_writer_finalize_archive(&zip) != 0;
  mz_zip_writer_end(&zip);
  if (!ok) {
    QMessageBox::warning(this, tr("Archive"), tr("Writing %1 failed").arg(f));
    return false;
  }
  setPrompt(tr("Archived the scene and %1 file(s) to %2").arg(files.size()).arg(QFileInfo(f).fileName()));
  if (!missing.isEmpty())
    QMessageBox::information(this, tr("Archive"), tr("These files were missing and are not in the archive:\n%1").arg(missing.join('\n')));
  return true;
}

QVector<uint32_t> MainWindow::mergeScene(const QString& path, const QStringList& names, bool asXRef, QString* error) {
  Scene src;
  std::string err;
  if (!src.load(path.toStdString(), &err)) {
    if (error) *error = QString::fromStdString(err);
    return {};
  }
  Scene& dst = doc_->scene();
  doc_->beginEdit(asXRef ? tr("XRef") : tr("Merge"));
  std::unordered_map<uint32_t, uint32_t> nodeMap, matMap;
  std::vector<std::pair<const Node*, Node*>> created;
  for (const auto& sn : src.nodes) {
    if (!names.isEmpty() && !names.contains(QString::fromStdString(sn->name))) continue;
    auto copy = sn->clone();
    copy->parentId = 0;
    if (sn->materialId) {
      auto it = matMap.find(sn->materialId);
      if (it == matMap.end()) {
        uint32_t mid = 0;
        if (const Material* m = src.findMaterial(sn->materialId)) {
          auto mc = std::make_unique<Material>();
          mc->fromJson(m->toJson());
          mc->name = dst.uniqueMaterialName(m->name);
          // Texture paths relative to the source file become absolute.
          for (const ParamDef& d : mc->params.defs())
            if (d.type == ParamType::File && !mc->params.getString(d.key).empty())
              mc->params.setString(d.key, src.resolvePath(mc->params.getString(d.key)));
          mid = dst.addMaterial(std::move(mc))->id;
        }
        it = matMap.emplace(sn->materialId, mid).first;
      }
      copy->materialId = it->second;
    }
    // Layers by name.
    if (const Layer* l = src.findLayer(sn->layerId); l && l->id != 0) {
      uint32_t lid = 0;
      for (const Layer& dl : dst.layers)
        if (dl.name == l->name) lid = dl.id;
      if (!lid) lid = dst.createLayer(l->name)->id;
      copy->layerId = lid;
    } else {
      copy->layerId = dst.currentLayerId;
    }
    if (asXRef) copy->frozen = true;
    Node* added = dst.addNode(std::move(copy));
    nodeMap[sn->id] = added->id;
    created.push_back({sn.get(), added});
  }
  for (auto& [sn, added] : created) {
    auto it = nodeMap.find(sn->parentId);
    if (sn->parentId && it != nodeMap.end()) added->parentId = it->second;  // keeps the local transform
    else added->setWorldMatrix(sn->worldMatrix());
    added->invalidate();
  }
  QVector<uint32_t> ids;
  for (auto& [sn, added] : created) ids.push_back(added->id);
  if (asXRef) {
    QVector<XRefEntry> list = xrefScenes(dst);
    XRefEntry e;
    e.path = QFileInfo(path).absoluteFilePath();
    e.nodeIds = ids;
    e.objects = names;
    list.push_back(e);
    setXRefScenes(dst, list);
  }
  doc_->notifyStructureChanged();
  doc_->notifyMaterialsChanged();
  doc_->setSelection(ids);
  doc_->endEdit();
  return ids;
}

void MainWindow::mergeFile(bool replace) {
  QString f = QFileDialog::getOpenFileName(this, replace ? tr("Replace") : tr("Merge"), projectSubfolder("scenes"),
                                           tr("Veraa scenes (*.veraa)"));
  if (f.isEmpty()) return;
  Scene src;
  std::string err;
  if (!src.load(f.toStdString(), &err)) {
    QMessageBox::warning(this, tr("Merge"), QString::fromStdString(err));
    return;
  }
  QDialog dlg(this);
  dlg.setWindowTitle(replace ? tr("Replace - %1").arg(QFileInfo(f).fileName()) : tr("Merge - %1").arg(QFileInfo(f).fileName()));
  dlg.resize(380, 460);
  auto* v = new QVBoxLayout(&dlg);
  if (replace) v->addWidget(new QLabel(tr("Objects with the same names in the scene get the chosen objects' geometry:"), &dlg));
  auto* list = new QListWidget(&dlg);
  list->setSelectionMode(QAbstractItemView::ExtendedSelection);
  for (const auto& n : src.nodes) {
    QString name = QString::fromStdString(n->name);
    if (replace && !doc_->scene().findNodeByName(n->name)) continue;
    list->addItem(name);
  }
  v->addWidget(list);
  auto* row = new QHBoxLayout;
  auto* all = new QPushButton(tr("All"), &dlg);
  auto* none = new QPushButton(tr("None"), &dlg);
  connect(all, &QPushButton::clicked, list, &QListWidget::selectAll);
  connect(none, &QPushButton::clicked, list, &QListWidget::clearSelection);
  row->addWidget(all);
  row->addWidget(none);
  row->addStretch(1);
  v->addLayout(row);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  v->addWidget(box);
  connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (list->count() == 0) {
    QMessageBox::information(this, dlg.windowTitle(), replace ? tr("No objects in that file match names in this scene.")
                                                              : tr("That file has no objects."));
    return;
  }
  list->selectAll();
  if (dlg.exec() != QDialog::Accepted) return;
  QStringList names;
  for (QListWidgetItem* it : list->selectedItems()) names << it->text();
  if (names.isEmpty()) return;
  if (!replace) {
    QString merr;
    QVector<uint32_t> ids = mergeScene(f, names, false, &merr);
    setPrompt(tr("Merged %1 object(s)").arg(ids.size()));
    return;
  }
  doc_->beginEdit(tr("Replace"));
  QVector<uint32_t> ids;
  for (const QString& name : names) {
    Node* target = doc_->scene().findNodeByName(name.toStdString());
    Node* source = src.findNodeByName(name.toStdString());
    if (!target || !source || !source->object) continue;
    target->object = std::shared_ptr<SceneObject>(source->object->clone());
    target->modifiers.clear();
    for (const auto& m : source->modifiers)
      if (m) target->modifiers.push_back(m->clone());
    target->objectOffset = source->objectOffset;
    target->invalidate();
    ids.push_back(target->id);
  }
  doc_->notifyNodesChanged(ids, ChangeGeometry);
  doc_->notifyStructureChanged();
  doc_->endEdit();
  setPrompt(tr("Replaced %1 object(s)").arg(ids.size()));
}

bool MainWindow::importPath(const QString& path, QVector<uint32_t>* ids, QString* error) {
  QString ext = QFileInfo(path).suffix().toLower();
  if (ext == "veraa") {
    QString err;
    QVector<uint32_t> merged = mergeScene(path, {}, false, &err);
    if (ids) *ids = merged;
    if (error) *error = err;
    return err.isEmpty();
  }
  ImportOptions o;
  o.convertAxes = ext != "stl" && ext != "ply" && ext != "3ds";
  return importPathWith(path, o, ids, error);
}

bool MainWindow::importPathWith(const QString& path, const ImportOptions& options, QVector<uint32_t>* ids, QString* error) {
  doc_->beginEdit(tr("Import %1").arg(QFileInfo(path).fileName()));
  ImportResult res;
  std::string err;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  bool ok = importScene(doc_->scene(), path.toStdString(), options, &res, &err);
  QApplication::restoreOverrideCursor();
  if (!ok) {
    doc_->cancelEdit();
    if (error) *error = QString::fromStdString(err);
    return false;
  }
  doc_->notifyStructureChanged();
  doc_->notifyMaterialsChanged();
  QVector<uint32_t> created(res.nodeIds.begin(), res.nodeIds.end());
  doc_->setSelection(created);
  doc_->endEdit();
  if (ids) *ids = created;
  QStringList warnings;
  for (const auto& w : res.warnings) warnings << QString::fromStdString(w);
  lastImportWarnings_ = warnings;
  setPrompt(tr("Imported %1 object(s), %2 triangles from %3")
                .arg(created.size())
                .arg(res.triangles)
                .arg(QFileInfo(path).fileName()));
  return true;
}

void MainWindow::importFile() {
  QSettings s = appSettings();
  QString f = QFileDialog::getOpenFileName(this, tr("Select File to Import"),
                                           s.value("lastImportDir", projectSubfolder("import")).toString(), importFilter());
  if (f.isEmpty()) return;
  s.setValue("lastImportDir", QFileInfo(f).absolutePath());
  QString ext = QFileInfo(f).suffix().toLower();
  QString err;
  QVector<uint32_t> ids;
  bool ok;
  if (ext == "veraa") {
    ok = importPath(f, &ids, &err);
  } else if (ext == "obj") {
    importObjFileFrom(f);
    return;
  } else {
    ImportOptionsDialog dlg(ext, this);
    if (dlg.exec() != QDialog::Accepted) return;
    ok = importPathWith(f, dlg.options(), &ids, &err);
  }
  if (!ok) {
    QMessageBox::warning(this, tr("Import"), tr("Could not import %1:\n%2").arg(f, err));
    return;
  }
  viewports_->zoomExtentsAll(false);
  if (!lastImportWarnings_.isEmpty())
    QMessageBox::information(this, tr("Import"), lastImportWarnings_.mid(0, 20).join('\n'));
}

bool MainWindow::exportPath(const QString& path, bool selectedOnly, QString* error) {
  doc_->commitSoftEdit();
  ExportOptions o;
  if (selectedOnly) {
    if (doc_->selection().isEmpty()) {
      if (error) *error = tr("Nothing is selected");
      return false;
    }
    o.nodeIds.assign(doc_->selection().begin(), doc_->selection().end());
  }
  QString ext = QFileInfo(path).suffix().toLower();
  o.yUp = ext == "obj" || ext == "gltf" || ext == "glb";
  std::string err;
  if (!exportScene(doc_->scene(), path.toStdString(), o, &err)) {
    if (error) *error = QString::fromStdString(err);
    return false;
  }
  return true;
}

void MainWindow::exportFile(bool selectedOnly) {
  if (selectedOnly && doc_->selection().isEmpty()) {
    setPrompt(tr("Select the objects to export first"));
    return;
  }
  QSettings s = appSettings();
  QString base = doc_->filePath().isEmpty() ? tr("untitled") : QFileInfo(doc_->filePath()).completeBaseName();
  QString selectedFilter;
  QString f = QFileDialog::getSaveFileName(this, selectedOnly ? tr("Export Selected") : tr("Select File to Export"),
                                           QDir(s.value("lastExportDir", projectSubfolder("export")).toString()).filePath(base + ".fbx"),
                                           exportFilter(), &selectedFilter);
  if (f.isEmpty()) return;
  if (QFileInfo(f).suffix().isEmpty()) {
    QRegularExpression re(R"(\*\.(\w+))");
    auto m = re.match(selectedFilter);
    f += "." + (m.hasMatch() ? m.captured(1) : QString("fbx"));
  }
  s.setValue("lastExportDir", QFileInfo(f).absolutePath());
  QString ext = QFileInfo(f).suffix().toLower();
  // Export options.
  QDialog dlg(this);
  dlg.setWindowTitle(tr("%1 Export Options").arg(ext.toUpper()));
  auto* form = new QFormLayout(&dlg);
  auto* scale = new QDoubleSpinBox(&dlg);
  scale->setRange(0.0001, 100000);
  scale->setDecimals(4);
  scale->setValue(1.0);
  form->addRow(tr("Scale:"), scale);
  auto* yUp = new QCheckBox(tr("Y-up (for applications that expect Y up)"), &dlg);
  yUp->setChecked(ext == "obj");
  if (ext == "obj" || ext == "stl" || ext == "ply") form->addRow(yUp);
  auto* binary = new QCheckBox(tr("Binary"), &dlg);
  binary->setChecked(true);
  if (ext == "stl" || ext == "ply") form->addRow(binary);
  auto* materials = new QCheckBox(tr("Export materials"), &dlg);
  materials->setChecked(true);
  if (ext == "obj" || ext == "fbx" || ext == "gltf" || ext == "glb") form->addRow(materials);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  form->addRow(box);
  connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;
  ExportOptions o;
  o.scale = static_cast<float>(scale->value());
  o.yUp = ext == "gltf" || ext == "glb" || yUp->isChecked();
  o.binary = binary->isChecked();
  o.exportMaterials = materials->isChecked();
  if (selectedOnly) o.nodeIds.assign(doc_->selection().begin(), doc_->selection().end());
  doc_->commitSoftEdit();
  std::string err;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  bool ok = exportScene(doc_->scene(), f.toStdString(), o, &err);
  QApplication::restoreOverrideCursor();
  if (!ok) QMessageBox::warning(this, tr("Export"), QString::fromStdString(err));
  else setPrompt(tr("Exported %1").arg(QFileInfo(f).fileName()));
}

void MainWindow::xrefScene() {
  QString f = QFileDialog::getOpenFileName(this, tr("XRef Scene"), projectSubfolder("scenes"), tr("Veraa scenes (*.veraa)"));
  if (f.isEmpty()) return;
  QString err;
  QVector<uint32_t> ids = mergeScene(f, {}, true, &err);
  if (!err.isEmpty()) QMessageBox::warning(this, tr("XRef Scene"), err);
  else setPrompt(tr("Referenced %1 object(s) from %2 (frozen; File > Reference > Update XRefs reloads)")
                     .arg(ids.size())
                     .arg(QFileInfo(f).fileName()));
}

void MainWindow::reloadXRefs() {
  QVector<XRefEntry> list = xrefScenes(doc_->scene());
  if (list.isEmpty()) return;
  // Remove the old copies, then merge each file again (it may have changed).
  doc_->beginEdit(tr("Update XRefs"));
  for (const XRefEntry& e : list)
    for (uint32_t id : e.nodeIds) {
      doc_->deselect(id);
      doc_->scene().removeNode(id);
    }
  setXRefScenes(doc_->scene(), {});
  QStringList failed;
  for (const XRefEntry& e : list) {
    if (!e.enabled) {
      QVector<XRefEntry> keep = xrefScenes(doc_->scene());
      XRefEntry off = e;
      off.nodeIds.clear();
      keep.push_back(off);
      setXRefScenes(doc_->scene(), keep);
      continue;
    }
    QString err;
    mergeScene(e.path, e.objects, true, &err);
    if (!err.isEmpty()) failed << e.path;
  }
  doc_->clearSelection();
  doc_->notifyStructureChanged();
  doc_->endEdit();
  doc_->setModified(false);
  if (!failed.isEmpty()) setPrompt(tr("Could not update XRefs: %1").arg(failed.join(", ")));
}

void MainWindow::viewImageFile() {
  QString f = QFileDialog::getOpenFileName(this, tr("View File"), projectSubfolder("renderoutput"),
                                           tr("Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.tga *.gif);;All files (*)"));
  if (f.isEmpty()) return;
  QImage img(f);
  if (img.isNull()) {
    QMessageBox::warning(this, tr("View File"), tr("Cannot read %1").arg(f));
    return;
  }
  auto* dlg = new QDialog(this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle(tr("%1 (%2 x %3)").arg(QFileInfo(f).fileName()).arg(img.width()).arg(img.height()));
  auto* v = new QVBoxLayout(dlg);
  auto* area = new QScrollArea(dlg);
  auto* label = new QLabel(area);
  label->setPixmap(QPixmap::fromImage(img));
  area->setWidget(label);
  area->setAlignment(Qt::AlignCenter);
  v->addWidget(area);
  dlg->resize(std::min(img.width() + 40, 1400), std::min(img.height() + 40, 900));
  dlg->show();
}

void MainWindow::showSummaryInfo() {
  const Scene& s = doc_->scene();
  size_t objects[6] = {}, faces = 0, verts = 0, tris = 0;
  for (const auto& n : s.nodes) {
    ++objects[static_cast<int>(n->kind())];
    if (n->kind() == ObjectKind::Geometry) {
      faces += n->mesh().faces.size();
      verts += n->mesh().positions.size();
      tris += n->triMesh().triangleCount();
    }
  }
  Aabb b = s.bounds(true);
  QString text;
  text += tr("Scene: %1\n").arg(doc_->filePath().isEmpty() ? tr("Untitled") : doc_->filePath());
  text += tr("Units: %1\n\n").arg(Units::unitName(Units::instance().display()));
  text += tr("Objects: %1\n").arg(s.nodes.size());
  const char* kinds[6] = {"Geometry", "Shapes", "Lights", "Cameras", "Helpers", "Space Warps"};
  for (int i = 0; i < 6; ++i) text += QString("  %1: %2\n").arg(tr(kinds[i])).arg(objects[i]);
  text += tr("\nVertices: %1\nPolygons: %2\nTriangles: %3\n").arg(verts).arg(faces).arg(tris);
  text += tr("Materials: %1\nLayers: %2\n").arg(s.materials.size()).arg(s.layers.size());
  if (b.valid()) {
    Vec3 e = b.extent();
    const Units& u = Units::instance();
    text += tr("Scene extents: %1 x %2 x %3\n").arg(u.format(e.x), u.format(e.y), u.format(e.z));
  }
  text += tr("\nRender size: %1 x %2\n").arg(s.renderSettings.getInt("width")).arg(s.renderSettings.getInt("height"));
  QDialog dlg(this);
  dlg.setWindowTitle(tr("Summary Info"));
  dlg.resize(460, 520);
  auto* v = new QVBoxLayout(&dlg);
  auto* info = new QPlainTextEdit(text, &dlg);
  info->setReadOnly(true);
  v->addWidget(info);
  v->addWidget(new QLabel(tr("Description:"), &dlg));
  auto* desc = new QPlainTextEdit(&dlg);
  json props = fileProperties(s);
  desc->setPlainText(QString::fromStdString(props.value("comments", std::string())));
  desc->setMaximumHeight(100);
  v->addWidget(desc);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  QPushButton* saveBtn = box->addButton(tr("Save to File..."), QDialogButtonBox::ActionRole);
  v->addWidget(box);
  connect(saveBtn, &QPushButton::clicked, &dlg, [this, text] {
    QString f = QFileDialog::getSaveFileName(this, tr("Save Summary"), QDir(projectSubfolder("scenes")).filePath("summary.txt"),
                                             tr("Text files (*.txt)"));
    QFile out(f);
    if (!f.isEmpty() && out.open(QIODevice::WriteOnly)) out.write(text.toUtf8());
  });
  connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() == QDialog::Accepted && desc->toPlainText().toStdString() != props.value("comments", std::string())) {
    props["comments"] = desc->toPlainText().toStdString();
    setFileProperties(doc_, props);
  }
}

void MainWindow::showFileProperties() {
  json props = fileProperties(doc_->scene());
  QDialog dlg(this);
  dlg.setWindowTitle(tr("File Properties"));
  dlg.resize(480, 460);
  auto* v = new QVBoxLayout(&dlg);
  auto* tabs = new QTabWidget(&dlg);
  v->addWidget(tabs);
  auto* summary = new QWidget(tabs);
  auto* sf = new QFormLayout(summary);
  const char* keys[] = {"title", "subject", "author", "manager", "company", "category", "keywords"};
  const char* labels[] = {"Title:", "Subject:", "Author:", "Manager:", "Company:", "Category:", "Keywords:"};
  QList<QPair<QString, QLineEdit*>> edits;
  for (int i = 0; i < 7; ++i) {
    auto* e = new QLineEdit(QString::fromStdString(props.value(keys[i], std::string())), summary);
    sf->addRow(tr(labels[i]), e);
    edits.push_back({keys[i], e});
  }
  auto* comments = new QPlainTextEdit(QString::fromStdString(props.value("comments", std::string())), summary);
  sf->addRow(tr("Comments:"), comments);
  tabs->addTab(summary, tr("Summary"));

  auto* contents = new QPlainTextEdit(tabs);
  contents->setReadOnly(true);
  {
    const Scene& s = doc_->scene();
    QString t = tr("Objects: %1\nMaterials: %2\nLayers: %3\n\nObjects:\n").arg(s.nodes.size()).arg(s.materials.size()).arg(s.layers.size());
    for (const auto& n : s.nodes) t += "  " + QString::fromStdString(n->name) + "\n";
    t += tr("\nMaterials:\n");
    for (const auto& m : s.materials) t += "  " + QString::fromStdString(m->name) + "\n";
    contents->setPlainText(t);
  }
  tabs->addTab(contents, tr("Contents"));

  auto* custom = new QWidget(tabs);
  auto* cv = new QVBoxLayout(custom);
  auto* table = new QTableWidget(0, 2, custom);
  table->setHorizontalHeaderLabels({tr("Name"), tr("Value")});
  table->horizontalHeader()->setStretchLastSection(true);
  if (props.contains("custom") && props["custom"].is_object())
    for (auto it = props["custom"].begin(); it != props["custom"].end(); ++it) {
      int r = table->rowCount();
      table->insertRow(r);
      table->setItem(r, 0, new QTableWidgetItem(QString::fromStdString(it.key())));
      table->setItem(r, 1, new QTableWidgetItem(QString::fromStdString(it->is_string() ? it->get<std::string>() : it->dump())));
    }
  cv->addWidget(table);
  auto* row = new QHBoxLayout;
  auto* add = new QPushButton(tr("Add"), custom);
  auto* del = new QPushButton(tr("Delete"), custom);
  connect(add, &QPushButton::clicked, table, [table] {
    int r = table->rowCount();
    table->insertRow(r);
    table->setItem(r, 0, new QTableWidgetItem(QObject::tr("Property%1").arg(r + 1)));
    table->setItem(r, 1, new QTableWidgetItem());
    table->editItem(table->item(r, 0));
  });
  connect(del, &QPushButton::clicked, table, [table] {
    if (table->currentRow() >= 0) table->removeRow(table->currentRow());
  });
  row->addWidget(add);
  row->addWidget(del);
  row->addStretch(1);
  cv->addLayout(row);
  tabs->addTab(custom, tr("Custom"));

  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  v->addWidget(box);
  connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;
  json out = json::object();
  for (const auto& [k, e] : edits)
    if (!e->text().isEmpty()) out[k.toStdString()] = e->text().toStdString();
  if (!comments->toPlainText().isEmpty()) out["comments"] = comments->toPlainText().toStdString();
  json c = json::object();
  for (int r = 0; r < table->rowCount(); ++r) {
    QString k = table->item(r, 0) ? table->item(r, 0)->text().trimmed() : QString();
    if (!k.isEmpty()) c[k.toStdString()] = table->item(r, 1) ? table->item(r, 1)->text().toStdString() : std::string();
  }
  if (!c.empty()) out["custom"] = c;
  if (out != props) setFileProperties(doc_, out);
}

void MainWindow::setProjectFolder(bool create) {
  QString start = projectFolder();
  QString dir = QFileDialog::getExistingDirectory(this, create ? tr("Choose a Folder for the New Project") : tr("Set Project Folder"),
                                                  QFileInfo(start).absolutePath());
  if (dir.isEmpty()) return;
  if (create) {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Create Default Project"), tr("Project name:"), QLineEdit::Normal,
                                         tr("VeraaProject"), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    dir = QDir(dir).filePath(name.trimmed());
    if (!createProjectStructure(dir)) {
      QMessageBox::warning(this, tr("Create Project"), tr("Cannot create %1").arg(dir));
      return;
    }
  }
  veraa::setProjectFolder(dir);
  createProjectStructure(dir);
  setPrompt(tr("Project folder: %1").arg(dir));
}

void MainWindow::autoBackup() {
  doc_->commitSoftEdit();
  if (!doc_->isModified() || doc_->scene().nodes.empty()) return;
  QSettings s = appSettings();
  int files = std::max(1, s.value("prefs/autoBackupFiles", 3).toInt());
  int next = s.value("prefs/autoBackupNext", 1).toInt();
  if (next < 1 || next > files) next = 1;
  QString path = QDir(projectSubfolder("autoback")).filePath(QString("AutoBackup%1.veraa").arg(next, 2, 10, QChar('0')));
  std::string keep = doc_->scene().filePath, err;
  bool ok = doc_->scene().save(path.toStdString(), &err);
  doc_->scene().filePath = keep;
  s.setValue("prefs/autoBackupNext", next % files + 1);
  if (ok) setPrompt(tr("Auto backup saved: %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::applyPreferences() {
  QSettings s = appSettings();
  int minutes = s.value("prefs/autoBackupMinutes", 5).toInt();
  autoBackupTimer_->stop();
  if (s.value("prefs/autoBackup", true).toBool() && minutes > 0) autoBackupTimer_->start(minutes * 60 * 1000);
  int levels = s.value("prefs/undoLevels", 0).toInt();
  if (doc_->undoStack()->count() == 0) doc_->undoStack()->setUndoLimit(levels);
  if (spinnerSnap_) setSpinnerSnap(true);
  applyGridLabel();
  updateRecentMenu();
}

}  // namespace veraa
