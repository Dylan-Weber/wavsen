#if LITO_FEAT_VIDEO
#    include <rstd/macro.hpp>
#    include <sys/resource.h>
#    include <chrono>
#    include <cstdlib>
#    include <string>

import rstd;
import wavsen.video;

namespace wavsen::tests::video_bench
{

using namespace rstd::prelude;
using namespace rstd::literals;

namespace
{

double cpu_seconds() {
    rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1e-6;
}

long rss_kb() {
    rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss;
}

} // namespace

// Manual measurement, not part of the default run:
//   wavsen-tests video-bench VIDEO MODE [RENDER_NODE] [FRAMES]
// MODE is auto, vulkan, vaapi, cuda or none. Decodes FRAMES frames as fast as possible and prints the
// process CPU time per frame, the wall time and the decoder in use.
int run(int argc, char** argv) {
    if (argc < 3) return 0;
    std::string path = argv[1];
    std::string mode = argv[2];
    std::string node = argc > 3 ? argv[3] : "";
    int         want = argc > 4 ? std::atoi(argv[4]) : 600;

    wavsen::video::OpenOpts opts;
    if (mode == "vulkan") opts.hwaccel = wavsen::video::HwAccel::Vulkan;
    else if (mode == "vaapi") opts.hwaccel = wavsen::video::HwAccel::Vaapi;
    else if (mode == "cuda") opts.hwaccel = wavsen::video::HwAccel::Cuda;
    else if (mode == "none") opts.hwaccel = wavsen::video::HwAccel::None;

    auto producer = (node.empty()
                         ? wavsen::video::Producer::create(u32(1920), u32(1080))
                         : wavsen::video::Producer::create_with_render_node(
                               u32(1920), u32(1080), rstd::ffi::CStr::from_ptr(node.c_str()).to_str().unwrap()))
                        .unwrap();
    auto decoder = wavsen::video::VideoDecoder::open_with_vk(
                       rstd::ffi::CStr::from_ptr(path.c_str()).to_str().unwrap(),
                       u32(0),
                       u32(0),
                       true,
                       *producer,
                       opts)
                       .unwrap();
    const bool vk = decoder->using_vk_frames();
    wavsen::video::Nv12Frame frame;

    const double cpu0  = cpu_seconds();
    const auto   wall0 = std::chrono::steady_clock::now();
    int          frames = 0;
    while (frames < want) {
        if (vk) {
            auto pulled = decoder->next_vk_frame().unwrap();
            (void)pulled;
        } else {
            (void)decoder->next_frame(frame).unwrap();
        }
        ++frames;
    }
    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
    const double cpu = cpu_seconds() - cpu0;
    rstd::io::println("bench mode={} backend={} kind={} frames={} cpu_ms_per_frame={} wall_s={} maxrss_mb={}",
                      mode.c_str(),
                      int(decoder->backend()),
                      int(decoder->kind()),
                      frames,
                      cpu * 1000.0 / frames,
                      wall,
                      rss_kb() / 1024);
    return 0;
}

} // namespace wavsen::tests::video_bench
#endif
