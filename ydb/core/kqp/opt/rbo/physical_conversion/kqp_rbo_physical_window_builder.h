#pragma once
#include "kqp_rbo_physical_op_builder.h"
#include "kqp_rbo_physical_convertion_utils.h"
#include <yql/essentials/core/yql_opt_utils.h>
#include <yql/essentials/utils/log/log.h>

using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;

// Lowers a window into a single forward pass:
//
//   ToFlow -> ExpandMap -> WideSort(partition keys ++ sort keys)
//          -> WideChopper(split on a partition key change)
//               -> NarrowMap -> Chain1Map(one accumulator per function) -> ExpandMap
//          -> NarrowMap -> FromFlow
//
// The rows arrive already hash shuffled by the partition keys, so sorting by those keys makes
// every partition contiguous and the chopper only has to watch for a key change.
//
// Sorting and chopping stay wide, which is what lets the sort spill. The chain itself is row at
// a time by nature and runs over structs: its state is a struct rebuilt per row, so the previous
// row's values are owned copies rather than slots the wide machinery may reuse.
class TPhysicalWindowBuilder: public TPhysicalUnaryOpBuilder {
public:
    TPhysicalWindowBuilder(TIntrusivePtr<TOpWindow> window, TExprContext& ctx, TPositionHandle pos)
        : TPhysicalUnaryOpBuilder(ctx, pos)
        , Window(window) {
    }

    TExprNode::TPtr BuildPhysicalOp(TExprNode::TPtr input) override;

    // A single pass works when every aggregate reads a frame that starts at the partition
    // boundary and ends no later than the current row. Ranking functions never read the frame,
    // so a window that only ranks always qualifies whatever the frame says.
    static bool CanBuildStreamingWindow(const TOpWindow& window);

private:
    void Prepare(const TVector<TInfoUnit>& inputs);
    ui32 IndexOf(const TInfoUnit& column) const;
    const TTypeAnnotationNode* InputItemType(const TInfoUnit& column) const;

    TVector<TExprNode::TPtr> BuildSortKeys() const;
    TExprNode::TPtr BuildKeyExtractorLambda() const;
    TExprNode::TPtr BuildGroupSwitchLambda() const;

    TExprNode::TPtr BuildChain(TExprNode::TPtr wideFlow) const;
    TExprNode::TPtr BuildChainLambda(bool update) const;
    TExprNode::TPtr BuildExpandFromChain(TExprNode::TPtr chained) const;

    // Members of the struct the chain carries between rows.
    TString AccumulatorName(ui32 funcIndex) const;
    TString PositionName(ui32 funcIndex) const;
    TString PeerName(ui32 sortIndex) const;

    TExprNode::TPtr Member(TExprNode::TPtr from, const TString& name) const;
    TExprNode::TPtr BuildStruct(const TVector<std::pair<TString, TExprNode::TPtr>>& members) const;
    TExprNode::TPtr BuildSumCastTarget(const TInfoUnit& column) const;
    TExprNode::TPtr MakeOptional(TExprNode::TPtr value, bool alreadyOptional) const;
    TExprNode::TPtr BuildUint64(ui64 value) const;

    TIntrusivePtr<TOpWindow> Window;
    TVector<TInfoUnit> Inputs;
    THashMap<TString, ui32> Indexes;
    const TStructExprType* InputStruct = nullptr;
    // The wide layout the chopper handler produces: the input columns then one per function.
    TVector<TInfoUnit> OutputLayout;
    bool NeedsPeerKey = false;
};
