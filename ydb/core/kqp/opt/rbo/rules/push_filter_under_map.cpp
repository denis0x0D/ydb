#include "kqp_rules_include.h"

#include <ydb/core/kqp/opt/peephole/kqp_opt_peephole.h>

namespace NKikimr {
namespace NKqp {

namespace {

// A definition copied into the filter is evaluated again by the map, so both must get the same value.
bool IsDeterministic(const TExpression& expression) {
    const auto nonDeterministic = NOpt::GetNonDeterministicFunctions();
    return !FindNode(expression.GetExpressionBody(), [&](const TExprNode::TPtr& node) {
        return node->IsCallable() && nonDeterministic.contains(node->Content());
    });
}

} // anonymous namespace

bool TPushFilterUnderMapRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Filter &&
        input->GetChildren().front()->Kind == EOperator::Map;
}

TIntrusivePtr<IOperator> TPushFilterUnderMapRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator> &input, TRBOContext &ctx, TPlanProps &props) {

    Y_UNUSED(ctx);

    if (input->Kind != EOperator::Filter) {
        return input;
    }

    auto filter = CastOperator<TOpFilter>(input);
    if (filter->GetInput()->Kind != EOperator::Map) {
        return input;
    }

    auto map = CastOperator<TOpMap>(filter->GetInput());

    auto conjuncts = filter->GetFilterExpression().SplitConjunct();
    TVector<TExpression> pushedFilters;
    TVector<TExpression> remainingFilters;

    const auto& newMapColumns = map->GetMapElements().Keys();

    // A conjunct over map outputs moves down with their definitions in place of them.
    THashMap<TInfoUnitId, TExpression> definitions;
    for (const auto& [id, element] : map->GetMapElements().Items()) {
        if (IsDeterministic(element.GetExpression())) {
            definitions.emplace(id, element.GetExpression());
        }
    }

    for (const auto & c : conjuncts) {
        const auto pushed = c.GetInputIUs(false, true).HasAny(newMapColumns) ? c.ApplyDefinitions(definitions) : c;
        if (!ReferencesUnresolvedSubplan(pushed, props) && !pushed.GetInputIUs(false, true).HasAny(newMapColumns)) {
            pushedFilters.push_back(pushed);
        } else {
            remainingFilters.push_back(c);
        }
    }

    if (pushedFilters.empty()) {
        return input;
    }

    filter->SetInput(map->GetInput());
    filter->SetFilterExpression(MakeConjunction(pushedFilters, props.PgSyntax));
    map->SetInput(filter);

    if (remainingFilters.size()) {
        auto pushedFilterExpr = MakeConjunction(remainingFilters, props.PgSyntax);
        return MakeIntrusive<TOpFilter>(map, map->Pos, pushedFilterExpr);
    } else {
        return map;
    }
}
}
}
