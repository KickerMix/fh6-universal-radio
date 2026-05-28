#pragma once
#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"
#include "fh6/ring_buffer.hpp"
#include <vector>
#include <string>
#include <windows.h>

namespace fh6 {

struct JellyfinTrack {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    uint64_t duration_ms = 0;
};

class JellyfinSource : public IAudioSource {
public:
    explicit JellyfinSource(JellyfinConfig cfg);
    ~JellyfinSource() override;

    bool initialize();
    void set_config(JellyfinConfig cfg);
    
    std::string_view name() const noexcept override { return "jellyfin"; }
    std::string_view display_name() const noexcept override { return "Jellyfin"; }
    
    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void previous() override;
    void cast(const std::string& playlist_id); // webUI
    void pump(RingBuffer& ring) override;
    void shutdown() noexcept override;

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override;
    AuthState auth_state() const noexcept override;
    SourceCapabilities capabilities() const noexcept override;

private:
    bool fetch_playlist();
    bool launch_ffmpeg(const std::string& item_id);

    JellyfinConfig cfg_;
    std::vector<JellyfinTrack> queue_;
    size_t current_idx_ = 0;
    bool playing_ = false;
    uint64_t bytes_consumed_ = 0;

    HANDLE ffmpeg_process_ = nullptr;
    HANDLE ffmpeg_stdout_ = nullptr;

    RingBuffer* current_ring_ = nullptr;
};

} // namespace fh6