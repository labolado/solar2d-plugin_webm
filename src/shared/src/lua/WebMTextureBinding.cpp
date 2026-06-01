// WebM Texture Binding for Solar2D
// Integrates the WebM (VP8/VP9 + Opus) decoder with Solar2D's external texture
// system, exposing a plugin.movie-compatible API (newMovieTexture).

// Windows: Corona's luaconf.h does `#define _wsystem lua_wsystem` (to sandbox
// Lua's os.execute). MSVC's <thread> later pulls in <process.h>, whose _wsystem
// declaration would then macro-expand to lua_wsystem with CRT linkage and clash
// with luaconf's declaration (error C2375). Include <process.h> up front so its
// real declaration is seen before that macro exists; the include guard then makes
// <thread>'s later include a no-op. No effect on non-Windows platforms.
#ifdef _WIN32
#include <process.h>
#endif

#include "CoronaLua.h"
#include "CoronaMacros.h"
#include "CoronaGraphics.h"
#include "CoronaLibrary.h"

#include "webm/WebMDecoder.h"

#include <memory>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>

#include "AL/al.h"
#include "AL/alc.h"

using namespace webm;

// Number of audio buffers for streaming
#define NUM_BUFFERS 8

// Forward declarations for texture methods dispatched via GetField.
static int update(lua_State *L);
static int play(lua_State *L);
static int pause(lua_State *L);
static int stop(lua_State *L);
static int replay(lua_State *L);
static int seek(lua_State *L);
static int invalidate(lua_State *L);
static int setVolume(lua_State *L);
static int isActive(lua_State *L, void *context);
static int isPlaying(lua_State *L, void *context);
static int currentTime(lua_State *L, void *context);
static int getVolume(lua_State *L, void *context);

// Cache a C function in the registry so repeated field lookups reuse it.
static int PushCachedFunction(lua_State *L, lua_CFunction func) {
    lua_pushlightuserdata(L, (void*)func);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushlightuserdata(L, (void*)func);
        lua_pushcfunction(L, func);
        lua_settable(L, LUA_REGISTRYINDEX);
        lua_pushcfunction(L, func);
    }
    return 1;
}

// WebMMovieTexture wrapper for Solar2D texture integration
struct WebMMovieTexture {
    std::unique_ptr<webm::WebMDecoder> decoder;
    webm::AudioFrame current_audio_frame;

    // Output (texture) size. Capped to max_w/max_h (the on-screen pixel size) so
    // a 4K source shown small isn't converted/uploaded at full res. 0 = native.
    int max_w = 0, max_h = 0;
    int out_w = 0, out_h = 0;

    // Playback state - matches plugin_movie field names
    bool playing = false;
    bool stopped = false;
    bool audiostarted = false;
    bool audiocompleted = false;

    // Timing - matches plugin_movie field names
    unsigned int elapsed = 0;

    // Audio/video sync
    double playback_start_time = 0.0;     // system time playback began
    double last_audio_timestamp = 0.0;    // timestamp of last audio frame
    // Absolute media time corresponding to playback_start_time. 0 for normal
    // playback (media starts at 0); set to the seek target after a seek so the
    // no-audio wall-clock video target tracks absolute media time, not 0.
    double clock_base_ts = 0.0;

    // Audio (OpenAL integration) - matches plugin_movie field names
    ALuint source = 0;
    ALenum audioformat = 0;
    ALuint buffers[NUM_BUFFERS];
    float volume = 1.0f; // AL_GAIN on our owned source (main thread only)

    // Default empty pixel data
    unsigned char empty[4] = {0, 0, 0, 0}; // Transparent black RGBA

    // --- Background video decode ---
    // A second decoder instance, owned by a worker thread, decodes + converts
    // video frames ahead into a bounded queue so the render thread never blocks
    // on a (possibly slow, 4K) decode. The main decoder above stays on the main
    // thread for audio only — each thread owns its decoder, so no locking on the
    // decoders themselves is needed, only on the queue.
    struct VFrame {
        double ts = 0.0;
        std::vector<uint8_t> rgba; // already converted at out_w x out_h
    };
    std::unique_ptr<webm::WebMDecoder> video_decoder;
    std::thread video_thread;
    std::mutex vq_mutex;
    std::condition_variable vq_cv;
    std::deque<VFrame> video_queue;   // guarded by vq_mutex
    bool worker_run = false;          // guarded by vq_mutex
    bool worker_eof = false;          // guarded by vq_mutex
    bool want_prefetch = false;       // guarded by vq_mutex; decode ahead only once playing
    std::vector<uint8_t> display_rgba; // current frame shown (main thread only)
    double display_ts = -1.0;

    // Seek is handed to the worker so the (possibly lengthy 4K) pre-roll runs off
    // the render thread — otherwise seeking one movie stalls every other movie's
    // update(). pending_seek* are set while the worker is stopped, then consumed
    // by the worker on start. worker_abort lets a newer stop/seek interrupt an
    // in-progress pre-roll so the join doesn't block on it.
    //
    // pending_seek is atomic (not under vq_mutex) because the worker reads it
    // outside the queue critical section; the seq_cst store/load also publishes
    // pending_seek_target, which is written before the flag and read after it.
    std::atomic<bool> pending_seek{false}; // set before startVideoWorker, read in worker
    double pending_seek_target = 0.0;
    std::atomic<bool> worker_abort{false};
};

