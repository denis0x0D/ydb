#include "kqp_rbo_physical_window_builder.h"

#include <yql/essentials/core/yql_expr_type_annotation.h>

using namespace NYql;
using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;

namespace {

// Aggregates whose running value can be folded with a constant amount of state.
bool IsStreamingAggregate(const TString& function) {
    return function == "sum" || function == "min" || function == "max" || function == "count" || function == "avg";
}

bool IsRankingFunction(const TString& function) {
    return function == "rank" || function == "denserank" || function == "rownumber";
}

} // anonymous namespace

namespace {

bool IsDecimal(const TTypeAnnotationNode* type) {
    if (type->IsOptionalOrNull()) {
        type = type->Cast<TOptionalExprType>()->GetItemType();
    }
    return type->GetKind() == ETypeAnnotationKind::Data && type->Cast<TDataExprType>()->GetName().starts_with("Decimal");
}

// Precision and scale of a Decimal, whatever optional wrapping it carries.
std::pair<TString, TString> DecimalParams(const TTypeAnnotationNode* type) {
    if (type->IsOptionalOrNull()) {
        type = type->Cast<TOptionalExprType>()->GetItemType();
    }
    const auto* params = dynamic_cast<const TDataExprParamsType*>(type);
    Y_ENSURE(params, "Expected a Decimal type");
    return {TString(params->GetParamOne()), TString(params->GetParamTwo())};
}

bool IsRunningFrame(const TOpWindowFrame& frame) {
    return frame.Type == EWindowFrameType::Rows && frame.BeginKind == EWindowFrameBound::UnboundedPreceding &&
           (frame.EndKind == EWindowFrameBound::CurrentRow ||
            (frame.EndKind == EWindowFrameBound::Following && frame.EndValue == 0));
}

bool IsWholePartitionFrame(const TOpWindowFrame& frame) {
    return frame.Type == EWindowFrameType::Rows && frame.BeginKind == EWindowFrameBound::UnboundedPreceding &&
           frame.EndKind == EWindowFrameBound::UnboundedFollowing;
}

} // anonymous namespace

// Ranking reads the row's position, which only the chain can supply, so a window that ranks has
// to take the chain whatever its frame says. That is also why a ranking function never rules a
// window out on frame grounds: it does not read the frame at all.
bool TPhysicalWindowBuilder::UsesBroadcast(const TOpWindow& window) {
    if (!IsWholePartitionFrame(window.GetFrame())) {
        return false;
    }
    for (const auto& func : window.GetWindowFuncs()) {
        if (func.Kind == EWindowFuncKind::Native) {
            return false;
        }
    }
    return true;
}

bool TPhysicalWindowBuilder::CanBuildWindow(const TOpWindow& window) {
    const bool running = IsRunningFrame(window.GetFrame());
    const bool broadcast = UsesBroadcast(window);

    const auto* inputType = window.GetInput()->Type;
    const auto* structType = inputType ? inputType->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>() : nullptr;

    for (const auto& func : window.GetWindowFuncs()) {
        if (func.Kind == EWindowFuncKind::Native) {
            // Rank(expr) ranks by an explicit expression, which the chain does not implement.
            if (!IsRankingFunction(func.Function) || !func.Arguments.empty()) {
                return false;
            }
            continue;
        }
        // An aggregate folds row by row, so outside the broadcast path its frame has to run from
        // the partition boundary to the current row.
        if (!IsStreamingAggregate(func.Function) || !(running || broadcast)) {
            return false;
        }
        if (func.Function == "avg" && (!structType || func.Arguments.empty())) {
            return false;
        }
    }
    return true;
}

