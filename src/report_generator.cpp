#include "slang-cdc/report_generator.h"
#include <fstream>

namespace slang_cdc {

int AnalysisResult::violation_count() const {
    int count = 0;
    for (auto& c : crossings)
        if (c.category == ViolationCategory::Violation) count++;
    return count;
}

int AnalysisResult::caution_count() const {
    int count = 0;
    for (auto& c : crossings)
        if (c.category == ViolationCategory::Caution) count++;
    return count;
}

int AnalysisResult::info_count() const {
    int count = 0;
    for (auto& c : crossings)
        if (c.category == ViolationCategory::Info) count++;
    return count;
}

ClockSource* ClockDatabase::addSource(std::unique_ptr<ClockSource> src) {
    auto* ptr = src.get();
    sources.push_back(std::move(src));
    return ptr;
}

ClockNet* ClockDatabase::addNet(std::unique_ptr<ClockNet> net) {
    auto* ptr = net.get();
    net_by_path[net->hier_path] = ptr;
    nets.push_back(std::move(net));
    return ptr;
}

ClockDomain* ClockDatabase::findOrCreateDomain(ClockSource* source, Edge edge) {
    for (auto& d : domains) {
        if (d->source == source && d->edge == edge)
            return d.get();
    }
    auto dom = std::make_unique<ClockDomain>();
    dom->canonical_name = source->name;
    dom->source = source;
    dom->edge = edge;
    auto* ptr = dom.get();
    domain_by_name[dom->canonical_name] = ptr;
    domains.push_back(std::move(dom));
    return ptr;
}

ClockDomain* ClockDatabase::domainForSignal(const std::string& hier_path) const {
    auto it = net_by_path.find(hier_path);
    if (it == net_by_path.end()) return nullptr;
    auto* net = it->second;
    // Find matching domain
    for (auto& d : domains) {
        if (d->source == net->source && d->edge == net->edge)
            return d.get();
    }
    return nullptr;
}

bool ClockDatabase::isAsynchronous(const ClockDomain* a, const ClockDomain* b) const {
    if (!a || !b) return true; // unknown → conservative
    if (a->source == b->source) return false; // same source

    for (auto& rel : relationships) {
        if ((rel.a == a->source && rel.b == b->source) ||
            (rel.a == b->source && rel.b == a->source)) {
            return rel.relationship == DomainRelationship::Type::Asynchronous;
        }
    }
    return true; // no relationship found → assume async
}

ReportGenerator::ReportGenerator(const AnalysisResult& result)
    : result_(result) {}

void ReportGenerator::generateMarkdown(const std::filesystem::path& output_path) const {
    std::ofstream out(output_path);
    out << "# CDC Analysis Report\n\n";
    out << "## Summary\n\n";
    out << "| Category | Count |\n";
    out << "|----------|-------|\n";
    out << "| VIOLATION | " << result_.violation_count() << " |\n";
    out << "| CAUTION | " << result_.caution_count() << " |\n";
    out << "| INFO | " << result_.info_count() << " |\n\n";

    out << "## Clock Domains\n\n";
    out << "| Domain | Source | Type | Edge |\n";
    out << "|--------|--------|------|------|\n";
    for (auto& d : result_.clock_db.domains) {
        out << "| " << d->canonical_name
            << " | " << d->source->name
            << " | ";
        switch (d->source->type) {
            case ClockSource::Type::Primary: out << "primary"; break;
            case ClockSource::Type::Generated: out << "generated"; break;
            case ClockSource::Type::Virtual: out << "virtual"; break;
            case ClockSource::Type::AutoDetected: out << "auto"; break;
        }
        out << " | " << (d->edge == Edge::Posedge ? "posedge" : "negedge")
            << " |\n";
    }

    out << "\n## Crossings\n\n";
    for (auto& c : result_.crossings) {
        out << "### " << c.id << ": "
            << (c.source_domain ? c.source_domain->canonical_name : "?")
            << " -> "
            << (c.dest_domain ? c.dest_domain->canonical_name : "?")
            << "\n";
        out << "- Source: " << c.source_signal << "\n";
        out << "- Dest: " << c.dest_signal << "\n";
        if (!c.recommendation.empty())
            out << "- Fix: " << c.recommendation << "\n";
        out << "\n";
    }
}

void ReportGenerator::generateJSON(const std::filesystem::path& output_path) const {
    std::ofstream out(output_path);
    out << "{\n";
    out << "  \"summary\": {\n";
    out << "    \"violations\": " << result_.violation_count() << ",\n";
    out << "    \"cautions\": " << result_.caution_count() << ",\n";
    out << "    \"info\": " << result_.info_count() << "\n";
    out << "  },\n";

    out << "  \"domains\": [\n";
    for (size_t i = 0; i < result_.clock_db.domains.size(); i++) {
        auto& d = result_.clock_db.domains[i];
        out << "    {\"name\": \"" << d->canonical_name
            << "\", \"source\": \"" << d->source->name << "\"}";
        if (i + 1 < result_.clock_db.domains.size()) out << ",";
        out << "\n";
    }
    out << "  ],\n";

    out << "  \"crossings\": [\n";
    for (size_t i = 0; i < result_.crossings.size(); i++) {
        auto& c = result_.crossings[i];
        out << "    {\"id\": \"" << c.id
            << "\", \"source\": \"" << c.source_signal
            << "\", \"dest\": \"" << c.dest_signal
            << "\", \"source_domain\": \""
            << (c.source_domain ? c.source_domain->canonical_name : "")
            << "\", \"dest_domain\": \""
            << (c.dest_domain ? c.dest_domain->canonical_name : "")
            << "\"}";
        if (i + 1 < result_.crossings.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

} // namespace slang_cdc
