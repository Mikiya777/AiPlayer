#pragma once

#include "core/MediaTypes.h"
#include <QQuickFramebufferObject>
#include <mutex>
#include <condition_variable>
#include <thread>
#include "effects/FaceStickerFilter.h"
#include "writer/FileWriter.h"

namespace PLAYER {
// 画布与渲染器共同持有通知通道，画布销毁后 Qt 自动断开接收连接。
class RenderErrorRelay final : public QObject {
    Q_OBJECT
signals:
    void failed(const QString& error, quint64 generation);
};

// Qt Quick 中的视频画布。GUI 线程只投递 CPU RGBA 帧，OpenGL 上传和绘制全部在
// Qt Quick 渲染线程完成，避免跨线程使用 OpenGL context。
class VideoItem : public QQuickFramebufferObject {
    Q_OBJECT
    Q_PROPERTY(bool verticalFlip READ GetFlip WRITE SetFlip NOTIFY filtersChanged)
    Q_PROPERTY(bool grayscale READ GetGray WRITE SetGray NOTIFY filtersChanged)
    Q_PROPERTY(bool inverted READ GetInvert WRITE SetInvert NOTIFY filtersChanged)
    Q_PROPERTY(bool faceSticker READ GetSticker WRITE SetSticker NOTIFY filtersChanged)
    Q_PROPERTY(QString filterError READ GetFilterError NOTIFY filterErrorChanged)
public:
    explicit VideoItem(QQuickItem* parent = nullptr);
    ~VideoItem() override;
    bool GetFlip() const { return m_flip; }
    bool GetGray() const { return m_gray; }
    bool GetInvert() const { return m_invert; }
    bool GetSticker() const { return m_sticker; }
    QString GetFilterError() const { return m_filter_error; }
    void SetFlip(bool value) { m_flip=value; emit filtersChanged(); update(); }
    void SetGray(bool value) { m_gray=value; emit filtersChanged(); update(); }
    void SetInvert(bool value) { m_invert=value; emit filtersChanged(); update(); }
    void SetSticker(bool value);
    std::vector<FaceStickerPose> TakeFaces();
    Renderer* createRenderer() const override;
    void SetFrame(FramePtr frame);
    void ClearFrame();
    uint64_t GetRenderGeneration() const { return m_render_generation; }
    // 投递线程无需访问画布状态；接收与代次检查在 GUI 线程执行。
    void PostRenderError(const QString& error, uint64_t generation);
    QSize GetRecordingSize() const;
    void SetRecorder(std::shared_ptr<FileWriter> recorder) {m_recorder=std::move(recorder);update();}
    std::shared_ptr<FileWriter> GetRecorder() const {return m_recorder;}

    FramePtr TakeFrame(bool& clearRequested); // 仅由 Renderer::synchronize() 调用。

signals:
    void filtersChanged();
    void filterErrorChanged();
    void renderFailed(const QString& error);
private:
    void DetectLoop();
    std::shared_ptr<RenderErrorRelay> m_render_errors;
    std::shared_ptr<FileWriter> m_recorder;
    bool m_flip=false, m_gray=false, m_invert=false, m_sticker=false;
    QString m_filter_error;
    std::thread m_worker;
    std::condition_variable m_wake;
    bool m_stop=false;
    uint64_t m_generation=0;
    uint64_t m_render_generation=0;
    bool m_render_error_reported=false;
    FramePtr m_latest_frame, m_detection_frame;
    std::vector<FaceStickerPose> m_faces;
    mutable std::mutex m_mutex;
    FramePtr m_pending_frame;
    bool m_clear_requested = false;
};
} // namespace PLAYER