void TPhysicalWindowBuilder::Prepare(const TVector<TInfoUnit>& inputs) {
    Inputs = inputs;
    for (ui32 i = 0; i < inputs.size(); ++i) {
        Indexes.emplace(inputs[i].GetFullName(), i);
    }

    const auto* inputType = Window->GetInput()->Type;
    Y_ENSURE(inputType, "Window input has no type annotation");
    InputStruct = inputType->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();

    OutputLayout = inputs;
    for (const auto& func : Window->GetWindowFuncs()) {
        OutputLayout.push_back(func.ResultColName);
        NeedsPeerKey = NeedsPeerKey || func.Function == "rank" || func.Function == "denserank";
    }
    NeedsPeerKey = NeedsPeerKey && !Window->GetSortElements().empty();
    WholePartition = UsesBroadcast(*Window);
}

ui32 TPhysicalWindowBuilder::IndexOf(const TInfoUnit& column) const {
    auto it = Indexes.find(column.GetFullName());
    Y_ENSURE(it != Indexes.end(), "Cannot find window column " << column.GetFullName() << " in the wide input");
    return it->second;
}

const TTypeAnnotationNode* TPhysicalWindowBuilder::InputItemType(const TInfoUnit& column) const {
    const auto* type = InputStruct->FindItemType(column.GetFullName());
    Y_ENSURE(type, "Cannot find a type for window column " << column.GetFullName());
    return type;
}

TString TPhysicalWindowBuilder::AccumulatorName(ui32 funcIndex) const {
    return "__kqp_win_acc_" + ToString(funcIndex);
}

TString TPhysicalWindowBuilder::PositionName(ui32 funcIndex) const {
    return "__kqp_win_pos_" + ToString(funcIndex);
}

TString TPhysicalWindowBuilder::PeerName(ui32 sortIndex) const {
    return "__kqp_win_peer_" + ToString(sortIndex);
}

TExprNode::TPtr TPhysicalWindowBuilder::Member(TExprNode::TPtr from, const TString& name) const {
    // clang-format off
    return Ctx.Builder(Pos)
        .Callable("Member")
            .Add(0, from)
            .Atom(1, name)
        .Seal().Build();
    // clang-format on
}

TExprNode::TPtr TPhysicalWindowBuilder::BuildStruct(const TVector<std::pair<TString, TExprNode::TPtr>>& members) const {
    TExprNode::TListType items;
    for (const auto& [name, value] : members) {
        // clang-format off
        items.push_back(Ctx.Builder(Pos)
            .List()
                .Atom(0, name)
                .Add(1, value)
            .Seal().Build());
        // clang-format on
    }
    return Ctx.NewCallable(Pos, "AsStruct", std::move(items));
}

TExprNode::TPtr TPhysicalWindowBuilder::BuildUint64(ui64 value) const {
    // clang-format off
    return Ctx.Builder(Pos)
        .Callable("Uint64")
            .Atom(0, ToString(value))
        .Seal().Build();
    // clang-format on
}

TExprNode::TPtr TPhysicalWindowBuilder::MakeOptional(TExprNode::TPtr value, bool alreadyOptional) const {
    if (alreadyOptional) {
        return value;
    }
    // clang-format off
    return Ctx.Builder(Pos)
        .Callable("Just")
            .Add(0, value)
        .Seal().Build();
    // clang-format on
}

// Sum widens its accumulator, and the type annotation already recorded the widened type. Cast to
// the same data type here so the physical result matches what was annotated.
TExprNode::TPtr TPhysicalWindowBuilder::BuildSumCastTarget(const TInfoUnit& column) const {
    const auto* itemType = InputItemType(column);
    const TTypeAnnotationNode* sumType = nullptr;
    Y_ENSURE(GetSumResultType(Pos, *itemType, sumType, Ctx), "Unsupported type for sum over a window");
    if (sumType->IsOptionalOrNull()) {
        sumType = sumType->Cast<TOptionalExprType>()->GetItemType();
    }
    return ExpandType(Pos, *sumType, Ctx);
}

