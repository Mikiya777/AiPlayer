#pragma once
#include "core/MediaTypes.h"
#include <inspireface.h>
#include <string>
#include <vector>

namespace PLAYER {
// 源图像像素坐标；渲染时统一处理缩放、黑边和翻转。
struct FaceStickerPose { float x, y, width, angle; };
class FaceStickerFilter {
public:
    FaceStickerFilter() = default;
    FaceStickerFilter(const FaceStickerFilter&) = delete;
    FaceStickerFilter& operator=(const FaceStickerFilter&) = delete;
    ~FaceStickerFilter();
    bool Initialize(const std::string& modelPath);
    bool Detect(const AVFrame& frame, std::vector<FaceStickerPose>& poses);
    const std::string& GetError() const { return m_error; }
private:
    HFSession m_session = nullptr;
    std::vector<uint8_t> m_pixels;
    std::string m_error;
};
}
