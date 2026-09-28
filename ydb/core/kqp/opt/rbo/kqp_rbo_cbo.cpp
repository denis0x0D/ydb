#include "kqp_rbo_cbo.h"
#include "kqp_rbo_lookup_join.h"
#include "kqp_rbo_utils.h"

namespace {

using namespace NYql;
using namespace NYql::NNodes;
using namespace NKikimr::NKqp;

// Checks whether a node is single consumer.
bool IsSingleConsumerRelNode(const std::shared_ptr<IBaseOptimizerNode>& node) {
    if (node->Kind != EOptimizerNodeKind::RelNodeType) {
        return true;
    }
    const auto& op = std::static_pointer_cast<TRBORelOptimizerNode>(node)->Op;
    return op->IsSingleConsumer();
}

bool IsLookupJoinApplicableDetailed(const std::shared_ptr<TRelOptimizerNode>& node, const TVector<TJoinColumn>& joinColumns, EJoinKind joinKind, const TKqpProviderContext& ctx) {
    auto rel = std::static_pointer_cast<TRBORelOptimizerNode>(node);
    auto rightInput = rel->Op;
    TIntrusivePtr<TOpFilter> rightFilter;

    if (rightInput->Kind == EOperator::Filter) {
        if (!rightInput->IsSingleConsumer()) {
            return false;
        }
        rightFilter = CastOperator<TOpFilter>(rightInput);
        rightInput = rightFilter->GetInput();
    }
    if (rightInput->Kind != EOperator::Source || !rightInput->IsSingleConsumer()) {
        return false;
    }

    auto read = CastOperator<TOpRead>(rightInput);

    TVector<TInfoUnit> rightJoinKeys;
    for (const auto& joinCol : joinColumns) {
        TInfoUnit joinIU(joinCol.RelName, joinCol.AttributeName);
        if (!rel->CBOToColumns.contains(joinIU)) {
            return false;
        }
        rightJoinKeys.push_back(rel->CBOToColumns.at(joinIU));
    }

    const auto joinKeyColumns = GetLookupJoinKeyColumns(*read, rightJoinKeys);
    if (!joinKeyColumns) {
        return false;
    }

    bool filterSupported = true;
    BuildFetchedRowFilter(*read, rightFilter, filterSupported);
    if (!filterSupported) {
        return false;
    }

    return ChooseLookupJoinTarget(*read, *joinKeyColumns, joinKind == EJoinKind::InnerJoin, ctx.KqpCtx).has_value();
}

bool IsLookupJoinApplicable(std::shared_ptr<IBaseOptimizerNode> left,
    std::shared_ptr<IBaseOptimizerNode> right,
    const TVector<TJoinColumn>& leftJoinKeys,
    const TVector<TJoinColumn>& rightJoinKeys,
    EJoinKind joinKind,
    TKqpProviderContext& ctx
) {
    Y_UNUSED(leftJoinKeys);

    // We need to follow rewrite rule.
    if (!IsSingleConsumerRelNode(left)) {
        return false;
    }

    if (!(right->Stats.StorageType == NKikimr::NKqp::EStorageType::RowStorage)) {
        return false;
    }

    auto rightStats = right->Stats;

    if (!rightStats.KeyColumns) {
        return false;
    }

    if (rightStats.Type != NKikimr::NKqp::EStatisticsType::BaseTable) {
        return false;
    }

    // for (auto rightCol : rightJoinKeys) {
    //     if (find(rightStats.KeyColumns->Data.begin(), rightStats.KeyColumns->Data.end(), rightCol.AttributeName) == rightStats.KeyColumns->Data.end()) {
    //         return false;
    //     }
    // }

    return IsLookupJoinApplicableDetailed(std::static_pointer_cast<TRelOptimizerNode>(right), rightJoinKeys, joinKind, ctx);
}

}

namespace NKikimr::NKqp::NOpt {

bool TRBOProviderContext::IsJoinApplicable(const std::shared_ptr<IBaseOptimizerNode>& left,
    const std::shared_ptr<IBaseOptimizerNode>& right,
    const TVector<TJoinColumn>& leftJoinKeys,
    const TVector<TJoinColumn>& rightJoinKeys,
    EJoinAlgoType joinAlgo,
    EJoinKind joinKind) {

    switch( joinAlgo ) {
        case EJoinAlgoType::LookupJoin: {
            if ((OptLevel != 3) && (left->Stats.Nrows > 5000)) {
                return false;
            }
            return IsLookupJoinApplicable(left, right, leftJoinKeys, rightJoinKeys, joinKind, *this);
        }
        // FIXME: Don't pick reverse lookup join yet
        /*
        case EJoinAlgoType::LookupJoinReverse: {
            if (joinKind != EJoinKind::LeftSemi) {
                return false;
            }
            if ((OptLevel != 3) && (right->Stats.Nrows > 5000)) {
                return false;
            }
            return IsLookupJoinApplicable(right, left, rightJoinKeys, leftJoinKeys, joinKind, *this);
        }
        */
        case EJoinAlgoType::MapJoin:
            return joinKind != EJoinKind::OuterJoin && joinKind != EJoinKind::Exclusion && right->Stats.ByteSize < 1e6;
        case EJoinAlgoType::GraceJoin:
            return true;
        case EJoinAlgoType::ReverseBlockJoin:
            return BlockJoinEnabled && (joinKind == EJoinKind::LeftJoin | joinKind == EJoinKind::LeftOnly | joinKind == EJoinKind::LeftSemi);
        default:
            return false;
    }
}
}
