// WebMDecoder.cpp
//
// Implements webm::WebMDecoder: demux a .webm (Matroska) container with libwebm
// mkvparser, decode VP8/VP9 video with libvpx and Opus audio with libopus.

#include "webm/WebMDecoder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <thread>

#include "mkvparser/mkvparser.h"
#include "mkvparser/mkvreader.h"

#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"

#include "opus.h"

namespace webm {

// Opus always decodes to 48 kHz. Max samples per channel for a 120ms frame.
static const int kOpusRate = 48000;
static const int kOpusMaxSamples = 5760;

// A read cursor walking blocks of a single track over the shared Segment.
struct Cursor {
    const mkvparser::Cluster* cluster = nullptr;
    const mkvparser::BlockEntry* be = nullptr;
    int frame = 0;
    bool done = false;
    bool started = false; // false only before the first cluster is fetched
};

struct WebMDecoder::Impl {
    mkvparser::MkvReader reader;
    mkvparser::Segment* segment = nullptr;
    long long duration_ns = 0;

    long video_track = -1;
    long audio_track = -1;
    std::string video_codec;
    std::string audio_codec;
    int audio_channels = 0;
    int video_w = 0;   // from the track header (no decode needed)
    int video_h = 0;

    vpx_codec_ctx_t vpx_ctx;
    bool vpx_inited = false;

    OpusDecoder* opus = nullptr;

    Cursor vcur;
    Cursor acur;

    bool has_video = false;
    bool has_audio = false;
    VideoFrame video_frame;
    AudioFrame audio_frame;

    // Backing storage so VideoFrame plane pointers stay valid until the next
    // video decode.
    std::vector<uint8_t> y_buf, u_buf, v_buf;
    std::vector<uint8_t> pkt;

    // Queue of frames emitted by a single vpx_codec_decode call (frame-parallel
    // VP9 can produce multiple frames per packet). Served FIFO by decodeNextVideoFrame.
    struct PendingFrame {
        std::vector<uint8_t> y, u, v;
        int w = 0, h = 0, ys = 0, us = 0;
        double ts = 0.0;
    };
    std::deque<PendingFrame> frame_queue;
    std::vector<int16_t> opus_tmp; // reused opus decode scratch (avoid per-frame alloc)

    ~Impl() {
        if (vpx_inited) vpx_codec_destroy(&vpx_ctx);
        if (opus) opus_decoder_destroy(opus);
        if (segment) delete segment;
    }

    // (Re)create the vpx decoder context with multithreaded decoding. Shared by
    // loadFromFile and replay so a looped clip keeps its decode threads (replay
    // used to re-init single-threaded, silently halving 4K decode speed).
    bool initVpx() {
        vpx_codec_iface_t* iface =
            (video_codec == "V_VP9") ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx();
        vpx_codec_dec_cfg_t cfg;
        std::memset(&cfg, 0, sizeof(cfg));
        unsigned int hw = std::thread::hardware_concurrency();
        cfg.threads = std::min(hw ? hw : 4u, 4u);
        unsigned int dec_flags = 0;
#ifdef VPX_CODEC_USE_FRAME_THREADING
        if (video_codec == "V_VP9") dec_flags |= VPX_CODEC_USE_FRAME_THREADING;
#endif
        return vpx_codec_dec_init(&vpx_ctx, iface, &cfg, dec_flags) == VPX_CODEC_OK;
    }

    // Advance the cursor's block entry. When the current cluster's entries are
    // exhausted, step to the next cluster (leaving be == nullptr so the reader
    // re-seeds from that cluster's first entry). Crucially this never leaves
    // be == nullptr pointing at an *already-consumed* cluster, which would make
    // the reader restart that cluster from the top and loop forever.
    void stepEntry(Cursor& c) {
        const mkvparser::BlockEntry* nb = nullptr;
        c.cluster->GetNext(c.be, nb);
        c.frame = 0;
        if (!nb || nb->EOS()) {
            c.cluster = segment->GetNext(c.cluster); // EOS sentinel or null at true EOF
            c.be = nullptr;
        } else {
            c.be = nb;
        }
    }

