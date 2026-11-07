module wavsen.decode;

import rstd;
import wavsen.ffi.ffmpeg;
import wavsen.ffi.ffmpeg.owner;

using namespace rstd::prelude;
using namespace rstd::literals;

namespace wavsen::decode
{

namespace
{

using ffi::ffmpeg::CodecCtxPtr;
using ffi::ffmpeg::FmtCtxPtr;
using ffi::ffmpeg::FramePtr;
using ffi::ffmpeg::PacketPtr;
using ffi::ffmpeg::SwsPtr;

String av_err_str(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(rc, buf, sizeof(buf));
    return String::make(rstd::ffi::CStr::from_ptr(buf).to_str().unwrap());
}

Error mk(ErrorKind kind, String message) { return Error { kind, rstd::move(message) }; }

Error mk(ErrorKind kind, const char* message) {
    return mk(kind, String::make(rstd::ffi::CStr::from_ptr(message).to_str().unwrap()));
}

void compute_target(int src_w, int src_h, u32 max_edge, u32& tw, u32& th) {
    if (src_w <= 0 || src_h <= 0) {
        tw = th = u32();
        return;
    }
    const auto sw = u32(static_cast<rstd::uint32_t>(src_w));
    const auto sh = u32(static_cast<rstd::uint32_t>(src_h));
    if (sw <= max_edge && sh <= max_edge) {
        tw = sw;
        th = sh;
        return;
    }
    if (sw >= sh) {
        tw = max_edge;
        th = u32(rstd::cmp::max<rstd::uint32_t>(
            1u,
            static_cast<rstd::uint32_t>(static_cast<double>(sh.to_primitive()) *
                                        static_cast<double>(max_edge.to_primitive()) /
                                        static_cast<double>(sw.to_primitive()))));
    } else {
        th = max_edge;
        tw = u32(rstd::cmp::max<rstd::uint32_t>(
            1u,
            static_cast<rstd::uint32_t>(static_cast<double>(sw.to_primitive()) *
                                        static_cast<double>(max_edge.to_primitive()) /
                                        static_cast<double>(sh.to_primitive()))));
    }
}

} // namespace

auto probe_media(ref<str> path, const ProbeOptions& opts) -> Result<MediaInfo, Error> {
    if (path.is_empty() || opts.operation_ms == u64() || opts.probe_bytes == u64())
        return Err(mk(ErrorKind::InvalidArgs, "probe requires a local path and limits"));
    struct Deadline {
        rstd::time::Instant started = rstd::time::Instant::now();
        u64                 limit;
        static int          interrupted(void* opaque) {
            auto& self = *static_cast<Deadline*>(opaque);
            return self.started.elapsed().as_millis() >= self.limit;
        }
    } deadline { .limit = opts.operation_ms };
    auto name = rstd::ffi::CString::make(String::make(path));
    if (name.is_err()) return Err(mk(ErrorKind::InvalidArgs, "invalid path"));
    auto* raw = avformat_alloc_context();
    if (! raw) return Err(mk(ErrorKind::OpenFailed, "cannot allocate format context"));
    raw->interrupt_callback = { Deadline::interrupted, &deadline };
    raw->probesize =
        static_cast<rstd::int64_t>(opts.probe_bytes.min(u64(64 * 1024 * 1024)).to_primitive());
    raw->max_analyze_duration = 1000000;
    raw->max_streams          = 64;
    AVDictionary* options     = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    int rc = avformat_open_input(&raw, name->as_ptr(), nullptr, &options);
    av_dict_free(&options);
    if (rc < 0) return Err(mk(ErrorKind::OpenFailed, av_err_str(rc)));
    FmtCtxPtr          format(raw);
    Vec<AVDictionary*> decoder_options;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        AVDictionary* decoder_option = nullptr;
        av_dict_set(&decoder_option, "threads", "1", 0);
        av_dict_set(&decoder_option, "max_pixels", "268435456", 0);
        decoder_options.push(rstd::move(decoder_option));
    }
    avformat_find_stream_info(format.get(),
                              decoder_options.is_empty() ? nullptr : decoder_options.data());
    for (auto& option : decoder_options) av_dict_free(&option);
    int stream = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0) return Err(mk(ErrorKind::NoVideoStream, "no video/image stream"));
    auto* parameters = format->streams[stream]->codecpar;
    if (parameters->width > 0 && parameters->height > 0)
        return Ok(MediaInfo { u32(parameters->width), u32(parameters->height) });
    auto* decoder = avcodec_find_decoder(parameters->codec_id);
    if (! decoder) return Err(mk(ErrorKind::DecoderInit, "decoder unavailable"));
    CodecCtxPtr codec(avcodec_alloc_context3(decoder));
    if (! codec || avcodec_parameters_to_context(codec.get(), parameters) < 0)
        return Err(mk(ErrorKind::DecoderInit, "cannot configure decoder"));
    codec->thread_count = 1;
    codec->max_pixels   = 268435456;
    if (avcodec_open2(codec.get(), decoder, nullptr) < 0)
        return Err(mk(ErrorKind::DecoderInit, "cannot open decoder"));
    PacketPtr packet(av_packet_alloc());
    FramePtr  frame(av_frame_alloc());
    if (! packet || ! frame) return Err(mk(ErrorKind::DecodeFailed, "cannot allocate frame"));
    for (int i = 0; i < 64 && ! Deadline::interrupted(&deadline); ++i) {
        rc = av_read_frame(format.get(), packet.get());
        if (rc < 0) break;
        if (packet->stream_index == stream) {
            rc = avcodec_send_packet(codec.get(), packet.get());
            if (rc >= 0 && avcodec_receive_frame(codec.get(), frame.get()) == 0 &&
                frame->width > 0 && frame->height > 0)
                return Ok(MediaInfo { u32(frame->width), u32(frame->height) });
        }
        av_packet_unref(packet.get());
    }
    return Err(mk(ErrorKind::DecodeFailed, "no valid dimensions within probe limits"));
}

