#pragma once

#include "audio/IAudioOutput.h"
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace PLAYER {
// 生产者（解码线程）：Enqueue → 队列满就保留原帧稍后重试 → EndInput。
// 消费者（Qt 主线程）：每隔几毫秒 Pump → 读设备空间 → 分批写 → 更新时钟。
// 设备相关方法必须由构造线程调用；Enqueue/EndInput/GetGeneration 可跨线 程。
// 销毁前控制者必须停止并 join 生产者，保证不会访问已经销毁的 renderer。
class AudioRenderer {
public:
  explicit AudioRenderer(std::unique_ptr<IAudioOutput> output);
  ~AudioRenderer();
  AudioRenderer(const AudioRenderer &) = delete;
  AudioRenderer &operator=(const AudioRenderer &) = delete;

  int Open(AudioOutputConfig requested = {});
  const AudioOutputConfig &GetOutputConfig() const {
    return m_config;
  } // 主线程
  std::uint64_t GetGeneration() const;
  // 首个成功入队 PCM 的媒体时间，供控制器保留音视频起始偏移；可跨线程读取。
  std::optional<double> GetStartTime() const;
  // frame 必须是与 GetOutputConfig 一致的 packed PCM，时间戳必须连续。
  // 内部保留帧引用，调用者可释放原帧；成功后不要再修改共享 PCM 内存。
  // EAGAIN=队列满，未接受；ECANCELED=已停止或旧代次；EOF=输入已结束。
  // ERANGE=时间戳换算或帧尾位置越界，未接受且不改变时间轴。
  int Enqueue(const AVFrame &frame, AVRational timeBase, std::uint64_t generation);
  int EndInput(std::uint64_t generation);

  // 主线程定时调用：0=仍在工作（也可能暂停/等待输入），EOF=正常播放结束。
  // 负数错误时停止设备并取消后续入队；空队列不等于整个媒体播放结束。
  int Pump();
  // 暂停/恢复失败时停止设备、清空队列并更新代次；再次使用需 Open。
  // 成功后才更新暂停状态；这不表示已经标定真实扬声器的输出延迟。
  int SetPaused(bool paused);
  int SetVolume(double volume);
  void Stop();
  // 最近一次 Pump/暂停前刷新得到的原始媒体时间，可传给同步器 UpdateAudioClock。
  // 首次提交前无值；这是设备处理进度的估算，未标定扬声器/蓝牙的额外延迟。
  std::optional<double> GetPlaybackTime() const {
    return m_clock;
  } // 主线程

private:
  struct PendingAudio {
    FramePtr m_frame;
    std::int64_t m_bytes = 0;
    std::int64_t m_offset = 0; // 用字节计数，部分写入时不能丢掉剩余数据。
  };
  bool IsOwnerThread() const;
  void RefreshClock();
  void ClearQueue(); // 调用前必须持锁。

  std::unique_ptr<IAudioOutput> m_output;
  const std::thread::id m_owner_thread;
  AudioOutputConfig m_config;
  mutable std::mutex m_mutex;
  std::deque<PendingAudio> m_queue;
  std::uint64_t m_generation = 0;
  std::int64_t m_capacity_bytes = 0;
  std::int64_t m_queued_bytes = 0;
  std::int64_t m_submitted_bytes = 0;
  std::int64_t m_next_pts = 0;
  int m_bytes_per_frame = 0; // 同一采样时刻的全部声道所占字节数。
  double m_first_time = 0.0;
  std::optional<double> m_clock;
  bool m_has_timestamp = false;
  bool m_opened = false;
  bool m_input_finished = false;
  bool m_finished = false;
  bool m_paused = false;
};
} // namespace PLAYER
