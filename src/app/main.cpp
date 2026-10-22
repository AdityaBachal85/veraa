#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QSurfaceFormat>
#include <QTimer>

#include "mainwindow.h"
#include "selftest.h"
#include "theme.h"

int main(int argc, char** argv) {
  QSurfaceFormat fmt;
  fmt.setVersion(3, 3);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  fmt.setStencilBufferSize(8);
  fmt.setSamples(4);
  QSurfaceFormat::setDefaultFormat(fmt);

  QApplication app(argc, argv);
  QApplication::setApplicationName("Veraa");
  QApplication::setOrganizationName("Veraa");
  QApplication::setApplicationVersion(VERAA_VERSION);
  veraa::applyDarkTheme(app);

  QCommandLineParser parser;
  parser.setApplicationDescription("Veraa - 3D modeling and physically based rendering");
  parser.addHelpOption();
  parser.addVersionOption();
  QCommandLineOption shot("screenshot", "Save screenshots to <file> and exit (testing).", "file");
  QCommandLineOption script("script", "Run a built-in script: demo or selftest (testing).", "name");
  parser.addOption(shot);
  parser.addOption(script);
  parser.addPositionalArgument("scene", "Scene file (.veraa) to open.");
  parser.process(app);

  veraa::MainWindow window;
  window.show();

  const QStringList files = parser.positionalArguments();
  if (!files.isEmpty() && QFileInfo::exists(files.first())) window.openFile(files.first());

  if (parser.isSet(shot) || parser.isSet(script)) {
    QString name = parser.value(script);
    QString path = parser.value(shot);
    QTimer::singleShot(200, &window, [&window, name, path] {
      int code = veraa::runScript(window, name, path);
      QApplication::exit(code);
    });
  }
  return app.exec();
}
