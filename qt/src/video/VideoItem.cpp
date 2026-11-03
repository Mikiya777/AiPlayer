#include "video/VideoItem.h"
#include "video/VideoRenderer.h"
#include "writer/RecordingCadence.h"
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <cstring>
#include <cmath>

namespace PLAYER {
namespace {
class QuickVideoRenderer final : public QQuickFramebufferObject::Renderer {
public:
    explicit QuickVideoRenderer(std::shared_ptr<RenderErrorRelay> errors)
        : m_errors(std::move(errors)) {}
    ~QuickVideoRenderer() override { m_renderer.Release(); }

    void synchronize(QQuickFramebufferObject* item) override
    {
        auto* videoItem = static_cast<VideoItem*>(item);
        const auto generation = videoItem->GetRenderGeneration();
        if (generation != m_generation) {
            m_generation = generation;
            m_failed = false;
        }
        bool clear = false;
        auto frame = videoItem->TakeFrame(clear);
        if (clear) { m_clear = true; m_frame.reset(); m_current_frame.reset(); }
        if (frame) m_frame = std::move(frame);
        auto recorder=videoItem->GetRecorder();
        if (recorder != m_recorder) m_capture_initial=bool(recorder);
        if (recorder != m_recorder) m_capture_cadence.Reset(recorder ? recorder->GetConfig().m_start : 0);
        m_recorder=std::move(recorder);
        m_raw_recording = !videoItem->GetFlip() && !videoItem->GetGray() &&
                          !videoItem->GetInvert() && !videoItem->GetSticker();
        m_renderer.SetFilters(videoItem->GetFlip(),videoItem->GetGray(),videoItem->GetInvert());
        m_renderer.SetFaces(videoItem->TakeFaces());
    }

    void render() override
    {
        if (m_failed) return;
        // 没有待显示内容时不初始化，停止播放后不重复触发同一初始化故障。
        if (!m_ready && !m_frame && !m_recorder) {
            if (auto* context = QOpenGLContext::currentContext()) {
                auto* functions = context->functions();
                functions->glClearColor(0, 0, 0, 1);
                functions->glClear(GL_COLOR_BUFFER_BIT);
            }
            return;
        }
        if (!m_ready) {
            m_ready = m_renderer.Initialize();
            if (!m_ready) {
                Fail(QStringLiteral("OpenGL 初始化失败：") + m_renderer.GetError());
                return;
            }
        }
        if (m_clear) {
            m_renderer.Clear();
            m_clear = false;
        }
        const bool newFrame=bool(m_frame);
        if (m_frame) {
            m_media_time=m_frame->pts/static_cast<double>(AV_TIME_BASE);
            if (!m_renderer.Upload(*m_frame)) {
                Fail(QStringLiteral("视频纹理上传失败：") + m_renderer.GetError());
                return;
            }
            m_current_frame=std::move(m_frame);
        }
        // 在 GPU 读回与分配大图之前限速；录像固定 30fps，无需采集 60fps 的每一帧。
        if (!m_recorder) m_capture.reset();
        if(m_recorder && (newFrame || m_capture_initial) &&
            m_capture_cadence.Take(m_capture_initial ? m_recorder->GetConfig().m_start : m_media_time)) {
            const auto& config=m_recorder->GetConfig();
            const double captureTime=m_capture_initial ? config.m_start : m_media_time;
            // 无滤镜且无需 SAR 尺寸转换时，直接共享原 RGBA 像素，避免 GPU 同步读回。
            if(m_raw_recording && m_current_frame &&
               m_current_frame->width==config.m_width && m_current_frame->height==config.m_height) {
                FramePtr copy(av_frame_clone(m_current_frame.get()));
                if(copy)m_recorder->Submit(std::move(copy),captureTime);
                else m_recorder->Fail("无法引用录制画面");
                m_capture.reset();
            } else {
            // 有滤镜时仍需录制渲染结果；只画视频，Qt 控件不会进入录制画面。
            if(!m_capture || m_capture->size()!=QSize(config.m_width,config.m_height))
                m_capture=std::make_unique<QOpenGLFramebufferObject>(config.m_width,config.m_height);
            if(m_capture->isValid() && m_capture->bind() && m_renderer.Draw(config.m_width,config.m_height)) {
                QImage pixels=m_capture->toImage().convertToFormat(QImage::Format_RGBA8888);
                FramePtr frame(av_frame_alloc());
                if(frame) {
                    frame->format=AV_PIX_FMT_RGBA;frame->width=pixels.width();frame->height=pixels.height();
                    if(!pixels.isNull() && av_frame_get_buffer(frame.get(),32)>=0) {
                        for(int y=0;y<frame->height;++y)std::memcpy(frame->data[0]+y*frame->linesize[0],pixels.constScanLine(y),frame->width*4);
                        m_recorder->Submit(std::move(frame),m_capture_initial ? config.m_start : m_media_time);
                    } else m_recorder->Fail("无法分配录制画面缓冲区");
                } else m_recorder->Fail("无法分配录制帧");
            } else m_recorder->Fail("无法读取 OpenGL 录制画面");
            framebufferObject()->bind();
            }
            m_capture_initial=false;
        }
        const QSize size = framebufferObject()->size();
        if (!m_renderer.Draw(size.width(), size.height()))
            Fail(QStringLiteral("视频绘制失败：") + m_renderer.GetError());
    }

    QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override
    {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        return new QOpenGLFramebufferObject(size, format);
    }

private:
    void Fail(const QString& error)
    {
        if (m_failed) return;
        m_failed = true;
        m_frame.reset();
        qWarning() << error;
        // 不等待下一次 synchronize：暂停或停止供帧时也必须及时通知 GUI。
        emit m_errors->failed(error, m_generation);
    }
    uint64_t m_generation=0;
    bool m_failed=false;
    std::shared_ptr<RenderErrorRelay> m_errors;
    VideoRenderer m_renderer;
    std::shared_ptr<FileWriter> m_recorder;
    std::unique_ptr<QOpenGLFramebufferObject> m_capture;
    bool m_capture_initial=false;
    double m_media_time=0;
    RecordingCadence m_capture_cadence;

    FramePtr m_frame, m_current_frame;
    bool m_raw_recording=true;
    bool m_ready = false;
    bool m_clear = false;
};
} // namespace

VideoItem::VideoItem(QQuickItem* parent) : QQuickFramebufferObject(parent)
{
    m_render_errors = std::shared_ptr<RenderErrorRelay>(new RenderErrorRelay,
        [](RenderErrorRelay* relay) { relay->deleteLater(); });
    connect(m_render_errors.get(), &RenderErrorRelay::failed, this,
        [this](const QString& error, quint64 generation) {
            if (generation != m_render_generation || m_render_error_reported) return;
            m_render_error_reported = true;
            emit renderFailed(error);
        }, Qt::QueuedConnection);
    setMirrorVertically(true);
}

QQuickFramebufferObject::Renderer* VideoItem::createRenderer() const
{
    return new QuickVideoRenderer(m_render_errors);
}

VideoItem::~VideoItem()
{
    { std::lock_guard<std::mutex> lock(m_mutex); m_stop=true; }
    m_wake.notify_one();
    if(m_worker.joinable()) m_worker.join();
}
void VideoItem::SetSticker(bool value)
{
    if(m_sticker==value) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sticker=value; ++m_generation; m_faces.clear(); m_detection_frame.reset();
        if(m_latest_frame) {
            m_pending_frame=FramePtr(av_frame_clone(m_latest_frame.get()));
            if(value) m_detection_frame=FramePtr(av_frame_clone(m_latest_frame.get()));
        }
    }
    m_filter_error.clear(); emit filterErrorChanged();
    if(value && !m_worker.joinable()) m_worker=std::thread(&VideoItem::DetectLoop,this);
    m_wake.notify_one(); emit filtersChanged(); update();
}
std::vector<FaceStickerPose> VideoItem::TakeFaces()
{
    std::lock_guard<std::mutex> lock(m_mutex); return m_faces;
}
void VideoItem::DetectLoop()
{
    FaceStickerFilter detector;
    const auto model=QDir(QCoreApplication::applicationDirPath()).filePath("../Resources/Pikachu").toStdString();
    for(;;) {
        FramePtr frame; uint64_t generation;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock,[this]{return m_stop || bool(m_detection_frame);});
            if(m_stop) return;
            frame=std::move(m_detection_frame); generation=m_generation;
        }
        std::vector<FaceStickerPose> faces;
        const bool ok=detector.Initialize(model) && detector.Detect(*frame,faces);
        const QString error=ok ? QString() : QString::fromStdString(detector.GetError());
        // 每个完成结果携带对应图像，避免贴纸与另外一帧错位。待处理队列只有一帧。
        // 投递回 GUI 后再次检查代次，跳转/关闭滤镜后的旧结果不能覆盖新画面。
        auto result=std::make_shared<FramePtr>(std::move(frame));
        QMetaObject::invokeMethod(this,[this,generation,result,faces=std::move(faces),error]() mutable {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if(generation!=m_generation || !m_sticker) return;
                m_pending_frame=std::move(*result); m_faces=std::move(faces); m_clear_requested=false;
            }
            if(m_filter_error!=error) { m_filter_error=error; emit filterErrorChanged(); }
            update();
        },Qt::QueuedConnection);
    }
}
QSize VideoItem::GetRecordingSize() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if(!m_latest_frame)return {};
    const auto& frame=*m_latest_frame;
    const double sar=frame.sample_aspect_ratio.num>0 && frame.sample_aspect_ratio.den>0 ? av_q2d(frame.sample_aspect_ratio):1.0;
    return QSize(std::max(2,static_cast<int>(std::round(frame.width*sar/2))*2),std::max(2,(frame.height+1)/2*2));
}
void VideoItem::SetFrame(FramePtr frame)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_latest_frame=FramePtr(av_frame_clone(frame.get()));
        if(m_sticker) m_detection_frame=std::move(frame);
        else m_pending_frame=std::move(frame);
        m_clear_requested = false;
    }
    m_wake.notify_one();
    update();
}

void VideoItem::ClearFrame()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_render_generation; m_render_error_reported=false;
    ++m_generation; m_latest_frame.reset(); m_detection_frame.reset(); m_faces.clear();
    m_pending_frame.reset();
    m_clear_requested = true;
    update();
}

void VideoItem::PostRenderError(const QString& error, uint64_t generation)
{
    emit m_render_errors->failed(error, generation);
}

FramePtr VideoItem::TakeFrame(bool& clearRequested)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    clearRequested = m_clear_requested;
    m_clear_requested = false;
    return std::move(m_pending_frame);
}
} // namespace PLAYER
