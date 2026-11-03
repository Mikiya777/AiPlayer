#include "writer/AudioEncoder.h"
#include <algorithm>
#include <stdexcept>
extern "C" {
#include <libavutil/channel_layout.h>
}
namespace PLAYER {
AudioEncoder::~AudioEncoder() {
  av_audio_fifo_free(m_fifo);
  avcodec_free_context(&m_context);
}
void AudioEncoder::Open(Muxer &muxer, const std::string &name) {
  if (m_context) throw std::logic_error("音频编码器只能打开一次");
  auto *codec = avcodec_find_encoder_by_name(name.c_str());
  if (!codec) throw std::runtime_error("音频编码器不可用");
  m_context = avcodec_alloc_context3(codec);
  if (!m_context) throw std::bad_alloc();
  m_context->sample_rate = 48000;
  m_context->sample_fmt = AV_SAMPLE_FMT_FLTP;
  m_context->channel_layout = AV_CH_LAYOUT_STEREO;
  m_context->channels = 2;
  m_context->time_base = {1, 48000};
  m_context->bit_rate = 192000;
  m_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  CheckWrite(avcodec_open2(m_context, codec, nullptr), "打开音频编码器");
  m_fifo = av_audio_fifo_alloc(m_context->sample_fmt, 2, 2048);
  if (!m_fifo) throw std::bad_alloc();
  m_stream = muxer.AddStream(*m_context);
  m_muxer = &muxer;
}
void AudioEncoder::Packets() {
  PacketPtr packet(av_packet_alloc());
  if (!packet) throw std::bad_alloc();
  int ret;
  while ((ret = avcodec_receive_packet(m_context, packet.get())) == 0) m_muxer->Write(*packet, m_context->time_base, m_stream);
  if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) CheckWrite(ret, "接收音频编码包");
}
void AudioEncoder::Encode(bool final) {
  const int block = std::max(1, m_context->frame_size);
  while (av_audio_fifo_size(m_fifo) >= block || (final && av_audio_fifo_size(m_fifo) > 0)) {
    FramePtr frame(av_frame_alloc());
    if (!frame) throw std::bad_alloc();
    frame->format = m_context->sample_fmt;
    frame->sample_rate = 48000;
    frame->channel_layout = AV_CH_LAYOUT_STEREO;
    frame->nb_samples = std::min(block, av_audio_fifo_size(m_fifo));
    frame->pts = m_pts;
    m_pts += frame->nb_samples;
    CheckWrite(av_frame_get_buffer(frame.get(), 0), "分配音频编码帧");
    CheckWrite(av_audio_fifo_read(m_fifo, reinterpret_cast<void **>(frame->data), frame->nb_samples), "读取音频 FIFO");
    CheckWrite(avcodec_send_frame(m_context, frame.get()), "发送音频帧");
    Packets();
  }
}
void AudioEncoder::Push(const AVFrame &pcm, int offset, int count) {
  void *planes[2] = {pcm.extended_data[0] + offset * 4, pcm.extended_data[1] + offset * 4};
  CheckWrite(av_audio_fifo_write(m_fifo, planes, count), "写入音频 FIFO");
  Encode(false);
}
void AudioEncoder::Silence(int64_t count) {
  FramePtr frame(av_frame_alloc());
  if (!frame) throw std::bad_alloc();
  frame->format = AV_SAMPLE_FMT_FLTP;
  frame->channel_layout = AV_CH_LAYOUT_STEREO;
  frame->nb_samples = 1024;
  CheckWrite(av_frame_get_buffer(frame.get(), 0), "分配静音帧");
  av_samples_set_silence(frame->data, 0, 1024, 2, AV_SAMPLE_FMT_FLTP);
  while (count > 0) {
    int n = static_cast<int>(std::min<int64_t>(1024, count));
    Push(*frame, 0, n);
    count -= n;
  }
}
void AudioEncoder::Finish() {
  Encode(true);
  CheckWrite(avcodec_send_frame(m_context, nullptr), "排空音频编码器");
  Packets();
}
} // namespace PLAYER
