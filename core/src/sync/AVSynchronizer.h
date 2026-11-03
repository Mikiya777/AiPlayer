#pragma once

#include "core/MediaTypes.h"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace PLAYER {
// 同步器只改变帧交付的时机，不做 PCM 重采样、YUV/RGBA 转换或设备播放。
// 配置中的时间单位全部为秒；15ms 是可调整的迟到阈值，不是感知标准。
struct SyncConfig {
  std::size_t m_max_audio_frames = 128;
  // RGBA 4K 每帧约 32 MiB；限制预解码缓存，满时由生产者背压等待。
  std::size_t m_max_video_frames = 4;
  double m_early_tolerance = 0.002;
  double m_late_tolerance = 0.015;
  double m_default_video_duration = 1.0 / 25.0;
};

// 自己持有 AVFrame 引用，解码器释放原帧后仍然有效。
// m_time_seconds 已减去统一时间原点；AVFrame 原始 PTS 和数据保持不变。
struct TimedFrame {
  double m_profile_enqueue_ms = 0;
  FramePtr m_frame;
  AVRational m_time_base = {0, 1};
  double m_time_seconds = 0.0;
  double m_duration_seconds = 0.0;
  std::uint64_t m_generation = 0;
};

enum class VideoAction {
  kEmpty, // 暂时没有视频，稍后再检查。
  kWaitingForClock, // 尚未取得音频实际播放进度。
  kPaused,
  kWait, // 队首视频早到，仍留在队列中。
  kPresent, // 交付一帧，调用者负责处理/渲染。
  kFinished // 输入结束且视频队列已取完。
};

struct VideoDecision {
  VideoAction m_action = VideoAction::kEmpty;
  TimedFrame m_video;
  double m_wait_seconds = 0.0;
  // videoTime - audioTime：正数表示视频早到；负数表示视频落后。
  double m_difference_seconds = 0.0;
  std::size_t m_dropped_frames = 0;
  std::uint64_t m_generation = 0;
};

class AVSynchronizer {
public:
  // 无效配置抛出 std::invalid_argument。
  explicit AVSynchronizer(const SyncConfig &config = {});
  AVSynchronizer(const AVSynchronizer &) = delete;
  AVSynchronizer &operator=(const AVSynchronizer &) = delete;

  std::uint64_t GetGeneration() const;
  // 新文件/跳转时清队列、时钟及结束标志，并返回新的播放代次。
  // 音视频和音频播放时钟使用同一个原点，不能各自减去各自首帧时间。
  // Reset 不替代解码器 seek/flush，也不替代设备清空与暂停。
  std::uint64_t Reset(double commonOriginSeconds = 0.0);
  void SetPaused(bool paused);

  // 适配 FileReader::ReadAll 的两种回调。内部 clone 引用，不接管调用者的帧。
  // 0=接受；EAGAIN=队列满，保留原帧等待后重试；ECANCELED=旧播放代次；
  // EOF=已经结束输入；INVALIDDATA=时间戳缺失/倒退；其他负数=错误。
  // 每一路必须按呈现顺序送入。允许相同 PTS，禁止无提示地用 0 代替未知 PTS。
  int PushAudioFrame(const AVFrame &frame, AVRational timeBase, std::uint64_t generation);
  int PushVideoFrame(const AVFrame &frame, AVRational timeBase, std::uint64_t generation);

  // 音频按队列顺序交给后续音频处理/播放模块，不等待视频。
  // 返回 0 时转移帧所有权；EAGAIN=暂停/暂时无帧；EOF=输入结束且音频队列空。
  // 取出音频不等于播放完成，也不会自动更新音频时钟！
  int PopAudioFrame(TimedFrame &output, std::uint64_t generation);

  // 由音频播放模块在调度前报告“实际播放到的媒体时间”，单位秒。
  // 时间仍采用媒体原始时间轴，本类统一减去 Reset 的 commonOriginSeconds。
  // 本类不擅自按墙钟推进：暂停、设备欠载时不能让音频时钟虚假前进。
  // 调用者需定期刷新；不能传入最新解码帧/最新提交帧的 PTS。
  int UpdateAudioClock(double mediaSeconds, std::uint64_t generation);
  // 第二种来源：已成功提交音频的末尾时刻 - 该提交范围内尚未播放的时长。
  // pendingSeconds 必须包含相应的软件/设备缓冲，不能漏算或重复扣减。
  int UpdateAudioClockFromBuffer(double submittedEndSeconds, double pendingSeconds,
                                 std::uint64_t generation);

  // 不 sleep、不调用外部回调。根据最新时钟返回等待/交付等决定。
  // 等待后应重新查询设备、更新时间，再调用此函数；不要只重复读取旧时钟。
  VideoDecision PollVideo();
  int EndInput(std::uint64_t generation);
  // 仅表示同步器的输入已结束且帧都已交付；不表示设备缓冲已经播放完。
  bool IsDrained() const;

private:
  int PushFrame(const AVFrame &frame, AVRational timeBase,
                std::uint64_t generation, bool audio);

  SyncConfig m_config;
  mutable std::mutex m_mutex;
  std::deque<TimedFrame> m_audio_frames;
  std::deque<TimedFrame> m_video_frames;
  std::optional<double> m_audio_clock;
  std::optional<double> m_last_audio_time;
  std::optional<double> m_last_video_time;
  double m_common_origin = 0.0;
  std::uint64_t m_generation = 1;
  bool m_paused = false;
  bool m_input_finished = false;
};
} // namespace PLAYER
