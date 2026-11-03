#pragma once
#include <cmath>
namespace PLAYER {
// 渲染线程使用；在昂贵的 GPU 读回之前按媒体时间选取 30fps 画面。
class RecordingCadence {
public:
    void Reset(double start) { m_start=start; m_next=start; }
    bool Take(double time) {
        if(!std::isfinite(time) || time+0.000001<m_next)return false;
        m_next=m_start+(std::floor((time-m_start)*30+0.00003)+1)/30;
        return true;
    }
private:
    double m_start=0, m_next=0;
};
}