// Convert a band of rows [y0, y1) of an I420 frame to RGBA. Full-range BT.601
// in 16.16 fixed point (no per-pixel float — much faster, esp. at 4K).
static inline uint8_t clamp255(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}
// Convert output rows [y0, y1) of an (outW x outH) RGBA image from the source
// I420 frame, nearest-neighbour sampled via the precomputed xmap. When
// outW/outH match the source size this is a 1:1 convert; otherwise it downscales
// (so a 4K frame shown small isn't converted/uploaded at full resolution).
static void convertRowsYUVtoRGBA(const webm::VideoFrame& yuv, uint8_t* rgba,
                                 int outW, int outH, const int* xmap, int y0, int y1) {
    const int vh = yuv.height;
    for (int y = y0; y < y1; ++y) {
        const int sy = (int)((int64_t)y * vh / outH);
        const uint8_t* yrow = yuv.y_plane + (size_t)sy * yuv.y_stride;
        const uint8_t* urow = yuv.u_plane + (size_t)(sy / 2) * yuv.uv_stride;
        const uint8_t* vrow = yuv.v_plane + (size_t)(sy / 2) * yuv.uv_stride;
        uint8_t* out = rgba + (size_t)y * outW * 4;
        for (int x = 0; x < outW; ++x) {
            const int sx = xmap[x];
            const int Y = yrow[sx];
            const int U = urow[sx >> 1] - 128;
            const int V = vrow[sx >> 1] - 128;
            out[0] = clamp255(Y + ((91881 * V) >> 16));                  // 1.402
            out[1] = clamp255(Y - ((22554 * U + 46802 * V) >> 16));      // 0.344136 / 0.714136
            out[2] = clamp255(Y + ((116130 * U) >> 16));                 // 1.772
            out[3] = 255;
            out += 4;
        }
    }
}

// YUV (I420) -> RGBA at (outW x outH), parallelised across output row bands.
// outW/outH <= 0 means use the source resolution (no scaling).
void convertYUVtoRGBA(const webm::VideoFrame& yuv, std::vector<uint8_t>& rgba, int outW, int outH) {
    if (!yuv.isValid()) {
        PLUGIN_WEBM_LOG( ("Invalid VideoFrame: y_plane=%p, u_plane=%p, v_plane=%p, size=%dx%d\n",
               yuv.y_plane, yuv.u_plane, yuv.v_plane, yuv.width, yuv.height) );
        return;
    }
    if (outW <= 0) outW = yuv.width;
    if (outH <= 0) outH = yuv.height;

    rgba.resize((size_t)outW * outH * 4);
    uint8_t* out = rgba.data();

    // Nearest-neighbour x sampling map. Depends only on (outW, srcW), which are
    // constant during a movie's playback, so cache it per worker thread (the only
    // caller) and recompute only when the dimensions change.
    static thread_local std::vector<int> xmap;
    static thread_local int xmap_outW = -1, xmap_srcW = -1;
    if (xmap_outW != outW || xmap_srcW != yuv.width) {
        xmap.resize((size_t)outW);
        for (int x = 0; x < outW; ++x) {
            int sx = (int)((int64_t)x * yuv.width / outW);
            if (sx >= yuv.width) sx = yuv.width - 1;
            xmap[x] = sx;
        }
        xmap_outW = outW;
        xmap_srcW = yuv.width;
    }

    unsigned int hw = std::thread::hardware_concurrency();
    int nthreads = (int)std::min(hw ? hw : 1u, 4u);
    // Only parallelise large frames. For the common case (downscaled to a small
    // on-screen size) the frame is small, so spawning threads every frame per
    // movie costs more than it saves and oversubscribes the CPU when several
    // movies play at once — convert inline on the worker instead.
    if ((long)outW * outH < 1280 * 720 || outH < 64) nthreads = 1;

    if (nthreads <= 1) {
        convertRowsYUVtoRGBA(yuv, out, outW, outH, xmap.data(), 0, outH);
        return;
    }

    const int band = (outH + nthreads - 1) / nthreads;
    std::vector<std::thread> workers;
    workers.reserve(nthreads - 1);
    for (int t = 1; t < nthreads; ++t) {
        const int yb0 = t * band;
        if (yb0 >= outH) break;
        const int yb1 = std::min(outH, yb0 + band);
        workers.emplace_back(convertRowsYUVtoRGBA, std::cref(yuv), out, outW, outH, xmap.data(), yb0, yb1);
    }
    convertRowsYUVtoRGBA(yuv, out, outW, outH, xmap.data(), 0, std::min(outH, band));
    for (auto& w : workers) w.join();
}

// ---------------------------------------------------------------------------
// Background video decode worker
// ---------------------------------------------------------------------------
static const size_t kMaxQueuedVideo = 4;

