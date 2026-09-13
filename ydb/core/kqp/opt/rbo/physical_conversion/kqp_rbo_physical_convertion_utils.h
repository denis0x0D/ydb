#pragma once
#include <ydb/core/kqp/opt/rbo/kqp_rbo.h>
#include <yql/essentials/core/yql_opt_utils.h>
#include <yql/essentials/utils/log/log.h>

using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;

namespace NKikimr::NKqp::NPhysicalConvertionUtils {

TString GetFullName(const TString& name);
TString GetFullName(const TInfoUnit& name);

// Returns LiveOut in logical schema order.
TVector<TInfoUnit> GetLiveOutputIUs(IOperator& op);

// Returns child-edge LiveIn in the child's logical schema order.
TVector<TInfoUnit> GetLiveInputIUs(IOperator& op, ui32 childIndex);

TExprNode::TPtr BuildMultiConsumerHandler(TExprNode::TPtr input, const ui32 numConsumers, TExprContext& ctx, TPositionHandle pos);
bool IsMultiConsumerHandlerNeeded(const TIntrusivePtr<IOperator>& op);
TCoAtomList BuildAtomList(TStringBuf value, TPositionHandle pos, TExprContext& ctx);
TExprNode::TPtr ReplaceArg(TExprNode::TPtr input, TExprNode::TPtr arg, TExprContext &ctx, bool removeAliases = false);
TExprNode::TPtr ExtractMembers(TExprNode::TPtr input, TExprContext &ctx, TVector<TInfoUnit> members);
TExprNode::TPtr BuildRenameMap(TExprNode::TPtr input, const TVector<std::pair<TString, TString>>& renames, TExprContext& ctx);
TExprNode::TPtr ConvertToWideJoinFilter(TExprNode::TPtr input, const TVector<TInfoUnit>& inputs,
                                        const TVector<bool>& unwrapOptionalInputs, TExprContext& ctx);
TExprNode::TPtr BuildVoidLambda(TExprContext& ctx, TPositionHandle pos);

template <typename T>
THashSet<TString> BuildNameSet(const TVector<T>& columns) {
    THashSet<TString> result;
    for (const auto& column : columns) {
        result.insert(GetFullName(column));
    }
    return result;
}

template <typename T>
TExprNode::TPtr BuildExpandMapForNarrowInput(TExprNode::TPtr input, const TVector<T>& inputs, TExprContext& ctx) {
    // clang-format off
    return ctx.Builder(input->Pos())
        .Callable("ExpandMap")
            .Add(0, input)
            .Lambda(1)
                .Param("narrow_input_param")
                .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                    for (ui32 i = 0; i < inputs.size(); ++i) {
                        parent
                            .Callable(i, "Member")
                                .Arg(0, "narrow_input_param")
                                .Atom(1, GetFullName(inputs[i]))
                            .Seal();
                    }
                    return parent;
                })
            .Seal()
        .Seal().Build();
    // clang-format on
}

template <typename T>
TExprNode::TPtr BuildNarrowMapForWideInput(TExprNode::TPtr input, const TVector<T>& inputs, const THashSet<TString>& outputs, TExprContext& ctx) {
    // clang-format off
    return ctx.Builder(input->Pos())
        .Callable("NarrowMap")
            .Add(0, input)
            .Lambda(1)
                .Params("wide_input", inputs.size())
                .Callable("AsStruct")
                .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                    ui32 outIndex = 0;
                    for (ui32 i = 0; i < inputs.size(); ++i) {
                        const auto name = GetFullName(inputs[i]);
                        if (outputs.contains(name)) {
                            parent.List(outIndex++)
                                .Atom(0, GetFullName(inputs[i]))
                                .Arg(1, "wide_input", i)
                            .Seal();
                        }
                    }
                    return parent;
                })
                .Seal()
            .Seal()
        .Seal()
    .Build();
    // clang-format on
}

