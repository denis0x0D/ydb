#include "kqp_rules_include.h"

#include "dependent_join.h"
#include <ydb/core/kqp/opt/rbo/map_renames.h>

namespace {

using namespace NKikimr::NKqp;
using namespace NKikimr::NKqp::NMapRenames;

// Conservatively decide whether a column may be null. Postgres types are always nullable, so
// callers have to exclude the postgres syntax separately.
bool IsNullableIU(const TIntrusivePtr<IOperator>& input, const TInfoUnit& iu) {
    if (!input->Type) {
        return true;
    }
    const auto* columnType = input->GetIUType(iu);
    return !columnType || columnType->IsOptionalOrNull();
}

void AddDomainColumn(TVector<TInfoUnit>& domain, const TInfoUnit& iu) {
    if (!ContainsInfoUnit(domain, iu)) {
        domain.push_back(iu);
    }
}

}

namespace NKikimr {
namespace NKqp {

bool TInlineGenericInExistsSubplanRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Filter;
}

TIntrusivePtr<IOperator> TInlineGenericInExistsSubplanRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx, TPlanProps& props) {
    if (input->Kind != EOperator::Filter) {
        return input;
    }

    // Check that the filter lambda contains at least one in/exists subplan
    auto filter = CastOperator<TOpFilter>(input);
    auto subplanIUs = filter->FilterExpr.GetInputIUs(true, false);
    TVector<TInfoUnit> inOrExistsSubplans;

    for (auto subplanIU : subplanIUs) {
        if(props.Subplans.PlanMap.contains(subplanIU)) {
            auto subplanEntry = props.Subplans.PlanMap.at(subplanIU);
            if (subplanEntry.Type == ESubplanType::IN_SUBPLAN || subplanEntry.Type == ESubplanType::EXISTS) {
                inOrExistsSubplans.push_back(subplanIU);
            }
        }
    }

    if (inOrExistsSubplans.empty()) {
        return input;
    }

    // Now we will pick the first subplan IU and join its subplan before filter
    // Then we'll remove the subplan from subplans list and rebuild the filter expression
    // so the current iu is no longer marked as SubplanIU

    auto subplanIU = inOrExistsSubplans[0];
    auto subplanEntry = props.Subplans.PlanMap.at(subplanIU);
    TIntrusivePtr<IOperator> newFilterInput;
    TVector<std::pair<TInfoUnit, TInfoUnit>> extraJoinKeys;
    auto uncorrSubplan = CastOperator<IOperator>(subplanEntry.Plan);
    const auto subPlanKind = uncorrSubplan->Kind;
    TVector<TExpression> joinFilters;
    TVector<TInfoUnit> dependencies;


    // If its a correlated subplan with filters pulled up, build join conditions from the pulled up filter
    const bool filterPullUpSucceeded =
        subPlanKind == EOperator::Filter && CastOperator<TOpFilter>(uncorrSubplan)->GetInput()->Kind == EOperator::AddDependencies;

    if (filterPullUpSucceeded) {
        auto subplanFilter = CastOperator<TOpFilter>(subplanEntry.Plan);
        auto addDeps = CastOperator<TOpAddDependencies>(subplanFilter->GetInput());
        uncorrSubplan = addDeps->GetInput();
        dependencies = addDeps->Dependencies;
        auto subplanConjuncts = subplanFilter->FilterExpr.SplitConjunct();

        for (const auto& conj : subplanConjuncts) {
            if (conj.MaybeEquiJoinCondition()) {

                auto jc = TEquiJoinCondition(conj);
                if (std::find(dependencies.begin(), dependencies.end(), jc.GetLeftIU()) != dependencies.end()) {
                    extraJoinKeys.push_back(std::make_pair(jc.GetLeftIU(), jc.GetRightIU()));
                } else if (std::find(dependencies.begin(), dependencies.end(), jc.GetRightIU()) != dependencies.end()) {
                    extraJoinKeys.push_back(std::make_pair(jc.GetRightIU(), jc.GetLeftIU()));
                } else {
                    Y_ENSURE(false, "Correlated filter missing join condition");
                }
            } else {
                joinFilters.push_back(conj);
            }

        }
    }

    // If the correlated predicate could not be pulled up to the top of the subplan, the correlation
    // stays inside the subplan and we decorrelate with a dependent join instead. The pushdown rules
    // of the "Decorrelate dependent joins" stage take it from here.
    const bool useDependentJoin = !filterPullUpSucceeded && HasFreeCorrelation(uncorrSubplan, subplanEntry.DependentIUs);
    if (useDependentJoin) {
        dependencies = subplanEntry.DependentIUs;
    }

    // We decorrelate an IN subplan or a correlated EXISTS subplan the way a dependent join is
    // unnested: first restrict the outer plan to the distinct values of the correlation columns
    // (the domain D), then join D with the subplan, reduce the result back to the domain values
    // that produced at least one match, and finally left join that mark back into the outer plan.
    //
    // A subplan is correlated as soon as it has correlation columns to bind, no matter whether the
    // pulled up predicate produced an equi join key: a plain join filter has to be evaluated against
    // the subplan columns as well, which the uncorrelated path below cannot do.
    const bool correlated = !dependencies.empty() && (filterPullUpSucceeded || useDependentJoin);

    if (subplanEntry.Type == ESubplanType::IN_SUBPLAN || correlated) {
        auto leftInput = filter->GetInput();
        auto rightInput = uncorrSubplan;
        const auto outerIUs = leftInput->GetOutputIUs();
        // The dependent join keeps the correlation inside the subplan, so the columns the tuple is
        // compared against are the subplan's result columns, not simply its first output columns.
        const auto originalPlanIUs = useDependentJoin ? GetSubplanResultIUs(rightInput) : rightInput->GetOutputIUs();

        // Collect the domain: every outer column the join condition refers to.
        TVector<TInfoUnit> domain;
        for (const auto& joinKey : extraJoinKeys) {
            AddDomainColumn(domain, joinKey.first);
        }
        for (const auto& joinFilter : joinFilters) {
            for (const auto& iu : IUSetIntersect(joinFilter.GetInputIUs(), dependencies)) {
                AddDomainColumn(domain, iu);
            }
        }
        if (useDependentJoin) {
            // The dependent join binds the correlated columns only. The tuple of an IN subplan is
            // compared in the mark join below instead, so that the subplan is not evaluated once per
            // (correlation, tuple) pair.
            for (const auto& iu : dependencies) {
                AddDomainColumn(domain, iu);
            }
        } else {
            for (const auto& iu : subplanEntry.Tuple) {
                AddDomainColumn(domain, iu);
            }
        }

        Y_ENSURE(!domain.empty(), "Cannot decorrelate an in/exists subplan without correlation columns");
        for (const auto& iu : domain) {
            Y_ENSURE(ContainsInfoUnit(outerIUs, iu),
                     TStringBuilder() << "Correlation column " << iu.GetFullName() << " is not produced by the outer plan");
        }

        // EXISTS is two-valued: a domain value without a match yields false. IN is three-valued, so
        // we may only collapse "no match" into false when neither side of the comparison is
        // nullable. Otherwise we keep the null the left join produces, which is the correct unknown
        // for a null operand and matches the behaviour we had before.
        bool markMissingAsFalse = subplanEntry.Type == ESubplanType::EXISTS;
        if (!markMissingAsFalse && !props.PgSyntax) {
            markMissingAsFalse = true;
            for (size_t i = 0; i < subplanEntry.Tuple.size() && markMissingAsFalse; i++) {
                markMissingAsFalse = !IsNullableIU(leftInput, subplanEntry.Tuple[i]) && !IsNullableIU(rightInput, originalPlanIUs[i]);
            }
        }

        TInfoUnitSet usedIUs;
        AddUsedIUs(usedIUs, outerIUs);
        AddUsedIUs(usedIUs, originalPlanIUs);

        // The columns the mark is keyed by and the columns of the outer plan they are matched
        // against. An IN subplan compares its result against the outer tuple, an EXISTS subplan only
        // has to match the correlation.
        TVector<TInfoUnit> markColumns = domain;
        TVector<std::pair<TInfoUnit, TInfoUnit>> markJoinKeys;
        for (const auto& iu : domain) {
            markJoinKeys.push_back(std::make_pair(iu, iu));
        }

        TIntrusivePtr<IOperator> matchSource;
        if (useDependentJoin) {
            // The subplan still references the outer columns, so it is evaluated for every value of
            // the domain. The correlation itself is bound by the dependent join pushdown rules.
            matchSource = MakeIntrusive<TOpDependentJoin>(MakeDomainProjection(leftInput, domain, filter->Pos), rightInput, domain, filter->Pos);

            // The dependent join carries the correlation columns next to the subplan result, so the
            // comparison against the outer tuple becomes another key of the mark join. The mark then
            // means "the subplan has, for this correlation, a row equal to the tuple".
            for (size_t i = 0; i < subplanEntry.Tuple.size(); i++) {
                AddDomainColumn(markColumns, originalPlanIUs[i]);
                markJoinKeys.push_back(std::make_pair(subplanEntry.Tuple[i], originalPlanIUs[i]));
            }
        } else {
            // The domain carries outer column names, so rename the colliding subplan columns.
            const auto commonIUs = IUSetIntersect(domain, originalPlanIUs);
            const auto rightRenamings = MakeRenameMap(commonIUs, props.InternalVarIdx, usedIUs);
            if (!rightRenamings.empty()) {
                rightInput = MakeMapFromRenames(rightInput, rightRenamings, filter->Pos, ctx.ExprCtx, props);
                extraJoinKeys = RemapRightJoinKeys(extraJoinKeys, rightRenamings);
                for (auto& joinFilter : joinFilters) {
                    joinFilter = joinFilter.ApplyRenames(rightRenamings);
                }
            }

            TVector<std::pair<TInfoUnit, TInfoUnit>> joinKeys;
            auto planIUs = rightInput->GetOutputIUs();

            for (size_t i = 0; i < subplanEntry.Tuple.size(); i++) {
                joinKeys.push_back(std::make_pair(subplanEntry.Tuple[i], planIUs[i]));
            }

            // Build the join over the domain instead of the full outer plan
            joinKeys.insert(joinKeys.begin(), extraJoinKeys.begin(), extraJoinKeys.end());
            matchSource = MakeIntrusive<TOpJoin>(MakeDomainProjection(leftInput, domain, filter->Pos), rightInput, input->Pos, "Inner", joinKeys,
                                                 joinFilters);
        }

        // Reduce the result back to the values that have at least one match and mark them. A distinct
        // projection is enough here, we don't need to count the matches, and it keeps the mark join
        // below from duplicating outer rows.
        auto matchedDomain = MakeDomainProjection(matchSource, markColumns, filter->Pos);

        auto markIU = MakeUniqueInternalIU(props.InternalVarIdx, usedIUs);
        TVector<TMapElement> markElements;
        markElements.emplace_back(markIU, MakeConstant("Bool", "true", filter->Pos, &ctx.ExprCtx));
        auto markMap = MakeIntrusive<TOpMap>(matchedDomain, filter->Pos, markElements);

        // Left join the mark back into the outer plan. The marked relation holds at most one row per
        // key of the mark join, so this cannot duplicate outer rows.
        const auto topCommonIUs = IUSetIntersect(outerIUs, markMap->GetOutputIUs());
        const auto topRenamings = MakeRenameMap(topCommonIUs, props.InternalVarIdx, usedIUs);

        auto markJoin = NMapRenames::MakeJoinWithRightRenames(
            leftInput, markMap, filter->Pos, "Left", markJoinKeys, {}, topRenamings, ctx.ExprCtx, props);

        // The mark is null for outer rows without a match, turn it into the subplan result
        TVector<TMapElement> resultElements;
        if (markMissingAsFalse) {
            resultElements.emplace_back(subplanIU,
                                        MakeBinaryPredicate("Coalesce", MakeColumnAccess(markIU, filter->Pos, &ctx.ExprCtx, &props),
                                                            MakeConstant("Bool", "false", filter->Pos, &ctx.ExprCtx)));
        } else {
            resultElements.emplace_back(subplanIU, markIU, filter->Pos, &ctx.ExprCtx, &props);
        }
        newFilterInput = MakeIntrusive<TOpMap>(markJoin, filter->Pos, resultElements);
    }
    // uncorrelated EXISTS
    else {
        auto zero = MakeConstant("Uint64", "0", filter->Pos, &ctx.ExprCtx);
        auto limit = MakeIntrusive<TOpLimit>(uncorrSubplan, filter->Pos, MakeConstant("Uint64", "1", filter->Pos, &ctx.ExprCtx), EOpPhase::Undefined);

        auto countResult = TInfoUnit("_rbo_arg_" + std::to_string(props.InternalVarIdx++), true);
        TVector<TMapElement> countMapElements;
        countMapElements.emplace_back(countResult, zero);
        auto countMap = MakeIntrusive<TOpMap>(limit, filter->Pos, countMapElements, true);

        TOpAggregationTraits aggFunction(countResult, "count", countResult);
        TVector<TOpAggregationTraits> aggs = {aggFunction};
        TVector<TInfoUnit> keyColumns;

        auto agg = MakeIntrusive<TOpAggregate>(countMap, aggs, keyColumns, EOpPhase::Final, false, filter->Pos);

        auto comparePredicate = MakeBinaryPredicate("!=", MakeColumnAccess(countResult, filter->Pos, &ctx.ExprCtx, &props), zero);
        TVector<TMapElement> mapElements;
        mapElements.emplace_back(subplanIU, comparePredicate);

        auto map = MakeIntrusive<TOpMap>(agg, filter->Pos, mapElements, true);

        TVector<std::pair<TInfoUnit, TInfoUnit>> joinKeys;
        newFilterInput = MakeIntrusive<TOpJoin>(filter->GetInput(), map, filter->Pos, "Cross", joinKeys, joinFilters);
    }

    props.Subplans.Remove(subplanIU);

    // Otherwise, we need to pack the remaining conjuncts back into the filter
    return MakeIntrusive<TOpFilter>(newFilterInput, filter->Pos, TExpression(filter->FilterExpr.GetLambda(), &ctx.ExprCtx, &props));
}
}
}