// Decode + convert video frames ahead into movie->video_queue until stopped or
// EOF. Only this thread touches movie->video_decoder.
static void videoDecodeLoop(WebMMovieTexture* movie) {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(movie->vq_mutex);
            movie->vq_cv.wait(lk, [movie] {
                // Until the movie is actually playing, only decode one frame (the
                // poster). This keeps loading/deleting a movie you never play
                // cheap — no full 4K prefetch burst to steal CPU from others.
                size_t cap = movie->want_prefetch ? kMaxQueuedVideo : 1;
                return !movie->worker_run || movie->video_queue.size() < cap;
            });
            if (!movie->worker_run) return;
        }

        // Handle a seek requested by the main thread. The forward pre-roll runs
        // here, on the worker, so the render thread (and every other movie) keeps
        // ticking. pending_seek is atomic; its load also publishes
        // pending_seek_target (written before the flag in seek()).
        if (movie->pending_seek) {
            double tgt = movie->pending_seek_target;
            movie->pending_seek = false;
            movie->video_decoder->seekTo(tgt, &movie->worker_abort);
            std::lock_guard<std::mutex> lk(movie->vq_mutex);
            movie->video_queue.clear(); // drop any frames from before the seek
            movie->worker_eof = false;
            movie->vq_cv.notify_all();
            continue;
        }

        if (!movie->video_decoder->hasNewVideoFrame()) {
            movie->video_decoder->decodeNextVideoFrame();
        }
        if (!movie->video_decoder->hasNewVideoFrame()) {
            std::lock_guard<std::mutex> lk(movie->vq_mutex);
            movie->worker_eof = true;
            movie->vq_cv.notify_all();
            return; // EOF; replay() starts a fresh worker
        }

        webm::VideoFrame f = movie->video_decoder->getCurrentVideoFrame();
        WebMMovieTexture::VFrame vf;
        vf.ts = f.timestamp;
        convertYUVtoRGBA(f, vf.rgba, movie->out_w, movie->out_h);

        std::lock_guard<std::mutex> lk(movie->vq_mutex);
        if (!movie->worker_run) return;
        movie->video_queue.push_back(std::move(vf));
        movie->vq_cv.notify_all();
    }
}

static void startVideoWorker(WebMMovieTexture* movie) {
    if (!movie->video_decoder) return;
    movie->worker_abort = false; // arm a fresh pre-roll
    {
        std::lock_guard<std::mutex> lk(movie->vq_mutex);
        movie->worker_run = true;
        movie->worker_eof = false;
    }
    movie->video_thread = std::thread(videoDecodeLoop, movie);
}

static void stopVideoWorker(WebMMovieTexture* movie) {
    movie->worker_abort = true; // interrupt any in-progress seek pre-roll
    {
        std::lock_guard<std::mutex> lk(movie->vq_mutex);
        movie->worker_run = false;
    }
    movie->vq_cv.notify_all();
    if (movie->video_thread.joinable()) movie->video_thread.join();
    // Worker is joined: clear any unconsumed seek request so it can't leak into a
    // later replay()/play() restart. seek() re-sets it after this call.
    movie->pending_seek = false;
}

// Allocate this movie's OpenAL source (+ buffers) if not already done. Safe to
// call repeatedly — a no-op once the source exists. Returns true when the source
// is ready.
//
// The retry matters at COLD APP STARTUP: Corona's OpenAL context may not be ready
// the instant the first movie is created, so alGenSources there fails. Without a
// retry that movie stays silent for its entire life — seen on device as "rect2
// sometimes has no sound on first launch". update() calls this again once playing,
// by which point the context is definitely up, so the source gets created then.
static bool ensureAudioSource(WebMMovieTexture *movie) {
    if (movie->source) return true;
    alGetError();
    alGenSources(1, &movie->source);
    if (alGetError() != AL_NO_ERROR || !movie->source) {
        movie->source = 0; // context not ready yet; caller retries next frame
        return false;
    }
    alSourcei(movie->source, AL_BUFFER, 0);
    alSourcef(movie->source, AL_GAIN, movie->volume);
    alGenBuffers(NUM_BUFFERS, movie->buffers); // startAudioStream re-gens if needed
    return true;
}