template <typename T>
TExprNode::TPtr BuildNarrowMapForWideInput(TExprNode::TPtr input, const TVector<T>& inputs, TExprContext& ctx) {
    // clang-format off
    return ctx.Builder(input->Pos())
        .Callable("NarrowMap")
            .Add(0, input)
            .Lambda(1)
                .Params("wide_input", inputs.size())
                .Callable("AsStruct")
                .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                    for (ui32 i = 0; i < inputs.size(); ++i) {
                        parent.List(i)
                            .Atom(0, GetFullName(inputs[i]))
                            .Arg(1, "wide_input", i)
                        .Seal();
                    }
                    return parent;
                })
                .Seal()
            .Seal()
        .Seal()
    .Build();
    // clang-format on
}

template <typename T>
TExprNode::TPtr BuildNarrowMapForWideInput(TExprNode::TPtr input, const TVector<T>& inputs, const THashMap<ui32, TString>& renameMap, TExprContext& ctx) {
    // clang-format off
    return ctx.Builder(input->Pos())
        .Callable("NarrowMap")
            .Add(0, input)
            .Lambda(1)
                .Params("wide_input", inputs.size())
                .Callable("AsStruct")
                .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                    for (ui32 i = 0; i < inputs.size(); ++i) {
                        auto it = renameMap.find(i);
                        const auto fullName = it != renameMap.end() ? it->second : GetFullName(inputs[i]);
                        parent.List(i)
                            .Atom(0, fullName)
                            .Arg(1, "wide_input", i)
                        .Seal();
                    }
                    return parent;
                })
                .Seal()
            .Seal()
        .Seal()
    .Build();
    // clang-format on
}

/**
 * Rearranges a wide flow so that its slots are exactly `to`, taken from `from` by name.
 * `to` must be a permutation of a subset of `from`.
 */
TExprNode::TPtr BuildWideProjection(TExprNode::TPtr input, const TVector<TInfoUnit>& from, const TVector<TInfoUnit>& to, TExprContext& ctx);

/**
 * Body of a physical stage while it is assembled operator by operator.
 *
 * Operators are emitted bottom-up and most of them are wide internally (WideMap,
 * WideFilter, WideSort, WideCombiner). Handing the body between two such operators in
 * wide form avoids emitting a NarrowMap/ExpandMap pair per operator that peephole would
 * only have to fuse away again, which costs one fixpoint round and a full re-annotation
 * of the stage lambda per layer.
 *
 * The narrow form - a stream of structs - is what stage arguments, connections and the
 * operators with no wide counterpart (Take/Skip, Switch, Extend) work with.
 */
class TStageBody {
public:
    TStageBody() = default;

    static TStageBody Narrow(TExprNode::TPtr stream) {
        TStageBody body;
        body.NarrowStream = std::move(stream);
        return body;
    }

    // `columns` names the slots of `flow`, in order.
    static TStageBody Wide(TExprNode::TPtr flow, TVector<TInfoUnit> columns) {
        TStageBody body;
        body.WideFlow = std::move(flow);
        body.WideColumns = std::move(columns);
        return body;
    }

    explicit operator bool() const {
        return bool(NarrowStream) || bool(WideFlow);
    }

    // Wide flow whose slots are exactly `columns`. Expands the narrow form, or permutes
    // the wide one, only when the layout does not already match.
    TExprNode::TPtr AsWide(const TVector<TInfoUnit>& columns, TExprContext& ctx) const;

    // Stream of structs holding the columns this body currently carries.
    TExprNode::TPtr AsNarrow(TExprContext& ctx) const;

private:
    TExprNode::TPtr NarrowStream;
    TExprNode::TPtr WideFlow;
    TVector<TInfoUnit> WideColumns;
};

// The Switch that fans a body out to several consumers works on narrow streams.
TStageBody BuildMultiConsumerHandler(const TStageBody& input, const ui32 numConsumers, TExprContext& ctx, TPositionHandle pos);

} // namespace NKikimr::NKqp::NPhysicalConvertionUtils
