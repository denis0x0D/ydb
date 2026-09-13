#include "kqp_rbo_physical_sort_builder.h"
using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;

std::pair<TExprNode::TPtr, TVector<TExprNode::TPtr>> TPhysicalSortBuilder::BuildSortKeySelector(const TVector<TSortElement>& sortElements) {
    auto arg = Build<TCoArgument>(Ctx, Pos).Name("arg").Done().Ptr();
    TVector<TExprNode::TPtr> directions;
    TVector<TExprNode::TPtr> members;

    for (const auto& element : sortElements) {
        // clang-format off
        members.push_back(Build<TCoMember>(Ctx, Pos)
            .Struct(arg)
            .Name().Build(element.SortColumn.GetFullName())
        .Done().Ptr());
        // clang-format on

        directions.push_back(Build<TCoBool>(Ctx, Pos).Literal().Build(element.Ascending ? "true" : "false").Done().Ptr());
    }

    TExprNode::TPtr selector;
    if (sortElements.size() == 1) {
        // clang-format off
        selector = Build<TCoLambda>(Ctx, Pos)
            .Args({arg})
            .Body(members[0])
            .Done().Ptr();
        // clang-format on
    } else {
        // clang-format off
        selector = Build<TCoLambda>(Ctx, Pos)
            .Args({arg})
            .Body<TExprList>().Add(members).Build()
            .Done().Ptr();
        // clang-format on
    }

    return std::make_pair(selector, directions);
}

TExprNode::TPtr TPhysicalSortBuilder::BuildSort(TExprNode::TPtr input, TOrderEnforcer& enforcer) {
    if (enforcer.Action != EOrderEnforcerAction::REQUIRE) {
        return input;
    }

    auto [selector, dirs] = BuildSortKeySelector(enforcer.SortElements);

    TExprNode::TPtr dirList;
    if (dirs.size() == 1) {
        dirList = dirs[0];
    } else {
        dirList = Build<TExprList>(Ctx, Pos).Add(dirs).Done().Ptr();
    }

    // clang-format off
    return Build<TCoSort>(Ctx, Pos)
        .Input(input)
        .SortDirections(dirList)
        .KeySelectorLambda(selector)
    .Done().Ptr();
    // clang-format on
}

TVector<TExprNode::TPtr> TPhysicalSortBuilder::BuildSortKeysForWideSort(const TVector<TInfoUnit>& inputs, const TVector<TSortElement>& sortElements) {
    // We have to map wide input with sort elements to find a right index.
    THashMap<TString, ui32> indices;
    for (ui32 i = 0; i < inputs.size(); ++i) {
        indices.emplace(inputs[i].GetFullName(), i);
    }

    TVector<TExprNode::TPtr> sortKeys;
    for (ui32 i = 0; i < sortElements.size(); ++i) {
        const auto& sortElement = sortElements[i];
        auto it = indices.find(sortElement.SortColumn.GetFullName());
        Y_ENSURE(it != indices.end(), "Cannot find a sort element in wide input.");
        const auto wideIndex = ToString(it->second);
        // clang-format off
        auto sortKey = Ctx.Builder(Pos)
            .List()
                .Atom(0, wideIndex)
                .Callable(1, "Bool")
                    .Atom(0, sortElement.Ascending ? "true" : "false")
                .Seal()
            .Seal()
        .Build();
        // clang-format off
        sortKeys.push_back(sortKey);
    }
    return sortKeys;
}

NPhysicalConvertionUtils::TStageBody TPhysicalSortBuilder::BuildPhysicalOp(const NPhysicalConvertionUtils::TStageBody& input) {
    const auto inputs = NPhysicalConvertionUtils::GetLiveInputIUs(*Sort, 0);
    const auto& sortElements = Sort->SortElements;

    auto wideInput = input.AsWide(inputs, Ctx);

    TExprNode::TPtr output;
    if (Sort->LimitCond.has_value()) {
        // clang-format off
        output = Build<TCoWideTopSort>(Ctx, Pos)
            .Input(wideInput)
            .Count(Sort->LimitCond->GetExpressionBody())
            .Keys<TCoSortKeys>()
                .Add(BuildSortKeysForWideSort(inputs, sortElements))
            .Build()
        .Done().Ptr();
        // clang-format on
    } else {
        // clang-format off
        output = Build<TCoWideSort>(Ctx, Pos)
            .Input(wideInput)
            .Keys<TCoSortKeys>()
                .Add(BuildSortKeysForWideSort(inputs, sortElements))
            .Build()
        .Done().Ptr();
        // clang-format on
    }

    // WideSort passes every input slot through, so drop the ones that are dead past this
    // operator. Merge-connection keys are already included in LiveOut.
    const auto liveOutputs = NPhysicalConvertionUtils::BuildNameSet(NPhysicalConvertionUtils::GetLiveOutputIUs(*Sort));
    TVector<TInfoUnit> outputColumns;
    outputColumns.reserve(inputs.size());
    for (const auto& column : inputs) {
        if (liveOutputs.contains(column.GetFullName())) {
            outputColumns.push_back(column);
        }
    }
    if (outputColumns.size() != inputs.size()) {
        output = NPhysicalConvertionUtils::BuildWideProjection(output, inputs, outputColumns, Ctx);
    }

    YQL_CLOG(TRACE, CoreDq) << "[NEW RBO Physical sort] " << KqpExprToPrettyString(TExprBase(output), Ctx);
    return NPhysicalConvertionUtils::TStageBody::Wide(output, std::move(outputColumns));
}
