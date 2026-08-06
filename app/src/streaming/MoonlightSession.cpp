#include "MoonlightSession.hpp"
#include "OtlpTraceExporter.hpp"
#include "OtlpMetricsExporter.hpp"
#include "AVFrameHolder.hpp"
#include "GameStreamClient.hpp"
#include "InputManager.hpp"
#include "Settings.hpp"
#include "borealis.hpp"
#include <string.h>

#if defined(PLATFORM_IOS) || defined(PLATFORM_VISIONOS)
extern void getWindowSize(int* w, int* h);
#endif

using namespace brls;

int m_video_format;
static MoonlightSession* m_active_session = nullptr;
static MoonlightSessionDecoderAndRenderProvider* m_provider = nullptr;


MoonlightSession* MoonlightSession::activeSession() {
    return m_active_session;
}

void MoonlightSession::set_provider(
    MoonlightSessionDecoderAndRenderProvider* provider) {
    m_provider = provider;
}

MoonlightSession::MoonlightSession(const std::string& address, int app_id) {
    m_address = address;
    m_app_id = app_id;
    m_active_session = this;

    m_video_decoder = m_provider->video_decoder();
    m_video_renderer = m_provider->video_renderer();
    m_audio_renderer = m_provider->audio_renderer();
}

MoonlightSession::~MoonlightSession() {
    if (m_video_decoder) {
        delete m_video_decoder;
    }

    if (m_video_renderer) {
        delete m_video_renderer;
    }

    if (m_audio_renderer) {
        delete m_audio_renderer;
    }

    m_active_session = nullptr;
}

// MARK: Connection callbacks

static const char* stages[] = {"STAGE_NONE",
                               "STAGE_PLATFORM_INIT",
                               "STAGE_NAME_RESOLUTION",
                               "STAGE_RTSP_HANDSHAKE",
                               "STAGE_CONTROL_STREAM_INIT",
                               "STAGE_VIDEO_STREAM_INIT",
                               "STAGE_AUDIO_STREAM_INIT",
                               "STAGE_INPUT_STREAM_INIT",
                               "STAGE_CONTROL_STREAM_START",
                               "STAGE_VIDEO_STREAM_START",
                               "STAGE_AUDIO_STREAM_START",
                               "STAGE_INPUT_STREAM_START"};

void MoonlightSession::connection_stage_starting(int stage) {
    brls::Logger::info("MoonlightSession: Starting: {}", stages[stage]);
}

void MoonlightSession::connection_stage_complete(int stage) {
    brls::Logger::info("MoonlightSession: Complete: {}", stages[stage]);
}

void MoonlightSession::connection_stage_failed(int stage, int error_code) {
    brls::Logger::error("MoonlightSession: Failed: {} with error code: {}", stages[stage], error_code);
}

void MoonlightSession::connection_started() {
    brls::Logger::info("MoonlightSession: Connection started");
    if (!m_active_session)
        return;

    m_active_session->m_stop_requested = false;
    m_active_session->m_is_active = true;
}

