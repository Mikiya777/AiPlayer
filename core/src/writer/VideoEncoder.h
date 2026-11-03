#pragma once
#include "writer/Muxer.h"
extern "C" {
#include <libswscale/swscale.h>
}
namespace PLAYER {
class VideoEncoder {
public:
    VideoEncoder() = default;
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;
    ~VideoEncoder();
    void Open(Muxer& muxer,int width,int height,const std::string& codecName);
    void Send(const AVFrame* rgba,int64_t pts=0,int64_t duration=3000);
private:
    Muxer* m_muxer=nullptr;
    AVCodecContext* m_context=nullptr;
    SwsContext* m_scale=nullptr;
    FramePtr m_frame;
    bool m_use_libyuv=false;
    int m_stream=-1;
};
}
