#include "assetlibrary.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "mainwindow.h"
#include "project.h"

namespace veraa {

namespace {

// A bundled folder ("scenes", "assets/hdri") next to the executable, in a parent folder or in
// the source tree.
QString bundledFolder(const QString& relative) {
  QList<QDir> roots = {QDir(QCoreApplication::applicationDirPath()), QDir::current()};
#ifdef VERAA_SOURCE_DIR
  roots.push_back(QDir(QStringLiteral(VERAA_SOURCE_DIR)));
#endif
  for (QDir d : roots)
    for (int i = 0; i < 5; ++i) {
      QString p = d.filePath(relative);
      if (QFileInfo(p).isDir()) return QFileInfo(p).absoluteFilePath();
      if (!d.cdUp()) break;
    }
  return {};
}

bool isScene(const QString& path) { return QFileInfo(path).suffix().compare("veraa", Qt::CaseInsensitive) == 0; }

bool isModel(const QString& path) {
  static const QStringList models = {"obj", "fbx", "gltf", "glb", "stl", "ply", "3ds"};
  return models.contains(QFileInfo(path).suffix().toLower());
}

}  // namespace

AssetLibraryDialog::AssetLibraryDialog(MainWindow* w) : QDialog(w), w_(w) {
  setObjectName("AssetLibrary");
  setWindowTitle(tr("Asset Library"));
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  resize(720, 460);
  auto* v = new QVBoxLayout(this);
  auto* split = new QSplitter(this);
  folders_ = new QTreeWidget(split);
  folders_->setHeaderHidden(true);
  folders_->setRootIsDecorated(false);
  files_ = new QListWidget(split);
  files_->setSelectionMode(QAbstractItemView::SingleSelection);
  split->setStretchFactor(1, 1);
  split->setSizes({200, 500});
  v->addWidget(split, 1);
  auto* hint = new QLabel(tr("Double-click a scene to open it, a model to import it."), this);
  hint->setStyleSheet("color: #a8a8a8;");
  v->addWidget(hint);
  auto* buttons = new QHBoxLayout;
  open_ = new QPushButton(tr("Open"), this);
  merge_ = new QPushButton(tr("Merge"), this);
  import_ = new QPushButton(tr("Import"), this);
  reveal_ = new QPushButton(tr("Show in Folder"), this);
  auto* close = new QPushButton(tr("Close"), this);
  for (QPushButton* b : {open_, merge_, import_, reveal_}) buttons->addWidget(b);
  buttons->addStretch(1);
  buttons->addWidget(close);
  v->addLayout(buttons);

  for (const auto& f : libraryFolders()) {
    auto* item = new QTreeWidgetItem(folders_, {f.first});
    item->setData(0, Qt::UserRole, f.second);
    item->setToolTip(0, f.second);
  }
  connect(folders_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
    if (item) showFolder(item->data(0, Qt::UserRole).toString());
  });
  connect(files_, &QListWidget::currentRowChanged, this, [this] { refreshButtons(); });
  connect(files_, &QListWidget::itemDoubleClicked, this, [this] {
    QString f = currentFile();
    if (isScene(f)) openCurrent(false);
    else if (isModel(f)) import_->click();
    else reveal_->click();
  });
  connect(open_, &QPushButton::clicked, this, [this] { openCurrent(false); });
  connect(merge_, &QPushButton::clicked, this, [this] { openCurrent(true); });
  connect(import_, &QPushButton::clicked, this, [this] {
    QString f = currentFile();
    QString err;
    QVector<uint32_t> ids;
    if (!f.isEmpty() && !w_->importPath(f, &ids, &err))
      QMessageBox::warning(this, tr("Import"), tr("Could not import %1:\n%2").arg(f, err));
  });
  connect(reveal_, &QPushButton::clicked, this, [this] {
    QString f = currentFile();
    if (!f.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(f).absolutePath()));
  });
  connect(close, &QPushButton::clicked, this, &QDialog::close);
  if (folders_->topLevelItemCount() > 0) folders_->setCurrentItem(folders_->topLevelItem(0));
  refreshButtons();
}

QList<QPair<QString, QString>> AssetLibraryDialog::libraryFolders() const {
  QList<QPair<QString, QString>> out;
  auto add = [&out](const QString& name, const QString& path) {
    if (!path.isEmpty() && QFileInfo(path).isDir()) out.push_back({name, path});
  };
  add(tr("Sample Scenes"), bundledFolder("scenes"));
  add(tr("HDR Environments"), bundledFolder("assets/hdri"));
  add(tr("IES Profiles"), bundledFolder("assets/ies"));
  add(tr("Project Scenes"), projectSubfolder("scenes"));
  add(tr("Project Imports"), projectSubfolder("import"));
  add(tr("Project Maps"), projectSubfolder("sceneassets"));
  return out;
}

void AssetLibraryDialog::showFolder(const QString& path) {
  files_->clear();
  QDir dir(path);
  for (const QFileInfo& fi : dir.entryInfoList(QDir::Files, QDir::Name)) {
    auto* item = new QListWidgetItem(fi.fileName(), files_);
    item->setData(Qt::UserRole, fi.absoluteFilePath());
    item->setToolTip(fi.absoluteFilePath());
  }
  if (files_->count() > 0) files_->setCurrentRow(0);
  refreshButtons();
}

QString AssetLibraryDialog::currentFile() const {
  QListWidgetItem* item = files_->currentItem();
  return item ? item->data(Qt::UserRole).toString() : QString();
}

void AssetLibraryDialog::refreshButtons() {
  QString f = currentFile();
  open_->setEnabled(isScene(f));
  merge_->setEnabled(isScene(f));
  import_->setEnabled(isModel(f));
  reveal_->setEnabled(!f.isEmpty());
}

void AssetLibraryDialog::openCurrent(bool merge) {
  QString f = currentFile();
  if (!isScene(f)) return;
  if (merge) {
    QString err;
    QVector<uint32_t> ids = w_->mergeScene(f, {}, false, &err);
    if (ids.isEmpty() && !err.isEmpty()) QMessageBox::warning(this, tr("Merge"), err);
    return;
  }
  if (w_->document()->isModified()) {
    auto r = QMessageBox::question(this, tr("Asset Library"), tr("The scene has been modified.\nSave your changes first?"),
                                   QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return;
    if (r == QMessageBox::Save) {
      w_->command("file.save")->trigger();
      if (w_->document()->isModified()) return;  // the save was cancelled
    }
  }
  w_->openFile(f);
}

}  // namespace veraa
