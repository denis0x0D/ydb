#include "dependent_join.h"

#include "kqp_rules_include.h"
#include <ydb/core/kqp/opt/rbo/map_renames.h>

namespace NKikimr {
namespace NKqp {

namespace {

bool ColumnsIntersect(const TVector<TInfoUnit>& left, const TVector<TInfoUnit>& right) {
    for (const auto& iu : left) {
        if (ContainsInfoUnit(right, iu)) {
            return true;
        }
    }
    return false;
}

bool HasOperatorBelow(const TIntrusivePtr<IOperator>& op, EOperator kind) {
    if (op->Kind == kind) {
        return true;
    }
    for (const auto& child : op->Children) {
        if (HasOperatorBelow(child, kind)) {
            return true;
        }
    }
    return false;
}

TIntrusivePtr<TOpDependentJoin> PushInto(const TIntrusivePtr<TOpDependentJoin>& dependentJoin, const TIntrusivePtr<IOperator>& newInput) {
    return MakeIntrusive<TOpDependentJoin>(dependentJoin->GetDomain(), newInput, dependentJoin->Dependencies, dependentJoin->Pos);
}

TIntrusivePtr<IOperator> MakeCrossJoinWithDomain(const TIntrusivePtr<TOpDependentJoin>& dependentJoin, const TIntrusivePtr<IOperator>& input) {
    return MakeIntrusive<TOpJoin>(dependentJoin->GetDomain(), input, dependentJoin->Pos, "Cross",
                                  TVector<std::pair<TInfoUnit, TInfoUnit>>{});
}

// The columns of the domain that the operator does not already produce. They have to be added to the
// output list of operators which enumerate their outputs explicitly, i.e. aggregates and unions.
TVector<TInfoUnit> MissingDomainColumns(const TVector<TInfoUnit>& dependencies, const TVector<TInfoUnit>& present) {
    TVector<TInfoUnit> result;
    for (const auto& iu : dependencies) {
        if (!ContainsInfoUnit(present, iu)) {
            result.push_back(iu);
        }
    }
    return result;
}

} // anonymous namespace

TIntrusivePtr<TOpAggregate> MakeDomainProjection(const TIntrusivePtr<IOperator>& input, const TVector<TInfoUnit>& columns, TPositionHandle pos) {
    Y_ENSURE(!columns.empty(), "Domain of a dependent join cannot be empty");

    TVector<TOpAggregationTraits> traits;
    traits.reserve(columns.size());
    for (const auto& iu : columns) {
        traits.emplace_back(iu, "distinct", iu);
    }
    return MakeIntrusive<TOpAggregate>(input, traits, columns, EOpPhase::Undefined, /*distinctAll=*/true, pos);
}

bool HasFreeCorrelation(const TIntrusivePtr<IOperator>& op, const TVector<TInfoUnit>& correlatedColumns) {
    if (op->Kind == EOperator::AddDependencies) {
        if (ColumnsIntersect(CastOperator<TOpAddDependencies>(op)->Dependencies, correlatedColumns)) {
            return true;
        }
    }

    for (const auto& child : op->Children) {
        if (HasFreeCorrelation(child, correlatedColumns)) {
            return true;
        }
    }

    return false;
}

/**
 * D ⋈ᵈᵉᵖ AddDependencies(T) = D × T
 */

bool TRemoveDependenciesUnderDependentJoinRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::DependentJoin;
}

TIntrusivePtr<IOperator> TRemoveDependenciesUnderDependentJoinRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx,
                                                                                       TPlanProps& props) {
    Y_UNUSED(ctx);
    Y_UNUSED(props);

    auto dependentJoin = CastOperator<TOpDependentJoin>(input);
    auto body = dependentJoin->GetInput();
    if (body->Kind != EOperator::AddDependencies) {
        return input;
    }

    auto addDependencies = CastOperator<TOpAddDependencies>(body);

    // Every column this operator injects has to be covered by the domain, otherwise removing it
    // would leave an unbound reference behind.
    if (!IUIsSubset(addDependencies->Dependencies, dependentJoin->Dependencies)) {
        return input;
    }

    // A nested correlation boundary below this one would stay unbound after the rewrite.
    if (HasFreeCorrelation(addDependencies->GetInput(), dependentJoin->Dependencies)) {
        return input;
    }

    return MakeCrossJoinWithDomain(dependentJoin, addDependencies->GetInput());
}

