#pragma once

#include "kqp_operator.h"

#include <ydb/core/kqp/opt/rbo/protos/kqp_rbo_snapshot.pb.h>

namespace NKikimr::NKqp {

/**
 * PreCboSnapshot: the plan between the early RBO stages and "Prepare for CBO".
 *
 * A root keeps its operators, information-unit registry, subplans and every
 * expression they hold. Derived state (types, metadata, statistics, lineage,
 * liveness, cached output sets, parent edges) is recomputed after restore.
 */

// Throws if the plan holds state a snapshot does not carry: operators created
// only at or after CBO, or physical properties.
NKikimrKqpRbo::TPlanRoot SerializePlanRoot(TOpRoot& root, TExprContext& ctx);

// Rebuilds a root in the current context. Expressions typed at serialization are
// annotated again with ctx.TypeAnnTransformer. Throws if the root is malformed.
TIntrusivePtr<TOpRoot> DeserializePlanRoot(const NKikimrKqpRbo::TPlanRoot& proto, TRBOContext& ctx);

} // namespace NKikimr::NKqp
