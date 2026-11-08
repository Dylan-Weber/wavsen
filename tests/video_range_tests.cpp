#if LITO_FEAT_VIDEO
#    include <rstd/macro.hpp>

import rstd;
import wavsen.video;

namespace wavsen::tests::video_range
{

using namespace rstd::prelude;
using namespace rstd::literals;

// tests/data/full-range.mp4: 64x64, yuvj420p (full range), black above and white below, lossless
int run(int, char**) {
    auto path = rstd::path::PathBuf::from(rstd::ffi::CStr::from_ptr(__FILE__).to_str().unwrap());
    auto dir  = rstd::path::PathBuf::from(*path.as_path().parent());
    auto clip = dir.join("data/full-range.mp4"_str);
    auto name = clip.as_path().as_os_str().to_str().unwrap();

    auto decoder = wavsen::video::VideoDecoder::open(name, u32(64), u32(64), false).unwrap();
    wavsen::video::Nv12Frame frame;
    rstd_assert(decoder->next_frame(frame).unwrap() == wavsen::video::NextFrame::Ok);
    // the frame says full range (1), so its luma must still span 0..255: black 0, white 255. swscale used to turn the
    // yuvj420p picture into limited-range NV12 (16..235) and the converter then expanded it a second time
    rstd_assert(frame.color_range == u32(1));
    const auto black = frame.data[usize(16 * 64 + 32)];
    const auto white = frame.data[usize(48 * 64 + 32)];
    rstd::io::println("full-range luma: black {} white {}", u32(black), u32(white));
    rstd_assert(black <= 2);
    rstd_assert(white >= 253);
    return 0;
}

} // namespace wavsen::tests::video_range
#endif