/**
 * D ⋈ᵈᵉᵖ T = D × T, when T has no free correlated variables
 */

bool TDependentJoinToCrossJoinRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::DependentJoin;
}

TIntrusivePtr<IOperator> TDependentJoinToCrossJoinRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx, TPlanProps& props) {
    Y_UNUSED(ctx);
    Y_UNUSED(props);

    auto dependentJoin = CastOperator<TOpDependentJoin>(input);
    if (HasFreeCorrelation(dependentJoin->GetInput(), dependentJoin->Dependencies)) {
        return input;
    }

    return MakeCrossJoinWithDomain(dependentJoin, dependentJoin->GetInput());
}

/**
 * D ⋈ᵈᵉᵖ σ_{⋀ dᵢ = rᵢ ∧ p}(AddDependencies_{d}(T)) = σ_p(Π_{T.*, dᵢ := rᵢ}(T))
 */

bool TEliminateDependentJoinDomainRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::DependentJoin;
}

TIntrusivePtr<IOperator> TEliminateDependentJoinDomainRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx,
                                                                                TPlanProps& props) {
    auto dependentJoin = CastOperator<TOpDependentJoin>(input);
    auto body = dependentJoin->GetInput();
    if (body->Kind != EOperator::Filter) {
        return input;
    }

    auto filter = CastOperator<TOpFilter>(body);
    if (filter->GetInput()->Kind != EOperator::AddDependencies) {
        return input;
    }

    auto addDependencies = CastOperator<TOpAddDependencies>(filter->GetInput());
    auto correlatedInput = addDependencies->GetInput();

    // Only columns the domain provides can be bound here.
    if (!IUIsSubset(addDependencies->Dependencies, dependentJoin->Dependencies)) {
        return input;
    }

    // A correlation boundary further down would stay unbound once the domain is gone.
    if (HasFreeCorrelation(correlatedInput, dependentJoin->Dependencies)) {
        return input;
    }

    const auto innerIUs = correlatedInput->GetOutputIUs();

    // Split the predicate into the equalities that bind a correlated column to a column of the
    // subplan and everything else. Only the first equality per correlated column becomes a binding,
    // a second one is a condition on the bound value and stays in the predicate.
    THashMap<TInfoUnit, TInfoUnit, TInfoUnit::THashFunction> bindings;
    TVector<TExpression> restConjuncts;

    for (const auto& conj : filter->FilterExpr.SplitConjunct()) {
        std::optional<std::pair<TInfoUnit, TInfoUnit>> binding;

        if (conj.MaybeEquiJoinCondition()) {
            TEquiJoinCondition condition(conj);
            const auto left = condition.GetLeftIU();
            const auto right = condition.GetRightIU();

            if (ContainsInfoUnit(addDependencies->Dependencies, left) && ContainsInfoUnit(innerIUs, right)) {
                binding = std::make_pair(left, right);
            } else if (ContainsInfoUnit(addDependencies->Dependencies, right) && ContainsInfoUnit(innerIUs, left)) {
                binding = std::make_pair(right, left);
            }
        }

        if (binding && bindings.emplace(binding->first, binding->second).second) {
            continue;
        }
        restConjuncts.push_back(conj);
    }

    for (const auto& iu : addDependencies->Dependencies) {
        // A correlated column the predicate does not bind still needs its domain values.
        if (!bindings.contains(iu)) {
            return input;
        }
        // The subplan produces a column of that name itself, the binding would shadow it.
        if (ContainsInfoUnit(innerIUs, iu)) {
            return input;
        }
    }

    // The binding is a copy, not a rename: the subplan column keeps its own name because the rest of
    // the subplan still refers to it.
    TVector<TMapElement> bindingElements;
    bindingElements.reserve(addDependencies->Dependencies.size());
    for (const auto& iu : addDependencies->Dependencies) {
        bindingElements.emplace_back(iu, MakeColumnAccess(bindings.at(iu), filter->Pos, &ctx.ExprCtx, &props));
    }

    TIntrusivePtr<IOperator> newBody = MakeIntrusive<TOpMap>(correlatedInput, filter->Pos, bindingElements);
    if (!restConjuncts.empty()) {
        newBody = MakeIntrusive<TOpFilter>(newBody, filter->Pos, MakeConjunction(restConjuncts, props.PgSyntax));
    }

    // Rows whose binding is not a value of the domain are kept, they are discarded by the join that
    // puts the subplan result back into the outer plan. Rows with a null binding are dropped by that
    // join as well, which is what the equality predicate did before.
    auto remainingDependencies = MissingDomainColumns(dependentJoin->Dependencies, addDependencies->Dependencies);
    if (remainingDependencies.empty()) {
        return newBody;
    }

    // Some domain columns are used above the marker, so a domain restricted to those stays behind.
    auto domain = dependentJoin->GetDomain();
    if (domain->Kind != EOperator::Aggregate || !CastOperator<TOpAggregate>(domain)->IsDistinctAll()) {
        return input;
    }

    auto smallerDomain = MakeDomainProjection(CastOperator<TOpAggregate>(domain)->GetInput(), remainingDependencies, domain->Pos);
    return MakeIntrusive<TOpDependentJoin>(smallerDomain, newBody, remainingDependencies, dependentJoin->Pos);
}

