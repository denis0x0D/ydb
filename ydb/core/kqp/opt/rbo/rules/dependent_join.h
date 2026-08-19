#pragma once

#include <ydb/core/kqp/opt/rbo/kqp_operator.h>

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

} // namespace NKqp
} // namespace NKikimr
