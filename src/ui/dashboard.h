#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <mutex>

namespace UI {

class Dashboard {
public:
    Dashboard(bool no_color = false, bool quiet = false) noexcept;
    ~Dashboard();

    void start();
    void stop();

    void set_current_path(const std::wstring& path);

    // Counters that Index::build_index will update
    std::atomic<uint64_t> files_seen{0};
    std::atomic<uint64_t> dirs_seen{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> entries_written{0};

private:
    void thread_main();
    void render_frame();
    std::string human_readable(uint64_t v) const;
    std::string truncate_path(const std::wstring& p, int width) const;

    bool no_color_;
    bool quiet_;
    bool running_ = false;
    std::thread thread_;
    std::mutex path_mtx_;
    std::wstring current_path_;
    std::chrono::steady_clock::time_point start_time_;
    double last_entries_written_ = 0.0;
};

} // namespace UI
