// Rendering animations: frame ranges to image sequences or a motion-JPEG AVI, and viewport
// previews (Tools > Preview - Grab Viewport).
#pragma once

#include <QString>

#include <functional>
#include <vector>

namespace veraa {

class MainWindow;

struct RenderAnimOptions {
  std::vector<double> frames;  // frames to render, in order
  int width = 0, height = 0;   // 0 = scene render settings
  int samples = 0;             // samples per pixel, 0 = scene setting
  double timeLimit = 0.0;      // seconds per frame, 0 = scene setting
  int format = 0;              // 0 PNG sequence, 1 JPEG sequence, 2 AVI (motion JPEG)
  int quality = 90;            // JPEG / AVI quality
  QString path;                // sequence base name ("C:/out/shot.png") or the AVI file
  int numberBase = 0;          // added to frame numbers in sequence file names
};

// Parses "0-10, 15, 20-30" into frames (with every-nth step for ranges).
std::vector<double> parseFrameList(const QString& text, int nth = 1);
// Sequence file name for a frame: base "shot.png" -> "shot0012.png".
QString sequenceFileName(const QString& base, int frame);

// Renders the frames with the production renderer. progress(done, total) returns false to cancel.
// Returns the number of frames written.
int renderAnimation(MainWindow& w, const RenderAnimOptions& o, const std::function<bool(int, int)>& progress,
                    QString* error = nullptr);

// Grabs the active viewport for each frame into an AVI (or PNG sequence when path ends in .png).
int createViewportPreview(MainWindow& w, const std::vector<double>& frames, const QString& path, int percentSize,
                          const std::function<bool(int, int)>& progress, QString* error = nullptr);

}  // namespace veraa
