#include <catch2/catch_test_macros.hpp>
#include "slang-cdc/clock_tree.h"
#include "slang-cdc/ff_classifier.h"
#include "slang-cdc/connectivity.h"
#include "slang-cdc/crossing_detector.h"
#include "slang-cdc/sync_verifier.h"
#include "slang/driver/Driver.h"

#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;
using namespace slang_cdc;

static std::unique_ptr<slang::ast::Compilation> compileSV(const std::string& sv_code) {
    static int counter = 0;
    auto path = fs::temp_directory_path() /
        ("test_sync_" + std::to_string(counter++) + ".sv");
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

struct FullPipeline {
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

        SyncVerifier verifier(crossings, classifier->getFFNodes(), edges);
        verifier.analyze();
    }
};

TEST_CASE("SyncVerifier: unsynchronized crossing stays VIOLATION", "[sync]") {
    auto compilation = compileSV(R"(
        module no_sync (
            input  logic clk_a, clk_b, rst_n, d
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

    FullPipeline pipeline;
    pipeline.run(*compilation);

    REQUIRE(pipeline.crossings.size() >= 1);
    // No synchronizer → should remain VIOLATION
    bool found_violation = false;
    for (auto& c : pipeline.crossings) {
        if (c.category == ViolationCategory::Violation)
            found_violation = true;
    }
    CHECK(found_violation);
    CHECK(pipeline.crossings[0].sync_type == SyncType::None);
}

TEST_CASE("SyncVerifier: 2-FF sync detected as TwoFF", "[sync]") {
    auto compilation = compileSV(R"(
        module with_sync (
            input  logic clk_a, clk_b, rst_n, d
        );
            logic q_a;
            logic sync1, sync2;

            always_ff @(posedge clk_a or negedge rst_n) begin
                if (!rst_n) q_a <= 1'b0;
                else        q_a <= d;
            end

            always_ff @(posedge clk_b or negedge rst_n) begin
                if (!rst_n) begin
                    sync1 <= 1'b0;
                    sync2 <= 1'b0;
                end else begin
                    sync1 <= q_a;
                    sync2 <= sync1;
                end
            end
        endmodule
    )");

    FullPipeline pipeline;
    pipeline.run(*compilation);

    // Should detect crossing with 2-FF synchronizer
    bool found_synced = false;
    for (auto& c : pipeline.crossings) {
        if (c.sync_type == SyncType::TwoFF) {
            found_synced = true;
            CHECK(c.category == ViolationCategory::Info);
        }
    }
    CHECK(found_synced);
}