TVector<TExprNode::TPtr> TPhysicalWindowBuilder::BuildSortKeys() const {
    TVector<TExprNode::TPtr> keys;

    auto add = [&](ui32 index, bool ascending) {
        // clang-format off
        keys.push_back(Ctx.Builder(Pos)
            .List()
                .Atom(0, ToString(index))
                .Callable(1, "Bool")
                    .Atom(0, ascending ? "true" : "false")
                .Seal()
            .Seal().Build());
        // clang-format on
    };

    // The partition keys come first so that every partition ends up contiguous; their direction
    // does not matter, only that equal keys sit together.
    for (const auto& key : Window->GetPartitionKeys()) {
        add(IndexOf(key), true);
    }
    for (const auto& element : Window->GetSortElements()) {
        add(IndexOf(element.SortColumn), element.Ascending);
    }
    return keys;
}

TExprNode::TPtr TPhysicalWindowBuilder::BuildKeyExtractorLambda() const {
    TExprNode::TListType args;
    for (ui32 i = 0; i < Inputs.size(); ++i) {
        args.push_back(Ctx.NewArgument(Pos, "key_row_" + ToString(i)));
    }

    TExprNode::TListType results;
    for (const auto& key : Window->GetPartitionKeys()) {
        results.push_back(args[IndexOf(key)]);
    }
    return Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, std::move(args)), std::move(results));
}

TExprNode::TPtr TPhysicalWindowBuilder::BuildGroupSwitchLambda() const {
    const auto& partitionKeys = Window->GetPartitionKeys();

    TExprNode::TListType args;
    TExprNode::TListType keyArgs;
    for (ui32 i = 0; i < partitionKeys.size(); ++i) {
        keyArgs.push_back(Ctx.NewArgument(Pos, "switch_key_" + ToString(i)));
        args.push_back(keyArgs.back());
    }
    TExprNode::TListType rowArgs;
    for (ui32 i = 0; i < Inputs.size(); ++i) {
        rowArgs.push_back(Ctx.NewArgument(Pos, "switch_row_" + ToString(i)));
        args.push_back(rowArgs.back());
    }

    // A new group starts as soon as any partition key differs from the one the group was opened
    // with. AggrNotEquals is NULL aware, so all the NULL keys form a single partition.
    TExprNode::TListType comparisons;
    for (ui32 i = 0; i < partitionKeys.size(); ++i) {
        // clang-format off
        comparisons.push_back(Ctx.Builder(Pos)
            .Callable("AggrNotEquals")
                .Add(0, keyArgs[i])
                .Add(1, rowArgs[IndexOf(partitionKeys[i])])
            .Seal().Build());
        // clang-format on
    }

    TExprNode::TPtr body = comparisons.size() == 1 ? comparisons.front() : Ctx.NewCallable(Pos, "Or", std::move(comparisons));
    return Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, std::move(args)), std::move(body));
}

TExprNode::TPtr TPhysicalWindowBuilder::BuildAvgAccumulatorDataType(const TInfoUnit& column) const {
    const auto* itemType = InputItemType(column);
    if (!IsDecimal(itemType)) {
        return Ctx.Builder(Pos).Callable("DataType").Atom(0, "Double").Seal().Build();
    }
    // 35 is the precision the aggregate accumulator uses, so that summing cannot overflow the
    // declared precision before the division brings it back.
    const auto [precision, scale] = DecimalParams(itemType);
    Y_UNUSED(precision);
    // clang-format off
    return Ctx.Builder(Pos)
        .Callable("DataType")
            .Atom(0, "Decimal")
            .Atom(1, "35")
            .Atom(2, scale)
        .Seal().Build();
    // clang-format on
}

TExprNode::TPtr TPhysicalWindowBuilder::BuildAvgAccumulatorType(const TInfoUnit& column) const {
    // clang-format off
    return Ctx.Builder(Pos)
        .Callable("TupleType")
            .Add(0, BuildAvgAccumulatorDataType(column))
            .Callable(1, "DataType")
                .Atom(0, "Uint64")
            .Seal()
        .Seal().Build();
    // clang-format on
}