// Audio streaming - matches plugin_movie logic
bool startAudioStream(WebMMovieTexture *movie) {
    if (!movie->current_audio_frame.isValid()) {
        PLUGIN_WEBM_LOG( ("startAudioStream failed: invalid audio frame\n") );
        return false;
    }

    PLUGIN_WEBM_LOG( ("Starting audio stream: %d channels, %d samples, %d Hz\n",
           movie->current_audio_frame.channels,
           (int)movie->current_audio_frame.samples.size(),
           movie->current_audio_frame.sample_rate) );

    // Ensure audio buffers exist (they may have been deleted in stopAudioStream).
    ALboolean buffers_valid = alIsBuffer(movie->buffers[0]);
    if (!buffers_valid) {
        PLUGIN_WEBM_LOG( ("Recreating audio buffers after stop/seek\n") );

        alGetError(); // clear previous error

        alSourceStop(movie->source);
        alSourceRewind(movie->source);
        alSourcei(movie->source, AL_BUFFER, 0);

        for (int i = 0; i < NUM_BUFFERS; i++) {
            movie->buffers[i] = 0;
        }

        alGenBuffers(NUM_BUFFERS, movie->buffers);
        ALenum error = alGetError();
        if (error != AL_NO_ERROR) {
            PLUGIN_WEBM_LOG( ("Failed to generate audio buffers: %d\n", error) );
            return false;
        }

        PLUGIN_WEBM_LOG( ("Successfully created %d audio buffers\n", NUM_BUFFERS) );
    }

    ALsizei i;
    for(i = 0; i < NUM_BUFFERS; i++) {
        ALsizei size = movie->current_audio_frame.samples.size() * sizeof(int16_t);

        PLUGIN_WEBM_LOG( ("Buffer %d: audioformat=%d, size=%d, sample_rate=%d, samples_ptr=%p\n",
               i, movie->audioformat, size, movie->current_audio_frame.sample_rate,
               movie->current_audio_frame.samples.data()) );

        if (size <= 0) {
            PLUGIN_WEBM_LOG( ("Invalid buffer size: %d\n", size) );
            return false;
        }

        if (movie->current_audio_frame.sample_rate <= 0) {
            PLUGIN_WEBM_LOG( ("Invalid sample rate: %d\n", movie->current_audio_frame.sample_rate) );
            return false;
        }

        if (movie->current_audio_frame.samples.empty()) {
            PLUGIN_WEBM_LOG( ("Empty audio samples\n") );
            return false;
        }

        alBufferData(movie->buffers[i], movie->audioformat,
                    movie->current_audio_frame.samples.data(), size,
                    movie->current_audio_frame.sample_rate);

        ALenum error = alGetError();
        if (error != AL_NO_ERROR) {
            PLUGIN_WEBM_LOG( ("OpenAL buffer error %d at buffer %d (format=%d, size=%d, rate=%d)\n",
                   error, i, movie->audioformat, size, movie->current_audio_frame.sample_rate) );
            return false;
        }

        // Pull the next audio frame (separate audio decode path).
        if (!movie->decoder->hasNewAudioFrame()) {
            movie->decoder->decodeNextAudioFrame();
        }

        if(movie->decoder->hasNewAudioFrame()) {
            movie->current_audio_frame = movie->decoder->getCurrentAudioFrame();
        } else {
            PLUGIN_WEBM_LOG( ("Only %d audio buffers filled (expected %d)\n", i+1, NUM_BUFFERS) );
            break;
        }
    }

    alSourceQueueBuffers(movie->source, i, movie->buffers);
    ALenum error = alGetError();
    if (error != AL_NO_ERROR) {
        PLUGIN_WEBM_LOG( ("OpenAL queue buffers error: %d\n", error) );
        return false;
    }

    alSourcePlay(movie->source);
    error = alGetError();
    if (error != AL_NO_ERROR) {
        PLUGIN_WEBM_LOG( ("OpenAL source play error: %d\n", error) );
        return false;
    }

    PLUGIN_WEBM_LOG( ("Audio stream started successfully with %d buffers\n", i) );
    return true;
}

void stopAudioStream(WebMMovieTexture *movie) {
    alSourceStop(movie->source);
    alSourceRewind(movie->source);
    alSourcei(movie->source, AL_BUFFER, 0);
    alDeleteBuffers(NUM_BUFFERS, movie->buffers);
    for (int i = 0; i < NUM_BUFFERS; ++i) movie->buffers[i] = 0;
}

// Texture callback implementations
static unsigned int GetWidth(void *context) {
    WebMMovieTexture *movie = (WebMMovieTexture*)context;
    return movie->out_w > 0 ? movie->out_w : 1;
}

static unsigned int GetHeight(void *context) {
    WebMMovieTexture *movie = (WebMMovieTexture*)context;
    return movie->out_h > 0 ? movie->out_h : 1;
}

static const void* GetImage(void *context) {
    WebMMovieTexture *movie = (WebMMovieTexture*)context;

    // Before playback starts, update() isn't called to advance frames, so adopt
    // the first decoded frame here as a poster so the texture isn't blank after
    // load. (Same thread as update(), so the queue lock is only vs. the worker.)
    if (movie->display_ts < 0.0) {
        std::lock_guard<std::mutex> lk(movie->vq_mutex);
        if (!movie->video_queue.empty()) {
            movie->display_rgba = std::move(movie->video_queue.front().rgba);
            movie->display_ts = movie->video_queue.front().ts;
            movie->video_queue.pop_front();
            movie->vq_cv.notify_all();
        }
    }

    // display_rgba is sized out_w*out_h*4 from creation onward, so this is always
    // the right number of bytes for Corona to read.
    if (!movie->display_rgba.empty()) {
        return movie->display_rgba.data();
    }
    return movie->empty;
}

// onGetField callback - dynamically provides methods, matching plugin_movie.
static int GetField(lua_State *L, const char *field, void *context) {
    int result = 0;

    if(strcmp(field, "update") == 0) {
        result = PushCachedFunction(L, update);
    }
    else if(strcmp(field, "play") == 0) {
        result = PushCachedFunction(L, play);
    }
    else if(strcmp(field, "pause") == 0) {
        result = PushCachedFunction(L, pause);
    }
    else if(strcmp(field, "stop") == 0) {
        result = PushCachedFunction(L, stop);
    }
    else if(strcmp(field, "replay") == 0) {
        result = PushCachedFunction(L, replay);
    }
    else if(strcmp(field, "seek") == 0) {
        result = PushCachedFunction(L, seek);
    }
    else if(strcmp(field, "invalidate") == 0) {
        result = PushCachedFunction(L, invalidate);
    }
    else if(strcmp(field, "setVolume") == 0) {
        result = PushCachedFunction(L, setVolume);
    }
    else if(strcmp(field, "isActive") == 0)
        result = isActive(L, context);
    else if(strcmp(field, "isPlaying") == 0)
        result = isPlaying(L, context);
    else if(strcmp(field, "currentTime") == 0)
        result = currentTime(L, context);
    else if(strcmp(field, "volume") == 0)
        result = getVolume(L, context);

    return result;
}

