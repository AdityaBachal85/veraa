// Content > Asset Library: browses the bundled sample scenes and assets and the project's
// scene, import and map folders; scenes open or merge, models import, other files are shown in
// their folder.
#pragma once

#include <QDialog>

class QListWidget;
class QPushButton;
class QTreeWidget;

namespace veraa {

class MainWindow;

class AssetLibraryDialog : public QDialog {
  Q_OBJECT
 public:
  explicit AssetLibraryDialog(MainWindow* w);
  // Folders listed on the left (name, absolute path); missing folders are left out.
  QList<QPair<QString, QString>> libraryFolders() const;
  void showFolder(const QString& path);
  QString currentFile() const;

 private:
  void refreshButtons();
  void openCurrent(bool merge);

  MainWindow* w_;
  QTreeWidget* folders_;
  QListWidget* files_;
  QPushButton *open_, *merge_, *import_, *reveal_;
};

}  // namespace veraa
