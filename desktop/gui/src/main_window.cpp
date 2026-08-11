// SPDX-License-Identifier: GPL-2.0-or-later
#include "main_window.hpp"

#include "openlens/media.hpp"
#include "openlens/session.hpp"
#include "openlens/usb_transport.hpp"
#include "openlens/wifi_discovery.hpp"
#include "openlens/wifi_transport.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QFuture>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace {
constexpr auto green = "#38d996";
constexpr auto muted = "#8faea0";
constexpr auto red = "#ff8078";
constexpr quint16 installerPort = 8765;

bool isPrivateIpv4(const QHostAddress& address) {
  const quint32 value = address.toIPv4Address();
  return (value & 0xff000000U) == 0x0a000000U ||
         (value & 0xfff00000U) == 0xac100000U ||
         (value & 0xffff0000U) == 0xc0a80000U;
}

QHostAddress localInstallAddress() {
  QHostAddress fallback;
  for (const auto& address : QNetworkInterface::allAddresses()) {
    if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback())
      continue;
    if (isPrivateIpv4(address))
      return address;
    if (fallback.isNull())
      fallback = address;
  }
  return fallback;
}

QString localSubnet(const QHostAddress& wanted) {
  for (const auto& interface : QNetworkInterface::allInterfaces()) {
    for (const auto& entry : interface.addressEntries()) {
      if (entry.ip() != wanted || entry.prefixLength() < 0)
        continue;
      const int prefix = entry.prefixLength();
      const auto subnet = QHostAddress::parseSubnet(
          wanted.toString() + QStringLiteral("/") + QString::number(prefix));
      if (!subnet.first.isNull())
        return subnet.first.toString() + QStringLiteral("/") + QString::number(prefix);
    }
  }
  return wanted.toString() + QStringLiteral("/32");
}