void MoonlightSession::connection_terminated(int error_code) {
    /* Every arrival, including the repeats from multiple threads that show
       up before a crash. The count is the signal; one is routine. */
    OtlpMetricsExporter::instance().increment("moonlight.connection_terminated");

    /* This runs on a detached thread that moonlight-common-c spawns, which is
     * the whole reason it is worth a span. When it overlaps the main thread
     * resuming the app, a log gives two lines 3ms apart and no way to tell
     * whether they overlapped. The span hangs off the session root so both
     * land in one trace and the overlap is visible rather than inferred. */
    OtlpSpanScope otlpSpan("session.connection_terminated");
    otlpSpan.attr("error.code", std::to_string(error_code));

    brls::Logger::info("MoonlightSession: Connection terminated with code: {}", error_code);

    /* The invariant nobody can see from a log: is the static still pointing
     * at a live session when the termination thread arrives. Recorded rather
     * than only branched on, so a run where it was already gone is
     * distinguishable from one where it was fine. */
    OtlpMetricsExporter::instance().gauge("moonlight.active_session_present",
                                          m_active_session ? 1 : 0);
    if (!m_active_session) {
        otlpSpan.attr("outcome", "no_active_session");
        return;
    }

    if (m_active_session->m_stop_requested) {
        otlpSpan.attr("outcome", "stop_acknowledged");
        brls::Logger::info("MoonlightSession: Termination acknowledged after stop request");
        m_active_session->m_is_active = false;
        m_active_session->m_is_terminated = true;
        return;
    }

    if (error_code != 0) {
        otlpSpan.attr("outcome", "reconnect_attempt");
        /* Counts every reconnect the session attempts on its own. A rate on
           this separates "the network blipped once" from the repeated
           termination storm that precedes the hang. */
        OtlpMetricsExporter::instance().increment("moonlight.reconnect_attempts");
        brls::Logger::info("MoonlightSession: Reconnection attempt");

        // Connection is already terminated here; avoid toggling the user stop flag.
        //
        // Both of the calls below happen on the termination thread, and
        // together they are the 100ms window the main thread was seen
        // resuming inside. Split so the next crash says which half was
        // running when the collision happened rather than only that one of
        // them was.
        {
            OtlpSpanScope stopSpan("session.LiStopConnection", &otlpSpan.span());
            /*
             * Any value above 1 is two threads inside LiStopConnection at once,
             * which is the defect itself rather than evidence of it.
             *
             * The count has to be taken at every call site or it cannot mean
             * that. One thread entering here while another enters through
             * stop() only reads as 2 if stop() is counted too; counting one
             * site measures how often that site runs, which nothing needed.
             *
             * Scope guard rather than a matched pair of calls, so the release
             * cannot be skipped by an early return and cannot be duplicated
             * without the acquire being duplicated with it.
             */
            OtlpGaugeScope inflight("moonlight.listop_inflight");
            LiStopConnection();
        }

        OtlpSpanScope restartSpan("session.restart", &otlpSpan.span());

        /* Read once. Every use below this point in the original code was a
         * fresh load of a raw static that another thread can clear, so the
         * pointer could differ between the null check and the call. */
        MoonlightSession* session = m_active_session;
        if (!session) {
            restartSpan.fail("session cleared before restart");
            return;
        }

        session->start([](const GSResult<bool>& result) {
            if (result.isSuccess()) {
                brls::Logger::info("MoonlightSession: Reconnected");
            } else {
                brls::Logger::info("MoonlightSession: Reconnection failed");
                if (m_active_session) {
                    m_active_session->m_is_active = false;
                    m_active_session->m_is_terminated = true;
                }
            }
        }, session->m_is_sunshine);
        return;
    }

    m_active_session->m_is_active = false;
    m_active_session->m_is_terminated = true;
}

void MoonlightSession::connection_log_message(const char* format, ...) {
    va_list arglist;
    va_start(arglist, format);
    int size = vsnprintf(NULL, 0, format, arglist);
    char buffer[size];
    vsnprintf(buffer, size, format, arglist);
    va_end(arglist);

    brls::Logger::info(fmt::runtime(std::string(buffer)));
}

void MoonlightSession::connection_rumble(unsigned short controller,
                                         unsigned short lowFreqMotor,
                                         unsigned short highFreqMotor) {
    MoonlightInputManager::instance().handleRumble(controller, lowFreqMotor,
                                                   highFreqMotor);
}


void MoonlightSession::connection_rumble_triggers(uint16_t controllerNumber, 
                                                  uint16_t leftTriggerMotor, 
                                                  uint16_t rightTriggerMotor) 
{
    // MoonlightInputManager::instance().handleRumbleTriggers(controllerNumber, leftTriggerMotor, rightTriggerMotor);                                                
}

void MoonlightSession::connection_status_update(int connection_status) {
    if (m_active_session) {
        m_active_session->m_connection_status_is_poor =
            connection_status == CONN_STATUS_POOR;
    }
}

