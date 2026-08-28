#include "kqp_rules_include.h"

#include "dependent_join.h"
#include <ydb/core/kqp/opt/rbo/map_renames.h>

namespace {

using namespace NKikimr::NKqp;
using namespace NKikimr::NKqp::NMapRenames;

// The null of the Bool type, which the three valued result of an IN needs as a value and not only
// as the absence of a row.
TExprNode::TPtr MakeNullBoolNode(TPositionHandle pos, TExprContext& ctx) {
    auto boolType = ctx.NewCallable(pos, "DataType", {ctx.NewAtom(pos, "Bool")});
    return ctx.NewCallable(pos, "Nothing", {ctx.NewCallable(pos, "OptionalType", {boolType})});
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
    auto subplan = CastOperator<IOperator>(subplanEntry.Plan);

    // A correlated subplan keeps its correlation inside, so we decorrelate it with a dependent join.
    // The pushdown rules of the "Decorrelate dependent joins" stage take it from here.
    //
    // Either way we unnest the way a dependent join is: first restrict the outer plan to the distinct
    // values of the domain columns, then join the domain with the subplan, reduce the result back to
    // the domain values that produced at least one match, and finally left join that mark back into
    // the outer plan.
    const bool useDependentJoin = HasFreeCorrelation(subplan, subplanEntry.DependentIUs);

    if (subplanEntry.Type == ESubplanType::IN_SUBPLAN || useDependentJoin) {
        TIntrusivePtr<IOperator> leftInput = filter->GetInput();
        auto rightInput = subplan;
        const auto outerIUs = leftInput->GetOutputIUs();
        // The dependent join keeps the correlation inside the subplan, so the columns the tuple is
        // compared against are the subplan's result columns, not simply its first output columns.
        const auto originalPlanIUs = useDependentJoin ? GetSubplanResultIUs(rightInput) : rightInput->GetOutputIUs();

        TVector<TInfoUnit> domain;
        if (useDependentJoin) {
            // The dependent join binds the correlated columns only. The tuple of an IN subplan is
            // compared in the mark join below instead, so that the subplan is not evaluated once per
            // (correlation, tuple) pair.
            for (const auto& iu : subplanEntry.DependentIUs) {
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
        // nullable. When one of them is, the unknown has to be told apart from the false, which is
        // what the three valued path below is for.
        bool markMissingAsFalse = subplanEntry.Type == ESubplanType::EXISTS;
        if (!markMissingAsFalse && !props.PgSyntax) {
            markMissingAsFalse = true;
            for (size_t i = 0; i < subplanEntry.Tuple.size() && markMissingAsFalse; i++) {
                markMissingAsFalse = !IsNullableIU(leftInput, subplanEntry.Tuple[i]) && !IsNullableIU(rightInput, originalPlanIUs[i]);
            }
        }

        // Everything the mark cannot tell apart from "no match" is built below out of two more facts
        // about the subquery. A tuple of several columns is not handled that way yet, so it keeps the
        // null the mark join produces, which is right for a null operand and wrong for a null result.
        const bool threeValued =
            !markMissingAsFalse && !props.PgSyntax && subplanEntry.Type == ESubplanType::IN_SUBPLAN && subplanEntry.Tuple.size() == 1;

        TInfoUnitSet usedIUs;
        AddUsedIUs(usedIUs, outerIUs);
        AddUsedIUs(usedIUs, originalPlanIUs);

        // The columns the mark is keyed by and the columns of the outer plan they are matched
        // against. An IN subplan compares its result against the outer tuple, an EXISTS subplan only
        // has to match the correlation.
        TVector<TInfoUnit> markColumns = domain;
        TVector<std::pair<TInfoUnit, TInfoUnit>> domainJoinKeys;
        TVector<std::pair<TInfoUnit, TInfoUnit>> tupleJoinKeys;
        for (const auto& iu : domain) {
            domainJoinKeys.push_back(std::make_pair(iu, iu));
        }

        // Where a three valued IN counts the results of the subquery, and the columns that counting
        // is grouped by. The correlated case counts per binding of the correlation, the uncorrelated
        // one counts the subquery as a whole.
        TIntrusivePtr<IOperator> statsSource;
        TVector<TInfoUnit> statsKeys;
        TInfoUnit compareResultIU;

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
                tupleJoinKeys.push_back(std::make_pair(subplanEntry.Tuple[i], originalPlanIUs[i]));
            }

            statsKeys = domain;
            if (threeValued) {
                compareResultIU = originalPlanIUs[0];
            }
        } else {
            // The domain carries outer column names, so rename the colliding subplan columns.
            const auto commonIUs = IUSetIntersect(domain, originalPlanIUs);
            const auto rightRenamings = MakeRenameMap(commonIUs, props.InternalVarIdx, usedIUs);
            if (!rightRenamings.empty()) {
                rightInput = MakeMapFromRenames(rightInput, rightRenamings, filter->Pos, ctx.ExprCtx, props);
            }

            TVector<std::pair<TInfoUnit, TInfoUnit>> joinKeys;
            auto planIUs = rightInput->GetOutputIUs();

            for (size_t i = 0; i < subplanEntry.Tuple.size(); i++) {
                joinKeys.push_back(std::make_pair(subplanEntry.Tuple[i], planIUs[i]));
            }

            // Build the join over the domain instead of the full outer plan
            matchSource =
                MakeIntrusive<TOpJoin>(MakeDomainProjection(leftInput, domain, filter->Pos), rightInput, input->Pos, "Inner", joinKeys);

            // The join above has already dropped the rows that did not match, so the subquery itself
            // is the only place the nulls it produced are still visible.
            statsSource = rightInput;
            if (threeValued) {
                compareResultIU = planIUs[0];
            }
        }

        // Reduce the result back to the values that have at least one match and mark them. A distinct
        // projection is enough here, we don't need to count the matches, and it keeps the mark join
        // below from duplicating outer rows.
        auto matchedDomain = MakeDomainProjection(matchSource, markColumns, filter->Pos);
        if (!statsSource) {
            // The distinct projection keeps both facts the counting below is after: a group exists
            // exactly when the subquery produced a row for that binding, and a null result survives
            // deduplication like any other value.
            statsSource = matchedDomain;
        }

        auto markIU = MakeUniqueInternalIU(props.InternalVarIdx, usedIUs);
        TVector<TMapElement> markElements;
        markElements.emplace_back(markIU, MakeConstant("Bool", "true", filter->Pos, &ctx.ExprCtx));
        auto markMap = MakeIntrusive<TOpMap>(matchedDomain, filter->Pos, markElements);

        // Left join the mark back into the outer plan. The marked relation holds at most one row per
        // key of the mark join, so this cannot duplicate outer rows.
        //
        // The join may only add the mark to the outer plan. Every other column of the marked relation
        // is renamed away: the domain columns carry outer names, and on the dependent join path the
        // subplan result column comes along as well, which is free to collide with a column the plan
        // above produces.
        const auto topRenamings = MakeRenameMap(markColumns, props.InternalVarIdx, usedIUs);

        // A binding of the correlation is matched back, and a null is a binding like any other. The
        // uncorrelated path keys by the tuple of the IN instead, where a null operand means unknown
        // and not a match, so its keys stay plain equalities.
        TIntrusivePtr<IOperator> markRight = markMap;
        auto markJoinKeys = useDependentJoin
                                ? MakeNullSafeJoinKeys(leftInput, markRight, domainJoinKeys, filter->Pos, ctx, props, usedIUs)
                                : domainJoinKeys;
        markJoinKeys.insert(markJoinKeys.end(), tupleJoinKeys.begin(), tupleJoinKeys.end());

        TIntrusivePtr<IOperator> markJoin = NMapRenames::MakeJoinWithRightRenames(
            leftInput, markRight, filter->Pos, "Left", markJoinKeys, {}, topRenamings, ctx.ExprCtx, props);

        // The mark is null for outer rows without a match, turn it into the subplan result
        auto column = [&](const TInfoUnit& iu) { return MakeColumnAccess(iu, filter->Pos, &ctx.ExprCtx, &props); };
        auto falseConst = MakeConstant("Bool", "false", filter->Pos, &ctx.ExprCtx);
        auto matched = MakeBinaryPredicate("Coalesce", column(markIU), falseConst);

        TIntrusivePtr<IOperator> resultInput = markJoin;
        TVector<TMapElement> resultElements;

        if (markMissingAsFalse) {
            resultElements.emplace_back(subplanIU, matched);
        } else if (threeValued) {
            // An unmatched row is false only when nothing could have hidden the answer behind a
            // null: the subquery produced no null of its own, and the operand is not itself null
            // over a subquery that produced anything at all. Both facts are per binding of the
            // correlation, and neither can be counted over the grouping the mark is built from, so
            // they come from a second aggregate joined in next to the mark.
            auto rowIU = MakeUniqueInternalIU(props.InternalVarIdx, usedIUs);
            TVector<TMapElement> rowElements;
            rowElements.emplace_back(rowIU, MakeConstant("Uint64", "1", filter->Pos, &ctx.ExprCtx));
            auto rowMap = MakeIntrusive<TOpMap>(statsSource, filter->Pos, rowElements);

            // count() counts the non null values of its argument, so counting the compared column
            // against a column that is never null is what tells a null result apart from no result.
            auto valueCountIU = MakeUniqueInternalIU(props.InternalVarIdx, usedIUs);
            auto rowCountIU = MakeUniqueInternalIU(props.InternalVarIdx, usedIUs);
            TVector<TOpAggregationTraits> statsTraits;
            statsTraits.emplace_back(compareResultIU, "count", valueCountIU);
            statsTraits.emplace_back(rowIU, "count", rowCountIU);
            auto statsAggregate =
                MakeIntrusive<TOpAggregate>(rowMap, statsTraits, statsKeys, EOpPhase::Undefined, /*distinctAll=*/false, filter->Pos);

            auto hasNullIU = MakeUniqueInternalIU(props.InternalVarIdx, usedIUs);
            auto nonEmptyIU = MakeUniqueInternalIU(props.InternalVarIdx, usedIUs);
            TVector<TMapElement> statsElements;
            statsElements.emplace_back(hasNullIU, MakeBinaryPredicate(">", column(rowCountIU), column(valueCountIU)));
            if (statsKeys.empty()) {
                // A global aggregate produces its row even over an empty input, so the count is the
                // only thing that tells an empty subquery apart from a non empty one.
                statsElements.emplace_back(nonEmptyIU, MakeBinaryPredicate(">", column(rowCountIU),
                                                                           MakeConstant("Uint64", "0", filter->Pos, &ctx.ExprCtx)));
            } else {
                // Grouped instead, so a group exists exactly where the subquery produced a row and
                // the count is never zero inside one. The empty bindings are the ones the join below
                // finds nothing for, and the Coalesce over this column is what turns them into false.
                statsElements.emplace_back(nonEmptyIU, MakeConstant("Bool", "true", filter->Pos, &ctx.ExprCtx));
            }
            auto statsMap = MakeIntrusive<TOpMap>(statsAggregate, filter->Pos, statsElements);

            // At most one row per binding of the correlation, so this cannot duplicate outer rows
            // either. Without a correlation the aggregate yields exactly one row and the join over
            // no keys at all is a cross join.
            TVector<std::pair<TInfoUnit, TInfoUnit>> statsJoinKeys;
            for (const auto& iu : statsKeys) {
                statsJoinKeys.push_back(std::make_pair(iu, iu));
            }
            const auto statsRenamings = MakeRenameMap(statsKeys, props.InternalVarIdx, usedIUs);

            // Keyed by the binding of the correlation again, so a null binding has to find its row
            // here as well.
            TIntrusivePtr<IOperator> statsLeft = markJoin;
            TIntrusivePtr<IOperator> statsRight = statsMap;
            statsJoinKeys = MakeNullSafeJoinKeys(statsLeft, statsRight, statsJoinKeys, filter->Pos, ctx, props, usedIUs);

            resultInput = MakeJoinWithRightRenames(statsLeft, statsRight, filter->Pos, statsKeys.empty() ? "Cross" : "Left", statsJoinKeys,
                                                   {}, statsRenamings, ctx.ExprCtx, props);

            TVector<TExpression> unknownTerms;
            unknownTerms.push_back(MakeBinaryPredicate("Coalesce", column(hasNullIU), falseConst));
            if (IsNullableIU(leftInput, subplanEntry.Tuple[0])) {
                // "x == x" is unknown for a null x and true otherwise, which is the null test this
                // has to make without knowing anything about the type of the operand.
                auto tuple = column(subplanEntry.Tuple[0]);
                auto tupleIsNull = MakeNegation(MakeBinaryPredicate("Coalesce", MakeBinaryPredicate("==", tuple, tuple), falseConst));
                unknownTerms.push_back(
                    MakeBinaryPredicate("And", tupleIsNull, MakeBinaryPredicate("Coalesce", column(nonEmptyIU), falseConst)));
            }

            auto unknown = unknownTerms[0];
            for (size_t i = 1; i < unknownTerms.size(); i++) {
                unknown = MakeBinaryPredicate("Or", unknown, unknownTerms[i]);
            }

            // "Or" is three valued, so anding the unknown with a null Bool turns it into exactly the
            // null the result needs, and the mark keeps its precedence over it. Cheaper than an If,
            // and there is no builder for one.
            auto nullBool = TExpression(MakeNullBoolNode(filter->Pos, ctx.ExprCtx), &ctx.ExprCtx, &props);
            resultElements.emplace_back(subplanIU, MakeBinaryPredicate("Or", matched, MakeBinaryPredicate("And", unknown, nullBool)));
        } else {
            resultElements.emplace_back(subplanIU, markIU, filter->Pos, &ctx.ExprCtx, &props);
        }

        newFilterInput = MakeIntrusive<TOpMap>(resultInput, filter->Pos, resultElements);
    }
    // uncorrelated EXISTS
    else {
        auto zero = MakeConstant("Uint64", "0", filter->Pos, &ctx.ExprCtx);
        auto limit = MakeIntrusive<TOpLimit>(subplan, filter->Pos, MakeConstant("Uint64", "1", filter->Pos, &ctx.ExprCtx), EOpPhase::Undefined);

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
        newFilterInput = MakeIntrusive<TOpJoin>(filter->GetInput(), map, filter->Pos, "Cross", joinKeys);
    }

    props.Subplans.Remove(subplanIU);

    // Otherwise, we need to pack the remaining conjuncts back into the filter
    return MakeIntrusive<TOpFilter>(newFilterInput, filter->Pos, TExpression(filter->FilterExpr.GetLambda(), &ctx.ExprCtx, &props));
}
}
}
