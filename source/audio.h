#pragma once
#include "core.h"
#include <switch.h>
#include <string>
#include <atomic>

class Player {
public:
    bool init();
    void shutdown();

    // Requests loading + playing a file. Non-blocking; the audio thread does the work.
    void playFile(const std::string& path);
    void stop();

    void togglePause();
    void setPaused(bool paused);
    void seek(double seconds);
    void setVolume(float v);
    float volume() const { return m_volumeTarget.load(); }
    void seekRelative(double delta) { seek(position() + delta); }

    bool loaded() const { return m_loaded.load(); }
    bool playing() const { return m_playing.load(); }
    double position() const { return m_position.load(); }
    double duration() const { return m_duration.load(); }
    std::string errorText();

    // Consumes the end-of-track flag.
    bool consumeEnded();

    std::string currentPath();

    void setNextCallback(void (*cb)(void*), void* user) { m_nextCb = cb; m_nextUser = user; }

private:
    static void threadEntry(void* arg);
    void run();

    void requestNext();

    Thread m_thread{};
    bool m_threadStarted = false;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_loaded{false};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_ended{false};
    std::atomic<double> m_position{0};
    std::atomic<double> m_duration{0};
    std::atomic<double> m_seekRequest{-1};
    std::atomic<float> m_volume{1.0f};
    std::atomic<float> m_volumeTarget{1.0f};
    std::atomic<int> m_command{0}; // 1 = load path, 2 = stop
    std::atomic<u64> m_lastToggleTick{0};

    SpinLock m_mutex;
    std::string m_pendingPath;
    std::string m_currentPath;
    std::string m_error;

    void (*m_nextCb)(void*) = nullptr;
    void* m_nextUser = nullptr;
};