TExprNode::TPtr TPhysicalWindowBuilder::BuildAccumulator(const TOpWindowFunc& func, ui32 funcIndex, TExprNode::TPtr itemArg,
                                                         TExprNode::TPtr previousState, TExprNode::TPtr sortKeyChanged,
                                                         TVector<std::pair<TString, TExprNode::TPtr>>& stateMembers) const {
    const bool update = static_cast<bool>(previousState);
    const auto accName = AccumulatorName(funcIndex);

    if (func.Kind == EWindowFuncKind::Native) {
        if (!update) {
            // The first row of a partition is row one and opens the first peer group.
            if (func.Function == "rank") {
                stateMembers.emplace_back(PositionName(funcIndex), BuildUint64(1));
            }
            return BuildUint64(1);
        }
        if (func.Function == "rownumber") {
            return Ctx.Builder(Pos).Callable("Inc").Add(0, Member(previousState, accName)).Seal().Build();
        }
        if (func.Function == "denserank") {
            // clang-format off
            return Ctx.Builder(Pos)
                .Callable("If")
                    .Add(0, sortKeyChanged)
                    .Callable(1, "Inc")
                        .Add(0, Member(previousState, accName))
                    .Seal()
                    .Add(2, Member(previousState, accName))
                .Seal().Build();
            // clang-format on
        }
        // A new peer group takes the position of its first row as its rank, so the position has
        // to be advanced before it is read.
        auto position = Ctx.Builder(Pos).Callable("Inc").Add(0, Member(previousState, PositionName(funcIndex))).Seal().Build();
        stateMembers.emplace_back(PositionName(funcIndex), position);
        // clang-format off
        return Ctx.Builder(Pos)
            .Callable("If")
                .Add(0, sortKeyChanged)
                .Add(1, position)
                .Add(2, Member(previousState, accName))
            .Seal().Build();
        // clang-format on
    }

    const auto& argument = func.Arguments.front();
    auto value = Member(itemArg, argument.GetFullName());
    const bool isOptional = InputItemType(argument)->IsOptionalOrNull();

    if (func.Function == "count") {
        if (!update) {
            // clang-format off
            return isOptional
                ? Ctx.Builder(Pos).Callable("AggrCountInit").Add(0, value).Seal().Build()
                : BuildUint64(1);
            // clang-format on
        }
        // clang-format off
        return isOptional
            ? Ctx.Builder(Pos).Callable("AggrCountUpdate").Add(0, value).Add(1, Member(previousState, accName)).Seal().Build()
            : Ctx.Builder(Pos).Callable("Inc").Add(0, Member(previousState, accName)).Seal().Build();
        // clang-format on
    }

    if (func.Function == "sum") {
        // clang-format off
        auto casted = MakeOptional(Ctx.Builder(Pos)
            .Callable("SafeCast")
                .Add(0, value)
                .Add(1, BuildSumCastTarget(argument))
            .Seal().Build(), isOptional);
        return update
            ? Ctx.Builder(Pos)
                .Callable("AggrAdd")
                    .Add(0, Member(previousState, accName))
                    .Add(1, casted)
                .Seal().Build()
            : casted;
        // clang-format on
    }

    if (func.Function == "avg") {
        // The accumulator is an optional (sum, count) pair: NULL until a row contributes, which
        // is also what an average over nothing but NULLs has to return.
        auto accType = BuildAvgAccumulatorType(argument);
        auto accData = BuildAvgAccumulatorDataType(argument);
        auto nothing = Ctx.Builder(Pos).Callable("Nothing").Callable(0, "OptionalType").Add(0, accType).Seal().Seal().Build();

        // clang-format off
        auto firstPair = [&](TExprNode::TPtr present) {
            return Ctx.Builder(Pos)
                .Callable("Just")
                    .List(0)
                        .Callable(0, "SafeCast")
                            .Add(0, present)
                            .Add(1, accData)
                        .Seal()
                        .Callable(1, "Uint64").Atom(0, "1").Seal()
                    .Seal()
                .Seal().Build();
        };
        // clang-format on

        if (!update) {
            if (!isOptional) {
                return firstPair(value);
            }
            auto initArg = Ctx.NewArgument(Pos, "avg_init");
            auto initLambda = Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {initArg}), firstPair(initArg));
            // clang-format off
            return Ctx.Builder(Pos)
                .Callable("IfPresent")
                    .Add(0, value)
                    .Add(1, initLambda)
                    .Add(2, nothing)
                .Seal().Build();
            // clang-format on
        }

        auto previous = Member(previousState, accName);
        // clang-format off
        auto addTo = [&](TExprNode::TPtr state, TExprNode::TPtr present) {
            return Ctx.Builder(Pos)
                .Callable("Just")
                    .List(0)
                        .Callable(0, "AggrAdd")
                            .Callable(0, "Nth").Add(0, state).Atom(1, "0").Seal()
                            .Callable(1, "SafeCast")
                                .Add(0, present)
                                .Add(1, accData)
                            .Seal()
                        .Seal()
                        .Callable(1, "Inc")
                            .Callable(0, "Nth").Add(0, state).Atom(1, "1").Seal()
                        .Seal()
                    .Seal()
                .Seal().Build();
        };
        // clang-format on

        auto valueArg = Ctx.NewArgument(Pos, "avg_value");
        auto stateArg = Ctx.NewArgument(Pos, "avg_state");
        auto withValue = Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {stateArg}), addTo(stateArg, valueArg));
        // clang-format off
        auto merged = Ctx.Builder(Pos)
            .Callable("IfPresent")
                .Add(0, previous)
                .Add(1, withValue)
                .Add(2, firstPair(valueArg))
            .Seal().Build();
        // clang-format on

        if (!isOptional) {
            return Ctx.ReplaceNode(std::move(merged), *valueArg, value);
        }
        auto outer = Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {valueArg}), std::move(merged));
        // clang-format off
        return Ctx.Builder(Pos)
            .Callable("IfPresent")
                .Add(0, value)
                .Add(1, outer)
                .Add(2, previous)
            .Seal().Build();
        // clang-format on
    }

    auto current = MakeOptional(value, isOptional);
    // clang-format off
    return update
        ? Ctx.Builder(Pos)
            .Callable(func.Function == "min" ? "AggrMin" : "AggrMax")
                .Add(0, Member(previousState, accName))
                .Add(1, current)
            .Seal().Build()
        : current;
    // clang-format on
}

