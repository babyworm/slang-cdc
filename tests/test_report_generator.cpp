#include <catch2/catch_test_macros.hpp>
#include "slang-cdc/types.h"
#include "slang-cdc/report_generator.h"

#include <fstream>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace slang_cdc;

static AnalysisResult makeTestResult() {
    AnalysisResult result;

    // Create sources and domains
    auto src_sys = std::make_unique<ClockSource>();
    src_sys->name = "sys_clk";
    src_sys->type = ClockSource::Type::Primary;
    auto* sys_ptr = result.clock_db.addSource(std::move(src_sys));
    auto* dom_sys = result.clock_db.findOrCreateDomain(sys_ptr, Edge::Posedge);

    auto src_ext = std::make_unique<ClockSource>();
    src_ext->name = "ext_clk";
    src_ext->type = ClockSource::Type::AutoDetected;
    auto* ext_ptr = result.clock_db.addSource(std::move(src_ext));
    auto* dom_ext = result.clock_db.findOrCreateDomain(ext_ptr, Edge::Posedge);

    result.clock_db.relationships.push_back(
        {sys_ptr, ext_ptr, DomainRelationship::Type::Asynchronous});

    // Create crossings
    CrossingReport v1;
    v1.id = "VIOLATION-001";
    v1.category = ViolationCategory::Violation;
    v1.severity = Severity::High;
    v1.source_signal = "top.u_ctrl.q_frame_start";
    v1.dest_signal = "top.u_display.q_frame_start";
    v1.source_domain = dom_sys;
    v1.dest_domain = dom_ext;
    v1.sync_type = SyncType::None;
    v1.recommendation = "Insert 2-FF synchronizer";
    result.crossings.push_back(v1);

    CrossingReport i1;
    i1.id = "INFO-001";
    i1.category = ViolationCategory::Info;
    i1.severity = Severity::Info;
    i1.source_signal = "top.u_a.q_data";
    i1.dest_signal = "top.u_b.sync_ff2";
    i1.source_domain = dom_sys;
    i1.dest_domain = dom_ext;
    i1.sync_type = SyncType::TwoFF;
    result.crossings.push_back(i1);

    return result;
}

TEST_CASE("ReportGenerator: violation and info counts", "[report]") {
    auto result = makeTestResult();
    CHECK(result.violation_count() == 1);
    CHECK(result.caution_count() == 0);
    CHECK(result.info_count() == 1);
}

TEST_CASE("ReportGenerator: markdown output contains key sections", "[report]") {
    auto result = makeTestResult();
    ReportGenerator gen(result);

    auto path = fs::temp_directory_path() / "test_report.md";
    gen.generateMarkdown(path);

    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    fs::remove(path);

    CHECK(content.find("# CDC Analysis Report") != std::string::npos);
    CHECK(content.find("VIOLATION") != std::string::npos);
    CHECK(content.find("INFO") != std::string::npos);
    CHECK(content.find("sys_clk") != std::string::npos);
    CHECK(content.find("ext_clk") != std::string::npos);
    CHECK(content.find("VIOLATION-001") != std::string::npos);
    CHECK(content.find("Insert 2-FF synchronizer") != std::string::npos);
}

TEST_CASE("ReportGenerator: JSON output is valid structure", "[report]") {
    auto result = makeTestResult();
    ReportGenerator gen(result);

    auto path = fs::temp_directory_path() / "test_report.json";
    gen.generateJSON(path);

    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    fs::remove(path);

    // Basic JSON structure checks
    CHECK(content.find("\"summary\"") != std::string::npos);
    CHECK(content.find("\"violations\": 1") != std::string::npos);
    CHECK(content.find("\"domains\"") != std::string::npos);
    CHECK(content.find("\"crossings\"") != std::string::npos);
    CHECK(content.find("VIOLATION-001") != std::string::npos);
    CHECK(content.find("sys_clk") != std::string::npos);
}

TEST_CASE("ReportGenerator: empty result produces valid output", "[report]") {
    AnalysisResult result;
    ReportGenerator gen(result);

    auto md_path = fs::temp_directory_path() / "test_empty.md";
    auto json_path = fs::temp_directory_path() / "test_empty.json";
    gen.generateMarkdown(md_path);
    gen.generateJSON(json_path);

    std::ifstream md(md_path);
    std::string md_content((std::istreambuf_iterator<char>(md)),
                            std::istreambuf_iterator<char>());

    std::ifstream json(json_path);
    std::string json_content((std::istreambuf_iterator<char>(json)),
                              std::istreambuf_iterator<char>());

    fs::remove(md_path);
    fs::remove(json_path);

    CHECK(md_content.find("VIOLATION | 0") != std::string::npos);
    CHECK(json_content.find("\"violations\": 0") != std::string::npos);
}
