#include <random>
#include <string>

#include "check.h"
#include "engine/matcher.h"

using namespace engine;

TEST(simd_find_matches_reference) {
    // Exercises whichever SIMD path this CPU uses (CI also runs with PEARCH_NO_AVX2=1).
    std::mt19937 rng(1234);
    const char alphabet[] = "abcab.-_ \n";
    for (int iter = 0; iter < 20000; ++iter) {
        size_t n = rng() % 300, m = 1 + rng() % 12;
        std::string hay(n, 'a'), needle(m, 'a');
        for (auto& c : hay) c = alphabet[rng() % 10];
        for (auto& c : needle) c = alphabet[rng() % 5];
        if (n > m && rng() % 2) hay.replace(rng() % (n - m), m, needle);  // plant a match
        size_t want = std::string_view(hay).find(needle);
        size_t got = match::find(hay, needle);
        if (want != got) {
            CHECK_EQ(got, want);
            return;
        }
    }
}

TEST(glob_and_fuzzy) {
    CHECK(match::glob("report.pdf", "*.pdf"));
    CHECK(match::glob("report.pdf", "rep?rt*"));
    CHECK(!match::glob("report.pdf", "*.doc"));
    CHECK(match::glob("café.txt", "caf?.txt"));
    CHECK(match::glob("abc", "***"));
    CHECK_EQ(std::string(match::longest_literal("*re?port*.pdf")), std::string("port"));
    CHECK(match::fuzzy("quarterly report", "qrtly") >= 0);
    CHECK(match::fuzzy("report", "rpx") < 0);
    CHECK(match::fuzzy("my_report", "rep") > match::fuzzy("xrxexp", "rep"));
    CHECK_EQ(match::name_tier("foo", "foo", "foo").score, 1000);
    CHECK_EQ(match::name_tier("foo.txt", "foo.txt", "foo").score, 920);
    CHECK_EQ(match::name_tier("foobar", "foobar", "foo").score, 800);
    CHECK_EQ(match::name_tier("my_foo", "my_foo", "foo").score, 650);
    CHECK_EQ(match::name_tier("myfoo", "myFoo", "foo").score, 650);  // camelCase
    CHECK_EQ(match::name_tier("myfoo", "myfoo", "foo").score, 500);
}

TEST(fuzzy_rejects_scattered_letters) {
    CHECK(match::fuzzy("quarterly-report-template.docx", "qrtly") >= 0);
    CHECK(match::fuzzy("report", "rprt") >= 0);
    CHECK(match::fuzzy("iso_schematron_message.xsl", "chrome") < 0);
    CHECK(match::fuzzy("stock_shared-to-me.icon", "chrome") < 0);
    CHECK(match::fuzzy("invalidrequireexplicitpolicytest3ee.crt", "report") < 0);
}
