#include "video/VideoWindow.h"
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QImage>
#include <QFileInfo>
#include <iostream>

namespace PLAYER {
VideoWindow::VideoWindow() : m_player(CreateQtAudioOutput())
{
    resize(960, 540);
    setTitle(QStringLiteral("AI Player · 空格暂停 / F全屏 / Esc关闭"));
    m_timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&m_timer, &QTimer::timeout, this, [this] { Advance(); });
}
VideoWindow::~VideoWindow()
{
    m_timer.stop(); m_player.Close();
    QObject::disconnect(m_context_connection);
    ReleaseGL(); // 必须先释放 GL 资源，再让基类销毁 context。
}
bool VideoWindow::Open(const QString& path)
{
    m_timer.stop(); m_frame.reset(); m_finished = false;
    const int ret = m_player.Open(path.toStdString());
    if (ret < 0) { std::cerr << m_player.GetError() << '\n'; return false; }
    m_timer.start(5);
    return true;
}
void VideoWindow::initializeGL()
{
    m_ready = m_renderer.Initialize();
    if (!m_ready) { ReportError(m_renderer.GetError()); return; }
    std::cout << "OpenGL " << reinterpret_cast<const char*>(context()->functions()->glGetString(GL_VERSION)) << std::endl;
    m_dirty = bool(m_frame);
    m_context_connection = QObject::connect(context(), &QOpenGLContext::aboutToBeDestroyed,
                                            this, [this] { ReleaseGL(); }, Qt::DirectConnection);
}
void VideoWindow::ReleaseGL()
{
    if (!context()) return;
    makeCurrent(); m_renderer.Release(); doneCurrent(); m_ready = false;
}
void VideoWindow::paintGL()
{
    if (!m_ready) return;
    if (m_dirty && m_frame) {
        if (!m_renderer.Upload(*m_frame)) { ReportError(m_renderer.GetError()); return; }
        m_dirty = false;
    }
    // OpenGL viewport 使用设备像素；Retina 下不能直接把逻辑窗口尺寸当像素。
    if (!m_renderer.Draw(qRound(width() * devicePixelRatio()), qRound(height() * devicePixelRatio()))) {
        ReportError(m_renderer.GetError()); return;
    }
    if (!m_capture_path.isEmpty() && m_frame) {
        // 在 paintGL 内读取当前 framebuffer，避免交换后抓取到未定义的 back buffer。
        const QString path = m_capture_path;
        m_capture_path.clear();
        if (QFileInfo::exists(path) || !grabFramebuffer().save(path)) {
            ReportError(QStringLiteral("截图保存失败或文件已存在：") + path); return;
        }
        std::cout << "已保存渲染画面: " << path.toStdString() << std::endl;
    }
}
void VideoWindow::Advance()
{
    if (!m_ready) return; // 窗口曝光并创建 GL 后才启动播放调度。
    TimedFrame video;
    int ret = 0;
    try {
        ret = m_player.Tick(video);
    } catch (const std::exception& error) {
        ReportError(QString::fromUtf8(error.what()));
        return;
    }
    if (video.m_frame) {
        // update 是异步重绘；只保留最新一帧，避免 GUI 事件队列积压大量图像。
        m_frame = std::move(video.m_frame); m_dirty = true; update();
    }
    if (ret == AVERROR_EOF) {
        m_finished = true; m_timer.stop();
        std::cout << "播放结束 position=" << m_player.GetPosition()
                  << " presented=" << m_player.GetPresentedCount()
                  << " dropped=" << m_player.GetDroppedCount() << std::endl;
        setTitle(QStringLiteral("AI Player · 播放结束 · Esc关闭"));
        // 留出最后一次重绘机会；默认保留末帧，自动测试可请求退出。
        if (m_quit_at_end) QTimer::singleShot(80, this, [] { QCoreApplication::quit(); });
    } else if (ret < 0) ReportError(QString::fromStdString(m_player.GetError()));
}
void VideoWindow::ReportError(const QString& message)
{
    std::cerr << "播放错误: " << message.toStdString() << std::endl;
    m_timer.stop(); m_player.Close();
    QCoreApplication::exit(1);
}
void VideoWindow::TogglePaused()
{
    if (m_finished) return;
    const bool paused = !m_player.IsPaused();
    if (m_player.SetPaused(paused) < 0) { ReportError(QStringLiteral("暂停/恢复失败")); return; }
    std::cout << (paused ? "暂停" : "恢复") << " position=" << m_player.GetPosition() << std::endl;
    setTitle(paused ? QStringLiteral("AI Player · 已暂停 · 空格继续")
                    : QStringLiteral("AI Player · 空格暂停 / F全屏 / Esc关闭"));
}
void VideoWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) TogglePaused();
    else if (event->key() == Qt::Key_Escape) close();
    else if (event->key() == Qt::Key_F) visibility() == QWindow::FullScreen ? showNormal() : showFullScreen();
    else QOpenGLWindow::keyPressEvent(event);
}
void VideoWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) TogglePaused();
    else QOpenGLWindow::mousePressEvent(event);
}
bool VideoWindow::event(QEvent* event)
{
    if (event->type() == QEvent::Close) { m_timer.stop(); m_player.Close(); }
    return QOpenGLWindow::event(event);
}
} // namespace PLAYER
