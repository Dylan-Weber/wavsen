#if LITO_FEAT_AUDIO
import rstd;
import wavsen.audio;
import wavsen.audio.core;
import wavsen.audio.capture;

namespace wavsen::tests::audio_backend
{

using namespace rstd::prelude;
using namespace wavsen::audio;

void pause_test() { rstd::thread::sleep(rstd::time::Duration::from_millis(u64(20))); }

class SilentChannel : public IPullChannel {
public:
    auto next_pcm(void* output, u32 frames) -> u64 override {
        rstd::mem::memset(output, u8(), usize(frames.to_primitive()) * usize(2 * sizeof(float)));
        return u64(frames.to_primitive());
    }
    void pass_desc(const DeviceDesc&) override {}
};

class TrackedChannel : public IPullChannel {
public:
    explicit TrackedChannel(rstd::sync::atomic::Atomic<bool>& destroyed): destroyed_(destroyed) {}
    ~TrackedChannel() override { destroyed_.store(true, rstd::sync::atomic::Ordering::Release); }
    auto next_pcm(void*, u32) -> u64 override { return u64(); }
    void pass_desc(const DeviceDesc&) override {}

private:
    rstd::sync::atomic::Atomic<bool>& destroyed_;
};

class TrackedStream : public SoundStream {
public:
    explicit TrackedStream(rstd::sync::atomic::Atomic<bool>& destroyed): destroyed_(destroyed) {}
    ~TrackedStream() override { destroyed_.store(true, rstd::sync::atomic::Ordering::Release); }
    auto next_pcm(void*, u32) -> u64 override { return u64(); }
    void pass_desc(const Desc&) override {}

private:
    rstd::sync::atomic::Atomic<bool>& destroyed_;
};

auto make_tracked_stream(rstd::sync::atomic::Atomic<bool>& destroyed)
    -> Box<dyn<SoundStreamObject>> {
    auto stream = Box<TrackedStream>::make(destroyed);
    return Box<dyn<SoundStreamObject>>::from_raw(
        dyn<SoundStreamObject>::from_ptr(rstd::move(stream).into_raw()));
}

int sound_manager_unmount_test() {
    using rstd::sync::atomic::Ordering;
    rstd::sync::atomic::Atomic<bool> first_destroyed { false };
    rstd::sync::atomic::Atomic<bool> second_destroyed { false };
    SoundManager                     manager;
    auto                             first  = manager.mount(make_tracked_stream(first_destroyed));
    auto                             second = manager.mount(make_tracked_stream(second_destroyed));
    if (first.is_none() || second.is_none() || first->value == second->value ||
        ! manager.unmount(*first))
        return 40;
    for (int i = 0; i < 150 && ! first_destroyed.load(Ordering::Acquire); ++i) pause_test();
    if (! first_destroyed.load(Ordering::Acquire) || second_destroyed.load(Ordering::Acquire))
        return 41;
    manager.unmount_all();
    for (int i = 0; i < 150 && ! second_destroyed.load(Ordering::Acquire); ++i) pause_test();
    if (! second_destroyed.load(Ordering::Acquire)) return 42;
    manager.shutdown();
    return 0;
}

auto make_tracked_channel(rstd::sync::atomic::Atomic<bool>& destroyed)
    -> Box<dyn<PullChannelObject>> {
    auto channel = Box<TrackedChannel>::make(destroyed);
    return Box<dyn<PullChannelObject>>::from_raw(
        dyn<PullChannelObject>::from_ptr(rstd::move(channel).into_raw()));
}

int channel_unmount_test() {
    using rstd::sync::atomic::Ordering;
    rstd::sync::atomic::Atomic<bool> first_destroyed { false };
    rstd::sync::atomic::Atomic<bool> second_destroyed { false };
    rstd::sync::atomic::Atomic<u64>  generation { u64() };
    AudioDevice                      device;
    device.set_event_sink(Some(AudioDeviceEventSink::make([&](AudioDeviceEvent event) {
        generation.store(event.generation, Ordering::Release);
    })));
    if (! device.mount(make_tracked_channel(first_destroyed), u64(1), u64(11)) ||
        ! device.mount(make_tracked_channel(second_destroyed), u64(1), u64(12)) ||
        ! device.unmount(u64(11), u64(1)) || ! device.unmount(u64(12), u64()))
        return 30;
    AudioDeviceDesiredState desired;
    desired.generation = u64(1);
    if (! device.apply(desired.clone())) return 31;
    for (int i = 0; i < 150 && generation.load(Ordering::Acquire) != u64(1); ++i) pause_test();
    if (generation.load(Ordering::Acquire) != u64(1) || ! first_destroyed.load(Ordering::Acquire) ||
        second_destroyed.load(Ordering::Acquire))
        return 32;
    if (device.unmount(u64(), u64(1)) || ! device.unmount_all(u64(2))) return 33;
    desired.generation = u64(2);
    if (! device.apply(desired.clone())) return 34;
    for (int i = 0; i < 150 && generation.load(Ordering::Acquire) != u64(2); ++i) pause_test();
    if (generation.load(Ordering::Acquire) != u64(2) || ! second_destroyed.load(Ordering::Acquire))
        return 35;
    device.shutdown();
    device.wait_stopped();
    return 0;
}

auto wait_for(AudioDevice& device, AudioDeviceState expected, u64 buffer_revision = {}) -> bool {
    for (int i = 0; i < 150; ++i) {
        if (device.state() == expected &&
            device.completed_playback_buffer_revision() == buffer_revision)
            return true;
        if (device.state() == AudioDeviceState::Failed) return false;
        pause_test();
    }
    return false;
}

int live_channel_unmount_test() {
    using rstd::sync::atomic::Ordering;
    rstd::sync::atomic::Atomic<bool> first_destroyed { false };
    rstd::sync::atomic::Atomic<bool> second_destroyed { false };
    AudioDevice                      device;
    if (! device.mount(make_tracked_channel(first_destroyed), u64(1), u64(11)) ||
        ! device.mount(make_tracked_channel(second_destroyed), u64(1), u64(12)))
        return 50;
    AudioDeviceDesiredState desired;
    desired.generation = u64(1);
    desired.active     = true;
    desired.playing    = true;
    if (! device.apply(rstd::move(desired)) || ! wait_for(device, AudioDeviceState::ReadyPlaying))
        return 51;
    if (! device.unmount(u64(11), u64(1))) return 52;
    for (int i = 0; i < 150 && ! first_destroyed.load(Ordering::Acquire); ++i) pause_test();
    if (! first_destroyed.load(Ordering::Acquire) || second_destroyed.load(Ordering::Acquire) ||
        device.state() != AudioDeviceState::ReadyPlaying)
        return 53;
    device.shutdown();
    device.wait_stopped();
    if (! second_destroyed.load(Ordering::Acquire)) return 54;
    return 0;
}

int run(int argc, char**) {
    if (int result = channel_unmount_test()) return result;
    if (int result = sound_manager_unmount_test()) return result;
    rstd::sync::atomic::Atomic<u64> generation { u64() };
    AudioDevice                     device;
    device.set_event_sink(Some(AudioDeviceEventSink::make([&](AudioDeviceEvent event) {
        generation.store(event.generation, rstd::sync::atomic::Ordering::Release);
    })));
    AudioDeviceDesiredState desired;
    desired.generation = u64(1);
    auto channel       = Box<SilentChannel>::make();
    if (! device.mount(Box<dyn<PullChannelObject>>::from_raw(
                           dyn<PullChannelObject>::from_ptr(rstd::move(channel).into_raw())),
                       u64(1)))
        return 1;
    if (! device.apply(desired.clone())) return 2;
    for (int i = 0; i < 150 && generation.load(rstd::sync::atomic::Ordering::Acquire) != u64(1);
         ++i)
        pause_test();
    if (generation.load(rstd::sync::atomic::Ordering::Acquire) != u64(1)) return 3;
    if (device.state() != AudioDeviceState::Idle) return 4;

    if (argc > 1) {
        if (int result = live_channel_unmount_test()) return result;
        desired.active                   = true;
        desired.playing                  = true;
        desired.playback_buffer_revision = u64(1);
        if (! device.apply(desired.clone()) ||
            ! wait_for(device, AudioDeviceState::ReadyPlaying, u64(1)))
            return 10;
        for (int i = 0; i < 150 && device.stream_position_frames() == u64(); ++i) pause_test();
        if (device.stream_position_frames() == u64()) return 11;
        const auto position = device.stream_position_frames();
        pause_test();
        if (device.stream_position_frames() < position) return 12;
        desired.playing = false;
        if (! device.apply(desired.clone()) ||
            ! wait_for(device, AudioDeviceState::ReadyPaused, u64(1)))
            return 13;
        desired.playback_buffer_revision = u64(2);
        desired.playing                  = true;
        if (! device.apply(desired.clone()) ||
            ! wait_for(device, AudioDeviceState::ReadyPlaying, u64(2)))
            return 14;
        desired.playback_buffer_revision = u64(3);
        if (! device.apply(desired.clone()) ||
            ! wait_for(device, AudioDeviceState::ReadyPlaying, u64(3)))
            return 15;

        AudioCapture capture;
        if (! capture.init()) return 20;
        AudioPcmWindow window {};
        bool           received = false;
        for (int i = 0; i < 150 && ! received; ++i) {
            pause_test();
            received = capture.snapshot(window);
        }
        capture.uninit();
        if (capture.is_inited()) return 21;
        if (! received || window.channels != 2 || window.sample_rate_hz != 48000) return 22;
        desired.active = false;
        if (! device.apply(desired.clone()) || ! wait_for(device, AudioDeviceState::Idle))
            return 16;
    }
#    if ! defined(__APPLE__) && ! LITO_FEAT_PULSE
    desired.active  = true;
    desired.playing = true;
    if (! device.apply(desired.clone())) return 23;
    if (device.state() != AudioDeviceState::Failed) return 24;
    AudioCapture unavailable_capture;
    if (unavailable_capture.init() || unavailable_capture.is_inited()) return 25;
#    endif
    if (! device.unmount_all(u64(2))) return 5;
    device.shutdown();
    device.wait_stopped();
    if (device.state() != AudioDeviceState::Stopped) return 6;
    if (device.completed_playback_buffer_revision() != u64()) return 8;
    if (device.apply(desired.clone())) return 7;
    device.shutdown();
    device.wait_stopped();
    return 0;
}

} // namespace wavsen::tests::audio_backend
#endif
