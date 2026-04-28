#include "kqp_rules_include.h"

namespace NKikimr {
namespace NKqp {

namespace {
const THashSet<TString> AllowedAggFunction{"sum", "min", "max"};

bool CanPushAggregateToStage(const TIntrusivePtr<TOpAggregate>& aggregate, const TIntrusivePtr<IOperator>& input) {
    const auto aggregateStageId = *aggregate->Props.StageId;
    const auto inputStageId = *input->Props.StageId;
    return aggregateStageId != inputStageId && input->IsSingleConsumer() && input->GetKind() != EOperator::Source;
}

bool AggregationTraitsAreValidForPropagation(const TVector<TOpAggregationTraits>& aggregationTraitsList) {
    for (const auto& aggTraits : aggregationTraitsList) {
        if (!AllowedAggFunction.contains(aggTraits.AggFunction)) {
            return false;
        }
    }
    return true;
}

bool IsSuitableToPropagateAggregateThroughStage(const TIntrusivePtr<IOperator>& input) {
    if (input->GetKind() != EOperator::Aggregate) {
        return false;
    }

    const auto aggregate = CastOperator<TOpAggregate>(input);
    const auto& aggTraits = aggregate->GetAggregationTraits();
    const auto distinctAll = aggregate->IsDistinctAll();

    return aggregate->GetAggregationPhase() != EOpPhase::Final && AggregationTraitsAreValidForPropagation(aggTraits) && !distinctAll &&
           !aggregate->GetKeyColumns().empty();
}

TIntrusivePtr<TOpAggregate> EmitFinalAndIntermediateAggregates(const TIntrusivePtr<TOpAggregate>& aggregate) {
    const auto pos = aggregate->Pos;
    const auto props = aggregate->Props;
    const auto& aggregationTraitsList = aggregate->GetAggregationTraits();
    const auto& aggKeys = aggregate->GetKeyColumns();
    const auto distinctAll = aggregate->IsDistinctAll();

    TVector<TOpAggregationTraits> intermediateTraits;
    TVector<TOpAggregationTraits> finalTraits;
    // Here we want to split aggregate to final and intermediate.
    for (const auto& originalTraits : aggregationTraitsList) {
        const auto& originalColName = originalTraits.OriginalColName;
        const auto& aggFunc = originalTraits.AggFunction;
        const auto& resultColName = originalTraits.ResultColName;
        const auto newIntermediateName = TInfoUnit("__inter" + resultColName.GetFullName());
        intermediateTraits.emplace_back(originalColName, aggFunc, newIntermediateName);
        finalTraits.emplace_back(newIntermediateName, aggFunc, resultColName);
    }

    const auto intermediate = MakeIntrusive<TOpAggregate>(aggregate->GetInput(), intermediateTraits, aggKeys, EOpPhase::Intermediate, distinctAll, props, pos);
    return MakeIntrusive<TOpAggregate>(intermediate, finalTraits, aggKeys, EOpPhase::Final, distinctAll, props, pos);
}

bool CanEliminateMapAndPushToReadStage(const TIntrusivePtr<TOpAggregate>& aggregate, const TIntrusivePtr<IOperator>& input, const TPlanProps& props) {
    const auto mapStageId = *input->Props.StageId;
    const auto aggregateStageId = *aggregate->Props.StageId;
    if (mapStageId != aggregateStageId) {
        return false;
    }
    if (input->GetKind() != EOperator::Map || !input->IsSingleConsumer()) {
        return false;
    }
    const auto map = CastOperator<TOpMap>(input);
    const auto mapInput = map->GetInput();
    if (mapInput->GetKind() != EOperator::Source || !mapInput->IsSingleConsumer()) {
        return false;
    }

    for (const auto& element : map->GetMapElements()) {
        if (!element.IsRename() || (element.GetRename().GetFullName() != element.GetElementName().GetFullName())) {
            return false;
        }
    }

    const auto read = CastOperator<TOpRead>(mapInput);
    return read->GetTableStorageType() == NYql::EStorageType::ColumnStorage && props.StageGraph.IsPossibleToEraseStage(mapStageId);
}

} // namespace

TIntrusivePtr<IOperator> TPropagateAggregateThroughStageRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx, TPlanProps& props) {
    Y_UNUSED(ctx);
    Y_UNUSED(props);

    if (!IsSuitableToPropagateAggregateThroughStage(input)) {
        return input;
    }

    const auto aggregate = CastOperator<TOpAggregate>(input);
    if (aggregate->GetAggregationPhase() == EOpPhase::Undefined) {
        return EmitFinalAndIntermediateAggregates(aggregate);
    }

    const auto aggInput = aggregate->GetInput();
    if (CanPushAggregateToStage(aggregate, aggInput)) {
        auto props = aggregate->Props;
        props.StageId = aggInput->Props.StageId;
        return MakeIntrusive<TOpAggregate>(aggInput, aggregate->GetAggregationTraits(), aggregate->GetKeyColumns(), EOpPhase::Intermediate,
                                           aggregate->IsDistinctAll(), props, aggregate->Pos);
    } else if (CanEliminateMapAndPushToReadStage(aggregate, aggInput, props)) {
        const auto read = CastOperator<TOpRead>(CastOperator<TOpMap>(aggInput)->GetInput());
        //const auto aggregateStageId = aggregate->Props.StageId;
        const auto readStageId = read->Props.StageId;
        const auto newRead = MakeIntrusive<TOpRead>(read->Alias, read->Columns, read->OutputIUs, read->StorageType, read->TableCallable, read->OlapFilterLambda,
                                                    read->Limit, read->GetRanges(), read->OriginalPredicate, read->SortDir, read->Props, read->Pos);
        const auto newAggregate = MakeIntrusive<TOpAggregate>(read, aggregate->GetAggregationTraits(), aggregate->GetKeyColumns(), EOpPhase::Intermediate,
                                                              aggregate->IsDistinctAll(), aggregate->Props, aggregate->Pos);
        newAggregate->Props.StageId = readStageId;
        const auto newConnection = MakeIntrusive<TShuffleConnection>(newAggregate->GetKeyColumns(), /*ouputIndex=*/0U);
        props.StageGraph.EraseStage(*aggInput->Props.StageId, newConnection);
        return newAggregate;
    }
    return input;
}
} // namespace NKqp
} // namespace NKikimr