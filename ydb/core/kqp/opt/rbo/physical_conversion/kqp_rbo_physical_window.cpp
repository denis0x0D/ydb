#include "kqp_rbo_physical_window.h"

#include <yql/essentials/core/yql_opt_utils.h>
#include <yql/essentials/core/yql_sqlselect.h>
#include <yql/essentials/core/yql_window_features.h>

namespace NKikimr::NKqp {
namespace {

TExprNode::TPtr BuildFrameBound(const TExprNode& frame, TStringBuf side,
                                   bool rows, TExprContext& ctx) {
    const auto bound = GetSetting(frame, side);
    Y_ENSURE(bound, "Missing window frame bound");
    const auto name = bound->Tail().Content();
    const auto valueSetting = GetSetting(frame, TString(side) + "_value");
    const auto value = valueSetting ? valueSetting->TailPtr() : nullptr;
    if (rows) {
        if (name == "up" || name == "uf") {
            return ctx.NewCallable(frame.Pos(), "Void", {});
        }
        i64 offset = 0;
        if (name != "c") {
            Y_ENSURE(value && value->ChildrenSize() == 1, "Expected a constant ROWS frame offset");
            offset = FromString<i32>(value->Head().Content());
            if (name == "p") {
                offset = -offset;
            }
        }
        return ctx.NewCallable(frame.Pos(), "Int32", {ctx.NewAtom(frame.Pos(), ToString(offset))});
    }
    if (name == "c") {
        return ctx.NewList(frame.Pos(), {ctx.NewAtom(frame.Pos(), "currentRow")});
    }
    return ctx.NewList(frame.Pos(), {
        ctx.NewAtom(frame.Pos(), name == "p" || name == "up" ? "preceding" : "following"),
        value ? value : ctx.NewAtom(frame.Pos(), "unbounded")});
}

} // namespace

TExprNode::TPtr BuildPhysicalWindow(TOpWindow& window, TExprNode::TPtr input, TRBOContext& rbo) {
    auto& ctx = rbo.ExprCtx;
    const auto pos = window.Pos;
    // A complete partition arrives through the stage's shuffle (or global gather).
    // Peephole lowers the local CalcOverWindow, including partition sorting.
    auto list = ctx.NewCallable(pos, "ForwardList", {input});
    auto listType = ctx.NewCallable(pos, "TypeOf", {list});
    auto row = ctx.NewArgument(pos, "window_row");
    TExprNode::TListType directions;
    TExprNode::TListType sortKeys;
    for (const auto& sort : window.Sort) {
        auto member = ctx.NewCallable(pos, "Member", {row, ctx.NewAtom(pos, sort.SortColumn.GetFullName())});
        // Encode NULL placement separately; reversing direction also reverses this marker.
        sortKeys.push_back(ctx.NewCallable(pos, "Exists", {member}));
        directions.push_back(ctx.NewCallable(pos, "Bool", {ctx.NewAtom(pos, sort.NullsFirst ? "true" : "false")}));
        sortKeys.push_back(member);
        directions.push_back(ctx.NewCallable(pos, "Bool", {ctx.NewAtom(pos, sort.Ascending ? "true" : "false")}));
    }
    auto sortLambda = ctx.NewLambda(pos, ctx.NewArguments(pos, {row}),
        sortKeys.empty() ? ctx.NewCallable(pos, "Void", {}) : ctx.NewList(pos, std::move(sortKeys)));
    auto sortTraits = directions.empty() ? ctx.NewCallable(pos, "Void", {}) :
        ctx.NewCallable(pos, "SortTraits", {listType, ctx.NewList(pos, std::move(directions)), sortLambda});

    const auto frameType = GetSetting(*window.Frame, "type");
    Y_ENSURE(frameType && (frameType->Tail().Content() == "rows" || frameType->Tail().Content() == "range"),
             "New RBO supports ROWS and RANGE window frames");
    Y_ENSURE(!GetSetting(*window.Frame, "exclude"), "Window frame exclusions are not supported by new RBO");
    const bool rows = frameType->Tail().Content() == "rows";
    TExprNode::TListType settings = {
        ctx.NewList(pos, {ctx.NewAtom(pos, "begin"), BuildFrameBound(*window.Frame, "from", rows, ctx)}),
        ctx.NewList(pos, {ctx.NewAtom(pos, "end"), BuildFrameBound(*window.Frame, "to", rows, ctx)}),
        ctx.NewList(pos, {ctx.NewAtom(pos, "compact")})};
    if (IsWindowNewPipelineEnabled(rbo.TypeCtx)) {
        settings.push_back(ctx.NewList(pos, {ctx.NewAtom(pos, "sortSpec"), sortTraits}));
    }
    TExprNode::TListType frame = {ctx.NewList(pos, std::move(settings))};
    const auto functions = NNodes::TCoLambda(window.Functions.Node);
    for (const auto& item : functions.Body().Ref().Children()) {
        auto call = item->TailPtr();
        auto rewrite = [&](TExprNode::TPtr expression, TExprNode::TPtr argument) {
            return ctx.ReplaceNode(std::move(expression), functions.Args().Arg(0).Ref(), std::move(argument));
        };
        TExprNode::TPtr traits;
        if (call->IsCallable("YqlAggWin")) {
            auto extractor = ctx.NewLambda(pos, ctx.NewArguments(pos, {row}), rewrite(call->ChildPtr(4), row));
            traits = ExpandYqlTraitsFactory(call->HeadPtr(), listType, extractor, ctx, rbo.TypeCtx);
        } else {
            traits = ExpandSqlWindowCall(call, listType, sortLambda, rewrite, ctx, rbo.TypeCtx);
        }
        Y_ENSURE(traits, "Cannot expand window function in new RBO");
        frame.push_back(ctx.NewList(pos, {item->HeadPtr(), traits}));
    }
    TExprNode::TListType keys;
    for (const auto& key : window.Keys) {
        keys.push_back(ctx.NewAtom(pos, key.GetFullName()));
    }
    auto calc = ctx.NewCallable(pos, "CalcOverWindow", {
        list, ctx.NewList(pos, std::move(keys)), sortTraits,
        ctx.NewList(pos, {ctx.NewCallable(pos, rows ? "WinOnRows" : "WinOnRange", std::move(frame))})});
    return ctx.NewCallable(pos, "Iterator", {calc});
}

} // namespace NKikimr::NKqp