void MoonlightSession::connection_set_hdr_mode(bool use_hdr) {
    if (m_active_session) {
        m_active_session->m_use_hdr = use_hdr;
    }
}

// MARK: Video decoder callbacks

int MoonlightSession::video_decoder_setup(int video_format, int width,
                                          int height, int redraw_rate,
                                          void* context, int dr_flags) {
    m_video_format = video_format;
    if (m_active_session && m_active_session->m_video_decoder) {
        return m_active_session->m_video_decoder->setup(
            video_format, width, height, redraw_rate, context, dr_flags);
    }
    return DR_OK;
}

void MoonlightSession::video_decoder_start() {
    if (m_active_session && m_active_session->m_video_decoder) {
        m_active_session->m_video_decoder->start();
    }
}

void MoonlightSession::video_decoder_stop() {
    if (m_active_session && m_active_session->m_video_decoder) {
        m_active_session->m_video_decoder->stop();
    }
}

void MoonlightSession::video_decoder_cleanup() {
    if (m_active_session && m_active_session->m_video_decoder) {
        m_active_session->m_video_decoder->cleanup();
    }
}

int MoonlightSession::video_decoder_submit_decode_unit(
    PDECODE_UNIT decode_unit) {
    if (m_active_session && m_active_session->m_video_decoder) {
        return m_active_session->m_video_decoder->submit_decode_unit(
            decode_unit);
    }
    return DR_OK;
}

// MARK: Audio callbacks

int MoonlightSession::audio_renderer_init(
    int audio_configuration, const POPUS_MULTISTREAM_CONFIGURATION opus_config,
    void* context, int ar_flags) {
    if (m_active_session && m_active_session->m_audio_renderer) {
        return m_active_session->m_audio_renderer->init(
            audio_configuration, opus_config, context, ar_flags);
    }
    return DR_OK;
}

void MoonlightSession::audio_renderer_start() {
    if (m_active_session && m_active_session->m_audio_renderer) {
        m_active_session->m_audio_renderer->start();
    }
}

void MoonlightSession::audio_renderer_stop() {
    if (m_active_session && m_active_session->m_audio_renderer) {
        m_active_session->m_audio_renderer->stop();
    }
}

void MoonlightSession::audio_renderer_cleanup() {
    if (m_active_session && m_active_session->m_audio_renderer) {
        m_active_session->m_audio_renderer->cleanup();
    }
}

void MoonlightSession::audio_renderer_decode_and_play_sample(
    char* sample_data, int sample_length) {
    if (m_active_session && m_active_session->m_audio_renderer) {
        m_active_session->m_audio_renderer->decode_and_play_sample(
            sample_data, sample_length);
    }
}

// MARK: MoonlightSession

