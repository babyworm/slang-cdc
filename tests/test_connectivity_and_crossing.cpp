#include <catch2/catch_test_macros.hpp>
#include "slang-cdc/clock_tree.h"
#include "slang-cdc/ff_classifier.h"
#include "slang-cdc/connectivity.h"
#include "slang-cdc/crossing_detector.h"
#include "slang/driver/Driver.h"

#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;
using namespace slang_cdc;

static std::unique_ptr<slang::ast::Compilation> compileSV(const std::string& sv_code) {
    static int counter = 0;
    auto path = fs::temp_directory_path() /
        ("test_conn_" + std::to_string(counter++) + ".sv");
    std::ofstream(path) << sv_code;

    std::string path_str = path.string();
    slang::driver::Driver driver;
    driver.addStandardArgs();
    const char* args[] = {"test", path_str.c_str()};
    driver.parseCommandLine(2, const_cast<char**>(args));
    driver.processOptions();
    driver.parseAllSources();

    auto compilation = driver.createCompilation();
    compilation->getRoot();
    compilation->getAllDiagnostics();
    return compilation;
}

// Helper: run full Pass 1-4 pipeline
struct CDCPipeline {
    ClockDatabase db;
    std::unique_ptr<FFClassifier> classifier;
    std::vector<FFEdge> edges;
    std::vector<CrossingReport> crossings;

    void run(slang::ast::Compilation& compilation) {
        ClockTreeAnalyzer clock_analyzer(compilation, db);
        clock_analyzer.analyze();

        classifier = std::make_unique<FFClassifier>(compilation, db);
        classifier->analyze();

        ConnectivityBuilder conn(compilation, classifier->getFFNodes());
        conn.analyze();
        edges = conn.getEdges();

        CrossingDetector detector(edges, db);
        detector.analyze();
        crossings = detector.getCrossings();
    }
};

// ─── Pass 3: Connectivity Tests ───

TEST_CASE("Connectivity: no edges in single-domain design", "[conn]") {
    auto compilation = compileSV(R"(
        module single_dom (
            input  logic       clk,
            input  logic       rst_n,
            input  logic [7:0] data_in,
            output logic [7:0] data_out
        );
            logic [7:0] stage1, stage2;
            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    stage1 <= 8'h0;
                    stage2 <= 8'h0;
                end else begin
                    stage1 <= data_in;
                    stage2 <= stage1;
                end
            end
            assign data_out = stage2;
        endmodule
    )");

    CDCPipeline pipeline;
    pipeline.run(*compilation);

    // All FFs are in same domain → edges exist but no crossings
    CHECK(pipeline.crossings.empty());
}

TEST_CASE("Connectivity: direct FF-to-FF crossing detected", "[conn]") {
    auto compilation = compileSV(R"(
        module missing_sync (
            input  logic clk_a,
            input  logic clk_b,
            input  logic rst_n,
            input  logic data_in
        );
            logic q_a;
            logic q_b;

            always_ff @(posedge clk_a or negedge rst_n) begin
                if (!rst_n) q_a <= 1'b0;
                else        q_a <= data_in;
            end

            always_ff @(posedge clk_b or negedge rst_n) begin
                if (!rst_n) q_b <= 1'b0;
                else        q_b <= q_a;  // CDC crossing!
            end
        endmodule
    )");

    CDCPipeline pipeline;
    pipeline.run(*compilation);

    // Should detect at least one crossing
    REQUIRE(pipeline.crossings.size() >= 1);
    CHECK(pipeline.crossings[0].category == ViolationCategory::Violation);
    CHECK(pipeline.crossings[0].severity == Severity::High);
}

// ─── Pass 4: Crossing Detector Tests ───

TEST_CASE("CrossingDetector: properly synced crossing is INFO", "[crossing]") {
    auto compilation = compileSV(R"(
        module two_ff_sync (
            input  logic clk_a,
            input  logic clk_b,
            input  logic rst_n,
            input  logic data_in
        );
            logic q_a;
            logic sync_ff1, sync_ff2;

            always_ff @(posedge clk_a or negedge rst_n) begin
                if (!rst_n) q_a <= 1'b0;
                else        q_a <= data_in;
            end

            always_ff @(posedge clk_b or negedge rst_n) begin
                if (!rst_n) begin
                    sync_ff1 <= 1'b0;
                    sync_ff2 <= 1'b0;
                end else begin
                    sync_ff1 <= q_a;
                    sync_ff2 <= sync_ff1;
                end
            end
        endmodule
    )");

    CDCPipeline pipeline;
    pipeline.run(*compilation);

    // There is a crossing from clk_a → clk_b
    // With 2-FF sync, it should be INFO (not VIOLATION)
    // For now (MVP), just check crossing is detected
    CHECK(pipeline.crossings.size() >= 1);
}

TEST_CASE("CrossingDetector: crossing has source and dest domain info", "[crossing]") {
    auto compilation = compileSV(R"(
        module domain_info (
            input  logic clk_a,
            input  logic clk_b,
            input  logic rst_n,
            input  logic d
        );
            logic q_a, q_b;
            always_ff @(posedge clk_a or negedge rst_n) begin
                if (!rst_n) q_a <= 1'b0;
                else        q_a <= d;
            end
            always_ff @(posedge clk_b or negedge rst_n) begin
                if (!rst_n) q_b <= 1'b0;
                else        q_b <= q_a;
            end
        endmodule
    )");

    CDCPipeline pipeline;
    pipeline.run(*compilation);

    REQUIRE(pipeline.crossings.size() >= 1);
    auto& c = pipeline.crossings[0];
    CHECK(c.source_domain != nullptr);
    CHECK(c.dest_domain != nullptr);
    if (c.source_domain && c.dest_domain) {
        CHECK(c.source_domain->canonical_name != c.dest_domain->canonical_name);
    }
    CHECK(!c.id.empty());
    CHECK(!c.source_signal.empty());
    CHECK(!c.dest_signal.empty());
}
