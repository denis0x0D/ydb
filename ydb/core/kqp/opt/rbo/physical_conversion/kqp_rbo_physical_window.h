#pragma once

#include <ydb/core/kqp/opt/rbo/kqp_operator.h>

namespace NKikimr::NKqp {

TExprNode::TPtr BuildPhysicalWindow(TOpWindow& window, TExprNode::TPtr input, TRBOContext& ctx);

} // namespace NKikimr::NKqp
