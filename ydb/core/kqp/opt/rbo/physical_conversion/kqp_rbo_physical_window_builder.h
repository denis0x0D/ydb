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

    // Two frames are lowered. A running frame, which starts at the partition boundary and ends
    // no later than the current row, folds as the rows stream by. A whole partition frame gives
    // every row the same value, so the partition is folded once and the result broadcast.
    // Ranking functions never read the frame, so they always qualify for the running path.
    static bool CanBuildWindow(const TOpWindow& window);
    // True when the whole partition folds to one value that every row shares.
    static bool UsesBroadcast(const TOpWindow& window);

private:
    void Prepare(const TVector<TInfoUnit>& inputs);
    ui32 IndexOf(const TInfoUnit& column) const;
    const TTypeAnnotationNode* InputItemType(const TInfoUnit& column) const;

    TVector<TExprNode::TPtr> BuildSortKeys() const;
    TExprNode::TPtr BuildKeyExtractorLambda() const;
    TExprNode::TPtr BuildGroupSwitchLambda() const;

    // Running frame: fold and emit per row.
    TExprNode::TPtr BuildChain(TExprNode::TPtr wideFlow) const;
    TExprNode::TPtr BuildChainLambda(bool update) const;
    TExprNode::TPtr BuildExpandFromChain(TExprNode::TPtr chained) const;

    // Whole partition frame: collect the partition, fold it once, then map it back.
    TExprNode::TPtr BuildBroadcast(TExprNode::TPtr wideFlow) const;
    TExprNode::TPtr BuildFoldLambda(bool update) const;
    TExprNode::TPtr BuildExpandFromStructs(TExprNode::TPtr list) const;

    // One accumulator value for a function. A null previousState builds the initial value.
    TExprNode::TPtr BuildAccumulator(const TOpWindowFunc& func, ui32 funcIndex, TExprNode::TPtr itemArg, TExprNode::TPtr previousState,
                                     TExprNode::TPtr sortKeyChanged, TVector<std::pair<TString, TExprNode::TPtr>>& stateMembers) const;
    // The column a function produces, computed from its accumulator.
    TExprNode::TPtr BuildResultFromAccumulator(const TOpWindowFunc& func, TExprNode::TPtr accumulator) const;
    // Average accumulates into a Double, except for Decimal which keeps its own scale at a
    // wider precision and is divided and cast back at the end.
    TExprNode::TPtr BuildAvgAccumulatorDataType(const TInfoUnit& column) const;
    TExprNode::TPtr BuildAvgAccumulatorType(const TInfoUnit& column) const;

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
    // True when the frame covers the whole partition, so every row gets the same value.
    bool WholePartition = false;
};