/**
 * D ⋈ᵈᵉᵖ σ_p(T) = σ_p(D ⋈ᵈᵉᵖ T)
 */

bool TPushDependentJoinThroughFilterRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::DependentJoin;
}

TIntrusivePtr<IOperator> TPushDependentJoinThroughFilterRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx,
                                                                                 TPlanProps& props) {
    auto dependentJoin = CastOperator<TOpDependentJoin>(input);
    auto body = dependentJoin->GetInput();
    if (body->Kind != EOperator::Filter) {
        return input;
    }

    auto filter = CastOperator<TOpFilter>(body);
    auto newInput = PushInto(dependentJoin, filter->GetInput());
    return MakeIntrusive<TOpFilter>(newInput, filter->Pos, TExpression(filter->FilterExpr.GetLambda(), &ctx.ExprCtx, &props));
}

/**
 * D ⋈ᵈᵉᵖ Π_e(T) = Π_e(D ⋈ᵈᵉᵖ T)
 */

bool TPushDependentJoinThroughMapRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::DependentJoin;
}

TIntrusivePtr<IOperator> TPushDependentJoinThroughMapRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx, TPlanProps& props) {
    Y_UNUSED(ctx);
    Y_UNUSED(props);

    auto dependentJoin = CastOperator<TOpDependentJoin>(input);
    auto body = dependentJoin->GetInput();
    if (body->Kind != EOperator::Map) {
        return input;
    }

    auto map = CastOperator<TOpMap>(body);

    // Maps in this IR are additive, they never drop a column except for the source of a rename. So
    // the domain columns survive the map for free, unless the map renames one of them away or
    // produces a column that shadows one.
    const auto renameSources = map->GetRenameSources();
    for (const auto& iu : dependentJoin->Dependencies) {
        if (renameSources.contains(iu)) {
            return input;
        }
    }
    for (const auto& mapElement : map->MapElements) {
        if (ContainsInfoUnit(dependentJoin->Dependencies, mapElement.GetElementName())) {
            return input;
        }
    }

    auto newInput = PushInto(dependentJoin, map->GetInput());
    return MakeIntrusive<TOpMap>(newInput, map->Pos, map->MapElements, map->IsOrdered());
}

/**
 * D ⋈ᵈᵉᵖ Γ_{g;a}(T) = Γ_{g ∪ A(D);a}(D ⋈ᵈᵉᵖ T)
 */

bool TPushDependentJoinThroughAggregateRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::DependentJoin;
}

