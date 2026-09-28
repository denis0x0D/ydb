#pragma once

#include "kqp_operator.h"

namespace NKikimr::NKqp {

// A table which a lookup join probes instead of the right side read: the read table itself, or the main
// table or a covering index the read is redirected to.
struct TLookupJoinTarget {
    TIntrusivePtr<NYql::TKikimrTableMetadata> Metadata;
    // Leading key columns pinned by the read predicate, they become a constant prefix of the lookup keys.
    const TOpRead::TPointPrefix* PointPrefix = nullptr;
};

// Maps the right side join keys to physical columns of the read, because the read can rename its columns.
std::optional<THashSet<TString>> GetLookupJoinKeyColumns(const TOpRead& read, const TVector<TInfoUnit>& rightJoinKeys);

// Builds a filter which a lookup join applies to fetched rows: the predicate pushed into the read and the filter above it.
std::optional<TExpression> BuildFetchedRowFilter(const TOpRead& read, const TIntrusivePtr<TOpFilter>& filter, bool& supported);

// Chooses a table to probe for the read: the one whose key has the longest prefix of point prefix columns followed by join keys.
// CBO and the rewrite rule have to use it both, so the rule can rewrite every lookup join chosen by CBO.
std::optional<TLookupJoinTarget> ChooseLookupJoinTarget(const TOpRead& read, const THashSet<TString>& joinKeyColumns, bool innerJoin,
                                                        const NOpt::TKqpOptimizeContext& kqpCtx);

TExprNode::TPtr BuildTableCallable(const NYql::TKikimrTableMetadata& meta, TPositionHandle pos, TExprContext& ctx);

} // namespace NKikimr::NKqp