static CoronaExternalBitmapFormat GetFormat(void *context) {
    return kExternalBitmapFormat_RGBA;
}

// Core texture creation function
int newMovieTexture(lua_State *L) {
    WebMMovieTexture *movie = new WebMMovieTexture;

    const char *path = lua_tostring(L, 1);

    if(!path) {
        delete movie;
        lua_pushnil(L);
        return 1;
    }

    // Optional max output (texture) size in pixels — the Lua front-end passes the
    // on-screen size so we don't convert/upload a full 4K frame for a small rect.
    movie->max_w = (int)lua_tointeger(L, 4);
    movie->max_h = (int)lua_tointeger(L, 5);

    movie->decoder = std::make_unique<webm::WebMDecoder>();

    // The main decoder is used for audio + header info only (video is decoded by
    // the worker's own decoder), so skip its vpx context / decode threads.
    if(!movie->decoder->loadFromFile(path, /*initVideoDecoder=*/false)) {
        delete movie;
        lua_pushnil(L);
        lua_pushfstring(L, "plugin.webm: failed to open '%s'", path ? path : "(nil)");
        return 2;
    }

    // Decide the output (texture) size from the *track header* (no frame decode
    // needed, so creation never blocks on a 4K decode). Cap to max_w/max_h
    // preserving aspect, never upscaling; native when no cap is given.
    {
        const int vw = movie->decoder->getVideoWidth();
        const int vh = movie->decoder->getVideoHeight();
        movie->out_w = vw;
        movie->out_h = vh;
        if (movie->max_w > 0 && movie->max_h > 0 && vw > 0 && vh > 0) {
            double s = std::min(1.0, std::min((double)movie->max_w / vw,
                                              (double)movie->max_h / vh));
            movie->out_w = std::max(1, (int)(vw * s + 0.5));
            movie->out_h = std::max(1, (int)(vh * s + 0.5));
        }
    }

    // Start the display buffer as a correctly-sized transparent frame. GetImage
    // must always return out_w*out_h*4 bytes (Corona reads that many), so it can
    // never fall back to the 4-byte `empty` once the texture is sized — that
    // would read out of bounds and show garbage.
    movie->display_rgba.assign((size_t)movie->out_w * movie->out_h * 4, 0);

    // Hand all video decoding (including the first frame) to a worker thread with
    // its own decoder instance, so creating a movie never blocks the render
    // thread. The texture shows transparent until the worker produces frame 0
    // (a few ms later); the main decoder above is used only for audio.
    movie->video_decoder = std::make_unique<webm::WebMDecoder>();
    if (movie->video_decoder->loadFromFile(path)) {
        startVideoWorker(movie);
    } else {
        movie->video_decoder.reset();
    }

    // Create our OWN OpenAL source instead of borrowing a Corona audio-channel
    // source. Sharing a channel source meant two movies (or repeated load/delete
    // cycles) could land on the same source, and one movie's setup/teardown
    // (alSourceRewind / alSourceStop) would then disrupt another that was
    // playing. An owned source can't collide with anything.
    // (arg 2 from Lua is ignored now, but its getSourceFromChannel call still
    // ensures Corona's OpenAL context is initialised before we get here.)
    // Best-effort here; if the context isn't ready yet (cold startup), update()
    // retries once playing so the movie isn't left permanently silent.
    ensureAudioSource(movie);

    // Setup Solar2D texture callbacks - onGetField + getFormat are essential.
    CoronaExternalTextureCallbacks callbacks = {};
    callbacks.size = sizeof(CoronaExternalTextureCallbacks);
    callbacks.getWidth = GetWidth;
    callbacks.getHeight = GetHeight;
    callbacks.onRequestBitmap = GetImage;
    callbacks.getFormat = GetFormat;
    callbacks.onGetField = GetField;
    callbacks.onFinalize = [](void* context) {
        WebMMovieTexture *movie = (WebMMovieTexture*)context;

        // Join the worker first — it references `movie`, so it must be stopped
        // before anything is torn down or freed.
        stopVideoWorker(movie);

        // Full resource cleanup - mirrors plugin_movie's Dispose logic.
        if(!movie->stopped) {
            movie->stopped = true;
            movie->playing = false;

            if(movie->audiostarted) {
                stopAudioStream(movie);
            }

            if(movie->decoder) {
                movie->decoder->stop();
            }
        }

        // Delete our owned OpenAL source (and any still-live buffers).
        if (movie->source) {
            alSourceStop(movie->source);
            alSourcei(movie->source, AL_BUFFER, 0);
            if (!movie->audiostarted) alDeleteBuffers(NUM_BUFFERS, movie->buffers);
            alDeleteSources(1, &movie->source);
            movie->source = 0;
        }

        delete movie;
    };

    return CoronaExternalPushTexture(L, &callbacks, movie);
}

