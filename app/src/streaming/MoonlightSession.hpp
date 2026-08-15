#pragma once

#include "GameStreamClient.hpp"
#include <atomic>
#include "MoonlightSessionDecoderAndRenderProvider.hpp"
#include <nanovg.h>

struct SessionStats {
    VideoDecodeStats video_decode_stats;
    VideoRenderStats video_render_stats;
};

class MoonlightSession {
  public:
    static void
    set_provider(MoonlightSessionDecoderAndRenderProvider* provider);

    MoonlightSession(const std::string& address, int app_id);
    ~MoonlightSession();

    static MoonlightSession* activeSession();

    void start(ServerCallback<bool> callback, bool is_sunshine);
    void stop(int terminate_app);
    void set_address(const std::string& address) { m_address = address; }

    void restart();

    void draw(NVGcontext* vg, int width, int height);

    // Stops the session touching the GPU while the app is not on screen, and
    // makes it drop any resource that may not have survived being off screen
    // once it comes back. See StreamingView::onWindowFocusChanged().
    void set_suspended(bool suspended);

    bool is_suspended() const { return m_suspended; }

    bool is_active() const { return m_is_active; }
    bool is_terminated() const { return m_is_terminated; }

    /**
     * True when the session says it is up but no video has arrived for long
     * enough that it cannot be.
     *
     * is_terminated() is set by exactly one thing, the termination callback,
     * and on 2026-08-14 that callback was never delivered: moonlight-common-c
     * failed to create the thread it runs on, having already latched the flag
     * that suppresses every later attempt. The session stayed active=1
     * terminated=0 with a dead socket underneath it, so draw() kept rendering
     * the last decoded frame and the console had to be power cycled.
     *
     * A second, independent way to notice removes that single point of failure.
     * Frames arriving is the ground truth about whether a stream is alive, and
     * unlike a callback it cannot be suppressed by a flag.
     */
    bool is_stalled() const;

    /** Age of the newest frame, for the log line that reports a stall. */
    uint64_t seconds_since_last_frame() const;

    bool connection_status_is_poor() const {
        return m_connection_status_is_poor;
    }

    bool use_hdr() const {
        return m_use_hdr;
    }

    SessionStats* session_stats() const {
        return (SessionStats*)&m_session_stats;
    }

  private:
    static void connection_stage_starting(int);
    static void connection_stage_complete(int);
    static void connection_stage_failed(int, int);
    static void connection_started();
    static void connection_terminated(int);
    static void connection_log_message(const char* format, ...);
    static void connection_rumble(unsigned short, unsigned short,
                                  unsigned short);
    static void connection_rumble_triggers(uint16_t controllerNumber,
                                           uint16_t leftTriggerMotor, uint16_t rightTriggerMotor);
    static void connection_status_update(int);
    static void connection_set_hdr_mode(bool);

    static int video_decoder_setup(int, int, int, int, void*, int);
    static void video_decoder_start();
    static void video_decoder_stop();
    static void video_decoder_cleanup();
    static int video_decoder_submit_decode_unit(PDECODE_UNIT);

    static int audio_renderer_init(int, const POPUS_MULTISTREAM_CONFIGURATION,
                                   void*, int);
    static void audio_renderer_start();
    static void audio_renderer_stop();
    static void audio_renderer_cleanup();
    static void audio_renderer_decode_and_play_sample(char*, int);

    std::string m_address;
    int m_app_id;
    bool m_is_sunshine = false;
    STREAM_CONFIGURATION m_config;
    CONNECTION_LISTENER_CALLBACKS m_connection_callbacks;
    DECODER_RENDERER_CALLBACKS m_video_callbacks;
    AUDIO_RENDERER_CALLBACKS m_audio_callbacks;

    IFFmpegVideoDecoder* m_video_decoder = nullptr;
    IVideoRenderer* m_video_renderer = nullptr;
    IAudioRenderer* m_audio_renderer = nullptr;

    // Atomic because they cross threads: the connection-status callbacks
    // arrive on moonlight-common-c's detached termination thread while the
    // UI thread polls is_active()/is_terminated() every frame. As plain
    // bools those concurrent accesses are a data race, which is undefined
    // behaviour, not merely a stale read.
    std::atomic<bool> m_is_active{false};
    std::atomic<bool> m_is_terminated{false};
    std::atomic<bool> m_stop_requested{false};

    // Written by the decoder thread on every frame, read by the render thread
    // every frame. Atomic for the same reason the flags above are: concurrent
    // access to a plain uint64_t here is a data race, not a stale read.
    //
    // Zero means no frame has arrived yet, which is a real state during
    // connection setup and must not be read as a stall.
    std::atomic<uint64_t> m_last_frame_ms{0};

    // When the console went off screen, and whether the sleep that followed
    // was too long to resume across. Written on the main thread by
    // set_suspended(), read on the termination thread by
    // connection_terminated(), so both are atomic.
    std::atomic<uint64_t> m_suspended_at_ms{0};
    std::atomic<bool> m_resume_declined{false};
    bool m_suspended = false;
    bool m_invalidate_renderer_pending = false;
    bool m_connection_status_is_poor = false;
    bool m_use_hdr = false;

    SessionStats m_session_stats = {};
    uint64_t m_last_stats_update_ms = 0;
};
