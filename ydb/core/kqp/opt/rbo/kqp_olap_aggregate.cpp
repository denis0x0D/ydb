#include "kqp_olap_aggregate.h"

#include <yql/essentials/core/yql_expr_optimize.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>

namespace NKikimr::NKqp {

using namespace NYql;
using namespace NYql::NNodes;

namespace {

// Column shards compute AVG as SUM and COUNT, which build its intermediate state.
const THashSet<TString> OlapAggregateFunctions{"count", "sum", "min", "max", "avg", "some"};

const TDataExprType* GetDataType(const TTypeAnnotationNode* type) {
    if (!type) {
        return nullptr;
    }
    const auto* itemType = &RemoveOptionality(*type);
    return itemType->GetKind() == ETypeAnnotationKind::Data ? itemType->Cast<TDataExprType>() : nullptr;
}

// Column shards do not aggregate decimals. SUM and AVG states are numeric:
// AVG converts the sum to its Double accumulator.
bool IsOlapAggregateType(const TString& aggFunction, const TTypeAnnotationNode* type) {
    const auto* dataType = GetDataType(type);
    if (!dataType || IsDataTypeDecimal(dataType->GetSlot())) {
        return false;
    }
    if (aggFunction == "sum" || aggFunction == "avg") {
        return IsDataTypeNumeric(dataType->GetSlot());
    }
    return true;
}

bool HasOlapProgramBlockingAggregate(const TOpRead& read) {
    if (!read.OlapFilterLambda) {
        return false;
    }
    // Projections overwrite read columns, while column shards aggregate the
    // stored values. A read program aggregates or deduplicates at most once.
    return !!FindNode(read.OlapFilterLambda, [](const TExprNode::TPtr& node) {
        return TKqpOlapProjections::Match(node.Get()) || TKqpOlapAgg::Match(node.Get()) || TKqpOlapDistinct::Match(node.Get());
    });
}

// The read column an aggregation input refers to, or std::nullopt for a
// literal that COUNT(*) counts. Returns false for any other input.
bool ResolveAggregationColumn(const TOlapAggregateInput& input, const TOpAggregationTraits& traits, std::optional<TInfoUnitId>& column) {
    const auto& columns = input.Read->GetColumns();
    if (columns.Contains(traits.Input)) {
        column = traits.Input;
        return true;
    }

    const auto* element = input.Map ? input.Map->FindOutputElement(traits.Input) : nullptr;
    if (!element) {
        return false;
    }
    if (element->IsColumnAccess()) {
        column = element->GetColumnAccess();
        return columns.Contains(*column);
    }
    // COUNT(*) counts Void or another non-null literal.
    const auto body = element->GetExpression().GetExpressionBody();
    if (traits.AggFunction == "count" && (TCoVoid::Match(body.Get()) || TCoDataCtor::Match(body.Get()))) {
        column = std::nullopt;
        return true;
    }
    return false;
}

} // anonymous namespace

std::optional<TOlapAggregateInput> MatchOlapAggregateInput(const TOpAggregate& aggregate, TExprContext& ctx) {
    const auto& aggregations = aggregate.GetAggregationTraits();
    if (aggregate.IsDistinctAll() || aggregations.Keys().Empty()) {
        return std::nullopt;
    }

    TOlapAggregateInput result;
    auto* input = aggregate.GetInput().Get();
    if (input->Kind == EOperator::Map) {
        result.Map = CastOperator<TOpMap>(input);
        input = result.Map->GetInput().Get();
    }
    if (input->Kind != EOperator::Source) {
        return std::nullopt;
    }
    result.Read = CastOperator<TOpRead>(input);

    const auto& read = *result.Read;
    // The read program computes the aggregate, so all of them share a stage.
    const auto stageId = aggregate.Props.StageId;
    if (!stageId || read.Props.StageId != stageId || (result.Map && result.Map->Props.StageId != stageId)) {
        return std::nullopt;
    }
    if (read.GetTableStorageType() != NYql::EStorageType::ColumnStorage || read.Limit || read.SortDir != ESortDir::None
        || HasOlapProgramBlockingAggregate(read)) {
        return std::nullopt;
    }

    // Maps only append definitions, so read keys reach the aggregate as is.
    for (const auto key : aggregate.GetKeyColumns().Items()) {
        if (!read.GetColumns().Contains(key) || !GetDataType(read.GetIUType(key, ctx))) {
            return std::nullopt;
        }
    }

    for (const auto& [output, traits] : aggregations.Items()) {
        if (traits.Distinct || traits.Unwrap || !OlapAggregateFunctions.contains(traits.AggFunction)) {
            return std::nullopt;
        }

        std::optional<TInfoUnitId> column;
        if (!ResolveAggregationColumn(result, traits, column)) {
            return std::nullopt;
        }
        if (column && !IsOlapAggregateType(traits.AggFunction, read.GetIUType(*column, ctx))) {
            return std::nullopt;
        }
        result.Columns.emplace(output, column);
    }

    return result;
}

} // namespace NKikimr::NKqp