TExprNode::TPtr TPhysicalWindowBuilder::BuildResultFromAccumulator(const TOpWindowFunc& func, TExprNode::TPtr accumulator) const {
    if (func.Kind == EWindowFuncKind::Native || func.Function != "avg") {
        return accumulator;
    }

    const auto& argument = func.Arguments.front();
    const auto* itemType = InputItemType(argument);
    const bool decimal = IsDecimal(itemType);

    auto pairArg = Ctx.NewArgument(Pos, "avg_result");
    auto sum = Ctx.Builder(Pos).Callable("Nth").Add(0, pairArg).Atom(1, "0").Seal().Build();
    auto count = Ctx.Builder(Pos).Callable("Nth").Add(0, pairArg).Atom(1, "1").Seal().Build();

    TExprNode::TPtr resultType;
    TExprNode::TPtr value;
    if (decimal) {
        // Back to the precision the column was declared with.
        const auto [precision, scale] = DecimalParams(itemType);
        // clang-format off
        resultType = Ctx.Builder(Pos)
            .Callable("DataType")
                .Atom(0, "Decimal")
                .Atom(1, precision)
                .Atom(2, scale)
            .Seal().Build();
        value = Ctx.Builder(Pos)
            .Callable("SafeCast")
                .Callable(0, "DecimalDiv")
                    .Add(0, sum)
                    .Add(1, count)
                .Seal()
                .Add(1, resultType)
            .Seal().Build();
        // clang-format on
    } else {
        resultType = Ctx.Builder(Pos).Callable("DataType").Atom(0, "Double").Seal().Build();
        // clang-format off
        value = Ctx.Builder(Pos)
            .Callable("Div")
                .Add(0, sum)
                .Add(1, count)
            .Seal().Build();
        // clang-format on
    }

    // clang-format off
    auto divide = Ctx.Builder(Pos)
        .Callable("Just")
            .Add(0, value)
        .Seal().Build();

    return Ctx.Builder(Pos)
        .Callable("IfPresent")
            .Add(0, accumulator)
            .Add(1, Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {pairArg}), std::move(divide)))
            .Callable(2, "Nothing")
                .Callable(0, "OptionalType")
                    .Add(0, resultType)
                .Seal()
            .Seal()
        .Seal().Build();
    // clang-format on
}

