// SceneDocument owns the scene, the selection and the undo history. Every edit goes
// through it so views stay in sync and the edit can be undone.
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QUndoStack>
#include <QSet>
#include <QVector>

#include <cstdint>
#include <functional>
#include <unordered_map>

#include "scene.h"

namespace veraa {

enum ChangeFlags : int {
  ChangeTransform = 1,
  ChangeGeometry = 2,
  ChangeMaterial = 4,
  ChangeName = 8,
  ChangeDisplay = 16,  // hidden flag, wire color
  ChangeAll = 31,
};

class SceneDocument : public QObject {
  Q_OBJECT
 public:
  explicit SceneDocument(QObject* parent = nullptr);

  Scene& scene() { return scene_; }
  const Scene& scene() const { return scene_; }
  QUndoStack* undoStack() { return &undo_; }

  // ---- Selection (node ids) ----
  const QVector<uint32_t>& selection() const { return selection_; }
  QVector<Node*> selectedNodes() const;
  Node* firstSelected() const;
  bool isSelected(uint32_t id) const { return selectionSet_.contains(id); }
  void setSelection(const QVector<uint32_t>& ids);
  void select(uint32_t id, bool add);
  void deselect(uint32_t id);
  void clearSelection() { setSelection({}); }
  // Selection center (average pivot) of selected nodes.
  Vec3 selectionCenter() const;
  Aabb selectionBounds() const;

  // ---- Edits ----
  // Hard edits: snapshot on begin, push one undo step on end (nesting is allowed).
  void beginEdit(const QString& label);
  void endEdit();
  void cancelEdit();  // restores the begin snapshot
  bool inEdit() const { return editDepth_ > 0; }
  // Soft edits merge a burst of changes (spinner drags, typing) into one undo step. They commit
  // after a short idle time, or when another edit starts. key identifies the control.
  void softEdit(const QString& label, const QString& key);
  void commitSoftEdit();

  // ---- Change notification (call after modifying the scene directly) ----
  void notifyNodesChanged(const QVector<uint32_t>& ids, int flags);
  void notifyNodeChanged(Node* node, int flags) { notifyNodesChanged({node->id}, flags); }
  void notifyStructureChanged();  // nodes added/removed/reordered
  void notifyMaterialsChanged();
  void notifyRenderSettingsChanged();

  // GPU cache stamp for a node's geometry; changes whenever its mesh may have changed.
  uint64_t geometryStamp(uint32_t nodeId) const;

  // ---- High-level operations (each is one undo step) ----
  Node* createNode(const QString& type);  // inside an open edit
  void deleteSelected();
  QVector<uint32_t> cloneSelected(bool select = true);  // inside an open edit
  void hideSelected();
  void unhideAll();
  void assignMaterialToSelection(uint32_t materialId);

  // ---- File ----
  void newScene();
  bool load(const QString& path, QString* error);
  bool save(const QString& path, QString* error);
  QString filePath() const { return QString::fromStdString(scene_.filePath); }
  bool isModified() const { return !undo_.isClean() || dirtyWithoutUndo_; }
  void setModified(bool m);

  // Makes the scene match a snapshot (undo/redo), rebuilding only the nodes that differ; keeps
  // the file path.
  void restore(const SceneSnapshot& state, const QVector<uint32_t>& sel);

  // Called when an edit is committed, before its undo step is recorded, with the scene as it was
  // before the edit. The animation module uses it for Auto Key (turning changes into keys).
  using CommitHook = std::function<void(const SceneSnapshot& before)>;
  void setCommitHook(CommitHook hook) { commitHook_ = std::move(hook); }

 Q_SIGNALS:
  void selectionChanged();
  void nodesChanged(const QVector<uint32_t>& ids, int flags);
  void structureChanged();
  void materialsChanged();
  void renderSettingsChanged();
  void modifiedChanged(bool modified);
  void fileChanged();

 private:
  void bumpGeometry(uint32_t id) { ++geomVersion_[id]; }
  void pushSnapshot(const QString& label, std::shared_ptr<const SceneSnapshot> before, const QVector<uint32_t>& selBefore);
  // Captures the scene, sharing unchanged entries with the last capture.
  std::shared_ptr<const SceneSnapshot> capture();

  Scene scene_;
  QUndoStack undo_;
  QVector<uint32_t> selection_;
  QSet<uint32_t> selectionSet_;  // same ids, for O(1) isSelected
  void setSelectionList(QVector<uint32_t> ids);
  int editDepth_ = 0;
  QString editLabel_;
  std::shared_ptr<const SceneSnapshot> editBefore_;
  std::shared_ptr<const SceneSnapshot> last_;  // most recent capture (entries are shared with it)
  QVector<uint32_t> editSelBefore_;
  // Soft edit state.
  bool softOpen_ = false;
  QString softKey_, softLabel_;
  std::shared_ptr<const SceneSnapshot> softBefore_;
  QVector<uint32_t> softSelBefore_;
  QTimer softTimer_;
  uint64_t epoch_ = 1;
  std::unordered_map<uint32_t, uint64_t> geomVersion_;
  bool dirtyWithoutUndo_ = false;
  CommitHook commitHook_;
  bool inCommitHook_ = false;
};

}  // namespace veraa
