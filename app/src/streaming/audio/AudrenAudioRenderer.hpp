#ifdef __SWITCH__

#include "IAudioRenderer.hpp"
#include <opus/opus_multistream.h>
#include <switch.h>
#pragma once

#define BUFFER_COUNT 5

class AudrenAudioRenderer : public IAudioRenderer {
  public:
    AudrenAudioRenderer(){};
    // Backstop for cleanup(), not a duplicate of it. cleanup() is driven by
    // moonlight-common-c's AUDIO_RENDERER_CALLBACKS.cleanup, which only fires
    // on paths that consider the audio stream to have been initialized; an
    // init() that fails partway through (audrvCreate, audrenInitialize) never
    // reaches that state and the driver/decoder/mempool it already allocated
    // had nothing that would ever free them. The destructor runs
    // unconditionally when the owning MoonlightSession is torn down, so
    // calling cleanup() here closes that gap. cleanup() already guards every
    // field with a null/bool check, so calling it a second time when the
    // callback path already ran is a no-op, not a double free.
    ~AudrenAudioRenderer() { cleanup(); }

    int init(int audio_configuration,
             const POPUS_MULTISTREAM_CONFIGURATION opus_config, void* context,
             int ar_flags) override;
    void cleanup() override;
    void decode_and_play_sample(char* sample_data, int sample_length) override;
    int capabilities() override;

  private:
    ssize_t free_wavebuf_index();
    size_t append_audio(const void* buf, size_t size);
    void write_audio(const void* buf, size_t size);
    bool flush();

    OpusMSDecoder* m_decoder = nullptr;
    s16* m_decoded_buffer = nullptr;
    void* mempool_ptr = nullptr;
    void* current_pool_ptr = nullptr;

    AudioDriver m_driver;
    AudioDriverWaveBuf m_wavebufs[BUFFER_COUNT];
    AudioDriverWaveBuf* m_current_wavebuf;
    Mutex m_update_lock;

    bool m_inited_driver = false;
    // Separate from m_inited_driver, which only becomes true once the voice
    // is fully up (see init()). These two gate audrenExit()/audrvClose()
    // independently, so a failure partway through init() -- audrvCreate
    // failing after audrenInitialize already succeeded, for example -- still
    // releases exactly what was actually acquired instead of nothing.
    bool m_audren_initialized = false;
    bool m_driver_created = false;
    int m_channel_count = 0;
    int m_sample_rate = 0;
    int m_buffer_size = 0;
    int m_samples = 0;
    size_t m_total_queued_samples = 0;
    ssize_t m_current_size = 0;

    const int m_samples_per_frame = AUDREN_SAMPLES_PER_FRAME_48KHZ;
    const int m_latency = 5;
};

#endif // __SWITCH__