// Chain1Map emits, for every row, a tuple of the output row and the state carried to the next
// row. The init lambda sees only the row; the update lambda also sees the tuple emitted for the
// previous row and reads its second element.
TExprNode::TPtr TPhysicalWindowBuilder::BuildChainLambda(bool update) const {
    auto itemArg = Ctx.NewArgument(Pos, "win_item");
    TExprNode::TListType args{itemArg};

    TExprNode::TPtr previousState;
    if (update) {
        auto previousArg = Ctx.NewArgument(Pos, "win_prev");
        args.push_back(previousArg);
        // clang-format off
        previousState = Ctx.Builder(Pos)
            .Callable("Nth")
                .Add(0, previousArg)
                .Atom(1, "1")
            .Seal().Build();
        // clang-format on
    }

    const auto& sortElements = Window->GetSortElements();

    // Whether this row opens a new peer group, compared against the key kept in the state.
    TExprNode::TPtr sortKeyChanged;
    if (update && NeedsPeerKey) {
        TExprNode::TListType comparisons;
        for (ui32 k = 0; k < sortElements.size(); ++k) {
            // clang-format off
            comparisons.push_back(Ctx.Builder(Pos)
                .Callable("AggrNotEquals")
                    .Add(0, Member(itemArg, sortElements[k].SortColumn.GetFullName()))
                    .Add(1, Member(previousState, PeerName(k)))
                .Seal().Build());
            // clang-format on
        }
        sortKeyChanged = comparisons.size() == 1 ? comparisons.front() : Ctx.NewCallable(Pos, "Or", std::move(comparisons));
    }

    TVector<std::pair<TString, TExprNode::TPtr>> stateMembers;
    TVector<std::pair<TString, TExprNode::TPtr>> outputMembers;
    for (const auto& column : Inputs) {
        outputMembers.emplace_back(column.GetFullName(), Member(itemArg, column.GetFullName()));
    }

    const auto& funcs = Window->GetWindowFuncs();
    for (ui32 f = 0; f < funcs.size(); ++f) {
        const auto& func = funcs[f];
        auto accumulator = BuildAccumulator(func, f, itemArg, previousState, sortKeyChanged, stateMembers);
        stateMembers.emplace_back(AccumulatorName(f), accumulator);
        outputMembers.emplace_back(func.ResultColName.GetFullName(), BuildResultFromAccumulator(func, accumulator));
    }

    if (NeedsPeerKey) {
        for (ui32 k = 0; k < sortElements.size(); ++k) {
            stateMembers.emplace_back(PeerName(k), Member(itemArg, sortElements[k].SortColumn.GetFullName()));
        }
    }

    // clang-format off
    auto body = Ctx.Builder(Pos)
        .List()
            .Add(0, BuildStruct(outputMembers))
            .Add(1, BuildStruct(stateMembers))
        .Seal().Build();
    // clang-format on

    return Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, std::move(args)), std::move(body));
}

