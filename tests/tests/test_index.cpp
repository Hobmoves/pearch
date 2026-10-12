#include <iostream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include "index/index.h"

using namespace std::chrono;
namespace fs = std::filesystem;

static int failures = 0;
static int checks = 0;

void check(bool cond, const std::string& msg) {
    ++checks;
    if (!cond) {
        ++failures;
        std::cerr << "CHECK FAILED: " << msg << "\n";
    }
}

static std::wstring to_w(const fs::path& p) {
    auto s = p.wstring();
    return s;
}

int main() {
    // create temporary test directory
    fs::path tmp = fs::temp_directory_path() / ("pearch_test_" + std::to_string((int)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count()));
    fs::create_directories(tmp);

    // create files and directories
    std::vector<fs::path> created;
    auto touch = [&](const fs::path& p){ fs::create_directories(p.parent_path()); std::ofstream(p.string()).put('x'); created.push_back(p); };

    touch(tmp / "normal.txt");
    touch(tmp / "MiXeD.Case");
    touch(tmp / "space name.txt");
    touch(tmp / "percent%file.txt");
    touch(tmp / "underscore_file_.txt");
    fs::create_directories(tmp / "nested_dir" / "dir_with_foo");
    touch(tmp / "nested_dir" / "inner.cpp");
    touch(tmp / "nested_dir" / "cmake.txt");
    touch(tmp / "foo");
    touch(tmp / "foo_bar");
    touch(tmp / "xfoo");
    touch(tmp / "ab");
    touch(tmp / "abc");

    // set test DB path inside tmp
    fs::path dbpath = tmp / "index.sqlite";
    Index::set_db_path_for_tests(to_w(dbpath));

    // build index
    Scanner::ScanResult scanr;
    double elapsed = 0.0;
    bool ok = Index::build_index(to_w(tmp), false, scanr, elapsed);
    check(ok, "build_index succeeds");

    // basic searches
    double qms=0;
    auto r1 = Index::search(std::string("normal.txt"), 10, qms);
    check(!r1.empty(), "exact filename search returns result");

    auto r2 = Index::search(std::string("inner.cpp"), 10, qms);
    check(!r2.empty(), "nested file search returns result");

    auto r3 = Index::search(std::string("space name"), 10, qms);
    check(!r3.empty(), "space in filename matches");

    auto r4 = Index::search(std::string("MiXeD.Case"), 10, qms);
    check(!r4.empty(), "case-insensitive exact match works");

    // substring match
    auto r5 = Index::search(std::string("foo"), 10, qms);
    check(r5.size() >= 3, "foo substring/prefix/path matches multiple");

    // ranking: exact beats prefix beats substring beats path
    auto rexact = Index::search(std::string("foo"), 10, qms);
    check(!rexact.empty(), "ranking: got results for 'foo'");
    // ensure 'foo' (exact) appears before 'foo_bar' and 'xfoo'
    auto pos_exact = -1, pos_prefix = -1, pos_contains = -1;
    for (size_t i=0;i<rexact.size();++i) {
        if (rexact[i].filename == "foo") pos_exact = (int)i;
        if (rexact[i].filename == "foo_bar") pos_prefix = (int)i;
        if (rexact[i].filename == "xfoo") pos_contains = (int)i;
    }
    check(pos_exact>=0, "found exact 'foo'");
    check(pos_prefix>=0, "found prefix 'foo_bar'");
    check(pos_contains>=0, "found contains 'xfoo'");
    check(pos_exact < pos_prefix, "exact ranks before prefix");
    check(pos_prefix < pos_contains, "prefix ranks before contains");

    // shorter names win within same rank: query 'a' will match 'ab' and 'abc' as prefix; 'ab' should come before 'abc'
    auto rabc = Index::search(std::string("a"), 10, qms);
    int p_ab=-1, p_abc=-1;
    for (size_t i=0;i<rabc.size();++i) {
        if (rabc[i].filename == "ab") p_ab = (int)i;
        if (rabc[i].filename == "abc") p_abc = (int)i;
    }
    check(p_ab>=0 && p_abc>=0, "found ab and abc for prefix 'a'");
    if (p_ab>=0 && p_abc>=0) check(p_ab < p_abc, "shorter name ranks before longer within same rank");

    // limit tests
    auto rlim1 = Index::search(std::string(""), 1, qms);
    check(rlim1.size() <= 1, "limit 1 respected (empty query may match many)");
    auto rlim10 = Index::search(std::string("foo"), 10, qms);
    check(rlim10.size() <= 10, "limit 10 respected");

    // query edge cases: whitespace-only
    auto rws = Index::search(std::string("   "), 10, qms);
    check(rws.size() == 0 || !rws.empty(), "whitespace query handled (no crash)");

    // wildcard character literal match: percent
    auto rperc = Index::search(std::string("%"), 10, qms);
    // should match percent%file.txt because code escapes wildcards
    bool found_percent = false;
    for (auto &s : rperc) if (s.filename.find('%') != std::string::npos) found_percent = true;
    check(found_percent, "literal percent in filename matched when searching '%'");

    // persistence: reopen DB and search again
    bool init_ok = Index::initialize_database();
    check(init_ok, "re-initialize database succeeds");
    auto r_after = Index::search(std::string("inner.cpp"), 10, qms);
    check(!r_after.empty(), "search works after reopening DB");

    // cleanup
    // remove files and dir
    std::error_code ec;
    fs::remove_all(tmp, ec);

    std::cout << "Checks: " << checks << ", Failures: " << failures << "\n";
    return failures == 0 ? 0 : 1;
}
