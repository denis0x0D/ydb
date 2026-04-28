#include "kqp_stage_graph.h"

namespace {
using namespace NKikimr;
using namespace NKqp;
using namespace NYql;
using namespace NNodes;

void DFS(ui32 vertex, TList<ui32>& sortedStages, THashSet<ui32>& visited, const THashMap<ui32, TVector<ui32>>& stageInputs) {
    visited.emplace(vertex);
    for (auto u : stageInputs.at(vertex)) {
        if (!visited.contains(u)) {
            DFS(u, sortedStages, visited, stageInputs);
        }
    }
    sortedStages.push_back(vertex);
}
}

namespace NKikimr {
namespace NKqp {

using namespace NYql;
using namespace NNodes;

template <typename DqConnectionType>
TExprNode::TPtr TConnection::BuildConnectionImpl(TExprNode::TPtr inputStage, TPositionHandle pos, TExprContext& ctx) {
    // clang-format off
    return Build<DqConnectionType>(ctx, pos)
        .Output()
            .Stage(inputStage)
            .Index().Build(ToString(OutputIndex))
        .Build()
    .Done().Ptr();
    // clang-format on
}

TExprNode::TPtr TBroadcastConnection::BuildConnection(TExprNode::TPtr inputStage, TPositionHandle pos, TExprContext& ctx) {
    return BuildConnectionImpl<TDqCnBroadcast>(inputStage, pos, ctx);
}

TExprNode::TPtr TMapConnection::BuildConnection(TExprNode::TPtr inputStage, TPositionHandle pos, TExprContext& ctx) {
    return BuildConnectionImpl<TDqCnMap>(inputStage, pos, ctx);
}

TExprNode::TPtr TUnionAllConnection::BuildConnection(TExprNode::TPtr inputStage, TPositionHandle pos, TExprContext& ctx) {
    return Parallel ? BuildConnectionImpl<TDqCnParallelUnionAll>(inputStage, pos, ctx) : BuildConnectionImpl<TDqCnUnionAll>(inputStage, pos, ctx);
}

TExprNode::TPtr TShuffleConnection::BuildConnection(TExprNode::TPtr inputStage, TPositionHandle pos, TExprContext& ctx) {
    TVector<TCoAtom> keyColumns;
    for (const auto& key : Keys) {
        const auto columnName = key.GetFullName();
        keyColumns.emplace_back(Build<TCoAtom>(ctx, pos).Value(columnName).Done());
    }

    // clang-format off
    return Build<TDqCnHashShuffle>(ctx, pos)
        .Output()
            .Stage(inputStage)
            .Index().Build(ToString(OutputIndex))
        .Build()
        .KeyColumns()
            .Add(keyColumns)
        .Build()
    .Done().Ptr();
    // clang-format on
}

TExprNode::TPtr TMergeConnection::BuildConnection(TExprNode::TPtr inputStage, TPositionHandle pos, TExprContext& ctx) {
    TVector<TExprNode::TPtr> sortColumns;
    for (const auto& sortElement : Order) {
        // clang-format off
        sortColumns.push_back(Build<TDqSortColumn>(ctx, pos)
            .Column<TCoAtom>().Build(sortElement.SortColumn.GetFullName())
            .SortDirection().Build(sortElement.Ascending ? TTopSortSettings::AscendingSort : TTopSortSettings::DescendingSort)
            .Done().Ptr());
        // clang-format on
    }

    // clang-format off
    return Build<TDqCnMerge>(ctx, pos)
        .Output()
            .Stage(inputStage)
            .Index().Build(ToString(OutputIndex))
        .Build()
        .SortColumns()
            .Add(sortColumns)
        .Build()
    .Done().Ptr();
    // clang-format on
}

TExprNode::TPtr TSourceConnection::BuildConnection(TExprNode::TPtr inputStage, TPositionHandle pos, TExprContext& ctx) {
    Y_UNUSED(pos);
    Y_UNUSED(ctx);
    return inputStage;
}

std::pair<TExprNode::TPtr, TExprNode::TPtr> TStageGraph::GenerateStageInput(ui32& stageInputCounter, TPositionHandle pos, TExprContext& ctx) const {
    const TString inputName = "input_arg_" + std::to_string(stageInputCounter++);
    YQL_CLOG(TRACE, CoreDq) << "Created stage argument " << inputName;
    const auto arg = Build<TCoArgument>(ctx, pos).Name(inputName).Done().Ptr();
    return std::make_pair(arg, arg);
}

void TStageGraph::TopologicalSort() {
    TList<ui32> sortedStages;
    THashSet<ui32> visited;

    for (auto id : StageIds) {
        if (!visited.contains(id)) {
            DFS(id, sortedStages, visited, StageInputs);
        }
    }

    StageIds.swap(sortedStages);
}

bool TStageGraph::IsPossibleToEraseStage(ui32 stageId) const {
    const auto it = std::find(StageIds.begin(), StageIds.end(), stageId);
    if (it == StageIds.end()) {
        return false;
    }
    const auto inputsIt = StageInputs.find(stageId);
    if (inputsIt == StageInputs.end() || inputsIt->second.size() != 1) {
        return false;
    }
    const auto inputOutputIt = StageOutputs.find(inputsIt->second.front());
    if (inputOutputIt == StageOutputs.end() || inputOutputIt->second.size() != 1) {
        return false;
    }

    const auto outputsIt = StageOutputs.find(stageId);
    if (outputsIt == StageOutputs.end() || outputsIt->second.size() != 1) {
        return false;
    }
    const auto outputInputIt = StageInputs.find(outputsIt->second.front());
    if (outputInputIt == StageInputs.end() || outputInputIt->second.size() != 1) {
        return false;
    }
    return true;
}

void TStageGraph::EraseStage(ui32 stageId, TIntrusivePtr<TConnection> newConnection) {
    const auto stageIdIt = std::find(StageIds.begin(), StageIds.end(), stageId);
    Y_ENSURE(stageIdIt != StageIds.end());
    const auto inputsIt = StageInputs.find(stageId);
    Y_ENSURE(inputsIt != StageInputs.end() && inputsIt->second.size() == 1);
    const auto outputsIt = StageOutputs.find(stageId);
    Y_ENSURE(outputsIt != StageOutputs.end() && outputsIt->second.size() == 1);

    const auto inputStageId = inputsIt->second.front();
    const auto outputStageId = outputsIt->second.front();

    const auto inputConnections = Connections.find(std::make_pair(inputStageId, stageId));
    Y_ENSURE(inputConnections != Connections.end() && inputConnections->second.size() == 1);

    const auto outputConnections = Connections.find(std::make_pair(stageId, outputStageId));
    Y_ENSURE(outputConnections != Connections.end() && outputConnections->second.size() == 1);

    StageIds.erase(stageIdIt);
    StageInputs.erase(stageId);
    StageOutputs.erase(stageId);
    Connections.erase(std::make_pair(inputStageId, stageId));
    Connections.erase(std::make_pair(stageId, outputStageId));

    StageOutputs[inputStageId] = {outputStageId};
    StageInputs[outputStageId] = {inputStageId};
    Connections[std::make_pair(inputStageId, outputStageId)] = {newConnection};
}
}
}