    // Read the next frame belonging to `track` into `buf`. Returns false at EOF.
    bool readNext(Cursor& c, long track, std::vector<uint8_t>& buf, double& tsec) {
        if (c.done) return false;

        for (;;) {
            if (!c.cluster && !c.started) {
                c.cluster = segment->GetFirst();
                c.started = true;
            }
            if (!c.cluster || c.cluster->EOS()) {
                c.done = true;
                return false;
            }
            if (!c.be) {
                if (c.cluster->GetFirst(c.be) < 0 || !c.be || c.be->EOS()) {
                    // Empty/invalid cluster -> advance to the next one and retry.
                    c.cluster = segment->GetNext(c.cluster);
                    c.frame = 0;
                    continue;
                }
                c.frame = 0;
            }

            while (c.be && !c.be->EOS()) {
                const mkvparser::Block* b = c.be->GetBlock();
                if (b && b->GetTrackNumber() == track) {
                    const int fc = b->GetFrameCount();
                    if (c.frame < fc) {
                        const mkvparser::Block::Frame& f = b->GetFrame(c.frame);
                        c.frame++;
                        bool last = (c.frame >= fc);

                        if (f.len <= 0 || f.len > (16 << 20)) {
                            // Skip malformed or oversized frames (> 16 MiB).
                            // A legitimate 4K VP9 keyframe is well under 2 MiB;
                            // skipping silently is safer than a potential OOM crash.
                            if (last) stepEntry(c);
                            continue;
                        }
                        buf.resize((size_t)f.len);
                        bool ok = (f.Read(&reader, buf.data()) >= 0);
                        tsec = b->GetTime(c.cluster) / 1000000000.0;

                        if (last) stepEntry(c);

                        if (ok) return true;
                        if (last) break; // entry already advanced; re-enter outer loop
                        continue;        // skip empty/failed frame within same block
                    }
                }
                stepEntry(c);
                if (!c.be) break; // crossed cluster boundary; re-enter outer loop
            }
        }
    }
};

WebMDecoder::WebMDecoder() : impl_(new Impl()) {}
WebMDecoder::~WebMDecoder() { delete impl_; }

bool WebMDecoder::loadFromFile(const char* path, bool initVideoDecoder) {
    Impl* d = impl_;

    if (d->reader.Open(path) < 0) {
        PLUGIN_WEBM_LOG( ("WebMDecoder: failed to open %s\n", path ? path : "(null)") );
        return false;
    }

    long long pos = 0;
    mkvparser::EBMLHeader ebml;
    if (ebml.Parse(&d->reader, pos) < 0) {
        PLUGIN_WEBM_LOG( ("WebMDecoder: EBML parse failed\n") );
        return false;
    }

    mkvparser::Segment* segment = nullptr;
    if (mkvparser::Segment::CreateInstance(&d->reader, pos, segment) < 0 || !segment) {
        PLUGIN_WEBM_LOG( ("WebMDecoder: Segment::CreateInstance failed\n") );
        return false;
    }
    d->segment = segment;
    if (segment->Load() < 0) {
        PLUGIN_WEBM_LOG( ("WebMDecoder: Segment::Load failed\n") );
        return false;
    }

    const mkvparser::SegmentInfo* info = segment->GetInfo();
    if (info) d->duration_ns = info->GetDuration();

    const mkvparser::Tracks* tracks = segment->GetTracks();
    if (!tracks) return false;

    for (unsigned long i = 0; i < tracks->GetTracksCount(); ++i) {
        const mkvparser::Track* t = tracks->GetTrackByIndex(i);
        if (!t) continue;
        const long type = t->GetType();
        const char* codec = t->GetCodecId();
        if (type == mkvparser::Track::kVideo && d->video_track < 0) {
            d->video_track = t->GetNumber();
            d->video_codec = codec ? codec : "";
            const mkvparser::VideoTrack* vt =
                static_cast<const mkvparser::VideoTrack*>(t);
            d->video_w = (int)vt->GetWidth();
            d->video_h = (int)vt->GetHeight();
        } else if (type == mkvparser::Track::kAudio && d->audio_track < 0) {
            d->audio_codec = codec ? codec : "";
            // Only Opus is supported for now.
            if (d->audio_codec == "A_OPUS") {
                const mkvparser::AudioTrack* at =
                    static_cast<const mkvparser::AudioTrack*>(t);
                d->audio_channels = (int)at->GetChannels();
                if (d->audio_channels < 1) d->audio_channels = 1;
                if (d->audio_channels > 2) d->audio_channels = 2;
                d->audio_track = t->GetNumber();
            }
        }
    }

    if (d->video_track < 0) {
        PLUGIN_WEBM_LOG( ("WebMDecoder: no video track found\n") );
        return false;
    }

    // Init libvpx for the detected codec, with multithreaded decoding (the
    // default is single-threaded, which is far too slow for HD/4K). VP8 uses the
    // thread count for row/loopfilter threading; VP9 additionally gets frame
    // threading. Skipped for the audio-only decoder instance so it doesn't spawn
    // decode threads it will never use (keeps the per-movie thread count down,
    // which matters when several movies play at once).
    if (initVideoDecoder) {
        if (!d->initVpx()) {
            PLUGIN_WEBM_LOG( ("WebMDecoder: vpx_codec_dec_init failed\n") );
            return false;
        }
        d->vpx_inited = true;
    }

    // Init Opus if present.
    if (d->audio_track >= 0) {
        int err = 0;
        d->opus = opus_decoder_create(kOpusRate, d->audio_channels, &err);
        if (err != OPUS_OK || !d->opus) {
            PLUGIN_WEBM_LOG( ("WebMDecoder: opus_decoder_create failed (%d)\n", err) );
            d->opus = nullptr;
            d->audio_track = -1; // fall back to video-only
        }
    }

    return true;
}

// Populate video_frame + backing buffers from a PendingFrame in the queue.
static void popPendingFrame(Impl* d) {
    Impl::PendingFrame& pf = d->frame_queue.front();
    d->y_buf = std::move(pf.y);
    d->u_buf = std::move(pf.u);
    d->v_buf = std::move(pf.v);
    d->video_frame.y_plane   = d->y_buf.data();
    d->video_frame.u_plane   = d->u_buf.data();
    d->video_frame.v_plane   = d->v_buf.data();
    d->video_frame.width     = pf.w;
    d->video_frame.height    = pf.h;
    d->video_frame.y_stride  = pf.ys;
    d->video_frame.uv_stride = pf.us;
    d->video_frame.timestamp = pf.ts;
    d->has_video = true;
    d->frame_queue.pop_front();
}

void WebMDecoder::decodeNextVideoFrame() {
    Impl* d = impl_;
    if (d->has_video || !d->vpx_inited) return;

    // Serve any frames buffered from a previous decode call first.
    if (!d->frame_queue.empty()) {
        popPendingFrame(d);
        return;
    }

    // VP9 may consume several packets before emitting a frame.
    for (int guard = 0; guard < 64; ++guard) {
        double ts = 0.0;
        if (!d->readNext(d->vcur, d->video_track, d->pkt, ts)) return;

        if (vpx_codec_decode(&d->vpx_ctx, d->pkt.data(),
                             (unsigned int)d->pkt.size(), nullptr, 0) != VPX_CODEC_OK) {
            PLUGIN_WEBM_LOG( ("WebMDecoder: vpx_codec_decode error: %s\n",
                              vpx_codec_error(&d->vpx_ctx)) );
            continue;
        }

        // Drain all frames emitted by this decode call (frame-parallel VP9 can
        // emit more than one image per vpx_codec_decode invocation).
        vpx_codec_iter_t it = nullptr;
        vpx_image_t* img;
        while ((img = vpx_codec_get_frame(&d->vpx_ctx, &it)) != nullptr) {
            const int w  = (int)img->d_w;
            const int h  = (int)img->d_h;
            const int ys = img->stride[VPX_PLANE_Y];
            const int us = img->stride[VPX_PLANE_U];
            const int ch = (h + 1) / 2;
            Impl::PendingFrame pf;
            pf.y.assign(img->planes[VPX_PLANE_Y], img->planes[VPX_PLANE_Y] + (size_t)ys * h);
            pf.u.assign(img->planes[VPX_PLANE_U], img->planes[VPX_PLANE_U] + (size_t)us * ch);
            pf.v.assign(img->planes[VPX_PLANE_V], img->planes[VPX_PLANE_V] + (size_t)us * ch);
            pf.w = w; pf.h = h; pf.ys = ys; pf.us = us; pf.ts = ts;
            d->frame_queue.push_back(std::move(pf));
        }

        if (!d->frame_queue.empty()) {
            popPendingFrame(d);
            return;
        }
    }
}

void WebMDecoder::decodeNextAudioFrame() {
    Impl* d = impl_;
    if (d->has_audio || !d->opus || d->audio_track < 0) return;

    double ts = 0.0;
    if (!d->readNext(d->acur, d->audio_track, d->pkt, ts)) return;

    const size_t cap = (size_t)kOpusMaxSamples * d->audio_channels;
    if (d->opus_tmp.size() < cap) d->opus_tmp.resize(cap);
    int16_t* tmp = d->opus_tmp.data();
    int n = opus_decode(d->opus, d->pkt.data(), (opus_int32)d->pkt.size(),
                        tmp, kOpusMaxSamples, 0);
    if (n <= 0) {
        PLUGIN_WEBM_LOG( ("WebMDecoder: opus_decode returned %d\n", n) );
        return;
    }

    d->audio_frame.samples.assign(tmp, tmp + (size_t)n * d->audio_channels);
    d->audio_frame.channels = d->audio_channels;
    d->audio_frame.sample_rate = kOpusRate;
    d->audio_frame.timestamp = ts;
    d->has_audio = true;
}

bool WebMDecoder::decodeNextFrame() {
    decodeNextVideoFrame();
    if (impl_->audio_track >= 0) decodeNextAudioFrame();
    return impl_->has_video || impl_->has_audio;
}

bool WebMDecoder::hasNewVideoFrame() { return impl_->has_video; }
bool WebMDecoder::hasNewAudioFrame() { return impl_->has_audio; }

VideoFrame WebMDecoder::getCurrentVideoFrame() {
    impl_->has_video = false;
    return impl_->video_frame;
}

AudioFrame WebMDecoder::getCurrentAudioFrame() {
    impl_->has_audio = false;
    return impl_->audio_frame;
}

bool WebMDecoder::hasAudioTrack() { return impl_->audio_track >= 0; }

double WebMDecoder::getDuration() { return impl_->duration_ns / 1000000000.0; }

int WebMDecoder::getVideoWidth() { return impl_->video_w; }
int WebMDecoder::getVideoHeight() { return impl_->video_h; }

void WebMDecoder::stop() {
    // Nothing to tear down between stop/replay; decoders are reused.
}

bool WebMDecoder::replay() {
    Impl* d = impl_;
    d->vcur = Cursor();
    d->acur = Cursor();
    d->has_video = false;
    d->has_audio = false;
    d->frame_queue.clear();

    // The vpx context retains reference frames from the previous playthrough.
    // Rewinding the cursors alone leaves that state stale, so the picture
    // freezes on replay while audio (reset below) restarts. Re-init the codec.
    if (d->vpx_inited) {
        vpx_codec_destroy(&d->vpx_ctx);
        d->vpx_inited = false;
        if (!d->initVpx()) {
            PLUGIN_WEBM_LOG( ("WebMDecoder: vpx re-init on replay failed\n") );
            return false;
        }
        d->vpx_inited = true;
    }

    if (d->opus) opus_decoder_ctl(d->opus, OPUS_RESET_STATE);
    return d->segment != nullptr;
}

bool WebMDecoder::seekTo(double t, std::atomic<bool>* abort) {
    Impl* d = impl_;
    if (!d->segment) return false;
    if (t < 0.0) t = 0.0;
    const long long target_ns = (long long)(t * 1000000000.0);

    const mkvparser::Tracks* tracks = d->segment->GetTracks();
    const mkvparser::Track* vt =
        tracks ? tracks->GetTrackByNumber(d->video_track) : nullptr;

    // Locate the video keyframe cluster/entry at or before the target. Prefer the
    // Cues index (a binary search over keyframes); fall back to FindCluster +
    // Cluster::GetEntry, which also returns the keyframe at/before the time.
    const mkvparser::Cluster* vcl = nullptr;
    const mkvparser::BlockEntry* vbe = nullptr;
    const mkvparser::Cues* cues = d->segment->GetCues();
    if (cues && vt) {
        while (!cues->DoneParsing()) cues->LoadCuePoint();
        const mkvparser::CuePoint* cp = nullptr;
        const mkvparser::CuePoint::TrackPosition* tp = nullptr;
        if (cues->Find(target_ns, vt, cp, tp) && cp && tp) {
            vcl = d->segment->FindOrPreloadCluster(tp->m_pos);
            if (vcl && !vcl->EOS()) vbe = vcl->GetEntry(*cp, *tp);
        }
    }
    if (!vbe && vt) {
        vcl = d->segment->FindCluster(target_ns);
        if (vcl && !vcl->EOS()) vbe = vcl->GetEntry(vt, target_ns);
    }
    if (!vcl || vcl->EOS()) return replay(); // couldn't locate -> rewind to start

    // Position the video cursor at the keyframe. (be may be null -> readNext
    // re-seeds from the cluster's first entry; the pre-roll below still discards
    // up to the target.)
    d->vcur = Cursor();
    d->vcur.started = true;
    d->vcur.cluster = vcl;
    d->vcur.be = vbe;
    d->frame_queue.clear();

    // Position the audio cursor near the target. Opus packets are independently
    // decodable, so starting at the cluster boundary closest to t is sufficient;
    // the binding's a/v sync converges from there.
    {
        const mkvparser::Cluster* acl = d->segment->FindCluster(target_ns);
        d->acur = Cursor();
        d->acur.started = true;
        d->acur.cluster = (acl && !acl->EOS()) ? acl : vcl; // be null -> first entry
    }

    d->has_video = false;
    d->has_audio = false;

    // Reset the vpx context (it holds reference frames from the old position) and
    // pre-roll from the keyframe up to the target, keeping the first frame whose
    // timestamp reaches t as the current frame (so the worker queues it next).
    if (d->vpx_inited) {
        vpx_codec_destroy(&d->vpx_ctx);
        d->vpx_inited = false;
        if (!d->initVpx()) {
            PLUGIN_WEBM_LOG( ("WebMDecoder: vpx re-init on seek failed\n") );
            return false;
        }
        d->vpx_inited = true;

        for (int guard = 0; guard < 8192; ++guard) {
            if (abort && abort->load()) break;              // a newer seek superseded us
            decodeNextVideoFrame();
            if (!d->has_video) break;                       // EOF before target
            if (d->video_frame.timestamp + 1e-6 >= t) break; // reached target; keep
            d->has_video = false;                           // discard, decode next
        }
    }

    if (d->opus) opus_decoder_ctl(d->opus, OPUS_RESET_STATE);
    return true;
}

bool WebMDecoder::isPlaybackFinished() {
    Impl* d = impl_;
    const bool video_done = d->vcur.done && !d->has_video;
    const bool audio_done = (d->audio_track < 0) || (d->acur.done && !d->has_audio);
    return video_done && audio_done;
}

} // namespace webm

