#pragma once
#include "writer/Muxer.h"
extern "C" {
#include <libavutil/audio_fifo.h>
}
namespace PLAYER {
class AudioEncoder {
public:
    AudioEncoder() = default;
    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;
    ~AudioEncoder();
    void Open(Muxer& muxer,const std::string& codecName);
    // 输入为 48kHz 双声道 FLTP；FIFO 以编码器 frame_size 分块。
    void Push(const AVFrame& pcm,int offset,int count);
    void Silence(int64_t count);
    void Finish();
private:
    void Encode(bool final);
    void Packets();
    Muxer* m_muxer=nullptr;
    AVCodecContext* m_context=nullptr;
    AVAudioFifo* m_fifo=nullptr;
    int m_stream=-1;
    int64_t m_pts=0;
};
}
