#include "ui/dashboard.h"
#include "ui/terminal.h"
#include <iostream>
#include <windows.h>
#include <sstream>
#include <iomanip>
#include <chrono>

using namespace std::chrono;

namespace UI {

static int get_terminal_width() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    int width = 80;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(hOut, &csbi)) {
        width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
    if (width < 40) width = 40;
    return width;
}

Dashboard::Dashboard(bool no_color, bool quiet) noexcept
    : no_color_(no_color), quiet_(quiet) {
}

Dashboard::~Dashboard() {
    stop();
}

void Dashboard::start() {
    if (quiet_) return;
    running_ = true;
    start_time_ = steady_clock::now();
    // hide cursor
    if (!no_color_ && Terminal::supportsANSI()) std::cout << "\x1b[?25l";
    thread_ = std::thread([this]{ thread_main(); });
}

void Dashboard::stop() {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    // show cursor
    if (!no_color_ && Terminal::supportsANSI()) std::cout << "\x1b[?25h";
}

void Dashboard::set_current_path(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(path_mtx_);
    current_path_ = path;
}

std::string Dashboard::human_readable(uint64_t v) const {
    std::ostringstream ss;
    if (v >= 1000000) {
        ss << std::fixed << std::setprecision(1) << (v / 1000000.0) << "M";
    } else if (v >= 1000) {
        ss << std::fixed << std::setprecision(1) << (v / 1000.0) << "K";
    } else {
        ss << v;
    }
    return ss.str();
}

std::string Dashboard::truncate_path(const std::wstring& p, int width) const {
    // convert to UTF-8 roughly
    int sz = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), (int)p.size(), NULL, 0, NULL, NULL);
    std::string s(sz, '\0');
    if (sz > 0) WideCharToMultiByte(CP_UTF8, 0, p.c_str(), (int)p.size(), s.data(), sz, NULL, NULL);
    if ((int)s.size() <= width) return s;
    // keep head and tail
    int keep = (width - 3) / 2;
    return s.substr(0, keep) + "..." + s.substr(s.size() - keep);
}

void Dashboard::thread_main() {
    constexpr int fps = 8;
    constexpr milliseconds frame_ms(1000 / fps);
    while (running_) {
        auto t0 = steady_clock::now();
        render_frame();
        auto t1 = steady_clock::now();
        auto dur = duration_cast<milliseconds>(t1 - t0);
        if (dur < frame_ms) std::this_thread::sleep_for(frame_ms - dur);
    }
    // final frame
    render_frame();
}

void Dashboard::render_frame() {
    int width = get_terminal_width();
    uint64_t f = files_seen.load();
    uint64_t d = dirs_seen.load();
    uint64_t e = errors.load();
    uint64_t written = entries_written.load();
    double elapsed = duration_cast<duration<double>>(steady_clock::now() - start_time_).count();
    double entries_per_sec = elapsed > 0.0 ? (double)written / elapsed : 0.0;

    std::ostringstream out;
    int inner = width - 2;
    if (inner < 20) inner = 20;
    
    out << "+" << std::string(inner, '-') << "+\n";
    // current path line
    std::wstring curp_w;
    {
        std::lock_guard<std::mutex> lk(path_mtx_);
        curp_w = current_path_;
    }
    std::string curp = truncate_path(curp_w, inner - 2);
    out << "| " << std::left << std::setw(inner - 2) << curp << " |\n";
    // empty line
    out << "|" << std::string(inner, ' ') << "|\n";
    // progress bar
    int barw = inner - 30;
    if (barw < 10) barw = 10;
    if (barw > inner - 10) barw = inner - 10;
    double ratio = entries_per_sec / 20000.0;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    int filled = (int)(ratio * barw);
    std::string bar;
    for (int i = 0; i < filled; ++i) bar += "#";
    for (int i = 0; i < barw - filled; ++i) bar += "-";
    int pad = inner - 4 - (int)bar.size() - 12;
    if (pad < 0) pad = 0;
    out << "|  " << bar << "  SCANNING " << std::string(pad, ' ') << "|\n";
    // empty
    out << "|" << std::string(inner, ' ') << "|\n";
    // counts
    std::string e_str = human_readable(written) + " entries";
    std::string f_str = human_readable(f) + " files";
    std::string d_str = human_readable(d) + " dirs";
    out << "|  " << e_str << "  " << f_str << "  " << d_str << std::string(inner - e_str.size() - f_str.size() - d_str.size() - 6, ' ') << "|\n";
    // breakdown
    std::string fs = human_readable(f);
    std::string ds = human_readable(d);
    std::string es = human_readable(e);
    out << "|  Files: " << std::left << std::setw(10) << fs << "  Dirs: " << std::left << std::setw(10) << ds << std::string(inner - 30, ' ') << "|\n";
    out << "|  Errors: " << std::left << std::setw(10) << es << std::string(inner - 20, ' ') << "|\n";
    out << "|" << std::string(inner, ' ') << "|\n";
    // rates
    std::string rate_str = human_readable((uint64_t)entries_per_sec) + " entries/sec";
    std::string time_str = std::to_string((int)elapsed) + "s elapsed";
    out << "|  " << rate_str << std::string(inner - rate_str.size() - time_str.size() - 4, ' ') << time_str << " |\n";
    out << "+" << std::string(inner, '-') << "+\n";

    std::cout << out.str();
    std::cout << std::flush;
}

} // namespace UI
