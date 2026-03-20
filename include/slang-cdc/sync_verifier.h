#pragma once

#include "slang-cdc/types.h"

namespace slang_cdc {

/// Pass 5: Synchronizer verification — pattern matching on crossing paths
///
/// For each crossing, examines the destination-domain FFs to detect
/// synchronizer patterns (2-FF, 3-FF, etc.).
/// Updates crossing reports with sync_type and adjusts category accordingly.
class SyncVerifier {
public:
    SyncVerifier(std::vector<CrossingReport>& crossings,
                 const std::vector<std::unique_ptr<FFNode>>& ff_nodes,
                 const std::vector<FFEdge>& edges);

    void analyze();

private:
    std::vector<CrossingReport>& crossings_;
    const std::vector<std::unique_ptr<FFNode>>& ff_nodes_;
    const std::vector<FFEdge>& edges_;

    /// Check if dest FF is the start of a 2-FF or 3-FF sync chain
    SyncType detectSyncPattern(const FFNode* dest_ff) const;

    /// Find downstream FF connected to given FF in the same domain
    const FFNode* findNextFF(const FFNode* ff) const;

    int info_counter_ = 0;
};

} // namespace slang_cdc