void MoonlightSession::start(ServerCallback<bool> callback, bool is_sunshine) {
    m_is_sunshine = is_sunshine;
    m_stop_requested = false;
    m_is_terminated = false;

    LiInitializeStreamConfiguration(&m_config);

    int resolution = Settings::instance().resolution();
    int h = resolution;
    int w = h * 16 / 9;
    if (resolution == -1) {
#if defined(PLATFORM_IOS) || defined(PLATFORM_VISIONOS)
        getWindowSize(&w, &h);
#else
        h = Application::windowHeight;
        w = Application::windowWidth;
#endif

        int nativeResolutionScale = Settings::instance().native_resolution_scale();
        if (nativeResolutionScale != 100 && w > 0 && h > 0) {
            w = (w * nativeResolutionScale + 50) / 100;
            h = (h * nativeResolutionScale + 50) / 100;
        }
    }

    // Prohibit odd values rounding up
    if ((h & 1) == 1) {
        h += 1;
    }

    if ((w & 1) == 1) {
        w += 1;
    }

    m_config.width = w;
    m_config.height = h;
    m_config.fps = Settings::instance().fps();
    m_config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    m_config.packetSize = 1392;
    m_config.streamingRemotely = STREAM_CFG_AUTO;
    m_config.bitrate = Settings::instance().bitrate();
    m_config.encryptionFlags = m_is_sunshine ? ENCFLG_ALL : ENCFLG_VIDEO;

    switch (Settings::instance().video_codec()) {
    case H264:
        m_config.supportedVideoFormats = VIDEO_FORMAT_H264;
        break;
    case H265:
        m_config.supportedVideoFormats = VIDEO_FORMAT_H265;
            if (Settings::instance().request_hdr())
                m_config.supportedVideoFormats |= VIDEO_FORMAT_H265_MAIN10;
        break;
    case AV1:
        m_config.supportedVideoFormats = VIDEO_FORMAT_AV1_MAIN8;
            if (Settings::instance().request_hdr())
                m_config.supportedVideoFormats |= VIDEO_FORMAT_AV1_MAIN10;
        break;
    default:
        break;
    }

    LiInitializeConnectionCallbacks(&m_connection_callbacks);
    m_connection_callbacks.stageStarting = connection_stage_starting;
    m_connection_callbacks.stageComplete = connection_stage_complete;
    m_connection_callbacks.stageFailed = connection_stage_failed;
    m_connection_callbacks.connectionStarted = connection_started;
    m_connection_callbacks.connectionTerminated = connection_terminated;
    m_connection_callbacks.logMessage = connection_log_message;
    m_connection_callbacks.rumble = connection_rumble;
    m_connection_callbacks.rumbleTriggers = connection_rumble_triggers;
    m_connection_callbacks.connectionStatusUpdate = connection_status_update;
    m_connection_callbacks.setHdrMode = connection_set_hdr_mode;

    LiInitializeVideoCallbacks(&m_video_callbacks);
    m_video_callbacks.setup = video_decoder_setup;
    m_video_callbacks.start = video_decoder_start;
    m_video_callbacks.stop = video_decoder_stop;
    m_video_callbacks.cleanup = video_decoder_cleanup;
    m_video_callbacks.submitDecodeUnit = video_decoder_submit_decode_unit;

    if (m_video_decoder) {
        m_video_callbacks.capabilities = m_video_decoder->capabilities();
    }

    LiInitializeAudioCallbacks(&m_audio_callbacks);
    m_audio_callbacks.init = audio_renderer_init;
    m_audio_callbacks.start = audio_renderer_start;
    m_audio_callbacks.stop = audio_renderer_stop;
    m_audio_callbacks.cleanup = audio_renderer_cleanup;
    m_audio_callbacks.decodeAndPlaySample =
        audio_renderer_decode_and_play_sample;

    if (m_audio_renderer) {
        m_audio_callbacks.capabilities = m_audio_renderer->capabilities();
    }

    GameStreamClient::instance().start(
        m_address, m_config, m_app_id, [this, callback](auto result) {
            if (result.isSuccess()) {
                m_config = result.value();
                brls::async([this, callback]() mutable {
                    auto m_data =
                        GameStreamClient::instance().server_data(m_address);

                    int result = LiStartConnection(
                        &m_data.serverInfo, &m_config, &m_connection_callbacks,
                        &m_video_callbacks, &m_audio_callbacks, NULL, 0, NULL, 0);

                    if (result != 0) {
                        /* Runs on a brls::async thread, so this is a fourth
                         * way into LiStopConnection and has to be counted like
                         * the others. A failed start cleaning itself up here
                         * while the user backs out of the view is two threads
                         * in the same teardown. */
                        OtlpGaugeScope inflight("moonlight.listop_inflight");
                        LiStopConnection();
                        callback(
                            GSResult<bool>::failure("error/stream_start"_i18n));
                    } else {
                        callback(GSResult<bool>::success(true));
                    }
                });
            } else {
                brls::Logger::error(
                    "MoonlightSession: Failed to start stream: {}",
                    result.error().c_str());
                callback(GSResult<bool>::failure(result.error()));
            }
        });
}

