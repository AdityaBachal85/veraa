// Scene Explorer: list of all objects with selection sync, rename and visibility toggles.
#pragma once

#include <QWidget>

#include "document.h"

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace veraa {

class SceneExplorer : public QWidget {
  Q_OBJECT
 public:
  explicit SceneExplorer(SceneDocument* doc, QWidget* parent = nullptr);

 private:
  void rebuild();
  void syncSelection();
  void applyFilter();

  SceneDocument* doc_;
  QLineEdit* filter_;
  QTreeWidget* tree_;
  bool syncing_ = false;
  bool rebuildPending_ = false;
};

}  // namespace veraa
