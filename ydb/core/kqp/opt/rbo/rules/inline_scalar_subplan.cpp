#include "kqp_rules_include.h"

#include "dependent_join.h"
#include <ydb/core/kqp/opt/rbo/map_renames.h>

namespace NKikimr {
namespace NKqp {
    
// Rewrite a single scalar subplan into a cross-join for uncorrelated queries
// or into a left join for correlated (assuming at most one tuple in the output of each subquery)
// FIXME: Need to do correct general case decorellation in the future

bool TInlineScalarSubplanRule::MatchAndApply(TIntrusivePtr<IOperator> &input, TRBOContext &ctx, TPlanProps &props) {
    auto subplanIUs = input->GetSubplanIUs(props);
    TVector<TInfoUnit> scalarIUs;
    for (const auto& iu : subplanIUs) {
        auto subplanEntry = props.Subplans.PlanMap.at(iu);
        if (subplanEntry.Type == ESubplanType::EXPR) {
            scalarIUs.push_back(iu);
            break;
        }
    }

    if (scalarIUs.empty()) {
        return false;
    }

    auto scalarIU = scalarIUs[0];
    auto subplanEntry = props.Subplans.PlanMap.at(scalarIU);
    auto subplan = CastOperator<IOperator>(subplanEntry.Plan);
    auto subplanResIU = GetSubplanResultIUs(subplan)[0];
    auto subplanResType = subplan->GetIUType(subplanResIU);

    Y_ENSURE(MatchOperator<IUnaryOperator>(input));
    auto unaryOp = CastOperator<IUnaryOperator>(input);

    auto child = unaryOp->GetInput();

    // Attach the subplan result, which the join below produced under joinedSubplanResIU, to the
    // operator the subplan reference came from.
    auto attachSubplanResult = [&](const TIntrusivePtr<IOperator>& join, const TInfoUnit& joinedSubplanResIU) {
        if (input->Kind == EOperator::Filter) {
            auto outerFilter = CastOperator<TOpFilter>(input);
            outerFilter->FilterExpr = outerFilter->FilterExpr.ApplyRenames({{scalarIU, joinedSubplanResIU}});
            outerFilter->SetInput(join);
        } else {
            TVector<TMapElement> renameElements;
            renameElements.emplace_back(scalarIU, joinedSubplanResIU, subplan->Pos, &ctx.ExprCtx, &props);
            auto rename = MakeIntrusive<TOpMap>(join, subplan->Pos, renameElements);
            unaryOp->SetInput(rename);
        }
    };

    // Check whether this is a correlated subplan with filter pushed up
    // FIXME: if the filter got stuck we will crash later in the optimizer
    if (subplan->Kind == EOperator::Filter && CastOperator<TOpFilter>(subplan)->GetInput()->Kind == EOperator::AddDependencies) {
        auto subplanFilter = CastOperator<TOpFilter>(subplan);
        auto addDeps = CastOperator<TOpAddDependencies>(subplanFilter->GetInput());
        auto uncorrSubplan = addDeps->GetInput();

        TVector<std::pair<TInfoUnit, TInfoUnit>> joinKeys;
        TVector<TExpression> joinFilters;
        NMapRenames::TRenameMap subplanOutputRenames;

        auto leftIUs = child->GetOutputIUs();
        auto rightIUs = uncorrSubplan->GetOutputIUs();
        THashSet<TInfoUnit, TInfoUnit::THashFunction> usedIUs;
        NMapRenames::AddUsedIUs(usedIUs, leftIUs);
        NMapRenames::AddUsedIUs(usedIUs, rightIUs);

        for (const auto& iu : rightIUs) {
            if (ContainsInfoUnit(leftIUs, iu) && !subplanOutputRenames.contains(iu)) {
                subplanOutputRenames.emplace(iu, NMapRenames::MakeUniqueInternalIU(props.InternalVarIdx, usedIUs));
            }
        }

        auto conjuncts = subplanFilter->FilterExpr.SplitConjunct();

        for (const auto & conj : conjuncts) {
            if (!conj.MaybeEquiJoinCondition()) {
                joinFilters.push_back(conj);
                continue;
            }

            TEquiJoinCondition jc(conj);
            TInfoUnit leftKey = jc.GetLeftIU();
            TInfoUnit rightKey = jc.GetRightIU();

            if (std::find(addDeps->Dependencies.begin(), addDeps->Dependencies.end(), rightKey) != addDeps->Dependencies.end()) {
                std::swap(leftKey, rightKey);
            } else if (std::find(addDeps->Dependencies.begin(), addDeps->Dependencies.end(), leftKey) == addDeps->Dependencies.end()) {
                Y_ENSURE(false, "Correlated filter missing join condition");
            }

            if (ContainsInfoUnit(leftIUs, rightKey)) {
                const auto renameIt = subplanOutputRenames.find(rightKey);
                if (renameIt != subplanOutputRenames.end()) {
                    rightKey = renameIt->second;
                } else {
                    auto newKey = NMapRenames::MakeUniqueInternalIU(props.InternalVarIdx, usedIUs);
                    subplanOutputRenames.emplace(rightKey, newKey);
                    rightKey = newKey;
                }
            }

            joinKeys.push_back(std::make_pair(leftKey, rightKey));
        }

        auto joinedSubplanResIU = subplanResIU;
        if (const auto renameIt = subplanOutputRenames.find(joinedSubplanResIU); renameIt != subplanOutputRenames.end()) {
            joinedSubplanResIU = renameIt->second;
        }

        auto leftJoin = NMapRenames::MakeJoinWithRightRenames(
            child, uncorrSubplan, subplan->Pos, "Left", joinKeys, joinFilters, subplanOutputRenames, ctx.ExprCtx, props);

        attachSubplanResult(leftJoin, joinedSubplanResIU);
    }

    // The correlated predicate could not be pulled up to the top of the subplan, so the correlation
    // stays inside it and we decorrelate with a dependent join instead. The pushdown rules of the
    // "Decorrelate dependent joins" stage move the domain down until the correlation is bound.
    else if (HasFreeCorrelation(subplan, subplanEntry.DependentIUs)) {
        const auto& dependencies = subplanEntry.DependentIUs;

        auto leftIUs = child->GetOutputIUs();
        for (const auto& iu : dependencies) {
            Y_ENSURE(ContainsInfoUnit(leftIUs, iu),
                     TStringBuilder() << "Correlation column " << iu.GetFullName() << " is not produced by the outer plan");
        }

        auto dependentJoin =
            MakeIntrusive<TOpDependentJoin>(MakeDomainProjection(child, dependencies, subplan->Pos), subplan, dependencies, subplan->Pos);

        // The dependent join re-exposes the correlation columns, and the subplan may well produce
        // columns that collide with the outer plan, so everything colliding gets renamed.
        auto rightIUs = dependentJoin->GetOutputIUs();
        THashSet<TInfoUnit, TInfoUnit::THashFunction> usedIUs;
        NMapRenames::AddUsedIUs(usedIUs, leftIUs);
        NMapRenames::AddUsedIUs(usedIUs, rightIUs);

        NMapRenames::TRenameMap subplanOutputRenames;
        for (const auto& iu : rightIUs) {
            if (ContainsInfoUnit(leftIUs, iu) && !subplanOutputRenames.contains(iu)) {
                subplanOutputRenames.emplace(iu, NMapRenames::MakeUniqueInternalIU(props.InternalVarIdx, usedIUs));
            }
        }

        // One row of the subplan per binding of the correlation columns, joined back on the binding.
        TVector<std::pair<TInfoUnit, TInfoUnit>> joinKeys;
        for (const auto& iu : dependencies) {
            joinKeys.push_back(std::make_pair(iu, iu));
        }

        auto joinedSubplanResIU = subplanResIU;
        if (const auto renameIt = subplanOutputRenames.find(joinedSubplanResIU); renameIt != subplanOutputRenames.end()) {
            joinedSubplanResIU = renameIt->second;
        }

        auto leftJoin = NMapRenames::MakeJoinWithRightRenames(child, dependentJoin, subplan->Pos, "Left", joinKeys, {}, subplanOutputRenames,
                                                              ctx.ExprCtx, props);

        attachSubplanResult(leftJoin, joinedSubplanResIU);
    }

    // Otherwise we assume an uncorrelated supbplan
    // Here we don't assume at most one tuple from the subplan
    else {
        auto emptySource = MakeIntrusive<TOpEmptySource>(subplan->Pos);

        TVector<TMapElement> mapElements;

        // FIXME: This works only for postgres types, because they are null-compatible
        // For YQL types we will need to handle optionality
        mapElements.emplace_back(scalarIU, MakeNothing(subplan->Pos, subplanResType, &ctx.ExprCtx));
        auto map = MakeIntrusive<TOpMap>(emptySource, subplan->Pos, mapElements);

        TVector<TMapElement> renameElements;
        renameElements.emplace_back(scalarIU, subplanResIU, subplan->Pos, &ctx.ExprCtx, &props);
        auto rename = MakeIntrusive<TOpMap>(subplan, subplan->Pos, renameElements);
        rename->Props.EnsureAtMostOne = true;

        auto unionAll = MakeIntrusive<TOpUnionAll>(
            rename,
            map,
            subplan->Pos,
            TVector<TInfoUnit>{scalarIU},
            true
        );

        auto limit = MakeIntrusive<TOpLimit>(unionAll, subplan->Pos, MakeConstant("Uint64", "1", subplan->Pos, &ctx.ExprCtx), EOpPhase::Undefined);
    
        TVector<std::pair<TInfoUnit, TInfoUnit>> joinKeys;
        auto cross = MakeIntrusive<TOpJoin>(child, limit, subplan->Pos, "Cross", joinKeys);
        unaryOp->SetInput(cross);
    }

    props.Subplans.Remove(scalarIU);

    return true;
}
}
}