TIntrusivePtr<IOperator> TPushDependentJoinThroughAggregateRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx,
                                                                                    TPlanProps& props) {
    Y_UNUSED(ctx);
    Y_UNUSED(props);

    auto dependentJoin = CastOperator<TOpDependentJoin>(input);
    auto body = dependentJoin->GetInput();
    if (body->Kind != EOperator::Aggregate) {
        return input;
    }

    auto aggregate = CastOperator<TOpAggregate>(body);

    // A physical aggregate is already split into partial and final parts, adding grouping keys to
    // one half only would break the pair.
    if (aggregate->GetAggregationPhase() != EOpPhase::Undefined) {
        return input;
    }

    // Grouping by the domain columns in addition to the original keys evaluates the aggregate once
    // per binding of the correlated columns.
    auto newKeyColumns = MissingDomainColumns(dependentJoin->Dependencies, aggregate->KeyColumns);
    newKeyColumns.insert(newKeyColumns.end(), aggregate->KeyColumns.begin(), aggregate->KeyColumns.end());

    auto newTraits = aggregate->AggregationTraitsList;
    if (aggregate->IsDistinctAll()) {
        // A distinct-all aggregate outputs its aggregate results only, the keys are just the state.
        // The domain columns have to be added to the output as distinct traits as well, which keeps
        // the operator a distinct projection over the extended key list.
        TVector<TInfoUnit> resultColumns;
        resultColumns.reserve(newTraits.size());
        for (const auto& traits : newTraits) {
            resultColumns.push_back(traits.ResultColName);
        }

        TVector<TOpAggregationTraits> domainTraits;
        for (const auto& iu : MissingDomainColumns(dependentJoin->Dependencies, resultColumns)) {
            domainTraits.emplace_back(iu, "distinct", iu);
        }
        domainTraits.insert(domainTraits.end(), newTraits.begin(), newTraits.end());
        newTraits = std::move(domainTraits);
    }

    auto newInput = PushInto(dependentJoin, aggregate->GetInput());
    return MakeIntrusive<TOpAggregate>(newInput, newTraits, newKeyColumns, aggregate->GetAggregationPhase(), aggregate->IsDistinctAll(),
                                       aggregate->Pos);
}

/**
 * D ⋈ᵈᵉᵖ (T₁ ∪ T₂) = (D ⋈ᵈᵉᵖ T₁) ∪ (D ⋈ᵈᵉᵖ T₂)
 */

bool TPushDependentJoinThroughUnionAllRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::DependentJoin;
}

TIntrusivePtr<IOperator> TPushDependentJoinThroughUnionAllRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx,
                                                                                   TPlanProps& props) {
    Y_UNUSED(ctx);
    Y_UNUSED(props);

    auto dependentJoin = CastOperator<TOpDependentJoin>(input);
    auto body = dependentJoin->GetInput();
    if (body->Kind != EOperator::UnionAll) {
        return input;
    }

    auto unionAll = CastOperator<TOpUnionAll>(body);

    // The union already carries columns with the names of the domain, pushing the domain in would
    // produce two different columns with the same name.
    if (ColumnsIntersect(dependentJoin->Dependencies, unionAll->Columns)) {
        return input;
    }

    // The union enumerates its output columns explicitly, so the domain columns have to be declared.
    auto newColumns = dependentJoin->Dependencies;
    newColumns.insert(newColumns.end(), unionAll->Columns.begin(), unionAll->Columns.end());

    TVector<TIntrusivePtr<IOperator>> newInputs;
    newInputs.reserve(unionAll->Children.size());
    for (const auto& child : unionAll->Children) {
        newInputs.push_back(PushInto(dependentJoin, child));
    }

    return MakeIntrusive<TOpUnionAll>(newInputs, unionAll->Pos, newColumns, unionAll->Ordered);
}

/**
 * D ⋈ᵈᵉᵖ (T₁ ⋈_p T₂)
 */

bool TPushDependentJoinThroughJoinRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::DependentJoin;
}