// Texture methods - matches plugin_movie logic.
static int update(lua_State *L) {
    WebMMovieTexture *movie = (WebMMovieTexture*)CoronaExternalGetUserData(L, 1);
    if (!movie) return 0;

    PLUGIN_WEBM_LOG( ("Update called - playing: %s, decoder: %s, stopped: %s\n",
           movie->playing ? "true" : "false",
           movie->decoder ? "exists" : "null",
           movie->stopped ? "true" : "false") );

    if(movie->playing && movie->decoder) {
        unsigned int delta = luaL_checkinteger(L, 2);

        PLUGIN_WEBM_LOG( ("Update main loop - delta: %u\n", delta) );

        // Independent audio decode (only if the file has audio)
        if (!movie->current_audio_frame.isValid() && movie->decoder->hasAudioTrack()) {
            if (!movie->decoder->hasNewAudioFrame()) {
                movie->decoder->decodeNextAudioFrame();
            }
            if (movie->decoder->hasNewAudioFrame()) {
                movie->current_audio_frame = movie->decoder->getCurrentAudioFrame();
            }
        }

        if(delta > 0) {
            unsigned int currentTime = movie->elapsed + delta;

            // Audio handling - only when the file actually has audio AND our
            // OpenAL source is ready. ensureAudioSource() retries the allocation
            // that may have failed at cold-startup creation; until it succeeds we
            // skip audio this frame (and retry next) rather than run the whole path
            // against source 0, which would never produce sound.
            if(movie->current_audio_frame.isValid() && movie->decoder->hasAudioTrack()
               && ensureAudioSource(movie)) {
                if(!movie->audioformat) {
                    movie->audioformat = (movie->current_audio_frame.channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
                }

                if(!movie->audiostarted) {
                    movie->audiostarted = startAudioStream(movie);
                    if (movie->audiostarted && movie->playback_start_time == 0.0) {
                        movie->playback_start_time = currentTime / 1000.0;
                        PLUGIN_WEBM_LOG( ("Audio playback started at time %.3fs\n", movie->playback_start_time) );
                    } else if (!movie->audiostarted) {
                        PLUGIN_WEBM_LOG( ("Audio failed to start, continuing with video-only playback\n") );
                        movie->audiocompleted = true;
                        if (movie->playback_start_time == 0.0) {
                            movie->playback_start_time = currentTime / 1000.0;
                            PLUGIN_WEBM_LOG( ("Video-only playback started at time %.3fs\n", movie->playback_start_time) );
                        }
                    }
                }

                ALint state, processed;
                alGetSourcei(movie->source, AL_SOURCE_STATE, &state);
                alGetSourcei(movie->source, AL_BUFFERS_PROCESSED, &processed);

                double expected_time = (currentTime / 1000.0) - movie->playback_start_time;
                double audio_timestamp = movie->current_audio_frame.timestamp;

                movie->last_audio_timestamp = audio_timestamp;

                double audio_offset = audio_timestamp - expected_time;

                while(processed > 0) {
                    ALuint buffID;
                    alSourceUnqueueBuffers(movie->source, 1, &buffID);
                    processed--;

                    ALsizei size = movie->current_audio_frame.samples.size() * sizeof(int16_t);
                    alBufferData(buffID, movie->audioformat,
                                movie->current_audio_frame.samples.data(), size,
                                movie->current_audio_frame.sample_rate);

                    alSourceQueueBuffers(movie->source, 1, &buffID);

                    if (!movie->decoder->hasNewAudioFrame()) {
                        movie->decoder->decodeNextAudioFrame();
                    }

                    if(movie->decoder->hasNewAudioFrame()) {
                        movie->current_audio_frame = movie->decoder->getCurrentAudioFrame();
                        movie->last_audio_timestamp = movie->current_audio_frame.timestamp;
                    } else {
                        movie->current_audio_frame = AudioFrame();
                        break;
                    }
                }

                if (fabs(audio_offset) > 0.1) {
                    PLUGIN_WEBM_LOG( ("Audio sync: expected=%.3fs, audio=%.3fs, offset=%.3fs\n",
                           expected_time, audio_timestamp, audio_offset) );
                }

                if(state == AL_STOPPED) {
                    // The source drained. Distinguish a transient underrun from a
                    // genuine end-of-stream: if we still have (or can decode) more
                    // audio, it's an underrun — most likely the render thread
                    // stalled while another movie was loading (a cold 4K file open
                    // can do a slow end-of-file read), so update() didn't refill in
                    // time. Re-prime and resume instead of permanently silencing
                    // this movie (the old code set audiocompleted here, which is
                    // why loading one movie could kill another's sound for good).
                    if (!movie->current_audio_frame.isValid()) {
                        if (!movie->decoder->hasNewAudioFrame()) {
                            movie->decoder->decodeNextAudioFrame();
                        }
                        if (movie->decoder->hasNewAudioFrame()) {
                            movie->current_audio_frame = movie->decoder->getCurrentAudioFrame();
                        }
                    }
                    if (movie->current_audio_frame.isValid()) {
                        ALint queued = 0;
                        alGetSourcei(movie->source, AL_BUFFERS_QUEUED, &queued);
                        if (queued > 0) {
                            alSourcePlay(movie->source); // buffers refilled above; resume
                        } else {
                            // Fully drained: re-seed from scratch next frame.
                            movie->audiostarted = false;
                        }
                    } else {
                        movie->audiocompleted = true; // genuine end of audio
                    }
                }
            }

            // Video: display the newest already-decoded frame whose presentation
            // time has arrived. The worker thread decoded + converted these ahead
            // into video_queue, so the render thread never blocks. Slave to the
            // audio clock while audio is producing frames; otherwise the wall
            // clock so video still advances to its own EOF (shorter audio track).
            {
                double currentTimeSeconds = currentTime / 1000.0;
                if (movie->playback_start_time == 0.0) {
                    movie->playback_start_time = currentTimeSeconds;
                }
                bool audio_active = movie->decoder->hasAudioTrack()
                                    && movie->current_audio_frame.isValid()
                                    && movie->last_audio_timestamp > 0.0;
                double target = audio_active
                    ? movie->last_audio_timestamp
                    : (movie->clock_base_ts + (currentTimeSeconds - movie->playback_start_time));

                std::lock_guard<std::mutex> lk(movie->vq_mutex);
                while (!movie->video_queue.empty() && movie->video_queue.front().ts <= target) {
                    movie->display_rgba = std::move(movie->video_queue.front().rgba);
                    movie->display_ts = movie->video_queue.front().ts;
                    movie->video_queue.pop_front();
                }
                movie->vq_cv.notify_all(); // freed queue space
            }

            movie->elapsed = currentTime;
        }
    }
    // Drain remaining audio after decode finishes.
    else if(movie->audiostarted && !movie->audiocompleted) {
        if(!movie->decoder || !movie->decoder->hasNewAudioFrame()) {
            movie->audiocompleted = true;
        }
    }

    return 0;
}

static int play(lua_State *L) {
    WebMMovieTexture *movie = (WebMMovieTexture*)CoronaExternalGetUserData(L, 1);
    if (!movie) return 0;

    if(!movie->playing) {
        movie->playing = true;
        if(movie->audiostarted && !movie->audiocompleted) {
            alSourcePlay(movie->source);
        }
        // Let the worker decode ahead now that we're playing.
        {
            std::lock_guard<std::mutex> lk(movie->vq_mutex);
            movie->want_prefetch = true;
        }
        movie->vq_cv.notify_all();
    }

    return 0;
}

static int pause(lua_State *L) {
    WebMMovieTexture *movie = (WebMMovieTexture*)CoronaExternalGetUserData(L, 1);
    if (!movie) return 0;

    if(movie->playing) {
        movie->playing = false;
        if(movie->audiostarted && !movie->audiocompleted) {
            alSourcePause(movie->source);
        }
    }

    return 0;
}

static int stop(lua_State *L) {
    WebMMovieTexture *movie = (WebMMovieTexture*)CoronaExternalGetUserData(L, 1);
    if (!movie) return 0;

    movie->stopped = true;
    movie->playing = false;

    stopVideoWorker(movie);

    bool hadAudio = movie->audiostarted;
    if(movie->audiostarted) {
        stopAudioStream(movie);
        movie->audiostarted = false;
        movie->audiocompleted = true;
    }

    if(movie->decoder) {
        movie->decoder->stop();
    }

    if (movie->source) {
        alSourceStop(movie->source);
        alSourcei(movie->source, AL_BUFFER, 0);
        if (!hadAudio) alDeleteBuffers(NUM_BUFFERS, movie->buffers);
        alDeleteSources(1, &movie->source);
        movie->source = 0;
    }

    return 0;
}

static int replay(lua_State *L) {
    WebMMovieTexture *movie = (WebMMovieTexture*)CoronaExternalGetUserData(L, 1);
    if (!movie) return 0;

    PLUGIN_WEBM_LOG( ("Lua replay called\n") );

    if (!movie->decoder) {
        PLUGIN_WEBM_LOG( ("ERROR: No decoder available for replay\n") );
        lua_pushboolean(L, false);
        return 1;
    }

    // Stop the worker before touching the video decoder / queue.
    stopVideoWorker(movie);

    if(movie->audiostarted) {
        stopAudioStream(movie);
        movie->audiostarted = false;
        movie->audiocompleted = false;
    }

    movie->stopped = false;
    movie->playing = false;
    movie->elapsed = 0;
    movie->playback_start_time = 0.0;
    movie->last_audio_timestamp = 0.0;
    movie->clock_base_ts = 0.0; // media restarts at 0

    movie->current_audio_frame = webm::AudioFrame();
    movie->display_ts = -1.0; // re-arm the poster pull for the rewound first frame

    {
        std::lock_guard<std::mutex> lk(movie->vq_mutex);
        movie->video_queue.clear();
        movie->worker_eof = false;
        movie->want_prefetch = true; // replay implies we're (re)playing
    }

    bool success = movie->decoder->replay();
    if (movie->video_decoder) movie->video_decoder->replay();

    if (success) {
        movie->playing = true;
        // Restart the worker; it (re)decodes video from the start off-thread, and
        // update()'s audio path re-primes audio on demand. No main-thread decode,
        // so looping a 4K clip doesn't hitch.
        startVideoWorker(movie);
    }

    lua_pushboolean(L, success);
    return 1;
}

// texture:seek(t) — jump to time t (seconds). Preserves play/pause state: the
// target frame is shown either way (paused via the poster pull). Modelled on
// replay()'s worker/audio lifecycle, but repositions to t instead of the start.
static int seek(lua_State *L) {
    WebMMovieTexture *movie = (WebMMovieTexture*)CoronaExternalGetUserData(L, 1);
    if (!movie) return 0;

    if (!movie->decoder) {
        lua_pushboolean(L, false);
        return 1;
    }

    double t = luaL_checknumber(L, 2);
    double dur = movie->decoder->getDuration();
    if (t < 0.0) t = 0.0;
    if (dur > 0.0 && t > dur) t = dur;

    const bool wasPlaying = movie->playing;

    // Stop the worker before touching the video decoder / queue. (worker_abort is
    // raised here, so if a previous seek's pre-roll is still running it bails out
    // fast instead of making this join block the render thread.)
    stopVideoWorker(movie);

    // Tear down the current audio stream; update() re-primes it from the new
    // position on the next frame.
    if (movie->audiostarted) {
        stopAudioStream(movie);
        movie->audiostarted = false;
        movie->audiocompleted = false;
    }

    // Reposition the AUDIO decoder here (cheap: cursor reposition + opus reset, no
    // pre-roll since its vpx context isn't inited). The VIDEO decoder's seek —
    // whose keyframe→target pre-roll can be lengthy at 4K — is deferred to the
    // worker thread (below) so it never stalls the render thread / other movies.
    bool ok = movie->decoder->seekTo(t);

    // Rebase the playback clock to the seek target (absolute media time).
    movie->stopped = false;
    movie->elapsed = (unsigned int)(t * 1000.0);
    movie->playback_start_time = 0.0; // update() re-inits to ~t on the next frame
    movie->clock_base_ts = t;         // wall-clock video target base (no-audio path)
    movie->last_audio_timestamp = 0.0;
    movie->current_audio_frame = webm::AudioFrame();
    movie->display_ts = -1.0;         // re-arm the poster pull for the seeked frame

    {
        std::lock_guard<std::mutex> lk(movie->vq_mutex);
        movie->video_queue.clear();
        movie->worker_eof = false;
        if (wasPlaying) movie->want_prefetch = true;
    }

    movie->playing = wasPlaying;

    // Hand the video seek to the worker: it runs video_decoder->seekTo(t) (the
    // pre-roll) off-thread, then queues the target frame. Until then the poster
    // pull keeps showing the previous frame — no freeze of this or any other movie.
    if (movie->video_decoder) {
        movie->pending_seek_target = t;
        movie->pending_seek = true;
        startVideoWorker(movie);
    }

    lua_pushboolean(L, ok);
    return 1;
}

static int isActive(lua_State *L, void *context) {
    WebMMovieTexture *movie = (WebMMovieTexture*)context;

    if (movie->stopped || !movie->decoder) {
        lua_pushboolean(L, false);
        return 1;
    }

    // Video is finished when the worker hit EOF and every decoded frame has been
    // shown (queue drained). This is the primary completion signal now that the
    // background worker owns video decoding.
    {
        std::lock_guard<std::mutex> lk(movie->vq_mutex);
        if (movie->worker_eof && movie->video_queue.empty()) {
            lua_pushboolean(L, false);
            return 1;
        }
    }

    // Container-duration fallback (e.g. audio-only, or odd EOF timing): the
    // playback clock vs. the container duration is a device-independent signal.
    double duration = movie->decoder->getDuration();
    if (duration > 0.0 && (movie->elapsed / 1000.0) >= duration) {
        lua_pushboolean(L, false);
        return 1;
    }

    lua_pushboolean(L, true);
    return 1;
}

static int isPlaying(lua_State *L, void *context) {
    WebMMovieTexture *movie = (WebMMovieTexture*)context;

    lua_pushboolean(L, movie->playing);
    return 1;
}

static int invalidate(lua_State *L) {
    // Solar2D detects texture data changes automatically; kept for plugin_movie
    // API compatibility.
    return 0;
}

static int currentTime(lua_State *L, void *context) {
    WebMMovieTexture *movie = (WebMMovieTexture*)context;

    lua_pushnumber(L, movie->elapsed * 0.001);
    return 1;
}

// texture:setVolume(v) — 0..1 linear gain on this movie's audio. Needed because
// the movie owns its OpenAL source (not a Corona channel), so Corona's audio.*
// volume APIs don't reach it.
static int setVolume(lua_State *L) {
    WebMMovieTexture *movie = (WebMMovieTexture*)CoronaExternalGetUserData(L, 1);
    if (!movie) return 0;
    double v = luaL_checknumber(L, 2);
    if (v < 0.0) v = 0.0;
    movie->volume = (float)v;
    if (movie->source) {
        alSourcef(movie->source, AL_GAIN, movie->volume);
    }
    return 0;
}

// read-only texture.volume
static int getVolume(lua_State *L, void *context) {
    WebMMovieTexture *movie = (WebMMovieTexture*)context;
    lua_pushnumber(L, movie->volume);
    return 1;
}

// Defined in the generated bytecode wrapper (generated/plugin_webm.c).
CORONA_EXPORT int CoronaPluginLuaLoad_plugin_webm(lua_State *L);

// Main plugin entry point
CORONA_EXPORT int luaopen_plugin_webm(lua_State *L) {
    lua_CFunction factory = Corona::Lua::Open<CoronaPluginLuaLoad_plugin_webm>;
    int result = CoronaLibraryNewWithFactory(L, factory, NULL, NULL);

    if(result) {
        const luaL_Reg kFunctions[] = {
            {"_newMovieTexture", newMovieTexture},
            {NULL, NULL}
        };

        luaL_register(L, NULL, kFunctions);
    }

    return result;
}