TExprNode::TPtr TPhysicalWindowBuilder::BuildExpandFromChain(TExprNode::TPtr chained) const {
    // clang-format off
    return Ctx.Builder(Pos)
        .Callable("ExpandMap")
            .Add(0, chained)
            .Lambda(1)
                .Param("win_chained")
                .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                    for (ui32 i = 0; i < OutputLayout.size(); ++i) {
                        parent
                            .Callable(i, "Member")
                                .Callable(0, "Nth")
                                    .Arg(0, "win_chained")
                                    .Atom(1, "0")
                                .Seal()
                                .Atom(1, OutputLayout[i].GetFullName())
                            .Seal();
                    }
                    return parent;
                })
            .Seal()
        .Seal().Build();
    // clang-format on
}

TExprNode::TPtr TPhysicalWindowBuilder::BuildChain(TExprNode::TPtr wideFlow) const {
    auto narrow = NPhysicalConvertionUtils::BuildNarrowMapForWideInput(wideFlow, Inputs, Ctx);

    // clang-format off
    auto chained = Ctx.Builder(Pos)
        .Callable("Chain1Map")
            .Add(0, narrow)
            .Add(1, BuildChainLambda(/*update=*/false))
            .Add(2, BuildChainLambda(/*update=*/true))
        .Seal().Build();
    // clang-format on

    return BuildExpandFromChain(chained);
}

// The fold that produces one state for a whole partition. Fold1 seeds the state from the first
// row and then folds the rest, so the two lambdas are the same accumulators the chain uses.
TExprNode::TPtr TPhysicalWindowBuilder::BuildFoldLambda(bool update) const {
    auto itemArg = Ctx.NewArgument(Pos, "fold_item");
    TExprNode::TListType args{itemArg};

    TExprNode::TPtr previousState;
    if (update) {
        previousState = Ctx.NewArgument(Pos, "fold_state");
        args.push_back(previousState);
    }

    TVector<std::pair<TString, TExprNode::TPtr>> stateMembers;
    const auto& funcs = Window->GetWindowFuncs();
    for (ui32 f = 0; f < funcs.size(); ++f) {
        stateMembers.emplace_back(AccumulatorName(f), BuildAccumulator(funcs[f], f, itemArg, previousState, nullptr, stateMembers));
    }

    return Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, std::move(args)), BuildStruct(stateMembers));
}

TExprNode::TPtr TPhysicalWindowBuilder::BuildExpandFromStructs(TExprNode::TPtr list) const {
    // clang-format off
    return Ctx.Builder(Pos)
        .Callable("ExpandMap")
            .Callable(0, "ToFlow")
                .Add(0, list)
            .Seal()
            .Lambda(1)
                .Param("win_row")
                .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                    for (ui32 i = 0; i < OutputLayout.size(); ++i) {
                        parent
                            .Callable(i, "Member")
                                .Arg(0, "win_row")
                                .Atom(1, OutputLayout[i].GetFullName())
                            .Seal();
                    }
                    return parent;
                })
            .Seal()
        .Seal().Build();
    // clang-format on
}

