#include "kqp_rules_include.h"

#include "decorrelation/dependent_join_pushdown.h"
#include <ydb/core/kqp/opt/rbo/copy_logical_subtree.h>

namespace NKikimr {
namespace NKqp {

namespace {

// Rebuild only the unary spine owned by this call. A Replicate or another
// operator is a boundary: correlation below it needs the general DJ rules.
TIntrusivePtr<IOperator> PullUpCorrelatedFilters(const TIntrusivePtr<IOperator>& input,
    const TUnorderedIUs& dependencies, const TUnorderedIUs& outerIUs,
    TSubstitutions& captures, TVector<TExpression>& lifted, TPlanProps& props)
{
    if (input->Kind != EOperator::Filter && input->Kind != EOperator::Map && input->Kind != EOperator::AddDependencies) {
        return HasFreeCorrelation(input, dependencies) ? nullptr : input;
    }
    if (!input->GetSubplanIUs(props.Subplans).Empty()) {
        return nullptr;
    }

    auto right = PullUpCorrelatedFilters(input->GetChild(0), dependencies, outerIUs, captures, lifted, props);
    if (!right) {
        return nullptr;
    }

    if (input->Kind == EOperator::AddDependencies) {
        for (const auto& [local, capture] : CastOperator<TOpAddDependencies>(*input).GetDependencies().Items()) {
            if (!dependencies.Contains(capture.Outer) || !outerIUs.Contains(capture.Outer)) {
                return nullptr;
            }
            captures.Add(local, capture.Outer);
        }
        return right;
    }

    if (input->Kind == EOperator::Map) {
        auto& map = CastOperator<TOpMap>(*input);
        // Maps append columns, so the inner inputs of a lifted predicate stay
        // available. A map computing an outer-dependent value needs a DJ.
        if (!map.GetUsedIUs(props).IsSubsetOf(right->GetOutputIUs())) {
            return nullptr;
        }
        return MakeIntrusive<TOpMap>(right, map.Pos, map.GetMapElements(), map.NeedToPush);
    }

    auto& filter = CastOperator<TOpFilter>(*input);
    auto available = right->GetOutputIUs();
    available.UnionWith(outerIUs);
    TVector<TExpression> local;
    for (const auto& conjunct : filter.GetFilterExpression().SplitConjunct()) {
        auto predicate = conjunct.ApplyRenames(captures);
        const auto& used = predicate.GetRawInputIUs();
        if (!used.IsSubsetOf(available)) {
            return nullptr;
        }
        if (used.IsSubsetOf(right->GetOutputIUs())) {
            local.push_back(std::move(predicate));
        } else {
            lifted.push_back(std::move(predicate));
        }
    }
    if (!local.empty()) {
        right = MakeIntrusive<TOpFilter>(right, filter.Pos, MakeConjunction(local, props.PgSyntax));
    }
    return right;
}

TIntrusivePtr<IOperator> TryInlineCorrelatedExists(const TIntrusivePtr<IOperator>& left,
    const TSubplanEntry& entry, const TString& joinKind, TPositionHandle pos, TRBOContext& ctx, TPlanProps& props)
{
    // Pullup changes how often the independent right input and the predicates
    // are evaluated. Leave volatile expressions to the existing decorrelator.
    if (!CanDuplicateSubtree(*entry.Plan, props.Subplans)) {
        return nullptr;
    }

    const auto& outerIUs = left->GetOutputIUs();
    TSubstitutions captures;
    TVector<TExpression> lifted;
    auto right = PullUpCorrelatedFilters(entry.Plan, entry.DependentIUs, outerIUs, captures, lifted, props);
    if (!right) {
        return nullptr;
    }

    TJoinIUs keys;
    TVector<TExpression> filters;
    const auto& innerIUs = right->GetOutputIUs();
    for (const auto& predicate : lifted) {
        if (predicate.MaybeEquiJoinCondition()) {
            TEquiJoinCondition condition(predicate);
            const auto a = condition.GetLeftIU();
            const auto b = condition.GetRightIU();
            if (outerIUs.Contains(a) && innerIUs.Contains(b)) {
                keys.Add(a, b);
                continue;
            }
            if (outerIUs.Contains(b) && innerIUs.Contains(a)) {
                keys.Add(b, a);
                continue;
            }
        }
        // This includes outer-only predicates: especially for NOT EXISTS,
        // they belong to ON and must not filter the outer input.
        filters.push_back(predicate);
    }
    // Keep non-equi-only joins on the general path until all physical join
    // implementations support filtered semi/anti joins without hash keys.
    if (keys.Items().empty()) {
        return nullptr;
    }
    if (!filters.empty()) {
        // Expanding a filtered semi/anti join requires a non-null unique key
        // on the outer input. EXISTS callers need not have one, so use this
        // shortcut only when the filters can execute in the block join.
        if (!ctx.KqpCtx.Config->GetUseBlockHashJoin() || ctx.KqpCtx.Config->GetEnableInlineJoinFiltersAfterCBO()) {
            return nullptr;
        }
        const auto isScalarKey = [&](const IOperator& op, TInfoUnitId id) {
            const auto* type = op.Type ? op.GetIUType(id, ctx.ExprCtx) : nullptr;
            while (type && type->GetKind() == ETypeAnnotationKind::Optional) {
                type = type->Cast<TOptionalExprType>()->GetItemType();
            }
            return type && type->GetKind() == ETypeAnnotationKind::Data;
        };
        for (const auto& key : keys.Items()) {
            if (!isScalarKey(*left, key.first) || !isScalarKey(*entry.Plan, key.second)) {
                return nullptr;
            }
        }
    }
    return MakeIntrusive<TOpJoin>(left, right, pos, joinKind, keys, filters);
}

} // anonymous namespace

bool TInlineSimpleInExistsSubplanRule::QuickMatch(const TIntrusivePtr<IOperator>& input, const TPlanProps& props) const {
    if (input->Kind != EOperator::Filter || props.Subplans.Empty()) {
        return false;
    }

    for (const auto& iu : input->GetSubplanIUs(props.Subplans)) {
        const auto type = props.Subplans.At(iu).Type;
        if (type == ESubplanType::IN_SUBPLAN || type == ESubplanType::EXISTS) {
            return true;
        }
    }

    return false;
}

bool TInlineSimpleInExistsSubplanRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Filter;
}

TIntrusivePtr<IOperator> TInlineSimpleInExistsSubplanRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx, TPlanProps& props) {
    if (input->Kind != EOperator::Filter || props.PgSyntax) {
        return input;
    }

    // Check that the filter lambda is a conjunction of one or more elements
    auto filter = CastOperator<TOpFilter>(input);
    auto lambdaBody = filter->GetFilterExpression().Node->ChildPtr(1);

    if (!TCoAnd::Match(lambdaBody.Get()) && !TCoNot::Match(lambdaBody.Get()) && !TCoMember::Match(lambdaBody.Get())) {
        return input;
    }

    // Decompose the conjunction into individual conjuncts
    auto conjuncts = filter->GetFilterExpression().SplitConjunct();

    // Find the first conjunct that is a simple in or exists subplan
    bool negated = false;
    TInfoUnitId iu = 0;
    const TSubplanEntry* subplanEntry = nullptr;
    size_t conjunctIdx;

    for (conjunctIdx = 0; conjunctIdx < conjuncts.size(); conjunctIdx++) {
        auto maybeSubplan = conjuncts[conjunctIdx].GetExpressionBody();

        bool conjunctNegated = false;
        if (TCoNot::Match(maybeSubplan.Get())) {
            maybeSubplan = maybeSubplan->ChildPtr(0);
            conjunctNegated = true;
        }
        // Only a member of the row argument names an IU.
        if (TCoMember::Match(maybeSubplan.Get()) && maybeSubplan->HeadPtr().Get() == &conjuncts[conjunctIdx].Node->Head().Head()) {
            iu = GetMemberId(*maybeSubplan);
            if (const auto* entry = props.Subplans.Find(iu)) {
                if (entry->Type == ESubplanType::IN_SUBPLAN || entry->Type == ESubplanType::EXISTS) {
                    subplanEntry = entry;
                    negated = conjunctNegated;
                    break;
                }
            }
        }
    }

    if (conjunctIdx == conjuncts.size()) {
        return input;
    }

    // The join consumes the call. If another conjunct needs its value, leave
    // the call to the mark-join path, which materializes that value.
    for (size_t i = 0; i < conjuncts.size(); i++) {
        if (i != conjunctIdx && conjuncts[i].GetRawInputIUs().Contains(iu)) {
            return input;
        }
    }

    TIntrusivePtr<IOperator> join;
    Y_ENSURE(subplanEntry);
    auto subplan = CastOperator<IOperator>(subplanEntry->Plan);
    const bool useDependentJoin = HasFreeCorrelation(subplan, subplanEntry->DependentIUs);

    // If `NOT` and optional column the result could be nothing.
    if (negated && subplanEntry->Type == ESubplanType::IN_SUBPLAN && subplanEntry->Tuple.Items().size() == 1) {
        const auto& leftInput = filter->GetInput();
        if (!subplanEntry->ResultIU || IsNullableIU(leftInput, subplanEntry->Tuple.Items()[0], ctx.ExprCtx) || IsNullableIU(subplan, *subplanEntry->ResultIU, ctx.ExprCtx)) {
            return input;
        }
    }

    // Simple rewrite into left only/ left semi.
    if (subplanEntry->Type == ESubplanType::IN_SUBPLAN || useDependentJoin) {
        TIntrusivePtr<IOperator> leftJoinInput = filter->GetInput();
        auto joinKind = negated ? "LeftOnly" : "LeftSemi";
        TJoinIUs tupleJoinKeys;

        for (size_t i = 0; i < subplanEntry->Tuple.Items().size(); i++) {
            Y_ENSURE(i == 0 && subplanEntry->ResultIU, "IN requires one result binding");
            tupleJoinKeys.Add(subplanEntry->Tuple.Items()[i], *subplanEntry->ResultIU);
        }

        if (useDependentJoin) {
            const auto outerIUs = leftJoinInput->GetOutputIUs();
            for (const auto& dependency : subplanEntry->DependentIUs) {
                Y_ENSURE(outerIUs.Contains(dependency),
                         TStringBuilder() << "Correlation column " << props.InfoUnitRegistry.GetDebugName(dependency) << " is not produced by the outer plan");
            }

            if (subplanEntry->Type == ESubplanType::EXISTS) {
                join = TryInlineCorrelatedExists(leftJoinInput, *subplanEntry, joinKind, input->Pos, ctx, props);
            }
            if (!join) {
                // (Domain dependent join leftInput)
                // The outer plan and the domain read leftInput through a Replicate, the domain under fresh IDs.
                auto domain = MakeSubplanDomain(leftJoinInput, subplanEntry->DependentIUs, filter->Pos, props);
                TJoinIUs joinKeys = domain.Keys;
                TIntrusivePtr<IOperator> rightInput = std::move(domain).Bind(subplan, filter->Pos);

                // Add domain keys for join keys.
                joinKeys = MakeNullSafeJoinKeys(leftJoinInput, rightInput, joinKeys, filter->Pos, ctx, props);
                for (const auto& key : tupleJoinKeys.Items()) {
                    joinKeys.Add(key);
                }

                join = MakeIntrusive<TOpJoin>(leftJoinInput, rightInput, input->Pos, joinKind, joinKeys);
            }
        } else {
            join = MakeIntrusive<TOpJoin>(leftJoinInput, subplan, input->Pos, joinKind, tupleJoinKeys);
        }

        conjuncts.erase(conjuncts.begin() + conjunctIdx);
    }
    // EXISTS and NOT EXISTS
    else {
        auto limit = MakeIntrusive<TOpLimit>(subplan, filter->Pos, MakeConstant("Uint64", "1", filter->Pos, &ctx.ExprCtx), EOpPhase::Undefined);

        // The counted rows and their count are distinct IUs.
        auto countInput = props.InfoUnitRegistry.AddGenerated("row");
        auto countResult = props.InfoUnitRegistry.AddGenerated("row_count");
        TMapIUs countMapElements;
        auto zero = MakeConstant("Uint64", "0", filter->Pos, &ctx.ExprCtx);
        countMapElements.Add(countInput, zero);
        auto countMap = MakeIntrusive<TOpMap>(limit, filter->Pos, countMapElements);

        TAggregationIUs aggs;
        aggs.Add(countResult, TOpAggregationTraits{countInput, "count"});
        TOrderedIUs<> keyColumns;

        auto agg = MakeIntrusive<TOpAggregate>(countMap, aggs, keyColumns, EOpPhase::Final, false, filter->Pos);
        const TString compareCallable = negated ? "==" : "!=";

        auto comparePredicate = MakeBinaryPredicate(compareCallable, MakeColumnAccess(countResult, filter->Pos, &ctx.ExprCtx, &props), zero);
        TMapIUs mapElements;
        auto compareResult = props.InfoUnitRegistry.AddGenerated("exists");
        mapElements.Add(compareResult, comparePredicate);
        auto map = MakeIntrusive<TOpMap>(agg, filter->Pos, mapElements);

        TJoinIUs joinKeys;
        join = MakeIntrusive<TOpJoin>(filter->GetInput(), map, filter->Pos, "Cross", joinKeys);

        conjuncts[conjunctIdx] = MakeColumnAccess(compareResult, filter->Pos, &ctx.ExprCtx, &props);
    }

    props.Subplans.Remove(iu);
    // If there was a single conjunct, we can get rid of the filter completely
    if (conjuncts.empty()) {
        return join;
    }

    // Otherwise, we need to pack the remaining conjuncts back into the filter
    return MakeIntrusive<TOpFilter>(join, filter->Pos, MakeConjunction(conjuncts, props.PgSyntax));
}
}
}
