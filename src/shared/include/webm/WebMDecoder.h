// WebMDecoder.h
//
// Decoder interface for the Solar2D plugin.webm.
// Demuxes a .webm (Matroska) container via libwebm, decodes VP8/VP9 video via
// libvpx and Opus/Vorbis audio, exposing decoded frames to WebMTextureBinding.
//
// NOTE: This header only declares the interface. The implementation lives in
// src/shared/src/decoders/WebMDecoder.cpp (see TASK.md).

#ifndef PLUGIN_WEBM_WEBMDECODER_H
#define PLUGIN_WEBM_WEBMDECODER_H

#include <atomic>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
// Toggle verbose decoder/binding logging. Wrap every diagnostic in this macro
// so release builds stay quiet.
#ifndef PLUGIN_WEBM_LOG_ENABLED
#define PLUGIN_WEBM_LOG_ENABLED 0
#endif

#if PLUGIN_WEBM_LOG_ENABLED
#define PLUGIN_WEBM_LOG(args) do { printf args; } while (0)
#else
#define PLUGIN_WEBM_LOG(args) do {} while (0)
#endif

namespace webm {

// Decoded video frame in planar YUV (I420). Planes point into decoder-owned
// memory and stay valid until the next decodeNextVideoFrame() call.
struct VideoFrame {
    const uint8_t* y_plane = nullptr;
    const uint8_t* u_plane = nullptr;
    const uint8_t* v_plane = nullptr;
    int width = 0;
    int height = 0;
    int y_stride = 0;
    int uv_stride = 0;
    double timestamp = 0.0; // seconds

    bool isValid() const {
        return y_plane && u_plane && v_plane && width > 0 && height > 0;
    }
};

// Decoded audio frame as interleaved signed 16-bit PCM.
struct AudioFrame {
    std::vector<int16_t> samples;
    int channels = 0;
    int sample_rate = 0;
    double timestamp = 0.0; // seconds

    bool isValid() const {
        return !samples.empty() && channels > 0 && sample_rate > 0;
    }
};

// Pull-based decoder. The binding repeatedly asks for the next video/audio
// frame and reads it back through getCurrent*Frame().
class WebMDecoder {
public:
    WebMDecoder();
    ~WebMDecoder();

    // Open and parse the container; initialise the codecs. Returns false on
    // any failure (missing file, unsupported codec, parse error).
    // initVideoDecoder=false skips creating the (multithreaded) vpx context —
    // used for the audio-only decoder instance so it doesn't spawn idle decode
    // threads it will never use.
    bool loadFromFile(const char* path, bool initVideoDecoder = true);

    // Decode the next available packet (either track). Returns false at EOF.
    bool decodeNextFrame();
    // Decode strictly the next video / audio packet.
    void decodeNextVideoFrame();
    void decodeNextAudioFrame();

    // Whether a freshly decoded frame is queued and not yet consumed.
    bool hasNewVideoFrame();
    bool hasNewAudioFrame();

    // Consume the queued frame. Marks it as consumed.
    VideoFrame getCurrentVideoFrame();
    AudioFrame getCurrentAudioFrame();

    bool hasAudioTrack();
    double getDuration(); // seconds; 0 if unknown
    int getVideoWidth();  // from the track header, available right after load
    int getVideoHeight();

    void stop();
    bool replay();          // rewind to start
    // Seek to time t (seconds): repositions to the nearest video keyframe at or
    // before t, then decodes forward (discarding) so the next produced video
    // frame is the one at/after t. Audio is repositioned to the nearest cluster.
    // Returns false on failure. Call only while the owning worker is stopped (or
    // from the worker thread itself).
    //
    // The forward decode (pre-roll) can be lengthy on HD/4K, so it should run on
    // the decode worker thread, not the render thread. Pass `abort` to let a newer
    // seek interrupt an in-progress pre-roll; it's polled between frames.
    bool seekTo(double t, std::atomic<bool>* abort = nullptr);
    bool isPlaybackFinished();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace webm

#endif // PLUGIN_WEBM_WEBMDECODER_H
