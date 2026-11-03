#include "reader/FileReader.h"
#include <cerrno>

namespace PLAYER {
namespace {
// 两种解码器的 send/receive 协议相同，用模板复用调度逻辑。
// 返回 EAGAIN/EOF 给调用者判断；它们在不同阶段具有不同含义。

//SendPacket() 返回 EAGAIN	当前包还没被接受，需要先取输出	接收帧，再重送原包
//ReceiveFrame() 返回 EAGAIN	当前没有可取的帧，需要更多输入	继续送包
template <typename Decoder>
int ReceiveAvailable(Decoder &decoder, AVFrame *frame, const FrameCallback &callback,
                     const std::atomic<bool> *cancelled) {
  while (true) {
    if (cancelled && cancelled->load()) return AVERROR(ECANCELED);
    const int ret = decoder.ReceiveFrame(frame);
    if (ret < 0) return ret;
    if (callback) callback(*frame, decoder.GetTimeBase());
    av_frame_unref(frame);
  }
}

template <typename Decoder>
int Decode(Decoder &decoder, const AVPacket *packet, AVFrame *frame,
           const FrameCallback &callback, const std::atomic<bool> *cancelled) {
  int ret;
  while ((ret = decoder.SendPacket(packet)) == AVERROR(EAGAIN)) {
    // 本次包没有被接受：先取输出，再重送同一个包，不能丢包。
    const int received = ReceiveAvailable(decoder, frame, callback, cancelled);
    if (received != AVERROR(EAGAIN)) return received;
  }
  if (ret < 0) return ret;
  return ReceiveAvailable(decoder, frame, callback, cancelled);
}
} // namespace

FileReader::~FileReader() {
  Close();
}

//准备FileReader的运行环境
int FileReader::Open(const std::string &url, bool audioEnabled, bool videoEnabled) {
  Close();
  int ret = m_demuxer.Open(url);
  if (ret < 0) return ret;

  // 后续任一步失败，都清理本次已打开的资源，并保留原错误码。
  const auto Fail = [this](int error) {
    Close();
    return error;
  };

  m_audio_index = audioEnabled ? m_demuxer.GetBestStream(AVMEDIA_TYPE_AUDIO) : AVERROR_STREAM_NOT_FOUND;
  m_video_index = videoEnabled ? m_demuxer.GetBestStream(AVMEDIA_TYPE_VIDEO) : AVERROR_STREAM_NOT_FOUND;
  // 缺少一种媒体类型是合法的，其他查找错误必须向外报告。
  if (m_audio_index < 0 && m_audio_index != AVERROR_STREAM_NOT_FOUND)
    return Fail(m_audio_index);
  if (m_video_index < 0 && m_video_index != AVERROR_STREAM_NOT_FOUND)
    return Fail(m_video_index);
  if (m_audio_index < 0 && m_video_index < 0)
    return Fail(AVERROR_STREAM_NOT_FOUND);

  //-1表示流不存在。
  if (m_audio_index < 0) m_audio_index = -1;
  if (m_video_index < 0) m_video_index = -1;

  if (m_audio_index >= 0) {
    ret = m_audio_decoder.Open(*m_demuxer.GetStream(m_audio_index));
    if (ret < 0) return Fail(ret);
  }
  if (m_video_index >= 0) {
    ret = m_video_decoder.Open(*m_demuxer.GetStream(m_video_index));
    if (ret < 0) return Fail(ret);
  }

  m_opened = true;
  return 0;
}

int FileReader::ReadAll(const FrameCallback &onAudio, const FrameCallback &onVideo,
                        const std::atomic<bool> *cancelled) {
  if (!m_opened || m_read_started) return AVERROR(EINVAL);
  m_read_started = true;
  PacketPtr packet(av_packet_alloc());
  FramePtr frame(av_frame_alloc());
  if (!packet || !frame) return AVERROR(ENOMEM);

  int ret = 0;
  //解复用器读取一个 packet，根据它的流索引选择音频或视频解码器；通过 Decode() 送入压缩包、取出当前可用的解码帧，再通过回调交付。其他流跳过。
  while (true) {
    // 一旦观察到取消就退出，不依赖下一次读取标志仍然为 true。
    if (cancelled && cancelled->load()) return AVERROR(ECANCELED);
    ret = m_demuxer.ReadPacket(packet.get());
    if (ret < 0) break;
    int decoded = AVERROR(EAGAIN);
    if (packet->stream_index == m_audio_index)
      decoded = Decode(m_audio_decoder, packet.get(), frame.get(), onAudio, cancelled);
    else if (packet->stream_index == m_video_index)
      decoded = Decode(m_video_decoder, packet.get(), frame.get(), onVideo, cancelled);

    // 其他流直接跳过；被解码器接受的包也可以释放调用者的引用。
    av_packet_unref(packet.get());
    if (decoded != AVERROR(EAGAIN)) {
      // 正常送包阶段不应提前进入解码 EOF，不能当成输入正常结束。
      return decoded == AVERROR_EOF ? AVERROR_INVALIDDATA : decoded;
    }
  }
  //读包循环结束后，先检查取消，再确认输入是否正常结束；如果正常结束，分别排空音视频解码器，交付延迟帧。全部完成后返回 0，解码器资源仍由对象持有，直到 Close() 或析构。
  if (cancelled && cancelled->load()) return AVERROR(ECANCELED);
  if (ret != AVERROR_EOF) return ret;

  // 文件没有新包，但两个解码器可能还有延迟输出，必须分别排空。
  // nullptr 是结束信号；不是 flush_buffers（后者用于丢弃缓存并重置）。
  if (m_audio_index >= 0) {
    ret = Decode(m_audio_decoder, nullptr, frame.get(), onAudio, cancelled);
    if (ret != AVERROR_EOF) return ret;
  }
  if (m_video_index >= 0) {
    ret = Decode(m_video_decoder, nullptr, frame.get(), onVideo, cancelled);
    if (ret != AVERROR_EOF) return ret;
  }
  return 0;
}

int FileReader::Seek(double seconds) {
  if (!m_opened || m_read_started) return AVERROR(EINVAL);
  const int ret = m_demuxer.Seek(seconds);
  if (ret < 0) return ret;
  m_audio_decoder.Flush();
  m_video_decoder.Flush();
  return 0;
}

void FileReader::Close() {
  m_audio_decoder.Close();
  m_video_decoder.Close();
  m_demuxer.Close();
  m_audio_index = m_video_index = -1;
  m_opened = m_read_started = false;
}
} // namespace PLAYER
