#pragma once

#include <ydb/core/kqp/opt/rbo/kqp_operator.h>
#include <ydb/core/kqp/opt/rbo/kqp_rbo_context.h>

namespace NKikimr {
namespace NKqp {

/**
 * Builds the domain D of a dependent join: the distinct values of the correlated columns the
 * correlated subplan has to be evaluated for. In the terms of Neumann/Kemper this is
 * D := Π^dist_{F(T₂) ∩ A(T₁)}(T₁).
 */
TIntrusivePtr<TOpAggregate> MakeDomainProjection(const TIntrusivePtr<IOperator>& input, const TVector<TInfoUnit>& columns, TPositionHandle pos);

/**
 * Checks whether the subtree still references any of the given correlated columns as a free
 * variable, i.e. whether it contains an AddDependencies operator that injects one of them.
 */
bool HasFreeCorrelation(const TIntrusivePtr<IOperator>& op, const TVector<TInfoUnit>& correlatedColumns);

/**
 * Conservatively decides whether a column may be null. Postgres types are always nullable, so callers
 * that care about the difference have to exclude the postgres syntax separately.
 */
bool IsNullableIU(const TIntrusivePtr<IOperator>& input, const TInfoUnit& iu);

/**
 * Turns equalities on the domain columns of a decorrelated subplan into IS NOT DISTINCT FROM.
 *
 * A null is a value of the domain: the domain is a distinct projection and SQL DISTINCT groups the
 * nulls together, so the subplan is evaluated for a null binding like for any other one. Matching
 * that binding back into the outer plan with a plain equality never succeeds though, and the outer
 * rows with a null binding would silently lose the subplan's answer. StablePickle encodes a value
 * together with its nullness into a string that is never null, which makes the equality on it mean
 * exactly IS NOT DISTINCT FROM on the original column.
 *
 * Only the nullable keys are encoded. Both inputs are wrapped into a map producing the encoded
 * columns when there is at least one, and the returned key list refers to those.
 */
TVector<std::pair<TInfoUnit, TInfoUnit>> MakeNullSafeJoinKeys(TIntrusivePtr<IOperator>& leftInput, TIntrusivePtr<IOperator>& rightInput,
                                                              const TVector<std::pair<TInfoUnit, TInfoUnit>>& joinKeys, TPositionHandle pos,
                                                              TRBOContext& ctx, TPlanProps& props, TInfoUnitSet& usedIUs);

} // namespace NKqp
} // namespace NKikimr
