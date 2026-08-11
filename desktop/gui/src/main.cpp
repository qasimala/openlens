// SPDX-License-Identifier: GPL-2.0-or-later
#include "main_window.hpp"

#include <QApplication>

#include <csignal>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QTimer>

namespace {
QIcon appIcon() {
  QPixmap image(128, 128);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setBrush(QColor("#38d996"));
  painter.setPen(Qt::NoPen);
  painter.drawRoundedRect(8, 20, 112, 88, 25, 25);
  painter.setBrush(QColor("#071a14"));
  painter.drawEllipse(36, 28, 64, 64);
  painter.setBrush(QColor("#a6f6d4"));
  painter.drawEllipse(48, 40, 40, 40);
  painter.setBrush(QColor("#38d996"));
  painter.drawEllipse(58, 50, 20, 20);
  return QIcon(image);
}
} // namespace

int main(int argc, char* argv[]) {
  // Transport writes can race a peer disconnect; the error path handles EPIPE.
  std::signal(SIGPIPE, SIG_IGN);
  QApplication application(argc, argv);
  QApplication::setApplicationName("OpenLens Desktop");
  QApplication::setApplicationDisplayName("OpenLens");
  QApplication::setOrganizationName("OpenLens");
  QApplication::setApplicationVersion(OPENLENS_VERSION);
  QApplication::setWindowIcon(appIcon());
  MainWindow window;
  window.show();
  const QStringList arguments = QApplication::arguments();
  if (arguments.contains(QStringLiteral("--smoke-test")))
    QTimer::singleShot(1200, &application, &QCoreApplication::quit);
  const qsizetype screenshotOption = arguments.indexOf(QStringLiteral("--screenshot"));
  if (screenshotOption >= 0 && screenshotOption + 1 < arguments.size()) {
    const QString path = arguments.at(screenshotOption + 1);
    QTimer::singleShot(1500, &window, [&application, &window, path] {
      window.grab().save(path);
      application.quit();
    });
  }
  return QApplication::exec();
}
