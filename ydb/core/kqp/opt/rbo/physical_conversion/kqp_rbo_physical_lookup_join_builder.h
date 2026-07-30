#pragma once
#include "kqp_rbo_physical_op_builder.h"
#include "kqp_rbo_physical_convertion_utils.h"

using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;

namespace NKikimr::NKqp::NLookupJoinBuilder {

struct TLookupKeysResult {
    // The input stage, or its body, rewritten to emit the tuples the stream lookup consumes.
    NYql::TExprNode::TPtr InputStage;
    // The type of the emitted tuples, as declared on the KqpCnStreamLookup connection.
    NYql::TExprNode::TPtr InputType;
};

/**
 * A stream lookup in join mode consumes (left row, lookup keys) tuples, so the stage feeding the
 * connection has to build them. Rewrites `inputStage` - either a stage body under construction or
 * an already built DqPhyStage - to emit those tuples, and returns their type alongside.
 */
TLookupKeysResult BuildLookupKeys(TOpTableLookup& lookup, NYql::TExprNode::TPtr inputStage, NYql::TExprContext& ctx);

} // namespace NKikimr::NKqp::NLookupJoinBuilder

/**
 * Flattens the tuples produced by the stream lookup into joined rows.
 */
class TPhysicalIndexLookupJoinBuilder: public TPhysicalUnaryOpBuilder {
public:
    TPhysicalIndexLookupJoinBuilder(TIntrusivePtr<TOpIndexLookupJoin> lookupJoin, TExprContext& ctx, TPositionHandle pos)
        : TPhysicalUnaryOpBuilder(ctx, pos)
        , LookupJoin(lookupJoin) {
    }

    TExprNode::TPtr BuildPhysicalOp(TExprNode::TPtr input) override;

private:
    TExprNode::TPtr ProcessFetchedRows(TExprNode::TPtr input, const TOpTableLookup& lookup) const;
    TExprNode::TPtr BuildRenamedRow(const TExprBase& fetchedRow, const TOpTableLookup& lookup, bool& needsRename) const;

    TIntrusivePtr<TOpIndexLookupJoin> LookupJoin;
};
