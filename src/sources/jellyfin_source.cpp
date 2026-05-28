#include "fh6/sources/jellyfin_source.hpp"
#include "fh6/log.hpp"
#include <nlohmann/json.hpp>
#include <winhttp.h>
#include <format>
#include <random>
#include <chrono>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace fh6 {

static std::wstring widen(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &result[0], size);
    return result;
}

JellyfinSource::JellyfinSource(JellyfinConfig cfg) : cfg_(std::move(cfg)) {}

JellyfinSource::~JellyfinSource() {
    shutdown();
}

void JellyfinSource::set_config(JellyfinConfig cfg) {
    cfg_ = std::move(cfg);
}

bool JellyfinSource::initialize() {
    // attempt an initial fetch so it's ready if the config is valid
    // but always return true so the tile appears in the Web UI regardless
    fetch_playlist();
    return true; 
}

void JellyfinSource::play() {
    // if the queue is empty when press play, try fetching it again
    // handles the case where the source is enabled before typing in the API key
    if (queue_.empty()) {
        if (!fetch_playlist()) {
            log::error("[jellyfin] Cannot play, playlist fetch failed.");
            return; 
        }
    }
    playing_ = true;
}

void JellyfinSource::pause() {
    playing_ = false;
}

void JellyfinSource::shutdown() noexcept {
    playing_ = false;
    if (ffmpeg_process_) {
        TerminateProcess(ffmpeg_process_, 0);
        CloseHandle(ffmpeg_process_);
        ffmpeg_process_ = nullptr;
    }
    if (ffmpeg_stdout_) {
        CloseHandle(ffmpeg_stdout_);
        ffmpeg_stdout_ = nullptr;
    }
}

