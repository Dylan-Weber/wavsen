export module wavsen.decode;

import rstd;

using namespace rstd::prelude;

export namespace wavsen::decode
{

enum class ErrorKind : rstd::int32_t
{
    InvalidArgs,
    OpenFailed,
    NoVideoStream,
    DecoderInit,
    SeekFailed,
    DecodeFailed,
    ScaleFailed,
};

struct Error {
    ErrorKind kind;
    String    message;
};

struct RgbaImage {
    Vec<rstd::uint8_t> data;
    u32                width;
    u32                height;
    u32                stride;
};

struct ThumbOptions {
    u32  max_edge { 512 };
    f64  seek_seconds { 1.0 };
    f64  seek_fraction { 0.10 };
    bool prefer_keyframe { true };
};

struct MediaInfo {
    u32 width, height;
};

struct ProbeOptions {
    u64 operation_ms { 10000 };
    u64 probe_bytes { 8 * 1024 * 1024 };
};

auto probe_media(ref<str> path, const ProbeOptions& opts = {}) -> Result<MediaInfo, Error>;

auto extract_thumbnail(ref<str> path, const ThumbOptions& opts) -> Result<RgbaImage, Error>;

} // namespace wavsen::decode
