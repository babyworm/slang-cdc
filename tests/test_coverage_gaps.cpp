#include <catch2/catch_test_macros.hpp>
#include "test_helpers.h"
#include "slang-cdc/types.h"
#include "slang-cdc/sdc_parser.h"
#include "slang-cdc/clock_tree.h"
#include "slang-cdc/ff_classifier.h"
#include "slang-cdc/connectivity.h"
#include "slang-cdc/crossing_detector.h"
#include "slang-cdc/sync_verifier.h"

#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;
using namespace slang_cdc;

static fs::path writeTempSdc(const std::string& content) {
    static int counter = 0;
    auto path = fs::temp_directory_path() / ("test_gap_" + std::to_string(counter++) + ".sdc");
    std::ofstream(path) << content;
    return path;
}

// ─── GAP-3: SDC parser non-existent file ───

TEST_CASE("GAP: SDC parser non-existent file returns empty", "[gap]") {
    auto sdc = SdcParser::parse("/nonexistent/path/does_not_exist.sdc");
    CHECK(sdc.clocks.empty());
    CHECK(sdc.generated_clocks.empty());
    CHECK(sdc.clock_groups.empty());
}

// ─── GAP-5: domainForSignal ───

TEST_CASE("GAP: ClockDatabase domainForSignal valid and invalid", "[gap]") {
    ClockDatabase db;
    auto src = std::make_unique<ClockSource>();
    src->name = "clk";
    auto* s = db.addSource(std::move(src));
    auto* dom = db.findOrCreateDomain(s, Edge::Posedge);

    auto net = std::make_unique<ClockNet>();
    net->hier_path = "top.clk";
    net->source = s;
    net->edge = Edge::Posedge;
    db.addNet(std::move(net));

    CHECK(db.domainForSignal("top.clk") == dom);
    CHECK(db.domainForSignal("nonexistent") == nullptr);
}

// ─── GAP-6: isAsynchronous with nullptr ───

TEST_CASE("GAP: isAsynchronous with nullptr domains", "[gap]") {
    ClockDatabase db;
    CHECK(db.isAsynchronous(nullptr, nullptr) == true);

    auto src = std::make_unique<ClockSource>();
    src->name = "clk";
    auto* s = db.addSource(std::move(src));
    auto* dom = db.findOrCreateDomain(s, Edge::Posedge);
    CHECK(db.isAsynchronous(dom, nullptr) == true);
    CHECK(db.isAsynchronous(nullptr, dom) == true);
    CHECK(db.isAsynchronous(dom, dom) == false);
}

// ─── GAP-7: null domain edge skipped ───

TEST_CASE("GAP: CrossingDetector skips edges with null domain", "[gap]") {
    FFNode ff_a{"top.a", nullptr, nullptr, {}};
    FFNode ff_b{"top.b", nullptr, nullptr, {}};
    std::vector<FFEdge> edges;
    edges.push_back({&ff_a, &ff_b, {}, SyncType::None, false});
    ClockDatabase db;
    CrossingDetector det(edges, db);
    det.analyze();
    CHECK(det.getCrossings().empty());
}

// ─── GAP-8: 3-FF sync end-to-end ───

TEST_CASE("GAP: 3-FF sync detected end-to-end", "[gap]") {
    auto compilation = test::compileSV(R"(
        module three_ff_e2e (input logic clk_a, clk_b, rst_n, d);
            logic q_a, s1, s2, s3;
            always_ff @(posedge clk_a or negedge rst_n)
                if (!rst_n) q_a <= 0; else q_a <= d;
            always_ff @(posedge clk_b or negedge rst_n)
                if (!rst_n) begin s1 <= 0; s2 <= 0; s3 <= 0; end
                else begin s1 <= q_a; s2 <= s1; s3 <= s2; end
        endmodule
    )", "gap");

    ClockDatabase db;
    ClockTreeAnalyzer ct(*compilation, db); ct.analyze();
    FFClassifier ff(*compilation, db); ff.analyze();
    ConnectivityBuilder conn(*compilation, ff.getFFNodes()); conn.analyze();
    CrossingDetector det(conn.getEdges(), db); det.analyze();
    auto crossings = det.getCrossings();
    SyncVerifier sv(crossings, ff.getFFNodes(), conn.getEdges()); sv.analyze();

    bool found_3ff = false;
    for (auto& c : crossings)
        if (c.sync_type == SyncType::ThreeFF) found_3ff = true;
    CHECK(found_3ff);
}

// ─── GAP-13: SDC -invert ───

TEST_CASE("GAP: SDC generated clock with -invert", "[gap]") {
    auto path = writeTempSdc(
        "create_generated_clock -name inv_clk -source [get_ports sys_clk] "
        "-invert [get_pins u_inv/Q]\n");
    auto sdc = SdcParser::parse(path);
    REQUIRE(sdc.generated_clocks.size() == 1);
    CHECK(sdc.generated_clocks[0].invert == true);
    CHECK(sdc.generated_clocks[0].name == "inv_clk");
}

// ─── GAP-14: SDC -multiply_by ───

TEST_CASE("GAP: SDC generated clock with -multiply_by", "[gap]") {
    auto path = writeTempSdc(
        "create_generated_clock -name fast_clk -source [get_ports sys_clk] "
        "-multiply_by 4 [get_pins u_pll/Q]\n");
    auto sdc = SdcParser::parse(path);
    REQUIRE(sdc.generated_clocks.size() == 1);
    CHECK(sdc.generated_clocks[0].multiply_by == 4);
}

// ─── GAP-25: Circular assign depth guard ───

TEST_CASE("GAP: circular assign does not hang", "[gap]") {
    auto compilation = test::compileSV(R"(
        module circ_assign (input logic clk, rst_n);
            logic a, b;
            assign a = b;
            assign b = a;
            always_ff @(posedge clk or negedge rst_n)
                if (!rst_n) a <= 0;
        endmodule
    )", "gap");

    ClockDatabase db;
    ClockTreeAnalyzer ct(*compilation, db); ct.analyze();
    FFClassifier ff(*compilation, db); ff.analyze();
    ConnectivityBuilder conn(*compilation, ff.getFFNodes()); conn.analyze();
    CHECK(true); // Must reach here without hanging
}

// ─── GAP: AnalysisResult counts ───

TEST_CASE("GAP: AnalysisResult counts are zero for empty result", "[gap]") {
    AnalysisResult result;
    CHECK(result.violation_count() == 0);
    CHECK(result.caution_count() == 0);
    CHECK(result.info_count() == 0);
    CHECK(result.waived_count() == 0);
    CHECK(result.convention_count() == 0);
}
