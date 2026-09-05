#include "kqp_operator.h"
#include "kqp_rbo_utils.h"

namespace NKikimr::NKqp {

TOpWindow::TOpWindow(TIntrusivePtr<IOperator> input, TPositionHandle pos,
                           TVector<TInfoUnit> keys, TVector<TSortElement> sort,
                           TExprNode::TPtr frame, TExpression functions)
    : IUnaryOperator(EOperator::Window, pos, input)
    , Keys(std::move(keys))
    , Sort(std::move(sort))
    , Frame(std::move(frame))
    , Functions(std::move(functions))
{
    for (const auto& item : Functions.Node->Tail().Children()) {
        Results.emplace_back(TString(item->Head().Content()));
    }
}

void TOpWindow::ComputeOutputIUs() {
    Props.OutputIUs = GetInput()->GetOutputIUs();
    Props.OutputIUs->insert(Props.OutputIUs->end(), Results.begin(), Results.end());
}

TVector<TInfoUnit> TOpWindow::GetUsedIUs(TPlanProps& props) {
    TExpression expression(Functions.Node, Functions.Ctx, &props);
    auto used = expression.GetInputIUs(false, true);
    for (const auto& key : Keys) {
        if (!ContainsInfoUnit(used, key)) {
            used.push_back(key);
        }
    }
    for (const auto& sort : Sort) {
        if (!ContainsInfoUnit(used, sort.SortColumn)) {
            used.push_back(sort.SortColumn);
        }
    }
    return used;
}

TVector<std::reference_wrapper<const TExpression>> TOpWindow::GetExpressions() const {
    return {std::cref(Functions)};
}

void TOpWindow::PropagateLiveness(ILivenessContext& ctx) {
    TInfoUnitSet live;
    const auto& output = ctx.GetLiveOut(this);
    for (const auto& input : GetInput()->GetOutputIUs()) {
        if (output.contains(input)) {
            AddInfoUnit(live, input);
        }
    }
    ctx.AddExpressionDeps(Functions, live);
    AddInfoUnits(live, Keys);
    for (const auto& sort : Sort) {
        AddInfoUnit(live, sort.SortColumn);
    }
    ctx.AddLiveInput(this, 0, live);
}

void TOpWindow::RenameUsedIUs(
    const THashMap<TInfoUnit, TInfoUnit, TInfoUnit::THashFunction>& renames, TExprContext& ctx) {
    Y_UNUSED(ctx);
    Functions = Functions.ApplyRenames(renames);
    for (auto& key : Keys) {
        if (renames.contains(key)) {
            key = renames.at(key);
        }
    }
    for (auto& sort : Sort) {
        if (renames.contains(sort.SortColumn)) {
            sort.SortColumn = renames.at(sort.SortColumn);
        }
    }
}

void TOpWindow::ComputeMetadata(TRBOContext& ctx, TPlanProps& props) {
    IUnaryOperator::ComputeMetadata(ctx, props);
    if (Props.Metadata) {
        Props.Metadata->ColumnsCount = GetOutputIUs().size();
        Props.Metadata->SortColumns.clear();
        Props.Metadata->SortingOrderingIdx.reset();
        Props.Metadata->ShuffledByColumns = Keys;
        Props.Metadata->ShufflingOrderingIdx.reset();
        for (const auto& result : Results) {
            Props.Metadata->ColumnLineage.AddMapping(result, TColumnLineageEntry("", "", ""));
        }
    }
}

TString TOpWindow::ToString(TExprContext& ctx) {
    Y_UNUSED(ctx);
    TStringBuilder text;
    text << "Window: [";
    for (const auto& result : Results) {
        text << result.GetFullName() << " ";
    }
    text << "] partition by [";
    for (const auto& key : Keys) {
        text << key.GetFullName() << " ";
    }
    text << "] order by [";
    for (const auto& sort : Sort) {
        text << sort.ToString() << " ";
    }
    return text << "]";
}

NJson::TJsonValue TOpWindow::ToJson(ui32 flags) {
    auto json = IOperator::ToJson(flags);
    json["PartitionBy"] = NJson::TJsonValue(NJson::JSON_ARRAY);
    json["OrderBy"] = NJson::TJsonValue(NJson::JSON_ARRAY);
    for (const auto& key : Keys) {
        json["PartitionBy"].AppendValue(key.GetFullName());
    }
    for (const auto& sort : Sort) {
        json["OrderBy"].AppendValue(sort.ToString());
    }
    return json;
}

} // namespace NKikimr::NKqp
