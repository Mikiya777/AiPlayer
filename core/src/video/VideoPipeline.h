#pragma once
#include "core/MediaTypes.h"
extern "C" {
#include <libswscale/swscale.h>
}
namespace PLAYER {
// CPU 视频处理链：解码帧（通常 YUV）→ RGBA AVFrame。
// 输出仍是 CPU 内存，不是 OpenGL 纹理；上传由 VideoRenderer 完成。
// 同一个实例由一个视频工作线程使用。PTS、时长、像素宽高比随输出保留。
class VideoPipeline {
public:
    VideoPipeline() = default;
    ~VideoPipeline();
    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;
    // 0/0 保持输入尺寸；指定尺寸时同时缩放。每帧按真实格式/尺寸更新 SwsContext。
    int Process(const AVFrame& input, FramePtr& output, int width = 0, int height = 0);
    void Close();
private:
    SwsContext* m_context = nullptr;
};
} // namespace PLAYER
