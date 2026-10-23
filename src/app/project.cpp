#include "project.h"

#include <QDir>
#include <QObject>
#include <QSettings>
#include <QStandardPaths>

namespace veraa {

namespace {
QSettings settings() { return QSettings("Veraa", "Veraa"); }
}  // namespace

QStringList projectSubfolderKeys() {
  return {"scenes", "renderoutput", "autoback", "import", "export", "maps", "scripts", "previews"};
}

QString projectSubfolderLabel(const QString& key) {
  if (key == "scenes") return QObject::tr("Scenes");
  if (key == "renderoutput") return QObject::tr("Render Output");
  if (key == "autoback") return QObject::tr("Auto Backup");
  if (key == "import") return QObject::tr("Import");
  if (key == "export") return QObject::tr("Export");
  if (key == "maps") return QObject::tr("Maps (textures)");
  if (key == "scripts") return QObject::tr("Scripts");
  if (key == "previews") return QObject::tr("Previews");
  return key;
}

QString projectFolder() {
  QString def = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath("Veraa");
  return settings().value("project/root", def).toString();
}

void setProjectFolder(const QString& root) {
  QSettings s = settings();
  s.setValue("project/root", QDir(root).absolutePath());
}

QString projectSubfolderName(const QString& key) { return settings().value("project/folders/" + key, key).toString(); }

void setProjectSubfolderName(const QString& key, const QString& name) {
  QSettings s = settings();
  s.setValue("project/folders/" + key, name.trimmed().isEmpty() ? key : name.trimmed());
}

QString projectSubfolder(const QString& key) {
  QString path = QDir(projectFolder()).filePath(projectSubfolderName(key));
  QDir().mkpath(path);
  return path;
}

bool createProjectStructure(const QString& root) {
  if (!QDir().mkpath(root)) return false;
  for (const QString& k : projectSubfolderKeys()) QDir(root).mkpath(projectSubfolderName(k));
  return true;
}

QString userPath(const QString& key) {
  QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QString def;
  if (key == "scripts") def = QDir(base).filePath("scripts");
  else if (key == "startupScripts") def = QDir(base).filePath("scripts/startup");
  else if (key == "plugins") def = QDir(base).filePath("plugins");
  else if (key == "temp") def = QDir::tempPath();
  else def = base;
  return settings().value("paths/" + key, def).toString();
}

void setUserPath(const QString& key, const QString& path) {
  QSettings s = settings();
  s.setValue("paths/" + key, path);
}

}  // namespace veraa
