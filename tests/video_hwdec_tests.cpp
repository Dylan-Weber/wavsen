#if LITO_FEAT_VIDEO
#    include <rstd/macro.hpp>
#    include <dlfcn.h>
#    include <cstdio>
#    include <string>

import rstd;
import wavsen.video;

namespace wavsen::tests::video_hwdec
{

using namespace rstd::prelude;
using namespace rstd::literals;

namespace
{

bool has_extension(const wavsen::video::Producer& producer, const char* name) {
    auto exts = producer.enabled_device_extensions();
    for (usize i {}; i < exts.len(); ++i) {
        if (std::string(exts[i]) == name) return true;
    }
    return false;
}

// tests/data/hwdec-h264.mp4: 320x240, 30 frames of yuv420p High-profile h264 with B-frames
String clip_path() {
    auto path = rstd::path::PathBuf::from(rstd::ffi::CStr::from_ptr(__FILE__).to_str().unwrap());
    auto dir  = rstd::path::PathBuf::from(*path.as_path().parent());
    auto clip = dir.join("data/hwdec-h264.mp4"_str);
    return String::make(clip.as_path().as_os_str().to_str().unwrap());
}

int count_frames(wavsen::video::VideoDecoder& decoder) {
    wavsen::video::Nv12Frame frame;
    int                      frames = 0;
    while (frames < 100) {
        auto next = decoder.next_frame(frame).unwrap();
        if (next == wavsen::video::NextFrame::Eof) break;
        ++frames;
    }
    return frames;
}

} // namespace

// Every Vulkan device that can decode h264 must take the Vulkan decode path. NVIDIA's decode
// output images do not support the STORAGE usage, which the frame check used to demand, so the
// decoder fell back to software on exactly the GPU that has the hardware.
static int vulkan_decode_is_used(const String& clip) {
    int tested = 0;
    for (int minor = 128; minor < 136; ++minor) {
        auto node = rstd::format("/dev/dri/renderD{}", minor);
        auto prod = wavsen::video::Producer::create_with_render_node(u32(320), u32(240), node.as_str());
        if (prod.is_err()) continue;
        auto producer = rstd::move(prod).unwrap();
        if (! has_extension(*producer, "VK_KHR_video_decode_h264")) {
            rstd::io::println("{}: no Vulkan h264 decode, skipped", node.as_str());
            continue;
        }
        wavsen::video::OpenOpts opts;
        opts.hwaccel = wavsen::video::HwAccel::Vulkan;
        auto decoder = wavsen::video::VideoDecoder::open_with_vk(
                           clip.as_str(), u32(320), u32(240), false, *producer, opts)
                           .unwrap();
        rstd::io::println("{}: kind {}", node.as_str(), int(decoder->kind()));
        rstd_assert(decoder->kind() == wavsen::video::FrameKind::VulkanShared);
        ++tested;
    }
    rstd::io::println("vulkan decode tested on {} device(s)", tested);
    return 0;
}

// hwdec=cuda decodes on NVDEC and hands the frames over as ordinary system-memory pictures, so a
// display GPU without any video decoder (Intel without a libva driver) still avoids software decode.
static int cuda_decode_is_used(const String& clip) {
    void* cuda = dlopen("libcuda.so.1", RTLD_LAZY);
    if (! cuda) {
        rstd::io::println("libcuda.so.1 not found, cuda decode skipped");
        return 0;
    }
    dlclose(cuda);
    auto prod = wavsen::video::Producer::create(u32(320), u32(240));
    if (prod.is_err()) {
        rstd::io::println("no Vulkan producer, cuda decode skipped");
        return 0;
    }
    auto                    producer = rstd::move(prod).unwrap();
    wavsen::video::OpenOpts opts;
    opts.hwaccel = wavsen::video::HwAccel::Cuda;
    auto decoder = wavsen::video::VideoDecoder::open_with_vk(
                       clip.as_str(), u32(320), u32(240), false, *producer, opts)
                       .unwrap();
    rstd::io::println("cuda: backend {}", int(decoder->backend()));
    rstd_assert(decoder->backend() == wavsen::video::HwAccel::Cuda);
    rstd_assert(decoder->kind() == wavsen::video::FrameKind::Sw);
    rstd_assert(count_frames(*decoder) == 30);
    return 0;
}

// hwdec=none never touches a hardware decoder
static int none_is_software(const String& clip) {
    auto producer = wavsen::video::Producer::create(u32(320), u32(240));
    if (producer.is_err()) return 0;
    auto                    prod = rstd::move(producer).unwrap();
    wavsen::video::OpenOpts opts;
    opts.hwaccel = wavsen::video::HwAccel::None;
    auto decoder = wavsen::video::VideoDecoder::open_with_vk(
                       clip.as_str(), u32(320), u32(240), false, *prod, opts)
                       .unwrap();
    rstd_assert(decoder->backend() == wavsen::video::HwAccel::None);
    rstd_assert(count_frames(*decoder) == 30);
    return 0;
}

int run(int, char**) {
    auto clip = clip_path();
    if (int rc = vulkan_decode_is_used(clip)) return rc;
    if (int rc = cuda_decode_is_used(clip)) return rc;
    return none_is_software(clip);
}

} // namespace wavsen::tests::video_hwdec
#endif
