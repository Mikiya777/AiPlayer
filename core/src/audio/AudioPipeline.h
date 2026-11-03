#pragma once

#include "core/MediaTypes.h"
#include <cstdint>
#include <string>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace PLAYER {
struct AudioOutputConfig {
  int m_sample_rate = 44100;
  AVSampleFormat m_sample_format = AV_SAMPLE_FMT_S16;
  std::uint64_t m_channel_layout = AV_CH_LAYOUT_STEREO;
};

//类承担的职责是：把解码得到的 PCM，转换为后续音频设备需要的 PCM 格式。
// 音频处理链的第一步：把已解码的 PCM 转换为下游需要的 PCM。
// 对应 FFmpeg 4.4：创建配置 → 初始化 → 多次转换 → 排空 → 释放。
// 不拥有设备，不更新音频播放时钟；由一个音频处理线程顺序调用，不支持并发/重入。
class AudioPipeline {
public:
  AudioPipeline() = default;
  ~AudioPipeline();
  AudioPipeline(const AudioPipeline &) = delete;
  AudioPipeline &operator=(const AudioPipeline &) = delete;

  // 从首帧读取输入格式，但不消耗该帧；Open 后仍需对首帧调用 Process。
  // 输出默认 44100Hz/双声道/S16，实际接入设备时可按设备要求配置。
  // 再次 Open 会丢弃旧缓存；正常结束需先 Drain，跳转则可以直接 Close/Open。
  int Open(const AVFrame &firstFrame, const AudioOutputConfig &config = {});

  // 0=输入已接受；output 可能为空（转换器暂时缓存输入）。
  // 非空 output 自己拥有内存，可转移给后续队列。每次调用会先清空旧 output。
  // input 不得引用 output 持有的同一帧；这种原地转换请求返回 EINVAL 且保留原帧。
  // 输出 PTS 的单位见 GetOutputTimeBase；保留原媒体起点，不强行归零。
  // 输入格式改变返回 AVERROR_INPUT_CHANGED；PTS 缺失/超过容差的不连续返回 INVALIDDATA。
  // 容差为一个输入时间基 tick 与 1ms 的较大者，输出仍按实际采样数连续累计。
  // 这两类验证错误不消耗输入，可 Drain 旧段后 Open 新段再重送。
  // 开始 Drain 后不能再送入新帧，需重新 Open。
  int Process(const AVFrame &input, AVRational inputTimeBase, FramePtr &output);

  // 全部解码帧处理完后反复调用：0=交付一帧，EOF=已排空；重复调用 EOF 安全。
  // 不传入新的 PCM，调用 swr_convert(nullptr, 0) 取出内部延迟数据。
  int Drain(FramePtr &output);
  void Close();

  // 一个时间单位对应输出每声道的一个采样，时长=nb_samples/sample_rate。
  AVRational GetOutputTimeBase() const;
  const std::string& GetError() const { return m_error; }

private:
  int Convert(const uint8_t **input, int samples, FramePtr &output);

  SwrContext *m_context = nullptr;
  std::string m_error;
  AudioOutputConfig m_output_config;
  int m_input_rate = 0;
  int m_input_channels = 0;
  std::uint64_t m_input_layout = 0;
  AVSampleFormat m_input_format = AV_SAMPLE_FMT_NONE;
  std::int64_t m_next_input_pts = 0;
  std::int64_t m_next_output_pts = 0;
  bool m_has_timestamp = false; //是否已经建立了输入时间轴
  bool m_draining = false; //是否已开始排空
  bool m_drained = false; //内部剩余数据是否已经取完
  bool m_failed = false;
};
} // namespace PLAYER