auto extract_thumbnail(ref<str> path, const ThumbOptions& opts) -> Result<RgbaImage, Error> {
    if (opts.max_edge == u32()) {
        return Err(mk(ErrorKind::InvalidArgs, "max_edge must be non-zero"));
    }

    auto path_c   = rstd::ffi::CString::make(String::make(path)).unwrap();
    auto path_raw = path_c.as_ptr();

    AVFormatContext* raw_fmt = nullptr;
    if (int rc = avformat_open_input(&raw_fmt, path_raw, nullptr, nullptr); rc < 0) {
        return Err(
            mk(ErrorKind::OpenFailed, rstd::format("avformat_open_input: {}", av_err_str(rc))));
    }
    FmtCtxPtr fmt(raw_fmt);

    if (int rc = avformat_find_stream_info(fmt.get(), nullptr); rc < 0) {
        return Err(mk(ErrorKind::OpenFailed,
                      rstd::format("avformat_find_stream_info: {}", av_err_str(rc))));
    }

    int video_idx = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_idx < 0) {
        return Err(mk(ErrorKind::NoVideoStream, "no video/image stream in file"));
    }

    AVStream*          st  = fmt->streams[video_idx];
    AVCodecParameters* par = st->codecpar;

    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (! dec) {
        return Err(mk(ErrorKind::DecoderInit,
                      rstd::format("no decoder for codec {}", avcodec_get_name(par->codec_id))));
    }

    CodecCtxPtr cctx(avcodec_alloc_context3(dec));
    if (! cctx) {
        return Err(mk(ErrorKind::DecoderInit, "avcodec_alloc_context3 failed"));
    }
    if (int rc = avcodec_parameters_to_context(cctx.get(), par); rc < 0) {
        return Err(mk(ErrorKind::DecoderInit,
                      rstd::format("avcodec_parameters_to_context: {}", av_err_str(rc))));
    }
    cctx->pkt_timebase = st->time_base;
    if (int rc = avcodec_open2(cctx.get(), dec, nullptr); rc < 0) {
        return Err(mk(ErrorKind::DecoderInit, rstd::format("avcodec_open2: {}", av_err_str(rc))));
    }

    // Seek to a sensible thumbnail target if the source is long enough. Failure
    // here is non-fatal — falling through decodes from the start.
    f64 duration_sec;
    if (fmt->duration > 0) {
        duration_sec = f64(static_cast<double>(fmt->duration) / AV_TIME_BASE);
    } else if (st->duration > 0 && st->time_base.den > 0) {
        duration_sec = f64(static_cast<double>(st->duration) * ffi::ffmpeg::av_q2d(st->time_base));
    }
    if (duration_sec > f64(0.5)) {
        f64 target_sec = opts.seek_seconds.max(duration_sec * opts.seek_fraction);
        target_sec     = target_sec.min((duration_sec - f64(0.05)).max(f64()));
        if (target_sec > f64()) {
            const rstd::int64_t ts =
                static_cast<rstd::int64_t>(target_sec.to_primitive() * AV_TIME_BASE);
            const int seek_flags = opts.prefer_keyframe ? AVSEEK_FLAG_BACKWARD : AVSEEK_FLAG_ANY;
            if (av_seek_frame(fmt.get(), -1, ts, seek_flags) >= 0) {
                avcodec_flush_buffers(cctx.get());
            }
        }
    }

    PacketPtr pkt(av_packet_alloc());
    FramePtr  src_frame(av_frame_alloc());
    if (! pkt || ! src_frame) {
        return Err(mk(ErrorKind::DecoderInit, "av_packet_alloc / av_frame_alloc failed"));
    }

    bool got_frame = false;
    while (! got_frame) {
        int rc = av_read_frame(fmt.get(), pkt.get());
        if (rc == AVERROR_EOF) {
            avcodec_send_packet(cctx.get(), nullptr);
        } else if (rc < 0) {
            return Err(
                mk(ErrorKind::DecodeFailed, rstd::format("av_read_frame: {}", av_err_str(rc))));
        } else if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt.get());
            continue;
        } else {
            rc = avcodec_send_packet(cctx.get(), pkt.get());
            av_packet_unref(pkt.get());
            if (rc < 0 && rc != AVERROR(EAGAIN)) {
                return Err(mk(ErrorKind::DecodeFailed,
                              rstd::format("avcodec_send_packet: {}", av_err_str(rc))));
            }
        }
        while (true) {
            rc = avcodec_receive_frame(cctx.get(), src_frame.get());
            if (rc == AVERROR(EAGAIN)) break;
            if (rc == AVERROR_EOF) {
                return Err(
                    mk(ErrorKind::DecodeFailed, "decoder flushed without producing a frame"));
            }
            if (rc < 0) {
                return Err(mk(ErrorKind::DecodeFailed,
                              rstd::format("avcodec_receive_frame: {}", av_err_str(rc))));
            }
            got_frame = true;
            break;
        }
    }

    const auto src_fmt = static_cast<AVPixelFormat>(src_frame->format);
    const int  src_w   = src_frame->width;
    const int  src_h   = src_frame->height;
    if (src_w <= 0 || src_h <= 0 || src_fmt == AV_PIX_FMT_NONE) {
        return Err(mk(ErrorKind::DecodeFailed, "decoded frame has invalid dimensions/format"));
    }

    u32 tw;
    u32 th;
    compute_target(src_w, src_h, opts.max_edge, tw, th);
    if (tw == u32() || th == u32()) {
        return Err(mk(ErrorKind::ScaleFailed, "computed target size is zero"));
    }

    SwsPtr sws(sws_getContext(src_w,
                              src_h,
                              src_fmt,
                              static_cast<int>(tw.to_primitive()),
                              static_cast<int>(th.to_primitive()),
                              AV_PIX_FMT_RGBA,
                              SWS_BICUBIC,
                              nullptr,
                              nullptr,
                              nullptr));
    if (! sws) {
        return Err(
            mk(ErrorKind::ScaleFailed,
               rstd::format("sws_getContext failed (src={})", av_get_pix_fmt_name(src_fmt))));
    }

    RgbaImage out;
    out.width  = tw;
    out.height = th;
    out.stride = tw * u32(4);
    out.data.resize(usize(out.stride.to_primitive()) * usize(th.to_primitive()), 0u);

    rstd::uint8_t* dst_planes[4]  = { out.data.data(), nullptr, nullptr, nullptr };
    int            dst_strides[4] = { static_cast<int>(out.stride.to_primitive()), 0, 0, 0 };

    int scaled = sws_scale(
        sws.get(), src_frame->data, src_frame->linesize, 0, src_h, dst_planes, dst_strides);
    if (scaled <= 0) {
        return Err(mk(ErrorKind::ScaleFailed, "sws_scale produced no rows"));
    }

    return Ok(rstd::move(out));
}

} // namespace wavsen::decode
