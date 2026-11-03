#pragma once

#include <string>
extern "C" {
#include <libavformat/avformat.h>
}

namespace PLAYER {
// 一次媒体输入的所有者：管理输入上下文，不负责解码、播放或同步。
// 不允许复制，避免两个对象关闭同一上下文；同一实例不能并发调用。
class Demuxer {
public:
  Demuxer() = default;
  ~Demuxer();
  Demuxer(const Demuxer &) = delete;
  Demuxer &operator=(const Demuxer &) = delete;

  // 打开并探测流信息。成功返回 0，失败返回 FFmpeg 负错误码。
  // 再次 Open 会先关闭旧输入；新输入失败后本对象处于关闭状态。
  int Open(const std::string &url);
  void Close();

  // 找不到指定类型返回 AVERROR_STREAM_NOT_FOUND；索引不应写死为 0 或 1。
  int GetBestStream(AVMediaType type) const;
  unsigned int GetStreamCount() const;
  double GetDurationSeconds() const;
  // 借用指针：只在当前输入保持打开期间有效，调用者不能释放。
  const AVStream *GetStream(int index) const;

  // 复用调用者提供的包：先 unref 旧内容，再读取下一包。
  // 成功返回 0；正常输入结束返回 AVERROR_EOF；其他负数是读取错误。
  int ReadPacket(AVPacket *packet);

  // 以整个媒体的时间轴定位，实际位置通常落在目标之前的关键帧。
    // 非有限值、负数或超出 int64 微秒范围的时间返回 AVERROR(EINVAL)。
  int Seek(double seconds);

private:
  AVFormatContext *m_context = nullptr;
};
} // namespace PLAYER
