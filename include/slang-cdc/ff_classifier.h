#pragma once

#include "slang-cdc/types.h"
#include "slang/ast/Compilation.h"

namespace slang_cdc {

/// Pass 2: FF classification — map every FF to its clock domain
class FFClassifier {
public:
    FFClassifier(slang::ast::Compilation& compilation,
                 ClockDatabase& clock_db);

    void analyze();
    const std::vector<std::unique_ptr<FFNode>>& getFFNodes() const;

private:
    slang::ast::Compilation& compilation_;
    ClockDatabase& clock_db_;
    std::vector<std::unique_ptr<FFNode>> ff_nodes_;
};

} // namespace slang_cdc
