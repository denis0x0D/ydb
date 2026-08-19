#include "kqp_rules_include.h"

#include "dependent_join.h"
#include <ydb/core/kqp/opt/rbo/map_renames.h>

namespace NKikimr {
namespace NKqp {

namespace {

// A scalar subquery may not produce more than one row per correlation. Reduce the subplan to a single
// row per group and abort the query when a group holds more than one, the way an outer join back into
// the plan cannot: it would silently duplicate the outer rows instead.
//
// Returns the reduced subplan and the column its value now lives in. The value is moved into a fresh
// column so that it cannot collide with a group key, which happens as soon as the subquery selects a
// column its correlated predicate compares against.
std::pair<TIntrusivePtr<IOperator>, TInfoUnit> MakeAtMostOneRowPerGroup(const TIntrusivePtr<IOperator>& input,
                                                                       const TVector<TInfoUnit>& groupKeys, const TInfoUnit& valueIU,
                                                                       TPositionHandle pos, TRBOContext& ctx, TPlanProps& props) {
    TInfoUnitSet usedIUs;
    NMapRenames::AddUsedIUs(usedIUs, input->GetOutputIUs());

    // count() counts the non null values of its argument, so it is given a column that is never null.
    auto rowIU = NMapRenames::MakeUniqueInternalIU(props.InternalVarIdx, usedIUs);
    TVector<TMapElement> rowElements;
    rowElements.emplace_back(rowIU, MakeConstant("Uint64", "1", pos, &ctx.ExprCtx));
    auto rowMap = MakeIntrusive<TOpMap>(input, pos, rowElements);

    auto countIU = NMapRenames::MakeUniqueInternalIU(props.InternalVarIdx, usedIUs);
    auto valueStateIU = NMapRenames::MakeUniqueInternalIU(props.InternalVarIdx, usedIUs);

    // A group that passes the check holds one row, so min() of it is that row's value. An empty
    // subplan produces no group at all, except for the uncorrelated case where the aggregate has no
    // keys and yields a single null, which is what a scalar subquery over no rows means.
    TVector<TOpAggregationTraits> traits;
    traits.emplace_back(rowIU, "count", countIU);
    traits.emplace_back(valueIU, "min", valueStateIU);
    auto aggregate = MakeIntrusive<TOpAggregate>(rowMap, traits, groupKeys, EOpPhase::Undefined, /*distinctAll=*/false, pos);

    auto atMostOne =
        MakeBinaryPredicate("<=", MakeColumnAccess(countIU, pos, &ctx.ExprCtx, &props), MakeConstant("Uint64", "1", pos, &ctx.ExprCtx));

    auto checkedIU = NMapRenames::MakeUniqueInternalIU(props.InternalVarIdx, usedIUs);
    TVector<TMapElement> valueElements;
    valueElements.emplace_back(checkedIU, MakeEnsure(MakeColumnAccess(valueStateIU, pos, &ctx.ExprCtx, &props), atMostOne,
                                                     "Scalar subquery returned more than one row"));

    return std::make_pair(MakeIntrusive<TOpMap>(aggregate, pos, valueElements), checkedIU);
}

} // anonymous namespace

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

        // Split the pulled up predicate first: the subplan side of its equi conditions is what groups
        // the rows of the subplan per outer row.
        for (const auto& conj : subplanFilter->FilterExpr.SplitConjunct()) {
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

            joinKeys.push_back(std::make_pair(leftKey, rightKey));
        }

        auto rightInput = uncorrSubplan;
        auto rightResIU = subplanResIU;

        // The join matches the subplan rows by these keys, so at most one row per outer row is the
        // same as at most one row per value of the subplan side keys. A join filter mixes outer and
        // subplan columns and cannot become a grouping, so those subqueries stay unchecked.
        if (joinFilters.empty() && !joinKeys.empty()) {
            TVector<TInfoUnit> groupKeys;
            for (const auto& joinKey : joinKeys) {
                if (!ContainsInfoUnit(groupKeys, joinKey.second)) {
                    groupKeys.push_back(joinKey.second);
                }
            }
            std::tie(rightInput, rightResIU) = MakeAtMostOneRowPerGroup(uncorrSubplan, groupKeys, subplanResIU, subplan->Pos, ctx, props);
        }

        auto leftIUs = child->GetOutputIUs();
        auto rightIUs = rightInput->GetOutputIUs();
        THashSet<TInfoUnit, TInfoUnit::THashFunction> usedIUs;
        NMapRenames::AddUsedIUs(usedIUs, leftIUs);
        NMapRenames::AddUsedIUs(usedIUs, rightIUs);

        NMapRenames::TRenameMap subplanOutputRenames;
        for (const auto& iu : rightIUs) {
            if (ContainsInfoUnit(leftIUs, iu) && !subplanOutputRenames.contains(iu)) {
                subplanOutputRenames.emplace(iu, NMapRenames::MakeUniqueInternalIU(props.InternalVarIdx, usedIUs));
            }
        }

        auto joinedSubplanResIU = rightResIU;
        if (const auto renameIt = subplanOutputRenames.find(joinedSubplanResIU); renameIt != subplanOutputRenames.end()) {
            joinedSubplanResIU = renameIt->second;
        }

        auto leftJoin = NMapRenames::MakeJoinWithRightRenames(
            child, rightInput, subplan->Pos, "Left", joinKeys, joinFilters, subplanOutputRenames, ctx.ExprCtx, props);

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

        // One row per binding of the correlation columns, and an error when the subquery produced
        // more. Without the check the left join below would silently duplicate the outer rows.
        auto [rightInput, rightResIU] = MakeAtMostOneRowPerGroup(dependentJoin, dependencies, subplanResIU, subplan->Pos, ctx, props);

        // The dependent join re-exposes the correlation columns, and the subplan may well produce
        // columns that collide with the outer plan, so everything colliding gets renamed.
        auto rightIUs = rightInput->GetOutputIUs();
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

        auto joinedSubplanResIU = rightResIU;
        if (const auto renameIt = subplanOutputRenames.find(joinedSubplanResIU); renameIt != subplanOutputRenames.end()) {
            joinedSubplanResIU = renameIt->second;
        }

        auto leftJoin = NMapRenames::MakeJoinWithRightRenames(child, rightInput, subplan->Pos, "Left", joinKeys, {}, subplanOutputRenames,
                                                              ctx.ExprCtx, props);

        attachSubplanResult(leftJoin, joinedSubplanResIU);
    }

    // Otherwise we assume an uncorrelated supbplan
    else {
        // An aggregate without grouping keys yields exactly one row: the value of the subquery, or a
        // null when the subplan is empty. That replaces the union with a null row this used to build,
        // and it turns a subplan of several rows into an error instead of an arbitrary one of them.
        auto [checkedInput, checkedResIU] = MakeAtMostOneRowPerGroup(subplan, {}, subplanResIU, subplan->Pos, ctx, props);

        TVector<TMapElement> renameElements;
        renameElements.emplace_back(scalarIU, checkedResIU, subplan->Pos, &ctx.ExprCtx, &props);
        auto rename = MakeIntrusive<TOpMap>(checkedInput, subplan->Pos, renameElements);

        TVector<std::pair<TInfoUnit, TInfoUnit>> joinKeys;
        auto cross = MakeIntrusive<TOpJoin>(child, rename, subplan->Pos, "Cross", joinKeys);
        unaryOp->SetInput(cross);
    }

    props.Subplans.Remove(scalarIU);

    return true;
}
}
}
