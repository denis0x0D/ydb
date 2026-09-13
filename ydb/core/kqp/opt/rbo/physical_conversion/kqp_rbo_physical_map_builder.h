#pragma once
#include "kqp_rbo_physical_op_builder.h"
#include "kqp_rbo_physical_convertion_utils.h"
#include <yql/essentials/core/yql_opt_utils.h>
#include <yql/essentials/utils/log/log.h>

using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;

class TPhysicalMapBuilder: public TPhysicalWideUnaryOpBuilder {
public:
    TPhysicalMapBuilder(TIntrusivePtr<TOpMap> map, TExprContext& ctx, TPositionHandle pos)
        : TPhysicalWideUnaryOpBuilder(ctx, pos)
        , Map(map) {
    }

    NPhysicalConvertionUtils::TStageBody BuildPhysicalOp(const NPhysicalConvertionUtils::TStageBody& input) override;

private:
    TIntrusivePtr<TOpMap> Map;
};
