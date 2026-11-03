#include "video/VideoPipeline.h"
#include <cerrno>
#include <cstdlib>
extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}
namespace PLAYER {
VideoPipeline::~VideoPipeline() { Close(); }
void VideoPipeline::Close() { sws_freeContext(m_context); m_context = nullptr; }

int VideoPipeline::Process(const AVFrame& input, FramePtr& output, int width, int height)
{
    if (output.get() == &input) return AVERROR(EINVAL);
    output.reset();
    if (width == 0 && height == 0) { width = input.width; height = input.height; }
    if (input.width <= 0 || input.height <= 0 || width <= 0 || height <= 0 ||
        av_image_check_size(input.width, input.height, 0, nullptr) < 0 ||
        av_image_check_size(width, height, 0, nullptr) < 0) return AVERROR(EINVAL);
    const auto format = static_cast<AVPixelFormat>(input.format);
    const auto* description = av_pix_fmt_desc_get(format);
    if (!description || !sws_isSupportedInput(format)) return AVERROR(ENOTSUP);
    // 这一版是 SDR 路径；HDR 需要色调映射，不能只转换像素排列就声称正确显示。
    if (input.color_trc == AVCOL_TRC_SMPTE2084 || input.color_trc == AVCOL_TRC_ARIB_STD_B67)
        return AVERROR(ENOTSUP);
    int minimumLinesizes[4]{};
    if (av_image_fill_linesizes(minimumLinesizes, format, input.width) < 0)
        return AVERROR(EINVAL);
    for (int i = 0; i < description->nb_components; ++i) {
        const int plane = description->comp[i].plane;
        // 平面存在还不够：一行必须容纳该像素格式所需的全部字节。
        if (!input.data[plane] || !input.linesize[plane] ||
            std::abs(static_cast<int64_t>(input.linesize[plane])) < minimumLinesizes[plane])
            return AVERROR(EINVAL);
    }
    if (format == AV_PIX_FMT_PAL8 && !input.data[1]) return AVERROR(EINVAL);
    // getCachedContext 在尺寸/格式改变时重新创建；其余帧复用转换器。
    m_context = sws_getCachedContext(m_context, input.width, input.height, format,
                                    width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                    nullptr, nullptr, nullptr);
    if (!m_context) return AVERROR(ENOMEM);
    int matrix = SWS_CS_ITU601; // 没有元数据时采用明确的兼容默认值。
    switch (input.colorspace) {
    case AVCOL_SPC_BT709: matrix = SWS_CS_ITU709; break;
    case AVCOL_SPC_BT2020_NCL: matrix = SWS_CS_BT2020; break;
    case AVCOL_SPC_SMPTE240M: matrix = SWS_CS_SMPTE240M; break;
    default: break;
    }
    const bool fullRange = input.color_range == AVCOL_RANGE_JPEG ||
        (description->flags & AV_PIX_FMT_FLAG_RGB) || format == AV_PIX_FMT_YUVJ420P ||
        format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUVJ444P;
    const int* coefficients = sws_getCoefficients(matrix);
    int ret = sws_setColorspaceDetails(m_context, coefficients, fullRange, coefficients,
                                      1, 0, 1 << 16, 1 << 16);
    if (ret < 0) return ret;
    FramePtr rgba(av_frame_alloc());
    if (!rgba) return AVERROR(ENOMEM);
    rgba->format = AV_PIX_FMT_RGBA;
    rgba->width = width;
    rgba->height = height;
    if ((ret = av_frame_get_buffer(rgba.get(), 32)) < 0) return ret;
    if ((ret = av_frame_copy_props(rgba.get(), &input)) < 0) return ret;
    // 行跨度可能大于 width*4，甚至输入是负跨度；把真实 linesize 交给 sws。
    ret = sws_scale(m_context, input.data, input.linesize, 0, input.height,
                    rgba->data, rgba->linesize);
    if (ret != height) return ret < 0 ? ret : AVERROR(EIO);
    rgba->colorspace = AVCOL_SPC_RGB;
    rgba->color_range = AVCOL_RANGE_JPEG;
    if (input.sample_aspect_ratio.num > 0 && input.sample_aspect_ratio.den > 0) {
        rgba->sample_aspect_ratio = av_mul_q(input.sample_aspect_ratio,
            av_div_q(AVRational{input.width, input.height}, AVRational{width, height}));
    }
    output = std::move(rgba);
    return 0;
}
} // namespace PLAYER
