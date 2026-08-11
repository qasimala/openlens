// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "openlens/sinks.hpp"
#include "openlens/wifi_discovery.hpp"

#include <QFutureWatcher>
#include <QImage>
#include <QMainWindow>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QSystemTrayIcon;

struct DeviceInfo {
  QString key;
  QString serial;
  QString suffix;
  QString model;
  QString state;
  bool appInstalled{};
  bool locked{};
  bool wifi{};
  bool usb{};
  bool paired{};
  openlens::WifiDevice wifiDevice;
};

struct Readiness {
  bool virtualCameraReady{};
  bool obsPluginReady{};
  QString error;
  QList<DeviceInfo> devices;
};

class DesktopSink final : public QObject, public openlens::FrameSink {
  Q_OBJECT

public:
  DesktopSink(std::string device, bool mirror, int rotation, QObject* parent = nullptr);
  void configure(int width, int height, int fps) override;
  void push(const openlens::VideoFrame& frame) override;
  void placeholder(std::string_view message) override;
  void flush() override;
  void stop() override;

signals:
  void previewReady(const QImage& image);
  void progress(std::uint64_t frames, double fps);
  void connectionMessage(const QString& message);

private:
  std::unique_ptr<openlens::V4l2Sink> output_;
  bool mirror_{};
  int rotation_{};
  std::uint64_t frames_{};
  std::chrono::steady_clock::time_point started_;

  static QImage toImage(const openlens::VideoFrame& frame);
};

class MainWindow final : public QMainWindow {
  Q_OBJECT

public:
  MainWindow();
  ~MainWindow() override;

protected:
  void closeEvent(QCloseEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private slots:
  void refreshReadiness();
  void applyReadiness();
  void toggleSession();
  void stopSession();
  void installObsPlugin();
  void showAndroidInstall();
  void openObs();

private:
  QFutureWatcher<Readiness>* readinessWatcher_{};
  QLabel* overallStatus_{};
  QLabel* phoneStatus_{};
  QLabel* usbStatus_{};
  QLabel* appStatus_{};
  QLabel* cameraStatus_{};
  QLabel* pluginStatus_{};
  QLabel* preview_{};
  QLabel* sessionDetail_{};
  QLabel* stats_{};
  QComboBox* device_{};
  QComboBox* quality_{};
  QComboBox* facing_{};
  QComboBox* rotation_{};
  QSpinBox* bitrate_{};
  QDoubleSpinBox* zoom_{};
  QSpinBox* exposure_{};
  QCheckBox* torch_{};
  QCheckBox* mirror_{};
  QPushButton* start_{};
  QPushButton* refresh_{};
  QPushButton* installPlugin_{};
  QSystemTrayIcon* tray_{};
  Readiness readiness_;
  QImage lastPreview_;
  std::atomic_bool cancelled_{false};
  std::thread worker_;
  std::unique_ptr<DesktopSink> sink_;
  std::unique_ptr<QObject> apkServer_;
  bool streaming_{};
  bool quitting_{};

  void buildInterface();
  void loadSettings();
  void saveSettings() const;
  void updateControls();
  void setSessionState(const QString& title, const QString& detail, bool active);
  void showPreview(const QImage& image);
  void finishSession(QString error, std::uint64_t frames, std::uint64_t gaps,
                     std::uint64_t decodeErrors);
  [[nodiscard]] QString selectedSerial() const;
  [[nodiscard]] const DeviceInfo* selectedDevice() const;
  [[nodiscard]] static Readiness inspectSystem(const QString& selectedSerial);
  [[nodiscard]] static QString obsPluginPath();
  [[nodiscard]] static QString findBundledPlugin();
  [[nodiscard]] static QString findBundledApk();
};
