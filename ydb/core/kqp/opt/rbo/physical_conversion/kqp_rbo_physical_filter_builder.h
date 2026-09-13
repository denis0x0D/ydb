#pragma once
#include "kqp_rbo_physical_op_builder.h"
#include <yql/essentials/utils/log/log.h>

using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;

class TPhysicalFilterBuilder: public TPhysicalWideUnaryOpBuilder {
public:
    TPhysicalFilterBuilder(TIntrusivePtr<TOpFilter> filter, TExprContext& ctx, TPositionHandle pos)
        : TPhysicalWideUnaryOpBuilder(ctx, pos)
        , Filter(filter) {
    }

    NPhysicalConvertionUtils::TStageBody BuildPhysicalOp(const NPhysicalConvertionUtils::TStageBody& input) override;

private:
    TIntrusivePtr<TOpFilter> Filter;
};
