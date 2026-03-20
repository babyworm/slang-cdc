#include "slang-cdc/sync_verifier.h"

namespace slang_cdc {

SyncVerifier::SyncVerifier(std::vector<CrossingReport>& crossings,
                           const std::vector<std::unique_ptr<FFNode>>& ff_nodes,
                           const std::vector<FFEdge>& edges)
    : crossings_(crossings), ff_nodes_(ff_nodes), edges_(edges) {}

const FFNode* SyncVerifier::findNextFF(const FFNode* ff) const {
    // Find an FF in the same domain that is directly fed by this FF
    for (auto& edge : edges_) {
        if (edge.source == ff && edge.dest &&
            edge.dest->domain == ff->domain) {
            return edge.dest;
        }
    }
    return nullptr;
}

SyncType SyncVerifier::detectSyncPattern(const FFNode* dest_ff) const {
    if (!dest_ff) return SyncType::None;

    // Check for 2-FF synchronizer:
    // dest_ff (first sync stage) → next_ff (second sync stage)
    // Both must be in the same domain with direct FF-to-FF connection
    const FFNode* second = findNextFF(dest_ff);
    if (!second) return SyncType::None;

    // 2-FF detected! Check for 3-FF
    const FFNode* third = findNextFF(second);
    if (third) return SyncType::ThreeFF;

    return SyncType::TwoFF;
}

void SyncVerifier::analyze() {
    for (auto& crossing : crossings_) {
        // Find the dest FF node for this crossing
        const FFNode* dest_ff = nullptr;
        for (auto& ff : ff_nodes_) {
            if (ff->hier_path == crossing.dest_signal) {
                dest_ff = ff.get();
                break;
            }
        }

        crossing.sync_type = detectSyncPattern(dest_ff);

        // Update category based on sync detection
        if (crossing.sync_type != SyncType::None) {
            crossing.category = ViolationCategory::Info;
            crossing.severity = Severity::Info;
            crossing.recommendation.clear();
            crossing.id = "INFO-" + std::to_string(++info_counter_);
        }
    }
}

} // namespace slang_cdc
