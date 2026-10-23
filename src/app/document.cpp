#include "document.h"

#include <QFileInfo>
#include <QUndoCommand>

#include <map>

namespace veraa {

namespace {

class SnapshotCommand : public QUndoCommand {
 public:
  SnapshotCommand(SceneDocument* doc, const QString& label, std::shared_ptr<const SceneSnapshot> before,
                  std::shared_ptr<const SceneSnapshot> after, QVector<uint32_t> selBefore, QVector<uint32_t> selAfter)
      : QUndoCommand(label),
        doc_(doc),
        before_(std::move(before)),
        after_(std::move(after)),
        selBefore_(std::move(selBefore)),
        selAfter_(std::move(selAfter)) {}
  void undo() override { doc_->restore(*before_, selBefore_); }
  void redo() override {
    if (first_) {  // the change is already applied
      first_ = false;
      return;
    }
    doc_->restore(*after_, selAfter_);
  }

 private:
  SceneDocument* doc_;
  std::shared_ptr<const SceneSnapshot> before_, after_;
  QVector<uint32_t> selBefore_, selAfter_;
  bool first_ = true;
};

}  // namespace

SceneDocument::SceneDocument(QObject* parent) : QObject(parent) {
  undo_.setUndoLimit(200);
  softTimer_.setSingleShot(true);
  softTimer_.setInterval(700);
  connect(&softTimer_, &QTimer::timeout, this, &SceneDocument::commitSoftEdit);
  connect(&undo_, &QUndoStack::cleanChanged, this, [this](bool) { Q_EMIT modifiedChanged(isModified()); });
}

QVector<Node*> SceneDocument::selectedNodes() const {
  QVector<Node*> out;
  for (uint32_t id : selection_)
    if (Node* n = scene_.findNode(id)) out.push_back(n);
  return out;
}

Node* SceneDocument::firstSelected() const {
  for (uint32_t id : selection_)
    if (Node* n = scene_.findNode(id)) return n;
  return nullptr;
}

void SceneDocument::setSelectionList(QVector<uint32_t> ids) {
  selection_ = std::move(ids);
  selectionSet_ = QSet<uint32_t>(selection_.begin(), selection_.end());
}

void SceneDocument::setSelection(const QVector<uint32_t>& ids) {
  QVector<uint32_t> clean;
  QSet<uint32_t> seen;
  clean.reserve(ids.size());
  for (uint32_t id0 : ids) {
    // Members of a closed group select the (outermost closed) group instead.
    uint32_t id = id0;
    for (Node* g = scene_.findNode(id0); g && g->groupMember;) {
      Node* p = g->parent();
      if (!p) break;
      if (p->groupHead && !p->groupOpen) id = p->id;
      g = p;
    }
    Node* n = scene_.findNode(id);
    if (n && !n->hidden && !seen.contains(id)) {
      seen.insert(id);
      clean.push_back(id);
    }
  }
  if (clean == selection_) return;
  setSelectionList(std::move(clean));
  Q_EMIT selectionChanged();
}

void SceneDocument::select(uint32_t id, bool add) {
  if (add) {
    if (selectionSet_.contains(id)) return;
    QVector<uint32_t> s = selection_;
    s.push_back(id);
    setSelection(s);
  } else {
    setSelection({id});
  }
}

void SceneDocument::deselect(uint32_t id) {
  QVector<uint32_t> s = selection_;
  s.removeAll(id);
  setSelection(s);
}

Vec3 SceneDocument::selectionCenter() const {
  auto nodes = selectedNodes();
  if (nodes.isEmpty()) return Vec3();
  if (nodes.size() == 1) return nodes[0]->transform.position;
  Aabb b = selectionBounds();
  return b.valid() ? b.center() : Vec3();
}

Aabb SceneDocument::selectionBounds() const {
  Aabb b;
  for (Node* n : selectedNodes()) b.expand(n->worldBounds());
  return b;
}

std::shared_ptr<const SceneSnapshot> SceneDocument::capture() {
  auto s = scene_.snapshot(last_.get());
  if (last_ && s->sameAs(*last_)) return last_;  // unchanged: share the whole capture
  last_ = s;
  return s;
}

void SceneDocument::beginEdit(const QString& label) {
  if (editDepth_ == 0) {
    commitSoftEdit();
    editLabel_ = label;
    editBefore_ = capture();
    editSelBefore_ = selection_;
  }
  ++editDepth_;
}

void SceneDocument::endEdit() {
  if (editDepth_ <= 0) return;
  if (--editDepth_ > 0) return;
  pushSnapshot(editLabel_, std::move(editBefore_), editSelBefore_);
  editBefore_.reset();
}

void SceneDocument::cancelEdit() {
  if (editDepth_ <= 0) return;
  editDepth_ = 0;
  if (editBefore_) restore(*editBefore_, editSelBefore_);
  editBefore_.reset();
}

void SceneDocument::softEdit(const QString& label, const QString& key) {
  if (editDepth_ > 0) return;  // part of an open hard edit
  if (softOpen_ && softKey_ != key) commitSoftEdit();
  if (!softOpen_) {
    softOpen_ = true;
    softKey_ = key;
    softLabel_ = label;
    softBefore_ = capture();
    softSelBefore_ = selection_;
  }
  softTimer_.start();
}

void SceneDocument::commitSoftEdit() {
  softTimer_.stop();
  if (!softOpen_) return;
  softOpen_ = false;
  pushSnapshot(softLabel_, std::move(softBefore_), softSelBefore_);
  softBefore_.reset();
}

void SceneDocument::pushSnapshot(const QString& label, std::shared_ptr<const SceneSnapshot> before,
                                 const QVector<uint32_t>& selBefore) {
  if (!before) return;
  if (commitHook_ && !inCommitHook_) {
    inCommitHook_ = true;
    commitHook_(*before);
    inCommitHook_ = false;
  }
  std::shared_ptr<const SceneSnapshot> after = capture();
  if ((after == before || after->sameAs(*before)) && selBefore == selection_) return;
  undo_.push(new SnapshotCommand(this, label, std::move(before), std::move(after), selBefore, selection_));
  Q_EMIT modifiedChanged(isModified());
}

void SceneDocument::restore(const SceneSnapshot& state, const QVector<uint32_t>& sel) {
  SceneRestoreResult r = scene_.restore(state);
  last_.reset();  // the next capture compares from scratch (entries are still shared by equality)
  for (uint32_t id : r.geometryNodes) bumpGeometry(id);
  QVector<uint32_t> newSel;
  for (uint32_t id : sel)
    if (scene_.findNode(id)) newSel.push_back(id);
  bool selChanged = newSel != selection_;
  setSelectionList(std::move(newSel));
  // Panels keep pointers to objects and modifiers, so rebuilt geometry counts as a structure change.
  if (r.structure || r.globals || !r.geometryNodes.empty()) {
    Q_EMIT structureChanged();
  } else {
    // Group the changed nodes by what changed.
    std::map<int, QVector<uint32_t>> byFlags;
    for (const auto& [id, what] : r.nodeChanges) byFlags[what].push_back(id);
    for (const auto& [flags, ids] : byFlags) Q_EMIT nodesChanged(ids, flags);
  }
  if (r.materials) Q_EMIT materialsChanged();
  if (r.renderSettings) Q_EMIT renderSettingsChanged();
  if (selChanged || r.structure) Q_EMIT selectionChanged();
}

void SceneDocument::notifyNodesChanged(const QVector<uint32_t>& ids, int flags) {
  if (flags & ChangeGeometry)
    for (uint32_t id : ids) {
      if (Node* n = scene_.findNode(id)) n->invalidate();
      bumpGeometry(id);
    }
  if (flags & ChangeDisplay) {
    // Hidden nodes cannot stay selected.
    QVector<uint32_t> s;
    for (uint32_t id : selection_)
      if (Node* n = scene_.findNode(id); n && !n->hidden) s.push_back(id);
    if (s != selection_) {
      setSelectionList(s);
      Q_EMIT selectionChanged();
    }
  }
  Q_EMIT nodesChanged(ids, flags);
}

void SceneDocument::notifyStructureChanged() {
  QVector<uint32_t> s;
  for (uint32_t id : selection_)
    if (scene_.findNode(id)) s.push_back(id);
  bool selChanged = s != selection_;
  setSelectionList(s);
  Q_EMIT structureChanged();
  if (selChanged) Q_EMIT selectionChanged();
}

void SceneDocument::notifyMaterialsChanged() { Q_EMIT materialsChanged(); }
void SceneDocument::notifyRenderSettingsChanged() { Q_EMIT renderSettingsChanged(); }

uint64_t SceneDocument::geometryStamp(uint32_t nodeId) const {
  auto it = geomVersion_.find(nodeId);
  uint64_t v = it == geomVersion_.end() ? 0 : it->second;
  // Include the node's own evaluation stamp so instances refresh when another instance changes.
  const Node* n = scene_.findNode(nodeId);
  uint64_t g = n ? n->geometryStamp() : 0;
  return (((epoch_ << 32) ^ v) * 1000003ull) ^ g;
}

Node* SceneDocument::createNode(const QString& type) {
  Node* n = scene_.createNode(type.toStdString());
  if (n) notifyStructureChanged();
  return n;
}

void SceneDocument::deleteSelected() {
  if (selection_.isEmpty()) return;
  beginEdit(tr("Delete"));
  for (uint32_t id : selection_) scene_.removeNode(id);
  setSelectionList({});
  notifyStructureChanged();
  Q_EMIT selectionChanged();
  endEdit();
}

QVector<uint32_t> SceneDocument::cloneSelected(bool doSelect) {
  QVector<uint32_t> created;
  for (Node* n : selectedNodes()) {
    Node* c = scene_.addNode(n->clone());
    created.push_back(c->id);
  }
  notifyStructureChanged();
  if (doSelect) setSelection(created);
  return created;
}

void SceneDocument::hideSelected() {
  if (selection_.isEmpty()) return;
  beginEdit(tr("Hide Selection"));
  QVector<uint32_t> ids = selection_;
  for (Node* n : selectedNodes()) n->hidden = true;
  notifyNodesChanged(ids, ChangeDisplay);
  endEdit();
}

void SceneDocument::unhideAll() {
  beginEdit(tr("Unhide All"));
  QVector<uint32_t> ids;
  for (auto& n : scene_.nodes)
    if (n->hidden) {
      n->hidden = false;
      ids.push_back(n->id);
    }
  notifyNodesChanged(ids, ChangeDisplay);
  endEdit();
}

void SceneDocument::assignMaterialToSelection(uint32_t materialId) {
  QVector<uint32_t> ids;
  for (Node* n : selectedNodes())
    if (n->kind() == ObjectKind::Geometry) ids.push_back(n->id);
  if (ids.isEmpty()) return;
  beginEdit(tr("Assign Material"));
  for (uint32_t id : ids) scene_.findNode(id)->materialId = materialId;
  notifyNodesChanged(ids, ChangeMaterial);
  endEdit();
}

void SceneDocument::newScene() {
  commitSoftEdit();
  scene_.clear();
  undo_.clear();
  last_.reset();
  setSelectionList({});
  ++epoch_;
  geomVersion_.clear();
  dirtyWithoutUndo_ = false;
  Q_EMIT structureChanged();
  Q_EMIT materialsChanged();
  Q_EMIT renderSettingsChanged();
  Q_EMIT selectionChanged();
  Q_EMIT fileChanged();
  Q_EMIT modifiedChanged(false);
}

bool SceneDocument::load(const QString& path, QString* error) {
  commitSoftEdit();
  Scene tmp;
  std::string err;
  if (!tmp.load(path.toStdString(), &err)) {
    if (error) *error = QString::fromStdString(err);
    return false;
  }
  // Move the loaded scene over without re-serializing it (meshes are shared, not copied).
  std::shared_ptr<const SceneSnapshot> loaded = tmp.snapshot();
  scene_.clear();
  scene_.restore(*loaded);
  scene_.filePath = path.toStdString();
  last_.reset();
  undo_.clear();
  setSelectionList({});
  ++epoch_;
  geomVersion_.clear();
  dirtyWithoutUndo_ = false;
  Q_EMIT structureChanged();
  Q_EMIT materialsChanged();
  Q_EMIT renderSettingsChanged();
  Q_EMIT selectionChanged();
  Q_EMIT fileChanged();
  Q_EMIT modifiedChanged(false);
  return true;
}

bool SceneDocument::save(const QString& path, QString* error) {
  commitSoftEdit();
  std::string err;
  if (!scene_.save(path.toStdString(), &err)) {
    if (error) *error = QString::fromStdString(err);
    return false;
  }
  undo_.setClean();
  dirtyWithoutUndo_ = false;
  Q_EMIT fileChanged();
  Q_EMIT modifiedChanged(false);
  return true;
}

void SceneDocument::setModified(bool m) {
  dirtyWithoutUndo_ = m;
  Q_EMIT modifiedChanged(isModified());
}

}  // namespace veraa