TIntrusivePtr<IOperator> TPushDependentJoinThroughJoinRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx, TPlanProps& props) {
    auto dependentJoin = CastOperator<TOpDependentJoin>(input);
    auto body = dependentJoin->GetInput();
    if (body->Kind != EOperator::Join) {
        return input;
    }

    auto join = CastOperator<TOpJoin>(body);
    const auto& dependencies = dependentJoin->Dependencies;

    const bool leftCorrelated = HasFreeCorrelation(join->GetLeftInput(), dependencies);
    const bool rightCorrelated = HasFreeCorrelation(join->GetRightInput(), dependencies);

    // The base case rule handles a join without any correlation below it.
    if (!leftCorrelated && !rightCorrelated) {
        return input;
    }

    const auto joinKind = GetValidJoinKind(join->JoinKind);
    const bool innerLike = joinKind == "Inner" || joinKind == "Cross";

    if (leftCorrelated && !rightCorrelated) {
        // Only the left side needs the domain. The join kind must keep the left columns, otherwise
        // the domain would not reach the output.
        if (!JoinOutputsLeft(joinKind)) {
            return input;
        }
        auto newLeft = PushInto(dependentJoin, join->GetLeftInput());
        return MakeIntrusive<TOpJoin>(newLeft, join->GetRightInput(), join->Pos, join->JoinKind, join->JoinKeys, join->JoinFilters);
    }

    if (!leftCorrelated && rightCorrelated) {
        // Only the right side needs the domain. Restricted to inner-like joins: for an outer join
        // the domain columns would become optional and rows without a match would carry a null
        // binding, which is not what the dependent join means.
        if (!innerLike) {
            return input;
        }
        auto newRight = PushInto(dependentJoin, join->GetRightInput());
        return MakeIntrusive<TOpJoin>(join->GetLeftInput(), newRight, join->Pos, join->JoinKind, join->JoinKeys, join->JoinFilters);
    }

    // Both sides are correlated, so both get their own copy of the domain and the join has to
    // additionally equate the two copies. The equality becomes a join condition, which an outer join
    // evaluates without dropping its preserved rows, so the domain columns of the result can come
    // from the left copy as long as the left rows survive the join. A right or full outer join null
    // extends them and would lose the binding.
    if (!innerLike && joinKind != "Left" && joinKind != "LeftSemi" && joinKind != "LeftOnly") {
        return input;
    }

    auto newLeft = PushInto(dependentJoin, join->GetLeftInput());
    auto newRight = PushInto(dependentJoin, join->GetRightInput());

    // The right copy of the domain is renamed so that the two bindings can be compared.
    TInfoUnitSet usedIUs;
    NMapRenames::AddUsedIUs(usedIUs, newLeft->GetOutputIUs());
    NMapRenames::AddUsedIUs(usedIUs, newRight->GetOutputIUs());
    const auto rightRenames = NMapRenames::MakeRenameMap(dependencies, props.InternalVarIdx, usedIUs);

    auto joinKeys = join->JoinKeys;
    for (const auto& iu : dependencies) {
        joinKeys.emplace_back(iu, iu);
    }

    // A cross join cannot carry the equality, it becomes an inner join. Every other kind is kept.
    const TString newJoinKind = joinKind == "Cross" ? "Inner" : joinKind;

    return NMapRenames::MakeJoinWithRightRenames(newLeft, newRight, join->Pos, newJoinKind, joinKeys, join->JoinFilters, rightRenames,
                                                 ctx.ExprCtx, props);
}

/**
 * Diagnostic rule, fires only if no other dependent join rule could be applied
 */

bool TDependentJoinNotSupportedRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::DependentJoin;
}

TIntrusivePtr<IOperator> TDependentJoinNotSupportedRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx, TPlanProps& props) {
    Y_UNUSED(ctx);
    Y_UNUSED(props);

    auto dependentJoin = CastOperator<TOpDependentJoin>(input);
    auto body = dependentJoin->GetInput();

    // A nested dependent join below this one has to be decorrelated first, this one gets another
    // chance once the inner one is gone.
    if (HasOperatorBelow(body, EOperator::DependentJoin)) {
        return input;
    }

    Y_ENSURE(false, "Cannot decorrelate the subquery, correlation cannot be pushed through " << body->GetExplainName());
    return input;
}

} // namespace NKqp
} // namespace NKikimr
