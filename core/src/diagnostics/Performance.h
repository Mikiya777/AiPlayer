#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace PLAYER {
// 显式启用且 Begin 后才采集；默认关闭。固定样本上限，退出测量后统一写盘。
// 数值默认单位为 ms；带 _frames/_mib/_count 后缀的项使用名称对应单位。
class Performance {
public:
  static Performance &Get() {
    static Performance value;
    return value;
  }
  static double Now() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  std::atomic<double> m_audio_media_time{0};
  bool Active() const {
    return m_active.load(std::memory_order_relaxed);
  }
  void Begin() {
    if (!std::getenv("AIPLAYER_PROFILE")) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_samples.clear();
    m_active = true;
  }
  void Add(const char *name, double value) {
    if (!Active() || !std::isfinite(value)) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!Active()) return;
    auto &samples = m_samples[name];
    if (samples.size() < 200000) samples.push_back(value);
  }
  bool End(const std::string &path) {
    m_active = false;
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ofstream file(path);
    file << "metric,samples,mean,p95,max,min,p05,sum\n";
    for (auto &[name, values] : m_samples) {
      if (values.empty()) continue;
      std::sort(values.begin(), values.end());
      double sum = 0;
      for (auto v : values) sum += v;
      file << name << ',' << values.size() << ',' << sum / values.size() << ','
           << values[static_cast<size_t>(std::ceil(values.size() * .95)) - 1] << ','
           << values.back() << ',' << values.front() << ',' << values[static_cast<size_t>((values.size() - 1) * .05)] << ',' << sum << '\n';
    }
    file.flush();
    return bool(file);
  }

private:
  std::atomic<bool> m_active{false};
  std::mutex m_mutex;
  std::map<std::string, std::vector<double>> m_samples;
};
class PerformanceScope {
public:
  explicit PerformanceScope(const char *name) : m_name(name),
                                                m_start(Performance::Get().Active() ? Performance::Now() : 0) {}
  ~PerformanceScope() {
    if (m_start) Performance::Get().Add(m_name, Performance::Now() - m_start);
  }

private:
  const char *m_name;
  double m_start;
};
} // namespace PLAYER
