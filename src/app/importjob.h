// Runs a file import on a worker thread so the UI stays responsive, with a progress dialog and
// Cancel for long imports.
#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "progress.h"

class QWidget;

namespace veraa {

class Scene;

// Runs `work` on a worker thread and returns its result; the UI keeps repainting meanwhile, and
// after half a second a progress dialog with Cancel (`label`) appears. `work` must not touch the
// document; it reports into and polls the given progress.
bool runInBackground(QWidget* parent, const QString& label,
                     const std::function<bool(ImportProgress& progress, std::string& error)>& work, std::string* error);

// `import` fills a scratch scene on a worker thread (it must report into and poll the given
// progress); the display meshes are then built on all cores, and on success everything is moved
// into `scene` (on the calling thread). `ids` holds the importer's node ids on return of
// `import`; they are translated to the ids in `scene`. Returns false with `error` set on failure
// or when cancelled ("Import cancelled"); `scene` is then unchanged.
using ImportWork = std::function<bool(Scene& into, ImportProgress& progress, std::string& error)>;

bool importInBackground(QWidget* parent, const QString& label, Scene& scene, const ImportWork& import,
                        std::vector<uint32_t>* ids, std::string* error);

}  // namespace veraa
