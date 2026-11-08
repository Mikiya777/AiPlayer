#pragma once
#include "core/MediaTypes.h"
#include <string>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
namespace PLAYER {
// 容器与编码器分离。输出格式由配置指定，扩展时检查编码/容器兼容性。
class Muxer {
public:
  Muxer() = default;
  Muxer(const Muxer &) = delete;
  Muxer &operator=(const Muxer &) = delete;
  ~Muxer();
  void Open(const std::string &path, const std::string &format);
  int AddStream(const AVCodecContext &codec);
  void WriteHeader();
  void Write(AVPacket &packet, AVRational timeBase, int stream);
  void Finish();

private:
  AVFormatContext *m_context = nullptr;
};
void CheckWrite(int result, const char *operation);
} // namespace PLAYER
