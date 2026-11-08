#pragma once
#include "core/Player.h"
#include "video/VideoRenderer.h"
#include <QOpenGLWindow>
#include <QTimer>

namespace PLAYER {
// Qt 提供窗口/context，Player 决定显示哪帧，VideoRenderer 只负责 GPU 绘制。
// 本窗口和音频设备都在 GUI 主线程使用；工作线程只生产 CPU 帧。
class VideoWindow final : public QOpenGLWindow {
public:
  VideoWindow();
  ~VideoWindow() override;
  bool Open(const QString &path);
  void SetQuitAtEnd(bool enabled) {
    m_quit_at_end = enabled;
  }
  void RequestCapture(const QString &path) {
    m_capture_path = path;
    update();
  }
  void TogglePaused();

protected:
  void initializeGL() override;
  void paintGL() override;
  void keyPressEvent(QKeyEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  bool event(QEvent *event) override;

private:
  void Advance();
  void ReportError(const QString &message);
  void ReleaseGL();
  Player m_player;
  VideoRenderer m_renderer;
  QTimer m_timer;
  FramePtr m_frame;
  bool m_dirty = false, m_ready = false, m_quit_at_end = false, m_finished = false;
  QString m_capture_path;
  QMetaObject::Connection m_context_connection;
};
} // namespace PLAYER
