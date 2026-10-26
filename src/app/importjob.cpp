#include "importjob.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QProgressDialog>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <thread>
#include <unordered_map>

#include "scene.h"

namespace veraa {

bool runInBackground(QWidget* parent, const QString& label,
                     const std::function<bool(ImportProgress& progress, std::string& error)>& work, std::string* error) {
  ImportProgress progress;
  std::string err;
  std::atomic<int> state{0};  // 0 running, 1 succeeded, 2 failed
  std::thread worker([&] {
    bool ok = false;
    try {
      ok = work(progress, err);
    } catch (const std::exception& e) {
      err = e.what();
    } catch (...) {
      err = "Failed";
    }
    state.store(ok ? 1 : 2);
  });

  // Short jobs finish without a dialog. Until it appears, user input is held back (it is
  // delivered afterwards) so nothing edits the scene in the middle of the job.
  QElapsedTimer clock;
  clock.start();
  while (state.load() == 0 && clock.elapsed() < 500) {
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (state.load() == 0) {
    QProgressDialog dlg(label, QCoreApplication::translate("Import", "Cancel"), 0, 1000, parent);
    dlg.setWindowTitle(QCoreApplication::applicationName());
    dlg.setWindowModality(Qt::ApplicationModal);
    dlg.setMinimumDuration(0);
    dlg.setAutoClose(false);
    dlg.setAutoReset(false);
    dlg.setValue(static_cast<int>(progress.fraction() * 1000));
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(30);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
      if (state.load() != 0) loop.quit();
      else if (!progress.cancelled()) dlg.setValue(static_cast<int>(progress.fraction() * 1000));
    });
    QObject::connect(&dlg, &QProgressDialog::canceled, &loop, [&] {
      progress.cancel();
      dlg.setLabelText(QCoreApplication::translate("Import", "Cancelling..."));
    });
    poll.start();
    if (state.load() == 0) loop.exec();
  }
  worker.join();
  if (state.load() != 1) {
    if (error) *error = err.empty() ? "Failed" : err;
    return false;
  }
  return true;
}

bool importInBackground(QWidget* parent, const QString& label, Scene& scene, const ImportWork& import,
                        std::vector<uint32_t>* ids, std::string* error) {
  Scene scratch;
  bool ok = runInBackground(
      parent, label,
      [&](ImportProgress& progress, std::string& err) {
        if (!import(scratch, progress, err)) return false;
        if (!scratch.prepareMeshes(&progress, std::max(progress.fraction(), 0.9), 1.0)) {
          err = "Import cancelled";
          return false;
        }
        return true;
      },
      error);
  if (!ok) {
    if (error && error->empty()) *error = "Import failed";
    return false;
  }
  std::unordered_map<uint32_t, uint32_t> idMap;
  std::vector<uint32_t> all = scene.adopt(scratch, &idMap);
  if (ids) {
    if (ids->empty()) {
      *ids = all;
    } else {
      for (uint32_t& id : *ids) {
        auto it = idMap.find(id);
        id = it != idMap.end() ? it->second : 0;
      }
      ids->erase(std::remove(ids->begin(), ids->end(), 0u), ids->end());
    }
  }
  return true;
}

}  // namespace veraa