// A frame that spans the partition gives every row the same value, so the rows are collected
// once, folded to a single state, and then read a second time to attach that state. The state is
// bound as a lambda argument rather than referenced inside the map, so the fold runs once.
TExprNode::TPtr TPhysicalWindowBuilder::BuildBroadcast(TExprNode::TPtr wideFlow) const {
    auto narrow = NPhysicalConvertionUtils::BuildNarrowMapForWideInput(wideFlow, Inputs, Ctx);
    auto rows = Ctx.Builder(Pos).Callable("Collect").Add(0, narrow).Seal().Build();

    // clang-format off
    auto folded = Ctx.Builder(Pos)
        .Callable("Fold1")
            .Add(0, rows)
            .Add(1, BuildFoldLambda(/*update=*/false))
            .Add(2, BuildFoldLambda(/*update=*/true))
        .Seal().Build();
    // clang-format on

    auto stateArg = Ctx.NewArgument(Pos, "win_partition_state");
    auto rowArg = Ctx.NewArgument(Pos, "win_partition_row");

    TVector<std::pair<TString, TExprNode::TPtr>> outputMembers;
    for (const auto& column : Inputs) {
        outputMembers.emplace_back(column.GetFullName(), Member(rowArg, column.GetFullName()));
    }
    const auto& funcs = Window->GetWindowFuncs();
    for (ui32 f = 0; f < funcs.size(); ++f) {
        outputMembers.emplace_back(funcs[f].ResultColName.GetFullName(),
                                   BuildResultFromAccumulator(funcs[f], Member(stateArg, AccumulatorName(f))));
    }

    auto rowLambda = Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {rowArg}), BuildStruct(outputMembers));
    // clang-format off
    auto attach = Ctx.Builder(Pos)
        .Callable("OrderedMap")
            .Add(0, rows)
            .Add(1, rowLambda)
        .Seal().Build();

    auto perState = Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, {stateArg}), std::move(attach));
    auto result = Ctx.Builder(Pos)
        .Callable("OrderedFlatMap")
            .Callable(0, "ToList")
                .Add(0, folded)
            .Seal()
            .Add(1, perState)
        .Seal().Build();
    // clang-format on

    return BuildExpandFromStructs(result);
}

TExprNode::TPtr TPhysicalWindowBuilder::BuildPhysicalOp(TExprNode::TPtr input) {
    Y_ENSURE(CanBuildWindow(*Window), "This window frame is not supported yet");

    Prepare(NPhysicalConvertionUtils::GetLiveInputIUs(*Window, 0));

    // clang-format off
    input = Build<TCoToFlow>(Ctx, Pos)
        .Input(input)
    .Done().Ptr();
    // clang-format on

    input = NPhysicalConvertionUtils::BuildExpandMapForNarrowInput(input, Inputs, Ctx);

    if (const auto sortKeys = BuildSortKeys(); !sortKeys.empty()) {
        // clang-format off
        input = Build<TCoWideSort>(Ctx, Pos)
            .Input(input)
            .Keys<TCoSortKeys>()
                .Add(sortKeys)
            .Build()
        .Done().Ptr();
        // clang-format on
    }

    if (Window->GetPartitionKeys().empty()) {
        // A window with no partition keys sees the whole stage as one partition.
        input = WholePartition ? BuildBroadcast(input) : BuildChain(input);
    } else {
        TExprNode::TListType handlerArgs;
        for (ui32 i = 0; i < Window->GetPartitionKeys().size(); ++i) {
            handlerArgs.push_back(Ctx.NewArgument(Pos, "chop_key_" + ToString(i)));
        }
        auto flowArg = Ctx.NewArgument(Pos, "chop_flow");
        handlerArgs.push_back(flowArg);
        auto handler = Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, std::move(handlerArgs)),
                                     WholePartition ? BuildBroadcast(flowArg) : BuildChain(flowArg));

        // clang-format off
        input = Ctx.Builder(Pos)
            .Callable("WideChopper")
                .Add(0, input)
                .Add(1, BuildKeyExtractorLambda())
                .Add(2, BuildGroupSwitchLambda())
                .Add(3, handler)
            .Seal().Build();
        // clang-format on
    }

    input = NPhysicalConvertionUtils::BuildNarrowMapForWideInput(
        input,
        OutputLayout,
        NPhysicalConvertionUtils::BuildNameSet(NPhysicalConvertionUtils::GetLiveOutputIUs(*Window)),
        Ctx);

    // clang-format off
    input = Build<TCoFromFlow>(Ctx, Pos)
        .Input(input)
    .Done().Ptr();
    // clang-format on

    YQL_CLOG(TRACE, CoreDq) << "[NEW RBO Physical window] " << KqpExprToPrettyString(TExprBase(input), Ctx);
    return input;
}