void MoonlightSession::stop(int terminate_app) {
    /* Usually the main thread. If this ever overlaps
     * session.connection_terminated in a trace, the object is being torn
     * down while the termination thread is still inside it. */
    OtlpSpanScope otlpSpan("session.stop");

    if (m_stop_requested)
        return;

    m_stop_requested = true;

    if (terminate_app) {
        GameStreamClient::instance().quit(m_address, [](auto _) {});
    }

    /* The main thread's way in. This is the other half of the pair the count
     * exists to catch: the guard above is m_stop_requested, and the reconnect
     * path deliberately does not set it, so nothing here excludes a
     * termination thread already inside LiStopConnection. */
    OtlpGaugeScope inflight("moonlight.listop_inflight");
    LiStopConnection();
}

void MoonlightSession::restart() {
    OtlpGaugeScope inflight("moonlight.listop_inflight");
    LiStopConnection();

    start([](const GSResult<bool>& result) {
        if (result.isSuccess()) {
            brls::Logger::info("MoonlightSession: Reconnected");
        } else {
            brls::Logger::info("MoonlightSession: Reconnection failed");
            if (m_active_session) {
                m_active_session->m_is_active = false;
                m_active_session->m_is_terminated = true;
            }
        }
    }, m_active_session->m_is_sunshine);
}

void MoonlightSession::set_suspended(bool suspended) {
    if (m_suspended == suspended) {
        return;
    }

    m_suspended = suspended;
    // DIAGNOSTIC BUILD: report the session state across the transition. After
    // a real sleep the wifi was off, so this is where a dead connection that
    // nobody has declared terminated shows up as active=1 terminated=0 with
    // no frames arriving.
    brls::Logger::info("MoonlightSession: rendering {} (active={} terminated={} "
                       "stop_requested={})",
                       suspended ? "suspended" : "resumed", m_is_active.load(),
                       m_is_terminated.load(), m_stop_requested.load());

    if (!suspended) {
        // The renderer is owned by the decoder callbacks and torn down on
        // their thread, so let draw() do this from the render thread inside
        // the guard it already holds rather than reaching for it here.
        m_invalidate_renderer_pending = true;
    }
}

void MoonlightSession::draw(NVGcontext* vg, int width, int height) {
    // While the app is off screen the compositor is not showing our frames and
    // the graphics service may be shutting down under us, so there is nothing
    // to gain by drawing and a suspended GPU to fault by trying.
    if (m_suspended) {
        return;
    }

    if (m_video_decoder && m_video_renderer) {
        if (m_invalidate_renderer_pending) {
            m_invalidate_renderer_pending = false;
            m_video_renderer->invalidateHardwareResources();
        }

        /* AVFrameHolder::get pops under the queue mutex and then releases it
           before calling this, so the frame is used with nothing holding it.
           If the decoder is being torn down on another thread at the same
           moment, av_frame_free has already run and this hands a freed
           AVFrame's nvmap handle to the GPU. That is the one candidate that
           explains a death with no crash report, because the graphics service
           kills the process from outside.

           frames_in_flight is the test: if it is ever non zero while
           decoder.cleanup is open, the two really do overlap. */
        otlp_metric_count_add("moonlight.frames_in_flight", 1);
        {
            OtlpSpanScope frameSpan("render.draw_frame");
            AVFrameHolder::instance().get(
                [this, vg, width, height](AVFrame* frame) {
                    m_video_renderer->draw(vg, width, height, frame, m_video_format);
                });
        }
        otlp_metric_count_add("moonlight.frames_in_flight", -1);

        const uint64_t now = LiGetMillis();
        if (m_last_stats_update_ms == 0 || now - m_last_stats_update_ms >= 250) {
            m_session_stats.video_decode_stats =
                *m_video_decoder->video_decode_stats();
            m_session_stats.video_render_stats =
                *m_video_renderer->video_render_stats();
            m_last_stats_update_ms = now;
        }
    }
}
