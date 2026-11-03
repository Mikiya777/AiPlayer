#pragma once
#include "core/MediaTypes.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace PLAYER {
// 解码输出为原始视频画面（通常是 YUV；这里不做 RGBA 转换或纹理上传）。
// 每条流使用自己的上下文；同一实例不能并发调用。
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // 复制流参数，初始化解码器；不持有 AVStream 的借用指针。
    int Open(const AVStream& stream);
    void Close();
    AVRational GetTimeBase() const { return m_time_base; }

    // 返回 0 表示已接受输入；EAGAIN 时先取帧，再重送同一个包。
    // nullptr 是输入结束信号：成功送入后持续 receive 直到 EOF。
    // 不接管 packet 对象；送入成功后调用者可以 unref 自己的包。
    int SendPacket(const AVPacket* packet);

    // 每次成功填充一帧；EAGAIN 表示需要更多输入；EOF 表示排空完成。
    // frame 由调用者分配；处理完后 unref，最后 free。
    int ReceiveFrame(AVFrame* frame);
    void Flush();

private:
    static AVPixelFormat SelectHardwareFormat(AVCodecContext*, const AVPixelFormat* formats);
    AVCodecContext* m_context = nullptr;
    FramePtr m_hardware_frame;
    AVRational m_time_base{0, 1};
};
} // namespace PLAYER