class ApkServer final : public QObject {
public:
  explicit ApkServer(QString apkPath) : apkPath_(std::move(apkPath)) {
    connect(&server_, &QTcpServer::newConnection, this, [this] {
      while (server_.hasPendingConnections()) {
        auto* socket = server_.nextPendingConnection();
        socket->setParent(this);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
          if (socket->property("openlensServed").toBool() || !socket->canReadLine())
            return;
          socket->setProperty("openlensServed", true);
          const QByteArray request = socket->readLine(4096);
          socket->readAll();
          if (!request.startsWith("GET /openlens.apk ")) {
            socket->write("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            socket->disconnectFromHost();
            return;
          }
          QFile apk(apkPath_);
          if (!apk.open(QIODevice::ReadOnly)) {
            socket->write("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n"
                          "Content-Length: 0\r\n\r\n");
            socket->disconnectFromHost();
            return;
          }
          const QByteArray contents = apk.readAll();
          const QByteArray headers =
              QByteArrayLiteral("HTTP/1.1 200 OK\r\n"
                                "Content-Type: application/vnd.android.package-archive\r\n"
                                "Content-Disposition: attachment; filename=OpenLens.apk\r\n"
                                "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: ") +
              QByteArray::number(contents.size()) + QByteArrayLiteral("\r\n\r\n");
          socket->write(headers);
          socket->write(contents);
          socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }

  QUrl start() {
    const QHostAddress address = localInstallAddress();
    if (address.isNull())
      throw std::runtime_error("no local IPv4 network address is available");
    if (!server_.listen(QHostAddress::AnyIPv4, installerPort))
      throw std::runtime_error(server_.errorString().toStdString());
    QUrl result;
    result.setScheme(QStringLiteral("http"));
    result.setHost(address.toString());
    result.setPort(static_cast<int>(server_.serverPort()));
    result.setPath(QStringLiteral("/openlens.apk"));
    return result;
  }

private:
  QString apkPath_;
  QTcpServer server_;
};

QPixmap qrCode(const QString& value) {
  QProcess process;
  process.start(QStringLiteral("qrencode"),
                {QStringLiteral("-t"), QStringLiteral("PNG"), QStringLiteral("-o"),
                 QStringLiteral("-"), QStringLiteral("-s"), QStringLiteral("9"),
                 QStringLiteral("-m"), QStringLiteral("2"), value});
  if (!process.waitForStarted(2000) || !process.waitForFinished(5000) ||
      process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    return {};
  QPixmap image;
  image.loadFromData(process.readAllStandardOutput(), "PNG");
  return image;
}

void statusText(QLabel* label, bool ready, const QString& title, const QString& detail) {
  const QString color = ready ? green : red;
  label->setText(QStringLiteral("<span style='color:%1;font-size:18px'>●</span> "
                                "<b>%2</b><br><span style='color:%3'>%4</span>")
                     .arg(color, title.toHtmlEscaped(), muted, detail.toHtmlEscaped()));
}

QFrame* statusCard(const QString& title, QLabel** output) {
  auto* frame = new QFrame;
  frame->setObjectName("statusCard");
  auto* layout = new QVBoxLayout(frame);
  layout->setContentsMargins(14, 11, 14, 11);
  auto* label = new QLabel;
  label->setTextFormat(Qt::RichText);
  label->setWordWrap(true);
  statusText(label, false, title, QStringLiteral("Checking…"));
  layout->addWidget(label);
  *output = label;
  return frame;
}

QLabel* sectionTitle(const QString& text) {
  auto* label = new QLabel(text);
  label->setObjectName("sectionTitle");
  return label;
}
} // namespace

DesktopSink::DesktopSink(std::string device, bool mirror, int rotation, QObject* parent)
    : QObject(parent), output_(std::make_unique<openlens::V4l2Sink>(std::move(device))),
      mirror_(mirror), rotation_(rotation), started_(std::chrono::steady_clock::now()) {}

void DesktopSink::configure(int width, int height, int fps) {
  if (rotation_ == 90 || rotation_ == 270)
    output_->configure(height, width, fps);
  else
    output_->configure(width, height, fps);
}

void DesktopSink::orientation(int degrees) { phoneRotation_.store(degrees); }

void DesktopSink::push(const openlens::VideoFrame& frame) {
  const int total = ((rotation_ + phoneRotation_.load()) % 360 + 360) % 360;
  auto transformed = openlens::transform_frame(frame, total, mirror_);
  // The virtual camera format must not change mid-stream, so the first frame
  // fixes the canvas and later rotations are letterboxed into it.
  if (canvasWidth_ == 0) {
    canvasWidth_ = transformed.width;
    canvasHeight_ = transformed.height;
  }
  if (transformed.width != canvasWidth_ || transformed.height != canvasHeight_)
    transformed = openlens::fit_frame(transformed, canvasWidth_, canvasHeight_);
  output_->push(transformed);
  ++frames_;
  if (frames_ % 3U == 0U)
    emit previewReady(toImage(transformed));
  if (frames_ % 15U == 0U) {
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    emit progress(frames_, seconds > 0.0 ? static_cast<double>(frames_) / seconds : 0.0);
  }
}

void DesktopSink::placeholder(std::string_view message) {
  output_->placeholder(message);
  QImage slate(1280, 720, QImage::Format_RGB32);
  slate.fill(QColor("#321515"));
  QPainter painter(&slate);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(QColor("#ffb3ad"));
  QFont font = painter.font();
  font.setPixelSize(36);
  font.setBold(true);
  painter.setFont(font);
  painter.drawText(slate.rect(), Qt::AlignCenter,
                   QStringLiteral("Phone disconnected\nWaiting for the same device"));
  emit previewReady(slate);
  emit connectionMessage(QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
}

void DesktopSink::flush() { output_->flush(); }
void DesktopSink::stop() { output_->stop(); }

QImage DesktopSink::toImage(const openlens::VideoFrame& frame) {
  QImage image(frame.width, frame.height, QImage::Format_RGB32);
  const std::size_t ySize =
      static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
  const std::size_t chromaSize = ySize / 4U;
  const auto* yPlane = frame.i420.data();
  const auto* uPlane = yPlane + ySize;
  const auto* vPlane = uPlane + chromaSize;
  for (int row = 0; row < frame.height; ++row) {
    auto* output = reinterpret_cast<QRgb*>(image.scanLine(row));
    for (int column = 0; column < frame.width; ++column) {
      const std::size_t yIndex =
          static_cast<std::size_t>(row) * static_cast<std::size_t>(frame.width) +
          static_cast<std::size_t>(column);
      const std::size_t chromaIndex =
          static_cast<std::size_t>(row / 2) * static_cast<std::size_t>(frame.width / 2) +
          static_cast<std::size_t>(column / 2);
      const int y = std::max(0, static_cast<int>(yPlane[yIndex]) - 16);
      const int u = static_cast<int>(uPlane[chromaIndex]) - 128;
      const int v = static_cast<int>(vPlane[chromaIndex]) - 128;
      const int r = std::clamp((298 * y + 409 * v + 128) >> 8, 0, 255);
      const int g = std::clamp((298 * y - 100 * u - 208 * v + 128) >> 8, 0, 255);
      const int b = std::clamp((298 * y + 516 * u + 128) >> 8, 0, 255);
      output[column] = qRgb(r, g, b);
    }
  }
  return image;
}

MainWindow::MainWindow() {
  buildInterface();
  loadSettings();
  readinessWatcher_ = new QFutureWatcher<Readiness>(this);
  connect(readinessWatcher_, &QFutureWatcher<Readiness>::finished, this,
          &MainWindow::applyReadiness);
  QTimer::singleShot(0, this, &MainWindow::refreshReadiness);
  auto* timer = new QTimer(this);
  timer->setInterval(5000);
  connect(timer, &QTimer::timeout, this, &MainWindow::refreshReadiness);
  timer->start();
}

MainWindow::~MainWindow() {
  cancelled_ = true;
  if (worker_.joinable())
    worker_.join();
}

void MainWindow::buildInterface() {
  setWindowTitle(QStringLiteral("OpenLens"));
  resize(1180, 780);
  setMinimumSize(920, 680);
  setStyleSheet(QStringLiteral(R"(
    QMainWindow, QWidget { background:#07130f; color:#ecfff6; font-size:14px; }
    QLabel, QCheckBox { background:transparent; }
    QLabel#brand { font-size:30px; font-weight:800; }
    QLabel#subtitle { color:#8faea0; }
    QLabel#overall { color:#07130f; background:#38d996; border-radius:12px; padding:7px 13px; font-weight:700; }
    QLabel#sectionTitle { color:#9bd8bc; font-weight:700; font-size:13px; text-transform:uppercase; }
    QFrame#statusCard, QGroupBox { background:#0d2119; border:1px solid #1f3c30; border-radius:12px; }
    QGroupBox { margin-top:13px; padding:16px 12px 12px 12px; font-weight:700; }
    QGroupBox::title { subcontrol-origin:margin; left:13px; padding:0 5px; color:#a6d9c2; }
    QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit { background:#102a20; border:1px solid #2c5242; border-radius:8px; padding:7px; min-height:21px; }
    QComboBox::drop-down { border:0; width:24px; }
    QPushButton { background:#163a2c; border:1px solid #2c5c48; border-radius:9px; padding:9px 14px; font-weight:650; }
    QPushButton:hover { background:#1d4b39; }
    QPushButton:disabled { color:#587065; background:#102018; border-color:#1b3027; }
    QPushButton#primary { background:#38d996; color:#052018; border:0; font-size:17px; padding:13px 20px; }
    QPushButton#primary:hover { background:#56e5aa; }
    QLabel#preview { background:#020806; border:1px solid #26483a; border-radius:16px; color:#6f9382; font-size:18px; }
    QLabel#sessionTitle { font-size:22px; font-weight:750; }
    QLabel#sessionDetail, QLabel#stats { color:#9dbbad; }
    QScrollArea { border:0; }
    QScrollBar:vertical { background:#07130f; width:8px; margin:0; }
    QScrollBar::handle:vertical { background:#315746; border-radius:4px; min-height:30px; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
    QCheckBox { spacing:8px; }
  )"));

  auto* central = new QWidget;
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(24, 20, 24, 22);
  root->setSpacing(18);

  auto* header = new QHBoxLayout;
  auto* titles = new QVBoxLayout;
  auto* brand = new QLabel(QStringLiteral("OpenLens"));
  brand->setObjectName("brand");
  auto* subtitle = new QLabel(QStringLiteral("Your Android camera, private on local Wi-Fi"));
  subtitle->setObjectName("subtitle");
  titles->addWidget(brand);
  titles->addWidget(subtitle);
  header->addLayout(titles);
  header->addStretch();
  auto* installPhone = new QPushButton(QStringLiteral("Install phone app"));
  header->addWidget(installPhone, 0, Qt::AlignTop);
  overallStatus_ = new QLabel(QStringLiteral("Checking setup…"));
  overallStatus_->setObjectName("overall");
  header->addWidget(overallStatus_, 0, Qt::AlignTop);
  root->addLayout(header);

  auto* body = new QHBoxLayout;
  body->setSpacing(18);
  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFixedWidth(390);
  auto* left = new QWidget;
  auto* leftLayout = new QVBoxLayout(left);
  leftLayout->setContentsMargins(0, 0, 8, 0);
  leftLayout->setSpacing(10);
  leftLayout->addWidget(sectionTitle(QStringLiteral("Setup")));
  leftLayout->addWidget(statusCard(QStringLiteral("Phone on Wi-Fi"), &phoneStatus_));
  leftLayout->addWidget(statusCard(QStringLiteral("Secure pairing"), &usbStatus_));
  leftLayout->addWidget(statusCard(QStringLiteral("Private connection"), &appStatus_));
  leftLayout->addWidget(statusCard(QStringLiteral("Virtual camera"), &cameraStatus_));
  leftLayout->addWidget(statusCard(QStringLiteral("OBS integration"), &pluginStatus_));

  auto* deviceGroup = new QGroupBox(QStringLiteral("Camera settings"));
  auto* form = new QFormLayout(deviceGroup);
  device_ = new QComboBox;
  quality_ = new QComboBox;
  quality_->addItem(QStringLiteral("1080p · 30 fps"), QStringLiteral("1080p30"));
  quality_->addItem(QStringLiteral("720p · 30 fps"), QStringLiteral("720p30"));
  facing_ = new QComboBox;
  facing_->addItem(QStringLiteral("Rear camera"), QStringLiteral("back"));
  facing_->addItem(QStringLiteral("Front camera"), QStringLiteral("front"));
  bitrate_ = new QSpinBox;
  bitrate_->setRange(2, 16);
  bitrate_->setSuffix(QStringLiteral(" Mbps"));
  bitrate_->setValue(8);
  zoom_ = new QDoubleSpinBox;
  zoom_->setRange(1.0, 100.0);
  zoom_->setSingleStep(0.5);
  zoom_->setSuffix(QStringLiteral("×"));
  exposure_ = new QSpinBox;
  exposure_->setRange(-20, 20);
  exposure_->setPrefix(QStringLiteral("EV step "));
  rotation_ = new QComboBox;
  for (const int angle : {0, 90, 180, 270})
    rotation_->addItem(QString::number(angle) + QStringLiteral("°"), angle);
  torch_ = new QCheckBox(QStringLiteral("Torch"));
  mirror_ = new QCheckBox(QStringLiteral("Mirror picture"));
  auto* toggles = new QHBoxLayout;
  toggles->addWidget(torch_);
  toggles->addWidget(mirror_);
  toggles->addStretch();
  form->addRow(QStringLiteral("Phone"), device_);
  form->addRow(QStringLiteral("Quality"), quality_);
  form->addRow(QStringLiteral("Camera"), facing_);
  form->addRow(QStringLiteral("Bitrate"), bitrate_);
  form->addRow(QStringLiteral("Zoom"), zoom_);
  form->addRow(QStringLiteral("Exposure"), exposure_);
  form->addRow(QStringLiteral("Rotation"), rotation_);
  form->addRow(toggles);
  leftLayout->addWidget(deviceGroup);

  auto* toolButtons = new QHBoxLayout;
  refresh_ = new QPushButton(QStringLiteral("Refresh"));
  installPlugin_ = new QPushButton(QStringLiteral("Install OBS plugin"));
  toolButtons->addWidget(refresh_);
  toolButtons->addWidget(installPlugin_);
  leftLayout->addLayout(toolButtons);
  auto* obsButton = new QPushButton(QStringLiteral("Open OBS"));
  leftLayout->addWidget(obsButton);
  leftLayout->addStretch();
  scroll->setWidget(left);
  body->addWidget(scroll);

  auto* right = new QVBoxLayout;
  preview_ = new QLabel(QStringLiteral("Open OpenLens on your phone to begin"));
  preview_->setObjectName("preview");
  preview_->setAlignment(Qt::AlignCenter);
  preview_->setMinimumHeight(330);
  preview_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  right->addWidget(preview_, 1);
  auto* sessionCard = new QFrame;
  sessionCard->setObjectName("statusCard");
  sessionCard->setMinimumHeight(178);
  auto* sessionLayout = new QVBoxLayout(sessionCard);
  auto* sessionTitle = new QLabel(QStringLiteral("Ready when you are"));
  sessionTitle->setObjectName("sessionTitle");
  sessionTitle->setProperty("role", "sessionTitle");
  sessionDetail_ =
      new QLabel(QStringLiteral("Open OpenLens on both devices. Pair once, then start from here."));
  sessionDetail_->setObjectName("sessionDetail");
  sessionDetail_->setWordWrap(true);
  stats_ = new QLabel(QStringLiteral("No active session"));
  stats_->setObjectName("stats");
  start_ = new QPushButton(QStringLiteral("Start camera"));
  start_->setObjectName("primary");
  sessionLayout->addWidget(sessionTitle);
  sessionLayout->addWidget(sessionDetail_);
  sessionLayout->addWidget(stats_);
  sessionLayout->addWidget(start_);
  right->addWidget(sessionCard);
  body->addLayout(right, 1);
  root->addLayout(body, 1);
  setCentralWidget(central);

  connect(refresh_, &QPushButton::clicked, this, &MainWindow::refreshReadiness);
  connect(start_, &QPushButton::clicked, this, &MainWindow::toggleSession);
  connect(installPhone, &QPushButton::clicked, this, &MainWindow::showAndroidInstall);
  connect(installPlugin_, &QPushButton::clicked, this, &MainWindow::installObsPlugin);
  connect(obsButton, &QPushButton::clicked, this, &MainWindow::openObs);
  connect(device_, &QComboBox::currentIndexChanged, this, &MainWindow::updateControls);

  tray_ = new QSystemTrayIcon(windowIcon(), this);
  auto* trayMenu = new QMenu(this);
  trayMenu->addAction(QStringLiteral("Show OpenLens"), this, [this] {
    showNormal();
    raise();
    activateWindow();
  });
  trayMenu->addAction(QStringLiteral("Stop camera"), this, &MainWindow::stopSession);
  trayMenu->addSeparator();
  trayMenu->addAction(QStringLiteral("Quit"), this, [this] {
    quitting_ = true;
    close();
  });
  tray_->setContextMenu(trayMenu);
  tray_->setToolTip(QStringLiteral("OpenLens"));
  if (QSystemTrayIcon::isSystemTrayAvailable())
    tray_->show();
}

void MainWindow::loadSettings() {
  QSettings settings;
  quality_->setCurrentIndex(std::max(0, quality_->findData(settings.value("quality", "1080p30"))));
  facing_->setCurrentIndex(std::max(0, facing_->findData(settings.value("facing", "back"))));
  bitrate_->setValue(settings.value("bitrate", 8).toInt());
  zoom_->setValue(settings.value("zoom", 1.0).toDouble());
  exposure_->setValue(settings.value("exposure", 0).toInt());
  rotation_->setCurrentIndex(std::max(0, rotation_->findData(settings.value("rotation", 0))));
  torch_->setChecked(settings.value("torch", false).toBool());
  mirror_->setChecked(settings.value("mirror", false).toBool());
  device_->setProperty("savedSerial", settings.value("device"));
}

void MainWindow::saveSettings() const {
  QSettings settings;
  settings.setValue("quality", quality_->currentData());
  settings.setValue("facing", facing_->currentData());
  settings.setValue("bitrate", bitrate_->value());
  settings.setValue("zoom", zoom_->value());
  settings.setValue("exposure", exposure_->value());
  settings.setValue("rotation", rotation_->currentData());
  settings.setValue("torch", torch_->isChecked());
  settings.setValue("mirror", mirror_->isChecked());
  settings.setValue("device", device_->currentData());
}

QString MainWindow::obsPluginPath() {
  QString config = qEnvironmentVariable("XDG_CONFIG_HOME");
  if (config.isEmpty())
    config = QDir::homePath() + QStringLiteral("/.config");
  return config + QStringLiteral("/obs-studio/plugins/openlens-obs/bin/64bit/openlens-obs.so");
}

QString MainWindow::findBundledPlugin() {
  const QDir applicationDirectory(QCoreApplication::applicationDirPath());
  const QStringList candidates{
      applicationDirectory.filePath(QStringLiteral("openlens-obs.so")),
      applicationDirectory.filePath(QStringLiteral("../obs/openlens-obs.so")),
      QDir::homePath() + QStringLiteral("/.local/share/openlens/openlens-obs.so"),
  };
  for (const auto& candidate : candidates)
    if (QFileInfo::exists(candidate))
      return QFileInfo(candidate).canonicalFilePath();
  return {};
}

QString MainWindow::findBundledApk() {
  const QDir applicationDirectory(QCoreApplication::applicationDirPath());
  const QString configured = qEnvironmentVariable("OPENLENS_ANDROID_APK");
  const QStringList candidates{
      configured,
      applicationDirectory.filePath(QStringLiteral("openlens-debug.apk")),
      applicationDirectory.filePath(
          QStringLiteral("../../../../android/app/build/outputs/apk/debug/app-debug.apk")),
      QDir::homePath() + QStringLiteral("/.local/share/openlens/openlens-debug.apk"),
  };
  for (const auto& candidate : candidates)
    if (!candidate.isEmpty() && QFileInfo::exists(candidate))
      return QFileInfo(candidate).canonicalFilePath();
  return {};
}

Readiness MainWindow::inspectSystem(const QString&) {
  Readiness result;
  result.virtualCameraReady = std::filesystem::exists("/dev/video42");
  result.obsPluginReady = QFileInfo::exists(obsPluginPath());
  try {
    openlens::WifiIdentityStore identity;
    // Pairing may be recorded under another transport's id (a USB serial);
    // the pinned TLS identity authenticates the phone at connect time, so any
    // stored pairing counts as paired here.
    const bool anyPaired = !identity.peers().empty();
    for (const auto& found : openlens::discover_wifi_devices(std::chrono::milliseconds(900))) {
      DeviceInfo device;
      device.key = QStringLiteral("wifi:") + QString::fromStdString(found.device_id);
      device.model = QString::fromStdString(found.service_name);
      device.state = found.busy ? QStringLiteral("busy") : QStringLiteral("device");
      device.appInstalled = true;
      device.wifi = true;
      device.paired = identity.peer(found.device_id).has_value() || anyPaired;
      device.wifiDevice = found;
      result.devices.push_back(std::move(device));
    }
  } catch (const std::exception& error) {
    result.error = QString::fromUtf8(error.what());
  }
  try {
    openlens::WifiIdentityStore identity;
    // A USB phone is authenticated by its pinned TLS identity, so any stored
    // pairing (Wi-Fi or USB) makes it connectable.
    const bool anyPaired = !identity.peers().empty();
    for (const auto& phone : openlens::list_usb_phones()) {
      DeviceInfo device;
      device.key = QStringLiteral("usb:") + QString::fromStdString(phone.serial);
      device.serial = QString::fromStdString(phone.serial);
      device.model = phone.product.empty() ? QStringLiteral("Android phone over USB")
                                           : QString::fromStdString(phone.product);
      device.state = QStringLiteral("device");
      device.appInstalled = true;
      device.usb = true;
      device.paired = anyPaired;
      result.devices.push_back(std::move(device));
    }
  } catch (const std::exception&) {
  }
  return result;
}

void MainWindow::refreshReadiness() {
  if (readinessWatcher_ == nullptr || readinessWatcher_->isRunning())
    return;
  refresh_->setEnabled(false);
  refresh_->setText(QStringLiteral("Checking…"));
  const QString serial = selectedSerial();
  readinessWatcher_->setFuture(QtConcurrent::run([serial] { return inspectSystem(serial); }));
}

void MainWindow::applyReadiness() {
  readiness_ = readinessWatcher_->result();
  const QString previous =
      !selectedSerial().isEmpty() ? selectedSerial() : device_->property("savedSerial").toString();
  {
    const QSignalBlocker blocker(device_);
    device_->clear();
    for (const auto& found : readiness_.devices) {
      QString label;
      if (found.wifi) {
        label = found.model + (found.paired ? QStringLiteral(" · Wi-Fi · Paired")
                                            : QStringLiteral(" · Wi-Fi · Pair once"));
      } else if (found.usb) {
        label = found.model + (found.paired ? QStringLiteral(" · USB · Paired")
                                            : QStringLiteral(" · USB · Pair once"));
      } else {
        label = found.model + QStringLiteral(" · USB development mode");
      }
      device_->addItem(label, found.key);
    }
    const int previousIndex = device_->findData(previous);
    if (previousIndex >= 0)
      device_->setCurrentIndex(previousIndex);
  }
  const auto selected =
      std::find_if(readiness_.devices.cbegin(), readiness_.devices.cend(),
                   [this](const DeviceInfo& value) { return value.key == device_->currentData(); });
  const bool hasPhone = selected != readiness_.devices.cend();
  const bool connectionReady = hasPhone && selected->state == QStringLiteral("device");
  const bool phoneReady = connectionReady && (selected->wifi || !selected->locked);
  const bool appReady = connectionReady && selected->appInstalled;
  statusText(phoneStatus_, phoneReady, QStringLiteral("Phone"),
             phoneReady ? selected->model + QStringLiteral(" is available on local Wi-Fi")
                        : QStringLiteral("Open OpenLens on a phone on this Wi-Fi"));
  const bool paired = hasPhone && selected->wifi && selected->paired;
  statusText(usbStatus_, paired, QStringLiteral("Secure pairing"),
             paired                       ? QStringLiteral("This phone recognizes this computer")
             : hasPhone && selected->wifi ? QStringLiteral("One matching code is required")
                                          : QStringLiteral("Waiting for a Wi-Fi phone"));
  statusText(appStatus_, appReady, QStringLiteral("Private connection"),
             appReady                     ? QStringLiteral("Encrypted locally; no cloud or account")
             : readiness_.error.isEmpty() ? QStringLiteral("Searching the local network")
                                          : readiness_.error);
  statusText(cameraStatus_, readiness_.virtualCameraReady, QStringLiteral("Virtual camera"),
             readiness_.virtualCameraReady
                 ? QStringLiteral("/dev/video42 is ready for OBS")
                 : QStringLiteral("v4l2loopback device /dev/video42 is missing"));
  statusText(pluginStatus_, readiness_.obsPluginReady, QStringLiteral("OBS integration"),
             readiness_.obsPluginReady ? QStringLiteral("Native OBS source is installed")
                                       : QStringLiteral("Plugin is not installed yet"));
  const bool ready = phoneReady && appReady && paired && readiness_.virtualCameraReady;
  overallStatus_->setText(ready ? QStringLiteral("Ready to stream")
                          : hasPhone && selected->wifi && !paired ? QStringLiteral("Pair once")
                                                                  : QStringLiteral("Searching…"));
  if (!streaming_) {
    if (hasPhone && selected->wifi && !paired) {
      setSessionState(QStringLiteral("Phone found — pair once"),
                      QStringLiteral("OpenLens found %1. Click “Pair phone” below, then confirm "
                                     "the same six-digit code on both screens.")
                          .arg(selected->model),
                      false);
    } else if (hasPhone && paired) {
      setSessionState(
          readiness_.virtualCameraReady ? QStringLiteral("Ready when you are")
                                        : QStringLiteral("Phone paired — finish OBS setup"),
          readiness_.virtualCameraReady
              ? QStringLiteral("Click “Start camera” to start the phone automatically.")
              : QStringLiteral("The phone is paired. Set up /dev/video42 before starting the camera."),
          false);
    } else {
      setSessionState(QStringLiteral("Looking for your phone"),
                      QStringLiteral("Open OpenLens on your phone and keep both devices on the "
                                     "same local network."),
                      false);
    }
  }
  refresh_->setEnabled(true);
  refresh_->setText(QStringLiteral("Refresh"));
  installPlugin_->setText(readiness_.obsPluginReady ? QStringLiteral("Repair OBS plugin")
                                                    : QStringLiteral("Install OBS plugin"));
  updateControls();
}

QString MainWindow::selectedSerial() const { return device_->currentData().toString(); }

const DeviceInfo* MainWindow::selectedDevice() const {
  const auto selected =
      std::find_if(readiness_.devices.cbegin(), readiness_.devices.cend(),
                   [this](const DeviceInfo& value) { return value.key == device_->currentData(); });
  return selected == readiness_.devices.cend() ? nullptr : &*selected;
}

void MainWindow::updateControls() {
  const auto selected =
      std::find_if(readiness_.devices.cbegin(), readiness_.devices.cend(),
                   [this](const DeviceInfo& value) { return value.key == device_->currentData(); });
  const bool phoneAvailable = selected != readiness_.devices.cend() &&
                              (selected->wifi || selected->usb) &&
                              selected->state == QStringLiteral("device");
  const bool canContinue = phoneAvailable &&
                           (!selected->paired || readiness_.virtualCameraReady);
  start_->setEnabled(streaming_ || canContinue);
  if (!streaming_ && selected != readiness_.devices.cend() && (selected->wifi || selected->usb))
    start_->setText(selected->paired ? QStringLiteral("Start camera")
                                     : QStringLiteral("Pair phone"));
  const std::array<QWidget*, 9> controls{device_,   quality_,  facing_, bitrate_, zoom_,
                                         exposure_, rotation_, torch_,  mirror_};
  for (QWidget* control : controls)
    control->setEnabled(!streaming_);
}

void MainWindow::toggleSession() {
  if (streaming_) {
    stopSession();
    return;
  }
  const DeviceInfo* selected = selectedDevice();
  if (selected == nullptr || (!selected->wifi && !selected->usb)) {
    QMessageBox::information(
        this, QStringLiteral("OpenLens is not ready"),
        QStringLiteral("Open OpenLens on your phone and keep both devices on the same local network."));
    return;
  }
  if (!selected->paired) {
    if (worker_.joinable())
      worker_.join();
    streaming_ = true;
    start_->setEnabled(false);
    setSessionState(QStringLiteral("Pairing phone"),
                    QStringLiteral("Creating one matching code on both devices…"), true);
    const openlens::WifiDevice wifiDevice = selected->wifiDevice;
    const bool viaUsb = selected->usb;
    worker_ = std::thread([this, wifiDevice, viaUsb] {
      QString error;
      try {
        openlens::WifiIdentityStore identity;
        const openlens::PairingConfirmation confirm = [this](std::string_view code) {
          bool accepted = false;
          QMetaObject::invokeMethod(
              this,
              [this, code = QString::fromUtf8(code.data(), static_cast<qsizetype>(code.size())),
               &accepted] {
                accepted = QMessageBox::question(
                               this, QStringLiteral("Check the pairing code"),
                               QStringLiteral("Does this code match your phone?\n\n%1\n\n"
                                              "Press Codes match here, then on your phone.")
                                   .arg(code),
                               QMessageBox::Yes | QMessageBox::Cancel,
                               QMessageBox::Yes) == QMessageBox::Yes;
              },
              Qt::BlockingQueuedConnection);
          return accepted;
        };
        if (viaUsb) {
          auto link = openlens::UsbAccessoryLink::open();
          static_cast<void>(openlens::pair_connected_descriptor(link.release_descriptor(),
                                                                "usb:" + link.serial(),
                                                                link.product(), identity, confirm));
        } else {
          static_cast<void>(openlens::pair_wifi_device(wifiDevice, identity, confirm));
        }
      } catch (const std::exception& exception) {
        error = QString::fromUtf8(exception.what());
      }
      QMetaObject::invokeMethod(
          this,
          [this, error] {
            if (worker_.joinable())
              worker_.join();
            streaming_ = false;
            start_->setEnabled(true);
            setSessionState(
                error.isEmpty() ? QStringLiteral("Phone paired")
                                : QStringLiteral("Pairing did not finish"),
                error.isEmpty()
                    ? QStringLiteral("You can now start the camera entirely from this computer.")
                    : error,
                false);
            refreshReadiness();
          },
          Qt::QueuedConnection);
    });
    return;
  }
  if (!readiness_.virtualCameraReady) {
    QMessageBox::information(
        this, QStringLiteral("Virtual camera is not ready"),
        QStringLiteral("Your phone is paired. Set up /dev/video42, then click Start camera."));
    return;
  }
  saveSettings();
  if (worker_.joinable())
    worker_.join();
  openlens::SessionOptions options;
  if (selected->usb)
    options.usb = true;
  else
    options.wifi_device = selected->wifiDevice;
  options.preset = quality_->currentData().toString().toStdString();
  options.facing = facing_->currentData().toString().toStdString();
  options.bitrate = bitrate_->value() * 1000000;
  options.zoom = zoom_->value();
  options.exposure = exposure_->value();
  options.torch = torch_->isChecked();
  options.video_device = "/dev/video42";
  cancelled_ = false;
  sink_ = std::make_unique<DesktopSink>(options.video_device, mirror_->isChecked(),
                                        rotation_->currentData().toInt());
  connect(sink_.get(), &DesktopSink::previewReady, this, &MainWindow::showPreview,
          Qt::QueuedConnection);
  connect(
      sink_.get(), &DesktopSink::progress, this,
      [this](std::uint64_t frames, double fps) {
        stats_->setText(QStringLiteral("%1 frames · %2 fps · sending to OBS virtual camera")
                            .arg(static_cast<qulonglong>(frames))
                            .arg(fps, 0, 'f', 1));
        if (frames > 0U)
          setSessionState(QStringLiteral("Camera is live"),
                          QStringLiteral("OpenLens is available in OBS as /dev/video42."), true);
      },
      Qt::QueuedConnection);
  connect(
      sink_.get(), &DesktopSink::connectionMessage, this,
      [this](const QString& message) { sessionDetail_->setText(message); }, Qt::QueuedConnection);
  streaming_ = true;
  start_->setText(QStringLiteral("Stop camera"));
  setSessionState(QStringLiteral("Waiting for your phone"),
                  QStringLiteral("OpenLens is securely starting the phone camera automatically."),
                  true);
  stats_->setText(QStringLiteral("Establishing the private Wi-Fi session…"));
  updateControls();
  worker_ = std::thread([this, options = std::move(options)]() mutable {
    QString error;
    openlens::SessionStats result;
    try {
      openlens::OpenLensSession session(std::move(options));
      result = session.run(*sink_, cancelled_);
    } catch (const std::exception& exception) {
      if (!cancelled_)
        error = QString::fromUtf8(exception.what());
      std::fprintf(stderr, "openlens-desktop: session ended: %s\n", exception.what());
    }
    QMetaObject::invokeMethod(
        this,
        [this, error, result] {
          finishSession(error, result.frames, result.sequence_gaps, result.decode_errors);
        },
        Qt::QueuedConnection);
  });
}

void MainWindow::stopSession() {
  if (!streaming_)
    return;
  cancelled_ = true;
  start_->setEnabled(false);
  setSessionState(QStringLiteral("Stopping camera"),
                  QStringLiteral("Closing the private Wi-Fi session…"), true);
}

void MainWindow::finishSession(QString error, std::uint64_t frames, std::uint64_t gaps,
                               std::uint64_t decodeErrors) {
  if (worker_.joinable())
    worker_.join();
  sink_.reset();
  streaming_ = false;
  start_->setText(QStringLiteral("Start camera"));
  if (!error.isEmpty()) {
    setSessionState(QStringLiteral("Could not start camera"), error, false);
  } else {
    setSessionState(QStringLiteral("Camera stopped"),
                    QStringLiteral("The phone camera and OBS virtual camera have been released."),
                    false);
  }
  stats_->setText(QStringLiteral("Last session: %1 frames · %2 sequence gaps · %3 decode errors")
                      .arg(static_cast<qulonglong>(frames))
                      .arg(static_cast<qulonglong>(gaps))
                      .arg(static_cast<qulonglong>(decodeErrors)));
  updateControls();
  refreshReadiness();
}

void MainWindow::setSessionState(const QString& title, const QString& detail, bool active) {
  const auto labels = findChildren<QLabel*>();
  for (auto* label : labels)
    if (label->property("role").toString() == QStringLiteral("sessionTitle"))
      label->setText(title);
  sessionDetail_->setText(detail);
  tray_->setToolTip(active ? QStringLiteral("OpenLens · Camera active")
                           : QStringLiteral("OpenLens"));
}

void MainWindow::showPreview(const QImage& image) {
  lastPreview_ = image;
  if (!lastPreview_.isNull()) {
    preview_->setPixmap(
        QPixmap::fromImage(lastPreview_)
            .scaled(preview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
  }
}

void MainWindow::showAndroidInstall() {
  const QString apk = findBundledApk();
  if (apk.isEmpty()) {
    QMessageBox::warning(
        this, QStringLiteral("Android app not found"),
        QStringLiteral("Build or bundle openlens-debug.apk, then open this installer again."));
    return;
  }

  auto server = std::make_unique<ApkServer>(apk);
  QUrl download;
  try {
    download = server->start();
  } catch (const std::exception& error) {
    QMessageBox::warning(this, QStringLiteral("Could not share the Android app"),
                         QString::fromUtf8(error.what()));
    return;
  }
  apkServer_ = std::move(server);

  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("Install OpenLens on Android"));
  dialog.setMinimumWidth(470);
  auto* layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(24, 22, 24, 22);
  layout->setSpacing(12);
  auto* title = new QLabel(QStringLiteral("Scan to install OpenLens"));
  title->setObjectName("sessionTitle");
  layout->addWidget(title);
  auto* detail = new QLabel(
      QStringLiteral("Connect the phone to the same local network, scan this code, then install "
                     "the downloaded APK. No USB is required."));
  detail->setObjectName("sessionDetail");
  detail->setWordWrap(true);
  layout->addWidget(detail);

  auto* qr = new QLabel;
  qr->setAlignment(Qt::AlignCenter);
  const QPixmap image = qrCode(download.toString());
  if (image.isNull()) {
    qr->setText(QStringLiteral("QR generation is unavailable. Use the link below."));
  } else {
    qr->setPixmap(image);
  }
  layout->addWidget(qr);

  auto* link = new QLineEdit(download.toString());
  link->setReadOnly(true);
  link->setMinimumHeight(38);
  layout->addWidget(link);
  auto* note = new QLabel(
      QStringLiteral("The download is available only while OpenLens Desktop is running. If the "
                     "phone cannot open the link, allow this one local port through the Linux "
                     "firewall below."));
  note->setObjectName("sessionDetail");
  note->setWordWrap(true);
  layout->addWidget(note);

  auto* actions = new QHBoxLayout;
  auto* firewall = new QPushButton(QStringLiteral("Allow through firewall"));
  auto* copy = new QPushButton(QStringLiteral("Copy link"));
  auto* close = new QPushButton(QStringLiteral("Done"));
  close->setObjectName("primary");
  actions->addWidget(firewall);
  actions->addWidget(copy);
  actions->addStretch();
  actions->addWidget(close);
  layout->addLayout(actions);
  connect(copy, &QPushButton::clicked, &dialog, [link] {
    QApplication::clipboard()->setText(link->text());
  });
  connect(firewall, &QPushButton::clicked, &dialog, [this, firewall, download] {
    const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    const QString ufw = QFileInfo::exists(QStringLiteral("/usr/sbin/ufw"))
                            ? QStringLiteral("/usr/sbin/ufw")
                            : QStandardPaths::findExecutable(QStringLiteral("ufw"));
    if (pkexec.isEmpty() || ufw.isEmpty()) {
      QMessageBox::information(
          this, QStringLiteral("Firewall helper unavailable"),
          QStringLiteral("Allow TCP port %1 from your local network in the Linux firewall.")
              .arg(installerPort));
      return;
    }
    firewall->setEnabled(false);
    firewall->setText(QStringLiteral("Waiting for permission…"));
    auto* process = new QProcess(firewall);
    const QString subnet = localSubnet(QHostAddress(download.host()));
    connect(process, &QProcess::finished, firewall,
            [this, firewall, process](int exitCode, QProcess::ExitStatus status) {
              const bool success = status == QProcess::NormalExit && exitCode == 0;
              firewall->setEnabled(!success);
              firewall->setText(success ? QStringLiteral("Firewall ready")
                                        : QStringLiteral("Try firewall permission again"));
              if (!success) {
                const QString detail = QString::fromUtf8(process->readAllStandardError()).trimmed();
                QMessageBox::warning(
                    this, QStringLiteral("Firewall permission did not finish"),
                    detail.isEmpty() ? QStringLiteral("The administrator prompt was cancelled.")
                                     : detail);
              }
              process->deleteLater();
            });
    process->start(pkexec,
                   {ufw, QStringLiteral("allow"), QStringLiteral("from"), subnet,
                    QStringLiteral("to"), QStringLiteral("any"), QStringLiteral("port"),
                    QString::number(installerPort), QStringLiteral("proto"),
                    QStringLiteral("tcp"), QStringLiteral("comment"),
                    QStringLiteral("OpenLens phone installer")});
  });
  connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
  dialog.exec();
}

void MainWindow::installObsPlugin() {
  const QString source = findBundledPlugin();
  if (source.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("Plugin file not found"),
                         QStringLiteral("Build the native OBS plugin first, then try again."));
    return;
  }
  const QString destination = obsPluginPath();
  if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
    QMessageBox::warning(this, QStringLiteral("Could not create OBS plugin folder"), destination);
    return;
  }
  QFile input(source);
  QSaveFile output(destination);
  if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly) ||
      output.write(input.readAll()) < 0 || !output.commit()) {
    QMessageBox::warning(this, QStringLiteral("Plugin installation failed"),
                         QStringLiteral("OpenLens could not write the OBS plugin file."));
    return;
  }
  QFile::setPermissions(destination, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                         QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                         QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                         QFileDevice::ExeOther);
  QMessageBox::information(
      this, QStringLiteral("OBS plugin installed"),
      QStringLiteral("Restart OBS, then add “OpenLens Phone Camera” as a source."));
  refreshReadiness();
}

void MainWindow::openObs() {
  if (!QProcess::startDetached(QStringLiteral("obs"), {}))
    QMessageBox::information(
        this, QStringLiteral("Could not open OBS"),
        QStringLiteral("Open OBS from your application menu, then add the OpenLens source."));
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (!quitting_ && tray_ != nullptr && tray_->isVisible()) {
    hide();
    tray_->showMessage(QStringLiteral("OpenLens keeps running"),
                       QStringLiteral("The camera stays available from the tray. "
                                      "Choose Quit in the tray menu to exit."),
                       QSystemTrayIcon::Information, 4000);
    event->ignore();
    return;
  }
  if (streaming_) {
    const auto answer =
        QMessageBox::question(this, QStringLiteral("Stop the camera?"),
                              QStringLiteral("Closing OpenLens will stop the phone camera and "
                                             "remove the virtual camera feed from OBS."));
    if (answer != QMessageBox::Yes) {
      quitting_ = false;
      event->ignore();
      return;
    }
  }
  cancelled_ = true;
  if (worker_.joinable())
    worker_.join();
  saveSettings();
  event->accept();
  // A visible tray icon keeps Qt >= 6.5 applications alive after the last
  // window closes, so quitting must be explicit.
  QCoreApplication::quit();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  if (!lastPreview_.isNull())
    showPreview(lastPreview_);
}
