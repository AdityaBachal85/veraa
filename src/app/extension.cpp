#include "extension.h"

#include <QHash>

#include <algorithm>
#include <string>
#include <vector>

namespace veraa {

namespace {

struct Entry {
  std::string name;
  int order;
  ExtensionInstaller installer;
};

std::vector<Entry>& extensions() {
  static std::vector<Entry> list;
  return list;
}

QHash<QString, CreationToolFactory>& creationTools() {
  static QHash<QString, CreationToolFactory> tools;
  return tools;
}

QHash<QString, ModifyRolloutFactory>& modifyRollouts() {
  static QHash<QString, ModifyRolloutFactory> factories;
  return factories;
}

}  // namespace

void registerModifyRollouts(const QString& type, ModifyRolloutFactory factory) {
  modifyRollouts().insert(type, std::move(factory));
}

ModifyRolloutFactory modifyRolloutsFor(const QString& type) { return modifyRollouts().value(type); }

ExtensionRegistrar::ExtensionRegistrar(const char* name, int order, ExtensionInstaller installer) {
  extensions().push_back({name, order, installer});
}

void runAppExtensions(MainWindow& window) {
  std::vector<Entry> list = extensions();
  std::stable_sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return a.order < b.order; });
  for (const auto& e : list)
    if (e.installer) e.installer(window);
}

void registerCreationTool(const QString& objectType, CreationToolFactory factory) {
  creationTools().insert(objectType, std::move(factory));
}

CreationToolFactory creationToolFor(const QString& objectType) { return creationTools().value(objectType); }

}  // namespace veraa
