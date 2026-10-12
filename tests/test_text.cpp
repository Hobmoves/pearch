#include "check.h"
#include "core/format.h"
#include "core/text.h"

TEST(fold_lowercases_and_strips_accents) {
    CHECK_EQ(text::fold("Café_ÜNÏCODE.TXT"), std::string("cafe_unicode.txt"));
    CHECK_EQ(text::fold("Straße"), std::string("strasse"));
    CHECK_EQ(text::fold("ÆON Œuvre"), std::string("aeon oeuvre"));
    CHECK_EQ(text::fold("ПРИВЕТ"), std::string("привет"));
    CHECK_EQ(text::fold("Ελλάδα"), std::string("ελλαδα"));
    CHECK_EQ(text::fold("e\xCC\x81"), std::string("e"));  // e + combining acute
    CHECK_EQ(text::fold("ＡＢＣ１"), std::string("abc1"));
}

TEST(fold_origin_maps_back_to_code_points) {
    std::string folded;
    auto origin = text::fold_origin("Éß!", folded);
    CHECK_EQ(folded, std::string("ess!"));
    CHECK_EQ(origin.size(), size_t{4});
    CHECK_EQ(origin[0], 0u);
    CHECK_EQ(origin[1], 1u);
    CHECK_EQ(origin[2], 1u);
    CHECK_EQ(origin[3], 2u);
}

TEST(utf8_wide_round_trip) {
    std::string s = "pearch 🍐 日本 Ünïcødé";
    CHECK_EQ(text::to_utf8(text::to_wide(s)), s);
    std::string bad = "ok\xFF\xFE";
    CHECK(!text::to_utf8(text::to_wide(bad)).empty());
}

TEST(display_width_handles_wide_and_combining) {
    CHECK_EQ(text::display_width("abc"), 3);
    CHECK_EQ(text::display_width("日本"), 4);
    CHECK_EQ(text::display_width("e\xCC\x81"), 1);
    CHECK_EQ(text::display_width("🍐"), 2);
}

TEST(truncation_respects_width) {
    std::string long_name = "a very long file name that will not fit.txt";
    CHECK(text::display_width(text::truncate_end(long_name, 10)) <= 10);
    CHECK(text::display_width(text::truncate_middle(long_name, 11)) <= 11);
    CHECK_EQ(text::truncate_end("short", 10), std::string("short"));
    std::string wide = "日本語のファイル名.txt";
    CHECK(text::display_width(text::truncate_end(wide, 7)) <= 7);
    std::string path = "C:\\Users\\me\\Documents\\Work\\2026\\Q3";
    std::string t = text::truncate_path(path, 22, '\\');
    CHECK(text::display_width(t) <= 22);
    CHECK(t.find("Q3") != std::string::npos);
    CHECK(t.rfind("C:", 0) == 0);
}

TEST(char_mask_is_a_superset_filter) {
    uint64_t name = text::char_mask("quarterly report.pdf");
    uint64_t q = text::char_mask("rprt");
    CHECK((name & q) == q);
    uint64_t z = text::char_mask("z");
    CHECK((name & z) != z);
}

TEST(formatting) {
    CHECK_EQ(fmt::thousands(0), std::string("0"));
    CHECK_EQ(fmt::thousands(1284), std::string("1,284"));
    CHECK_EQ(fmt::thousands(412331), std::string("412,331"));
    CHECK_EQ(fmt::thousands(1000000), std::string("1,000,000"));
    CHECK_EQ(fmt::size(12), std::string("12 B"));
    CHECK_EQ(fmt::size(2200000), std::string("2.1 MB"));
    CHECK_EQ(fmt::compact(412331), std::string("412K"));
    int64_t now = fmt::now_ms();
    CHECK_EQ(fmt::relative_time(now - 5000, now), std::string("just now"));
    CHECK_EQ(fmt::relative_time(now - 12 * 60000, now), std::string("12 min ago"));
    CHECK_EQ(fmt::relative_time(now - 3 * 3600000LL, now), std::string("3 hr ago"));
}
