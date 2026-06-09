#include <ydb/core/kqp/opt/rbo/kqp_rbo_rules.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>

namespace NKikimr::NKqp {

namespace {
using namespace NYql::NNodes;

bool IsSuitableToExpandDistinctAggregation(const TIntrusivePtr<IOperator>& input) {
    if (input->GetKind() != EOperator::Aggregate) {
        return false;
    }

    const auto& aggTraitsList = CastOperator<TOpAggregate>(input)->GetAggregationTraits();
    return std::any_of(aggTraitsList.begin(), aggTraitsList.end(), [](const TOpAggregationTraits& aggTraits) { return aggTraits.Distinct; });
}

std::pair<TString, TString> GetAggFunctions(const TString& aggFunc) {
    if (aggFunc == "min" || aggFunc == "max" || aggFunc == "sum" || aggFunc == "avg" || aggFunc == "variance_1_1") {
        return std::make_pair(aggFunc, aggFunc);
    }
    if (aggFunc == "count") {
        return std::make_pair("count", "sum");
    }
    Y_ENSURE(false, "Aggregation function is not supported for splitting.");
}

TIntrusivePtr<IOperator> ExpandSingleDistinct(const TIntrusivePtr<TOpAggregate>& aggregate) {
    const auto& aggTraits = aggregate->GetAggregationTraits().front();
    TVector<TInfoUnit> distinctKeys = aggregate->GetKeyColumns();
    const auto pos = aggregate->Pos;

    // Split into distinct and original aggregation.
    TVector<TOpAggregationTraits> distinctTraitsList;
    for (const auto& key : distinctKeys) {
        distinctTraitsList.emplace_back(TOpAggregationTraits(key, "distinct", key));
    }
    distinctTraitsList.emplace_back(TOpAggregationTraits(aggTraits.OriginalColName, "distinct", aggTraits.OriginalColName));
    distinctKeys.emplace_back(aggTraits.OriginalColName);

    const TIntrusivePtr<IOperator> distinctAggregation =
        MakeIntrusive<TOpAggregate>(aggregate->GetInput(), distinctTraitsList, distinctKeys, EOpPhase::Undefined,
                                    /*distinctAll=*/true, pos);
    TOpAggregationTraits aggregationTraits = aggTraits;
    aggregationTraits.Distinct = false;
    const TVector<TOpAggregationTraits> newAggTraitsList{aggregationTraits};
    return MakeIntrusive<TOpAggregate>(distinctAggregation, newAggTraitsList, aggregate->GetKeyColumns(), EOpPhase::Undefined, /*distinctAll=*/false, pos);
}

TIntrusivePtr<IOperator> BuildDistinct(const TIntrusivePtr<IOperator>& input, TVector<TInfoUnit>&& distColumns) {
    TVector<TOpAggregationTraits> distAggTraitsList;
    for (const auto& distColumn : distColumns) {
        distAggTraitsList.emplace_back(TOpAggregationTraits(distColumn, "distinct", distColumn));
    }
    return MakeIntrusive<TOpAggregate>(input, distAggTraitsList, distColumns, EOpPhase::Undefined, /*distinctAll=*/true, input->Pos);
}

TIntrusivePtr<IOperator> BuildNullMapElementsExceptOneColumn(const TIntrusivePtr<IOperator>& input, const TTypeAnnotationNode* inputType,
                                                             const TVector<std::pair<TString, TString>>& nullColumns, TString&& exceptColumn, const TString& prefix,
                                                             TPlanProps& props, TExprContext& ctx) {
    Y_ENSURE(inputType);
    auto inputStructType = inputType->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
    TVector<TMapElement> mapElements;
    for (const auto& nullColumnPair : nullColumns) {
        const auto& originalColName = nullColumnPair.first;
        const auto& resultColName = nullColumnPair.second;
        const auto mapColName = TInfoUnit(prefix + resultColName);
        TMapElement mapElement;
        TExprNode::TPtr columnExpr;
        if (resultColName == exceptColumn) {
            // clang-format on
            columnExpr = Build<TCoLambda>(ctx, input->Pos)
                .Args({"arg"})
                .Body<TCoJust>()
                    .Input<TCoMember>()
                        .Struct("arg")
                        .Name<TCoAtom>()
                            .Value(mapColName.GetFullName())
                        .Build()
                    .Build()
                .Build()
            .Done().Ptr();
            // clang-format off
        } else {
            auto fieldType = inputStructType->FindItemType(originalColName);
            Y_ENSURE(fieldType, "Aggregation column not found in input type:" << originalColName;);
            // clang-format off
            columnExpr = Build<TCoLambda>(ctx, input->Pos)
                .Args({"arg"})
                .Body<TCoNothing>()
                    .OptionalType<TCoOptionalType>()
                        .ItemType(ExpandType(input->Pos, *fieldType, ctx))
                    .Build()
                .Build()
            .Done().Ptr();
            // clang-format on
        }
        mapElement = TMapElement(mapColName, TExpression(columnExpr, &ctx, &props));
        mapElements.emplace_back(mapElement);
    }

    if (mapElements.empty()) {
        return input;
    }

    return MakeIntrusive<TOpMap>(input, input->Pos, mapElements);
}

TIntrusivePtr<IOperator> ExpandMultiDistinct(const TIntrusivePtr<TOpAggregate>& aggregate, TPlanProps& props, TExprContext& ctx) {
    const auto& aggTraitsList = aggregate->GetAggregationTraits();
    const auto pos = aggregate->Pos;
    const auto intermediateColumnPrefix = "__intermediate_";
    TVector<std::pair<TString, TString>> nullColumns;
    for (const auto& key : aggregate->GetKeyColumns()) {
        const auto keyName = key.GetFullName();
        nullColumns.emplace_back(std::make_pair(keyName, keyName));
    }

    for (const auto& aggTraits : aggTraitsList) {
        const auto originalColName = aggTraits.OriginalColName.GetFullName();
        const auto resultColName = aggTraits.ResultColName.GetFullName();
        nullColumns.emplace_back(std::make_pair(originalColName, resultColName));
    }

    TIntrusivePtr<IOperator> unionAllResult;
    TVector<TOpAggregationTraits> finalAggTraitsList;
    for (const auto& aggTraits : aggTraitsList) {
        auto partialResult = aggregate->GetInput();
        if (aggTraits.Distinct) {
            // Aggregation column + keys.
            TVector<TInfoUnit> distColumns = aggregate->GetKeyColumns();
            distColumns.emplace_back(aggTraits.OriginalColName);
            partialResult = BuildDistinct(partialResult, std::move(distColumns));
        }

        const auto aggFunctions = GetAggFunctions(aggTraits.AggFunction);
        const auto intermediateColName = TInfoUnit(intermediateColumnPrefix + aggTraits.ResultColName.GetFullName());
        const auto partialAggTraits = TOpAggregationTraits(aggTraits.OriginalColName, aggFunctions.first, intermediateColName);
        const auto finalAggTraits = TOpAggregationTraits(intermediateColName, aggFunctions.second, aggTraits.ResultColName);
        TVector<TOpAggregationTraits> partialAggregationTraitsList{partialAggTraits};
        finalAggTraitsList.emplace_back(finalAggTraits);

        partialResult = MakeIntrusive<TOpAggregate>(partialResult, partialAggregationTraitsList, aggregate->GetKeyColumns(), EOpPhase::Intermediate,
                                                    /*distinctAll=*/false, pos);

        partialResult = BuildNullMapElementsExceptOneColumn(partialResult, aggregate->GetInput()->Type, nullColumns, aggTraits.ResultColName.GetFullName(),
                                                            intermediateColumnPrefix, props, ctx);

        if (unionAllResult) {
            unionAllResult = MakeIntrusive<TOpUnionAll>(unionAllResult, partialResult, aggregate->Pos);
        } else {
            unionAllResult = partialResult;
        }
    }

    return MakeIntrusive<TOpAggregate>(unionAllResult, finalAggTraitsList, aggregate->GetKeyColumns(), EOpPhase::Final, /*distinctAll=*/false, pos);
}

} // anonymous namespace

TIntrusivePtr<IOperator> TExpandDistinctAggregationRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& rboCtx, TPlanProps& props) {
    if (!IsSuitableToExpandDistinctAggregation(input)) {
        return input;
    }

    const auto aggregate = CastOperator<TOpAggregate>(input);
    if (aggregate->GetAggregationTraits().size() == 1) {
       return ExpandSingleDistinct(aggregate);
    }
    return ExpandMultiDistinct(aggregate, props, rboCtx.ExprCtx);
}

} // namespace NKikimr::NKqp
