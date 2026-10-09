#pragma once
#include "kqp_rbo_physical_op_builder.h"
#include <ydb/core/kqp/opt/rbo/kqp_olap_aggregate.h>
#include <yql/essentials/utils/log/log.h>

using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;

class TPhysicalSourceBuilder: public TPhysicalNullaryOpBuilder {
public:
    TPhysicalSourceBuilder(TOpRead& read, TExprContext& ctx, TPositionHandle pos, const TPhysicalNames& names,
        const TInfoUnitRegistry& registry, const TString& stageGUID, bool isSysView = false, TString carrierColumn = {})
        : TPhysicalNullaryOpBuilder(ctx, pos, names)
        , Read(read)
        , Registry(registry)
        , StageGUID(stageGUID)
        , IsSysView(isSysView)
        , CarrierColumn(std::move(carrierColumn)) {}

    // Compute an aggregate pushed to column shards in the read program. The
    // source then yields the rows of the aggregate instead of the read.
    TPhysicalSourceBuilder& WithOlapAggregate(const TOpAggregate& aggregate, const TOlapAggregateInput& input) {
        Y_ENSURE(input.Read == &Read, "A pushed aggregate must compute over this read");
        OlapAggregate = &aggregate;
        OlapAggregateInput = &input;
        return *this;
    }

    TExprNode::TPtr BuildPhysicalOp() override;

private:
    // The program row field of the aggregation at `index`. AVG has two: sum and count.
    TString GetOlapAggregateField(ui32 index, TStringBuf function) const;
    // Replace the program output with the aggregate rows and list their fields.
    TExprNode::TPtr AddOlapAggregate(TExprNode::TPtr processLambda, TVector<TString>& outputFields) const;
    // Rebuild the aggregate's output IDs, including the AVG states, from the program row.
    TExprNode::TPtr BuildOlapAggregateRow(const THashMap<TString, TExprNode::TPtr>& fields) const;

    TOpRead& Read;
    const TInfoUnitRegistry& Registry;
    TString StageGUID;
    bool IsSysView;
    TString CarrierColumn;
    const TOpAggregate* OlapAggregate = nullptr;
    const TOlapAggregateInput* OlapAggregateInput = nullptr;
};
