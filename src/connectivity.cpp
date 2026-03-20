#include "slang-cdc/connectivity.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Statement.h"

#include <algorithm>

namespace slang_cdc {

ConnectivityBuilder::ConnectivityBuilder(slang::ast::Compilation& compilation,
                                         const std::vector<std::unique_ptr<FFNode>>& ff_nodes)
    : compilation_(compilation), ff_nodes_(ff_nodes) {}

// Extract signal names referenced in an expression (RHS of assignment)
static void collectReferencedSignals(const slang::ast::Expression& expr,
                                     std::vector<std::string>& signals) {
    switch (expr.kind) {
        case slang::ast::ExpressionKind::NamedValue:
        case slang::ast::ExpressionKind::HierarchicalValue: {
            auto& named = expr.as<slang::ast::ValueExpressionBase>();
            std::string name(named.symbol.name);
            if (std::find(signals.begin(), signals.end(), name) == signals.end())
                signals.push_back(name);
            return;
        }
        case slang::ast::ExpressionKind::UnaryOp: {
            auto& unary = expr.as<slang::ast::UnaryExpression>();
            collectReferencedSignals(unary.operand(), signals);
            return;
        }
        case slang::ast::ExpressionKind::BinaryOp: {
            auto& binary = expr.as<slang::ast::BinaryExpression>();
            collectReferencedSignals(binary.left(), signals);
            collectReferencedSignals(binary.right(), signals);
            return;
        }
        case slang::ast::ExpressionKind::ConditionalOp: {
            auto& cond = expr.as<slang::ast::ConditionalExpression>();
            collectReferencedSignals(cond.left(), signals);
            collectReferencedSignals(cond.right(), signals);
            return;
        }
        default:
            // For other expressions, skip
            return;
    }
}

// Collect fanin info: for each assignment LHS = RHS, record which signals RHS references
struct AssignInfo {
    std::string lhs_name;
    std::vector<std::string> rhs_signals;
};

static void collectAssignments(const slang::ast::Statement& stmt,
                               std::vector<AssignInfo>& assignments) {
    switch (stmt.kind) {
        case slang::ast::StatementKind::ExpressionStatement: {
            auto& exprStmt = stmt.as<slang::ast::ExpressionStatement>();
            auto& expr = exprStmt.expr;
            if (expr.kind == slang::ast::ExpressionKind::Assignment) {
                auto& assign = expr.as<slang::ast::AssignmentExpression>();
                AssignInfo info;
                if (assign.left().kind == slang::ast::ExpressionKind::NamedValue) {
                    info.lhs_name = std::string(
                        assign.left().as<slang::ast::NamedValueExpression>().symbol.name);
                }
                collectReferencedSignals(assign.right(), info.rhs_signals);
                if (!info.lhs_name.empty())
                    assignments.push_back(std::move(info));
            }
            break;
        }
        case slang::ast::StatementKind::Timed: {
            auto& timed = stmt.as<slang::ast::TimedStatement>();
            collectAssignments(timed.stmt, assignments);
            break;
        }
        case slang::ast::StatementKind::Block: {
            auto& block = stmt.as<slang::ast::BlockStatement>();
            collectAssignments(block.body, assignments);
            break;
        }
        case slang::ast::StatementKind::List: {
            auto& list = stmt.as<slang::ast::StatementList>();
            for (auto* child : list.list)
                if (child) collectAssignments(*child, assignments);
            break;
        }
        case slang::ast::StatementKind::Conditional: {
            auto& cond = stmt.as<slang::ast::ConditionalStatement>();
            collectAssignments(cond.ifTrue, assignments);
            if (cond.ifFalse)
                collectAssignments(*cond.ifFalse, assignments);
            break;
        }
        default: break;
    }
}

std::unordered_map<std::string, FFNode*> ConnectivityBuilder::buildFFOutputMap() const {
    std::unordered_map<std::string, FFNode*> map;
    for (auto& ff : ff_nodes_) {
        // Key: the simple variable name (last component of hier_path)
        auto& path = ff->hier_path;
        auto dot = path.rfind('.');
        std::string var_name = (dot != std::string::npos) ?
            path.substr(dot + 1) : path;
        map[var_name] = ff.get();

        // Also store full path for cross-module matching
        map[path] = ff.get();
    }
    return map;
}

void ConnectivityBuilder::findFFtoFFEdges(
    const std::unordered_map<std::string, FFNode*>& output_map)
{
    // For each instance, collect assignments in always_ff blocks
    // Then check if RHS references any FF output
    auto& root = compilation_.getRoot();

    for (auto& member : root.members()) {
        if (member.kind != slang::ast::SymbolKind::Instance) continue;
        auto& inst = member.as<slang::ast::InstanceSymbol>();

        for (auto& body_member : inst.body.members()) {
            if (body_member.kind != slang::ast::SymbolKind::ProceduralBlock) continue;
            auto& block = body_member.as<slang::ast::ProceduralBlockSymbol>();
            if (block.procedureKind != slang::ast::ProceduralBlockKind::AlwaysFF &&
                block.procedureKind != slang::ast::ProceduralBlockKind::Always)
                continue;

            auto& body = block.getBody();
            std::vector<AssignInfo> assignments;
            collectAssignments(body, assignments);

            for (auto& assign : assignments) {
                // Find the dest FF (LHS)
                FFNode* dest = nullptr;
                auto it = output_map.find(assign.lhs_name);
                if (it != output_map.end())
                    dest = it->second;
                if (!dest) {
                    // Try with instance prefix
                    std::string full_name = std::string(inst.name) + "." + assign.lhs_name;
                    it = output_map.find(full_name);
                    if (it != output_map.end())
                        dest = it->second;
                }
                if (!dest) continue;

                // Check each RHS signal — if it's an FF output, create an edge
                for (auto& rhs_sig : assign.rhs_signals) {
                    FFNode* source = nullptr;
                    auto sit = output_map.find(rhs_sig);
                    if (sit != output_map.end())
                        source = sit->second;
                    if (!source) {
                        std::string full_name = std::string(inst.name) + "." + rhs_sig;
                        sit = output_map.find(full_name);
                        if (sit != output_map.end())
                            source = sit->second;
                    }

                    if (source && source != dest) {
                        FFEdge edge;
                        edge.source = source;
                        edge.dest = dest;
                        edges_.push_back(edge);
                    }
                }
            }
        }
    }
}

void ConnectivityBuilder::analyze() {
    auto output_map = buildFFOutputMap();
    findFFtoFFEdges(output_map);
}

const std::vector<FFEdge>& ConnectivityBuilder::getEdges() const {
    return edges_;
}

} // namespace slang_cdc
