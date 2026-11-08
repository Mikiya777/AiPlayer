#pragma once
#include "core/MediaTypes.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
namespace PLAYER {
struct RecordingConfig {
  std::string m_path, m_source;
  std::string m_container = "mp4", m_video_codec = "libx264", m_audio_codec = "aac";
  int m_width = 0, m_height = 0;
  double m_start = 0;
};
// 一次录制会话。编码和文件操作只在工作线程；Submit 不阻塞渲染线程。
class FileWriter {
public:
  explicit FileWriter(RecordingConfig config);
  ~FileWriter();
  void Submit(FramePtr frame, double mediaTime);
  void Stop(double mediaTime);
  void Fail(const std::string &error);
  bool IsReady() const {
    return m_ready.load();
  }
  double GetStartTime() const {
    return m_actual_start.load();
  }
  bool IsDone() const {
    return m_done.load();
  }
  std::string GetError() const;
  const RecordingConfig &GetConfig() const {
    return m_config;
  }

private:
  void Run();
  RecordingConfig m_config;
  struct Entry {
    FramePtr m_frame;
    double m_time;
    double m_profile_enqueue_ms;
  };
  std::mutex m_mutex;
  std::condition_variable m_wake;
  std::deque<Entry> m_queue;
  size_t m_bytes = 0;
  bool m_stopping = false;
  double m_end = 0;
  std::atomic<bool> m_done{false};
  std::atomic<bool> m_ready{false};
  std::atomic<double> m_actual_start{-1};
  std::string m_error;
  mutable std::mutex m_error_mutex;
  std::thread m_thread;
};
} // namespace PLAYER
