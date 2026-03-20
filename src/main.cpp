#include <iostream>
#include <filesystem>
#include <string>
#include <vector>

#include "slang/driver/Driver.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"

#include "slang-cdc/types.h"
#include "slang-cdc/clock_tree.h"
#include "slang-cdc/ff_classifier.h"
#include "slang-cdc/connectivity.h"
#include "slang-cdc/crossing_detector.h"
#include "slang-cdc/sync_verifier.h"
#include "slang-cdc/report_generator.h"

namespace fs = std::filesystem;

static void printUsage() {
    std::cout << "slang-cdc v0.1.0 — Structural CDC Analysis Tool\n\n"
              << "Usage: slang-cdc [OPTIONS] <SV_FILES...>\n\n"
              << "Required:\n"
              << "  <SV_FILES...>           SystemVerilog source files\n"
              << "  --top <module>          Top-level module name\n\n"
              << "Output:\n"
              << "  -o, --output <dir>      Output directory (default: ./cdc_reports/)\n"
              << "  --format <fmt>          md|json|all (default: all)\n\n"
              << "Options:\n"
              << "  -v, --verbose           Detailed output\n"
              << "  -q, --quiet             Only violations and summary\n"
              << "  --version               Show version\n"
              << "  -h, --help              Show this help\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    // Check for --version or --help early
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--version") {
            std::cout << "slang-cdc 0.1.0\n";
            return 0;
        }
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        }
    }

    // Use slang's Driver for argument parsing and compilation
    slang::driver::Driver driver;
    driver.addStandardArgs();

    if (!driver.parseCommandLine(argc, argv))
        return 1;

    if (!driver.processOptions())
        return 1;

    if (!driver.parseAllSources())
        return 1;

    auto compilation = driver.createCompilation();
    auto& root = compilation->getRoot();

    // Verify we got a valid compilation
    auto& diags = compilation->getAllDiagnostics();
    if (!diags.empty()) {
        for (auto& diag : diags) {
            // TODO: proper diagnostic printing
            std::cerr << "slang diagnostic reported\n";
        }
    }

    std::cout << "slang-cdc: Design elaborated successfully.\n";

    // Count top-level instances for sanity check
    int instance_count = 0;
    for (auto& member : root.members()) {
        if (member.kind == slang::ast::SymbolKind::Instance) {
            instance_count++;
            auto& inst = member.as<slang::ast::InstanceSymbol>();
            std::cout << "  Top instance: " << inst.name << "\n";
        }
    }
    std::cout << "  Total top instances: " << instance_count << "\n";

    // TODO: Wire up analysis passes
    // Pass 1: ClockTreeAnalyzer
    // Pass 2: FFClassifier
    // Pass 3: ConnectivityBuilder
    // Pass 4: CrossingDetector
    // Pass 5: SyncVerifier
    // Pass 6: ReportGenerator

    return 0;
}
