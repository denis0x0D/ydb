#include <ydb/core/kqp/opt/rbo/kqp_olap_aggregate.h>
#include <ydb/core/kqp/opt/rbo/kqp_rbo_rules.h>
#include <ydb/core/kqp/provider/yql_kikimr_settings.h>

#include <yql/essentials/utils/log/log.h>

namespace NKikimr::NKqp {

bool TPushOlapAggregateRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Aggregate;
}

TIntrusivePtr<IOperator> TPushOlapAggregateRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx, TPlanProps& props) {
    if (!ctx.KqpCtx.Config->HasOptEnableOlapPushdown() || !ctx.KqpCtx.Config->GetEnableOlapPushdownAggregate()) {
        return input;
    }

    // Column shards compute partial aggregates, the final aggregate merges them.
    const auto aggregate = CastOperator<TOpAggregate>(input);
    if (aggregate->GetAggregationPhase() != EOpPhase::Intermediate || aggregate->IsPushedToOlap()) {
        return input;
    }

    const auto olapInput = MatchOlapAggregateInput(*aggregate, ctx.ExprCtx);
    if (!olapInput) {
        return input;
    }
    // The read program replaces the read and the map, so nothing else may consume them.
    if (olapInput->Read->Parents.size() != 1 || (olapInput->Map && olapInput->Map->Parents.size() != 1)) {
        return input;
    }

    auto pushed = MakeIntrusive<TOpAggregate>(aggregate->GetInput(), aggregate->GetAggregationTraits(), aggregate->GetKeyColumns(),
                                              EOpPhase::Intermediate, aggregate->IsDistinctAll(), aggregate->Props, aggregate->Pos);
    pushed->SetPushedToOlap();
    YQL_CLOG(TRACE, ProviderKqp) << "Pushed OLAP aggregate: " << pushed->ToString(ctx.ExprCtx, props.InfoUnitRegistry);
    return pushed;
}

} // namespace NKikimr::NKqp
