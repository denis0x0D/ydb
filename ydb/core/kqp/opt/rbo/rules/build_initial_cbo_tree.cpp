#include "kqp_rules_include.h"

namespace NKikimr {
namespace NKqp {

bool TBuildInitialCBOTreeRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Join;
}

/**
 * Initially we build CBO only for joins that don't have other joins or CBO trees as arguments
 * There could be an intermediate filter in between, we also check that
 */
TIntrusivePtr<IOperator> TBuildInitialCBOTreeRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator> &input, TRBOContext &ctx, TPlanProps &props) {
    Y_UNUSED(ctx);
    Y_UNUSED(props);

    // The CBO has no mark join: it stays out of the trees, as a leaf of those above it.
    auto isMarkJoin = [](const TIntrusivePtr<IOperator>& op) {
        return op->Kind == EOperator::Join && CastOperator<TOpJoin>(op)->IsMarkJoin();
    };

    auto containsJoins = [&](const TIntrusivePtr<IOperator>& op) {
        TIntrusivePtr<IOperator> maybeJoin = op;
        if (op->Kind == EOperator::Filter) {
            maybeJoin = CastOperator<TOpFilter>(op)->GetInput();
        }
        return (maybeJoin->Kind == EOperator::Join && !isMarkJoin(maybeJoin)) || maybeJoin->Kind == EOperator::CBOTree;
    };

    if (input->Kind == EOperator::Join && !isMarkJoin(input)) {
        auto join = CastOperator<TOpJoin>(input);
        if (join->JoinFilters.empty() && !containsJoins(join->GetLeftInput()) && !containsJoins(join->GetRightInput())) {
            return MakeIntrusive<TOpCBOTree>(input, input->Pos);
        }
    }

    return input;
}
}
}
