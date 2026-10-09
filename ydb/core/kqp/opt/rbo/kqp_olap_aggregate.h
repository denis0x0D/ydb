#pragma once

#include "kqp_operator.h"

#include <optional>

namespace NKikimr::NKqp {

// The input of an intermediate aggregate that column shards compute in the
// program of a column table read.
struct TOlapAggregateInput {
    TOpRead* Read = nullptr;
    // An optional Map between the read and the aggregate. The aggregate uses
    // only its read column copies and the literals COUNT(*) counts.
    TOpMap* Map = nullptr;
    // Aggregation output -> aggregated read column, std::nullopt counts rows.
    THashMap<TInfoUnitId, std::optional<TInfoUnitId>> Columns;
};

// Match an aggregate whose aggregations and keys column shards can compute
// over the read below it. The aggregate phase and pushdown settings are the
// caller's concern.
std::optional<TOlapAggregateInput> MatchOlapAggregateInput(const TOpAggregate& aggregate, TExprContext& ctx);

} // namespace NKikimr::NKqp
