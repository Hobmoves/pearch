#include "check.h"
#include "core/fileio.h"
#include "core/format.h"
#include "engine/query.h"

using namespace engine;

static Query P(const char* s) { return parse_query(s, fmt::now_ms()); }

TEST(query_words_and_phrases) {
    Query q = P("q3 Report");
    CHECK_EQ(q.terms.size(), size_t{2});
    CHECK_EQ(q.terms[0].text, std::string("q3"));
    CHECK_EQ(q.terms[1].text, std::string("report"));
    Query ph = P("\"q3 report\" draft");
    CHECK_EQ(ph.terms.size(), size_t{2});
    CHECK_EQ(ph.terms[0].text, std::string("q3 report"));
    CHECK(P("   ").empty());
}

TEST(query_filters) {
    Query q = P("*.pdf");
    CHECK(q.terms.empty());
    CHECK_EQ(q.exts.size(), size_t{1});
    CHECK_EQ(q.exts[0], std::string("pdf"));
    Query e = P("ext:pdf,.DOCX report");
    CHECK_EQ(e.exts.size(), size_t{2});
    CHECK_EQ(e.exts[1], std::string("docx"));
    CHECK_EQ(e.terms.size(), size_t{1});
    CHECK(P("folder:").type == TypeFilter::Folders);
    Query f = P("file:report");
    CHECK(f.type == TypeFilter::Files);
    CHECK_EQ(f.terms.size(), size_t{1});
    CHECK(P("app: chrome").type == TypeFilter::Apps);
    Query s = P("size:>10mb");
    CHECK(s.size_min.has_value());
    CHECK_EQ(*s.size_min, uint64_t{10} * 1024 * 1024);
    CHECK(P("size:<1kb").size_max.value_or(0) == 1024);
    Query m = P("modified:week");
    CHECK(m.modified_after.has_value());
    CHECK(*m.modified_after < fmt::now_ms() - 6LL * 86400000);
    CHECK(P("modified:3d").modified_after.has_value());
}

TEST(query_special_terms) {
    Query w = P("rep?rt*");
    CHECK(w.terms.size() == 1 && w.terms[0].kind == Term::Wildcard);
    Query n = P("-draft");
    CHECK(n.terms.size() == 1 && n.terms[0].negate && n.terms[0].text == "draft");
    CHECK(P("-").terms[0].kind == Term::Substr);  // a lone dash is just a dash
    Query in = P("in:Work");
    CHECK(in.terms.size() == 1 && in.terms[0].kind == Term::InFolder && in.terms[0].text == "work");
    Query p = P("work/q3");
    CHECK(p.terms.size() == 1 && p.terms[0].kind == Term::Path);
    CHECK_EQ(p.terms[0].text, std::string("work") + fileio::kPathSep + "q3");
    CHECK(P("C:\\Users").terms[0].kind == Term::Path);
    CHECK(P("\"*.pdf\"").terms[0].kind == Term::Substr);  // quoted: literal
}

TEST(query_refinement_detection) {
    CHECK(P("repo").refines(P("rep")));
    CHECK(!P("rep").refines(P("repo")));
    CHECK(P("a bc").refines(P("a b")));
    CHECK(!P("a b").refines(P("a")));
    CHECK(P("*.pdf repo").refines(P("*.pdf rep")));
    CHECK(!P("*.doc repo").refines(P("*.pdf rep")));
    CHECK(P("xrepx").refines(P("rep")));
    CHECK(!P("-a rep").refines(P("rep")));
}

TEST(parse_size_units) {
    uint64_t v = 0;
    CHECK(parse_size("512", v) && v == 512);
    CHECK(parse_size("1.5 KB", v) && v == 1536);
    CHECK(parse_size("2gb", v) && v == 2ull * 1024 * 1024 * 1024);
    CHECK(!parse_size("lots", v));
}