bool JellyfinSource::fetch_playlist() {
    if (cfg_.server_url.empty() || cfg_.api_key.empty() || cfg_.default_playlist.empty()) {
        log::error("[jellyfin] Missing required config.");
        return false;
    }

    std::string path;
    if (cfg_.user_id.empty()) {
        log::error("[jellyfin] Invalid user_id.");
        return false;
    }

    // use the user endpoint
    path = std::format("/Users/{}/Items?ParentId={}&Filters=IsNotFolder", 
                        cfg_.user_id, cfg_.default_playlist);
    
    std::wstring whost, wpath = widen(path);
    URL_COMPONENTS urlComp = {0};
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwHostNameLength  = (DWORD)-1;
    
    std::wstring full_url = widen(cfg_.server_url);
    
    // guard against bad URLs (missing http(s)://)
    if (!WinHttpCrackUrl(full_url.c_str(), 0, 0, &urlComp) || !urlComp.lpszHostName) {
        log::error("[jellyfin] Invalid server_url. Ensure it starts with http:// or https://");
        return false;
    }
    whost = std::wstring(urlComp.lpszHostName, urlComp.dwHostNameLength);

    // guard against missing internet handles
    HINTERNET hSession = WinHttpOpen(L"FH6 Universal Radio/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 
                                           (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring auth_header = widen(std::format("Authorization: MediaBrowser Token=\"{}\"", cfg_.api_key));
    WinHttpAddRequestHeaders(hRequest, auth_header.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    std::string response_body;
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {
        
        // grab the HTTP status code
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

        DWORD dwSize = 0;
        DWORD dwDownloaded = 0;
        do {
            WinHttpQueryDataAvailable(hRequest, &dwSize);
            if (dwSize == 0) break;
            std::string buffer(dwSize, 0);
            WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded);
            response_body += buffer.substr(0, dwDownloaded);
        } while (dwSize > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    // if the server rejected the request, log exactly what it said instead of crashing
    if (statusCode != 200) {
        log::error("[jellyfin] Server returned HTTP {}. Message: {}", statusCode, response_body);
        return false;
    }

    if (response_body.empty()) {
        log::error("[jellyfin] Server returned an empty response.");
        return false;
    }

    try {
        auto json = nlohmann::json::parse(response_body);
        queue_.clear();
        for (const auto& item : json["Items"]) {
            JellyfinTrack t;
            t.id = item.value("Id", "");
            t.title = item.value("Name", "Unknown Track");
            
            if (item.contains("AlbumArtist") && item["AlbumArtist"].is_string()) {
                t.artist = item["AlbumArtist"].get<std::string>();
            } else if (item.contains("Artists") && item["Artists"].is_array() && !item["Artists"].empty()) {
                t.artist = item["Artists"][0].get<std::string>();
            }
            
            t.album = item.value("Album", "");
            // Jellyfin uses Ticks (10,000 ticks = 1 millisecond)
            if (item.contains("RunTimeTicks") && item["RunTimeTicks"].is_number()) {
                t.duration_ms = item["RunTimeTicks"].get<uint64_t>() / 10000;
            }
            queue_.push_back(t);
        }

        if (cfg_.shuffle && !queue_.empty()) {
            std::random_device rd;
            std::mt19937 g(rd());
            std::shuffle(queue_.begin(), queue_.end(), g);
        }
        
        log::info("[jellyfin] Loaded {} tracks.", queue_.size());
        return true;
    } catch (const std::exception& e) {
        log::error("[jellyfin] JSON parse error: {}", e.what());
        return false;
    }
}

bool JellyfinSource::launch_ffmpeg(const std::string& item_id) {
    bytes_consumed_ = 0;

    std::string ffmpeg_exe = cfg_.ffmpeg_path.empty() ? "ffmpeg.exe" : cfg_.ffmpeg_path;
    
    std::string stream_url = std::format("{}/Audio/{}/stream?api_key={}&static=true", 
                                         cfg_.server_url, item_id, cfg_.api_key);

    std::wstring cmd = widen(std::format(
        "\"{}\" -v quiet -i \"{}\" -f s16le -ar 48000 -ac 2 -", 
        ffmpeg_exe, stream_url
    ));

    SECURITY_ATTRIBUTES saAttr = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    HANDLE hChildStd_OUT_Wr = NULL;
    
    if (!CreatePipe(&ffmpeg_stdout_, &hChildStd_OUT_Wr, &saAttr, 0)) {
        return false;
    }
    SetHandleInformation(ffmpeg_stdout_, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {sizeof(STARTUPINFOW)};
    si.hStdOutput = hChildStd_OUT_Wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, cmd.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        ffmpeg_process_ = pi.hProcess;
        CloseHandle(pi.hThread);
        CloseHandle(hChildStd_OUT_Wr);
        return true;
    }

    CloseHandle(ffmpeg_stdout_);
    CloseHandle(hChildStd_OUT_Wr);
    ffmpeg_stdout_ = nullptr;
    return false;
}

void JellyfinSource::pump(RingBuffer& ring) {
    current_ring_ = &ring;

    if (!playing_ || queue_.empty()) return;

    if (!ffmpeg_process_) {
        if (!launch_ffmpeg(queue_[current_idx_].id)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            current_idx_ = (current_idx_ + 1) % queue_.size();
            return;
        }
    }

    // if the ring buffer doesn't have enough free space to safely hold 
    // the 64KB chunk, sleep and let FMOD catch up before pulling more
    // prevents PCM byte corruption
    if (ring.capacity() - ring.readable() < 65536) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return; 
    }

    // read a chunk of data. the AudioSourceManager loops this continuously.
    std::vector<uint8_t> buffer(65536); 
    DWORD bytesRead = 0;
    
    if (ReadFile(ffmpeg_stdout_, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, NULL) && bytesRead > 0) {
        ring.write(buffer.data(), bytesRead); 
        bytes_consumed_ += bytesRead;
    } else {
        // process ended (track finished) or pipe broken, move to next track
        shutdown();
        current_idx_ = (current_idx_ + 1) % queue_.size();
        playing_ = true; // turn it back on for the next track
    }
}

void JellyfinSource::stop() {
    pause();
    current_idx_ = 0;
    
    if (ffmpeg_process_) {
        TerminateProcess(ffmpeg_process_, 0);
        CloseHandle(ffmpeg_process_);
        ffmpeg_process_ = nullptr;
    }
    if (ffmpeg_stdout_) {
        CloseHandle(ffmpeg_stdout_);
        ffmpeg_stdout_ = nullptr;
    }
}

TrackInfo JellyfinSource::current_track() const {
    TrackInfo info;
    if (!queue_.empty() && current_idx_ < queue_.size()) {
        const auto& t = queue_[current_idx_];
        info.title = t.title;
        info.artist = t.artist;
        info.album = t.album;
        info.duration_ms = t.duration_ms;
        
        // calculate the exact playtime offset
        uint64_t unplayed_bytes = current_ring_ ? current_ring_->readable() : 0;
        uint64_t actually_played = 0;
        
        // guard against underflow
        if (bytes_consumed_ > unplayed_bytes) {
            actually_played = bytes_consumed_ - unplayed_bytes;
        }

        // 48000Hz * 2 channels * 16-bit (2 bytes) = 192,000 bytes per second
        info.position_ms = actually_played / 192; 
    }
    return info;
}

void JellyfinSource::next() {
    if (queue_.empty()) return;
    shutdown();
    current_idx_ = (current_idx_ + 1) % queue_.size();
    playing_ = true;
}

void JellyfinSource::previous() {
    if (queue_.empty()) return;
    shutdown();
    current_idx_ = (current_idx_ == 0) ? queue_.size() - 1 : current_idx_ - 1;
    playing_ = true;
}

void JellyfinSource::cast(const std::string& playlist_id) {
    cfg_.default_playlist = playlist_id;
    shutdown();
    if (fetch_playlist()) {
        current_idx_ = 0;
        playing_ = true;
    }
}

PlaybackState JellyfinSource::playback_state() const noexcept {
    return playing_ ? PlaybackState::playing : PlaybackState::stopped;
}

AuthState JellyfinSource::auth_state() const noexcept {
    // handle auth via API key, so from the UI's perspective, no OAuth is needed
    return AuthState::none_required;
}

SourceCapabilities JellyfinSource::capabilities() const noexcept {
    SourceCapabilities cap;
    cap.seek = false;
    cap.previous = true;
    cap.queue = true;
    return cap;
}
} // namespace fh6