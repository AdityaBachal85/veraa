// Project folder (File > Project, Customize > Configure Project Paths): a root folder with
// standard subfolders for scenes, renders, backups, imports, exports, maps, scripts and previews.
#pragma once

#include <QString>
#include <QStringList>

namespace veraa {

// Keys of the standard subfolders, in display order.
QStringList projectSubfolderKeys();
QString projectSubfolderLabel(const QString& key);

QString projectFolder();
void setProjectFolder(const QString& root);
// Folder name used for a key (configurable); the default is the key itself.
QString projectSubfolderName(const QString& key);
void setProjectSubfolderName(const QString& key, const QString& name);
// Absolute path of a subfolder ("scenes", "renderoutput", "autoback", ...), created on demand.
QString projectSubfolder(const QString& key);
// Creates the root and all subfolders; returns false if the root cannot be created.
bool createProjectStructure(const QString& root);

// User and system paths (Customize > Configure User and System Paths).
QString userPath(const QString& key);  // "scripts", "startupScripts", "plugins", "temp"
void setUserPath(const QString& key, const QString& path);

}  // namespace veraa
