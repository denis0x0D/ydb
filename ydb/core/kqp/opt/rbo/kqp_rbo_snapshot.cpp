#include "kqp_rbo_snapshot.h"

#include <yql/essentials/ast/serialize/yql_expr_serialize.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>

#include <algorithm>

namespace NKikimr::NKqp {

using namespace NYql;

// Public APIs only allocate fresh IDs and Replicate ports; a snapshot restores
// the existing ones.
class TPlanSnapshotAccess {
public:
    static void WriteRegistry(const TInfoUnitRegistry& registry, NKikimrKqpRbo::TPlanRoot& proto) {
        Y_ENSURE(!registry.DisplayNames_, "Display names are finalized only after the early stages");
        for (const auto& entry : registry.Entries_) {
            auto& unit = *proto.AddInfoUnits();
            unit.SetAlias(entry.InfoUnit.GetAlias());
            unit.SetColumn(entry.InfoUnit.GetColumnName());
            if (entry.Annotation) {
                unit.SetAnnotation(*entry.Annotation);
            }
        }

        // Sorted, so that equal plans serialize equally.
        TVector<std::pair<TString, ui32>> counters(registry.GeneratedCounters_.begin(), registry.GeneratedCounters_.end());
        std::sort(counters.begin(), counters.end());
        for (const auto& [prefix, value] : counters) {
            auto& counter = *proto.AddGeneratedCounters();
            counter.SetPrefix(prefix);
            counter.SetValue(value);
        }
    }

    static void ReadRegistry(const NKikimrKqpRbo::TPlanRoot& proto, TInfoUnitRegistry& registry) {
        Y_ENSURE(registry.Size() == 0, "Restoring into a non-empty registry");
        for (const auto& unit : proto.GetInfoUnits()) {
            registry.Add(TInfoUnit(unit.GetAlias(), unit.GetColumn()),
                unit.HasAnnotation() ? std::make_optional(unit.GetAnnotation()) : std::nullopt);
        }
        for (const auto& counter : proto.GetGeneratedCounters()) {
            registry.GeneratedCounters_[counter.GetPrefix()] = counter.GetValue();
        }
    }

    static ui32 GetNextOutputIndex(const TReplicate& hub) {
        return hub.NextOutputIndex_;
    }

    // Raw, without refreshing against the current input: a refresh may allocate IDs.
    static const TMappedIUs<TInfoUnitId>& GetRebindings(const TOpReplicate& port) {
        return port.Rebindings_;
    }

    static TIntrusivePtr<TReplicate> MakeHub(TIntrusivePtr<IOperator> input, TPositionHandle pos,
        TInfoUnitRegistry& registry, ui32 nextOutputIndex)
    {
        auto hub = TReplicate::Create(std::move(input), pos, registry);
        hub->NextOutputIndex_ = nextOutputIndex;
        return hub;
    }

    static TIntrusivePtr<TOpReplicate> MakePort(TIntrusivePtr<TReplicate> hub, ui32 index, TMappedIUs<TInfoUnitId> rebindings) {
        Y_ENSURE(index < hub->NextOutputIndex_, "Replicate port " << index << " was never allocated");
        TIntrusivePtr<TOpReplicate> port(new TOpReplicate(std::move(hub), index));
        port->Rebindings_ = std::move(rebindings);
        return port;
    }
};

namespace {

using TProtoIds = google::protobuf::RepeatedField<ui32>;

template <typename TRange>
void WriteIds(const TRange& ids, TProtoIds& out) {
    for (const TInfoUnitId id : ids) {
        out.Add(id);
    }
}

template <typename TEnum>
TEnum ReadEnum(ui32 value, TEnum last) {
    Y_ENSURE(value <= static_cast<ui32>(last), "Invalid snapshot enum value " << value);
    return static_cast<TEnum>(value);
}

void WritePosition(TPositionHandle pos, const TExprContext& ctx, NKikimrKqpRbo::TPosition& proto) {
    const auto& position = ctx.GetPosition(pos);
    proto.SetRow(position.Row);
    proto.SetColumn(position.Column);
    proto.SetFile(TString(position.File.data(), position.File.size()));
}

class TPlanWriter {
public:
    TPlanWriter(TOpRoot& root, TExprContext& ctx)
        : Root(root)
        , Ctx(ctx)
    {}

    NKikimrKqpRbo::TPlanRoot Write() && {
        TPlanSnapshotAccess::WriteRegistry(Root.PlanProps.InfoUnitRegistry, Proto);

        // Referenced subplans and inputs precede their consumers.
        for (const auto& item : Root.Iterate(ETraversalOrder::PostOrder)) {
            AddOperator(*item.Current);
        }
        for (const auto& [binding, entry] : Root.PlanProps.Subplans) {
            // Keep registered subplans that no expression references any more.
            for (const auto& item : IterateSubtreeWithSubplans(entry.Plan.Get(), Root.PlanProps)) {
                if (!Operators.contains(item.Current)) {
                    AddOperator(*item.Current);
                }
            }
        }

        for (const auto& [binding, entry] : Root.PlanProps.Subplans) {
            auto& subplan = *Proto.AddSubplans();
            subplan.SetBinding(binding);
            subplan.SetPlan(GetOperator(entry.Plan.Get()));
            subplan.SetType(static_cast<ui32>(entry.Type));
            WriteIds(entry.Tuple.Items(), *subplan.MutableTuple());
            if (entry.ResultIU) {
                subplan.SetResultIU(*entry.ResultIU);
            }
        }

        Proto.SetInput(GetOperator(Root.GetInput().Get()));
        WritePosition(Root.Pos, Ctx, *Proto.MutablePos());
        for (const auto& [id, name] : Root.GetColumns().Items()) {
            WriteNamedIU(id, name, *Proto.AddColumns());
        }
        for (const auto& column : Root.GetQueryColumns()) {
            Proto.AddQueryColumns(column);
        }
        Proto.SetPgSyntax(Root.PlanProps.PgSyntax);
        Proto.SetWithEffects(Root.PlanProps.WithEffects);
        Proto.SetWithReturning(Root.PlanProps.WithReturning);

        const auto expressions = Ctx.NewList(Root.Pos, std::move(Expressions));
        Proto.SetExpressions(SerializeGraph(*expressions, Ctx, TSerializedExprGraphComponents::Positions));
        return std::move(Proto);
    }

private:
    ui32 GetOperator(const IOperator* op) const {
        const auto it = Operators.find(op);
        Y_ENSURE(it != Operators.end(), "Operator " << op->GetExplainName() << " is not serialized before its consumer");
        return it->second;
    }

    ui32 AddExpr(const TExprNode::TPtr& node) {
        Y_ENSURE(node, "Null expression in a plan");
        if (const auto it = ExpressionIds.find(node.Get()); it != ExpressionIds.end()) {
            return it->second;
        }

        const auto index = static_cast<ui32>(Expressions.size());
        Expressions.push_back(node);
        ExpressionIds.emplace(node.Get(), index);
        Proto.AddExpressionEntries();

        if (node->GetTypeAnn()) {
            TVector<ui32> argTypes;
            if (node->IsLambda()) {
                for (const auto& arg : node->Head().Children()) {
                    Y_ENSURE(arg->GetTypeAnn(), "Typed lambda with an untyped argument");
                    argTypes.push_back(AddType(*arg->GetTypeAnn()));
                }
            }
            auto& entry = *Proto.MutableExpressionEntries(static_cast<int>(index));
            entry.SetTyped(true);
            for (const auto type : argTypes) {
                entry.AddArgTypes(type);
            }
        }
        return index;
    }

    ui32 AddExpr(const TExpression& expression) {
        return AddExpr(expression.Node);
    }

    // A type travels as its type expression and is annotated back on restore.
    ui32 AddType(const TTypeAnnotationNode& type) {
        if (const auto it = TypeIds.find(&type); it != TypeIds.end()) {
            return it->second;
        }
        const auto index = AddExpr(ExpandType(Root.Pos, type, Ctx));
        Proto.MutableExpressionEntries(static_cast<int>(index))->SetIsType(true);
        TypeIds.emplace(&type, index);
        return index;
    }

    static void WriteNamedIU(TInfoUnitId id, const TString& name, NKikimrKqpRbo::TNamedIU& proto) {
        proto.SetId(id);
        proto.SetName(name);
    }

    static void WriteSortKeys(const TSortIUs& keys, google::protobuf::RepeatedPtrField<NKikimrKqpRbo::TSortKey>& out) {
        for (const auto& [id, order] : keys.Items()) {
            auto& key = *out.Add();
            key.SetId(id);
            key.SetAscending(order.Ascending);
            key.SetNullsFirst(order.NullsFirst);
        }
    }

    static void WriteJoinKeys(const TJoinIUs& keys, google::protobuf::RepeatedPtrField<NKikimrKqpRbo::TJoinKey>& out) {
        for (const auto& key : keys.Items()) {
            auto& proto = *out.Add();
            proto.SetLeft(key.first);
            proto.SetRight(key.second);
            proto.SetEqualNulls(key.EqualNulls);
        }
    }

    static void WriteIdPairs(const TMappedIUs<TInfoUnitId>& pairs, google::protobuf::RepeatedPtrField<NKikimrKqpRbo::TIdPair>& out) {
        for (const auto& [key, value] : pairs.Items()) {
            auto& pair = *out.Add();
            pair.SetKey(key);
            pair.SetValue(value);
        }
    }

    void WritePointPrefix(const TOpRead::TPointPrefix& prefix, NKikimrKqpRbo::TPointPrefix& proto) {
        proto.SetTable(prefix.Table);
        if (prefix.Points) {
            proto.SetPoints(AddExpr(prefix.Points));
        }
        if (prefix.PointsItemType) {
            proto.SetPointsItemType(AddType(*prefix.PointsItemType));
        }
        for (const auto& column : prefix.Columns) {
            proto.AddColumns(column);
        }
        if (prefix.ExpectedMaxPoints) {
            proto.SetExpectedMaxPoints(*prefix.ExpectedMaxPoints);
        }
    }

    void WriteRead(TOpRead& read, NKikimrKqpRbo::TRead& proto) {
        proto.SetAlias(read.Alias);
        proto.SetStorageType(static_cast<ui32>(read.StorageType));
        proto.SetTableCallable(AddExpr(read.TableCallable));
        if (read.OlapFilterLambda) {
            proto.SetOlapFilterLambda(AddExpr(read.OlapFilterLambda));
        }
        if (read.Limit) {
            proto.SetLimit(AddExpr(read.Limit));
        }
        if (read.OriginalPredicate) {
            proto.SetOriginalPredicate(AddExpr(*read.OriginalPredicate));
        }
        proto.SetSortDir(static_cast<ui32>(read.SortDir));
        if (read.RangeInfo) {
            const auto& ranges = *read.RangeInfo;
            auto& rangesProto = *proto.MutableRangeInfo();
            if (ranges.ComputeNode) {
                rangesProto.SetComputeNode(AddExpr(ranges.ComputeNode));
            }
            for (const auto& column : ranges.KeyColumns) {
                rangesProto.AddKeyColumns(column);
            }
            rangesProto.SetUsedPrefixLen(ranges.UsedPrefixLen);
            rangesProto.SetPointPrefixLen(ranges.PointPrefixLen);
            if (ranges.ExpectedMaxRanges) {
                rangesProto.SetExpectedMaxRanges(*ranges.ExpectedMaxRanges);
            }
            for (const auto& prefix : ranges.PointPrefixes) {
                WritePointPrefix(prefix, *rangesProto.AddPointPrefixes());
            }
            rangesProto.SetMainTable(ranges.MainTable);
            if (ranges.LiteralRange) {
                rangesProto.SetLiteralRange(AddExpr(ranges.LiteralRange));
            }
        }
        WriteIds(read.GetColumns(), *proto.MutableColumns());
    }

    void WriteTableLookup(TOpTableLookup& lookup, NKikimrKqpRbo::TTableLookup& proto) {
        proto.SetTable(AddExpr(lookup.Table));
        WriteIds(lookup.GetColumns(), *proto.MutableColumns());
        for (const auto& [id, name] : lookup.LookupKeys.Items()) {
            WriteNamedIU(id, name, *proto.AddLookupKeys());
        }
        proto.SetJoinKind(lookup.JoinKind);
        if (lookup.FetchedRowFilter) {
            proto.SetFetchedRowFilter(AddExpr(*lookup.FetchedRowFilter));
        }
        if (lookup.Prefix) {
            auto& prefix = *proto.MutablePrefix();
            prefix.SetPoints(AddExpr(lookup.Prefix->Points));
            prefix.SetPointsItemType(AddType(*lookup.Prefix->PointsItemType));
            for (const auto& column : lookup.Prefix->Columns) {
                prefix.AddColumns(column);
            }
            for (const auto& [id, name] : lookup.Prefix->Equalities.Items()) {
                WriteNamedIU(id, name, *prefix.AddEqualities());
            }
        }
        proto.SetStrategy(static_cast<ui32>(lookup.Strategy));
        WriteJoinKeys(lookup.ResidualJoinKeys, *proto.MutableResidualJoinKeys());
        proto.SetAllowNullKeys(lookup.AllowNullKeys);
        proto.SetKeysFromInputLookup(lookup.KeysFromInputLookup);
        if (lookup.SourceRead) {
            auto& source = *proto.MutableSourceRead();
            source.SetPredicate(AddExpr(lookup.SourceRead->Predicate));
            for (const auto& prefix : lookup.SourceRead->PointPrefixes) {
                WritePointPrefix(prefix, *source.AddPointPrefixes());
            }
        }
    }

    void WriteTableEffect(TOpTableEffect& effect, NKikimrKqpRbo::TTableEffect& proto) {
        proto.SetTable(AddExpr(effect.Table));
        proto.SetType(static_cast<ui32>(effect.EffectType));

        // An engaged empty list stays engaged.
        auto writeStrings = [](const std::optional<TVector<TString>>& values, auto mutableList) {
            if (values) {
                auto* list = mutableList();
                for (const auto& value : *values) {
                    list->AddValues(value);
                }
            }
        };
        const auto& options = effect.Options;
        auto& optionsProto = *proto.MutableOptions();
        writeStrings(options.Columns, [&] { return optionsProto.MutableColumns(); });
        writeStrings(options.ReturningColumns, [&] { return optionsProto.MutableReturningColumns(); });
        writeStrings(options.DefaultColumns, [&] { return optionsProto.MutableDefaultColumns(); });
        if (options.OnConflict) {
            optionsProto.SetOnConflict(*options.OnConflict);
        }
        if (options.IsBatch) {
            optionsProto.SetIsBatch(*options.IsBatch);
        }
        if (options.Settings) {
            auto& settings = *optionsProto.MutableSettings();
            for (const auto& setting : *options.Settings) {
                settings.AddValues(AddExpr(setting));
            }
        }

        for (const auto& [id, name] : effect.GetColumns().Items()) {
            WriteNamedIU(id, name, *proto.AddColumns());
        }
        for (const auto& [id, name] : effect.GetReturningColumns().Items()) {
            WriteNamedIU(id, name, *proto.AddReturning());
        }
    }

    ui32 GetHub(const TReplicate& hub) {
        if (const auto it = Hubs.find(&hub); it != Hubs.end()) {
            return it->second;
        }
        const ui32 index = Proto.HubsSize();
        auto& proto = *Proto.AddHubs();
        proto.SetInput(GetOperator(hub.GetInput().Get()));
        WritePosition(hub.Pos, Ctx, *proto.MutablePos());
        proto.SetNextOutputIndex(TPlanSnapshotAccess::GetNextOutputIndex(hub));
        Hubs.emplace(&hub, index);
        return index;
    }

    void AddOperator(IOperator& op) {
        const auto& props = op.Props;
        // Physical decisions are made at CBO or later.
        Y_ENSURE(!props.StageId && !props.Algorithm && !props.OrderEnforcer && !props.JoinAlgo && !props.UseBlockHashJoin
            && !props.LeftShuffleBy && !props.RightShuffleBy, op.GetExplainName() << " has physical properties before CBO");

        NKikimrKqpRbo::TOperator proto;
        WritePosition(op.Pos, Ctx, *proto.MutablePos());

        switch (op.Kind) {
            case EOperator::EmptySource: {
                auto& source = CastOperator<TOpEmptySource>(op);
                auto& sourceProto = *proto.MutableEmptySource();
                if (source.Input) {
                    sourceProto.SetInput(AddExpr(source.Input));
                }
                WriteIds(source.GetOutputIUs(), *sourceProto.MutableColumns());
                break;
            }
            case EOperator::Source:
                WriteRead(CastOperator<TOpRead>(op), *proto.MutableRead());
                break;
            case EOperator::Map: {
                auto& map = CastOperator<TOpMap>(op);
                auto& mapProto = *proto.MutableMap();
                for (const auto& [output, element] : map.GetMapElements().Items()) {
                    auto& elementProto = *mapProto.AddElements();
                    elementProto.SetOutput(output);
                    elementProto.SetExpression(AddExpr(element.GetExpression()));
                }
                mapProto.SetNeedToPush(map.NeedToPush);
                break;
            }
            case EOperator::Filter: {
                auto& filter = CastOperator<TOpFilter>(op);
                proto.MutableFilter()->SetExpression(AddExpr(filter.GetFilterExpression()));
                proto.MutableFilter()->SetPartiallyPushedDown(filter.PartiallyPushedDown);
                break;
            }
            case EOperator::Join: {
                auto& join = CastOperator<TOpJoin>(op);
                auto& joinProto = *proto.MutableJoin();
                joinProto.SetKind(join.JoinKind);
                WriteJoinKeys(join.JoinKeys, *joinProto.MutableKeys());
                for (const auto& filter : join.JoinFilters) {
                    joinProto.AddFilters(AddExpr(filter));
                }
                break;
            }
            case EOperator::Aggregate: {
                auto& aggregate = CastOperator<TOpAggregate>(op);
                auto& aggregateProto = *proto.MutableAggregate();
                for (const auto& [output, traits] : aggregate.GetAggregationTraits().Items()) {
                    auto& aggregation = *aggregateProto.AddAggregations();
                    aggregation.SetOutput(output);
                    aggregation.SetInput(traits.Input);
                    aggregation.SetFunction(traits.AggFunction);
                    aggregation.SetDistinct(traits.Distinct);
                    aggregation.SetUnwrap(traits.Unwrap);
                }
                WriteIds(aggregate.GetKeyColumns().Items(), *aggregateProto.MutableKeys());
                aggregateProto.SetPhase(static_cast<ui32>(aggregate.GetAggregationPhase()));
                aggregateProto.SetDistinctAll(aggregate.IsDistinctAll());
                aggregateProto.SetPushedToOlap(aggregate.IsPushedToOlap());
                break;
            }
            case EOperator::GroupingSets: {
                auto& groupingSets = CastOperator<TOpGroupingSets>(op);
                auto& groupingSetsProto = *proto.MutableGroupingSets();
                for (const auto& set : groupingSets.GetGroupingSets()) {
                    WriteIds(set, *groupingSetsProto.AddSets()->MutableIds());
                }
                WriteIdPairs(groupingSets.GetColumns(), *groupingSetsProto.MutableColumns());
                WriteIdPairs(groupingSets.GetGroupingIndicators(), *groupingSetsProto.MutableIndicators());
                break;
            }
            case EOperator::Window: {
                auto& window = CastOperator<TOpWindow>(op);
                auto& windowProto = *proto.MutableWindow();
                for (const auto& [output, function] : window.GetWindowFuncs().Items()) {
                    auto& functionProto = *windowProto.AddFunctions();
                    functionProto.SetOutput(output);
                    functionProto.SetFunction(function.Function);
                    functionProto.SetKind(static_cast<ui32>(function.Kind));
                    WriteIds(function.Arguments.Items(), *functionProto.MutableArguments());
                }
                WriteIds(window.GetPartitionKeys().Items(), *windowProto.MutablePartitionKeys());
                WriteSortKeys(window.GetSortElements(), *windowProto.MutableSortKeys());
                const auto& frame = window.GetFrame();
                auto& frameProto = *windowProto.MutableFrame();
                frameProto.SetType(static_cast<ui32>(frame.Type));
                frameProto.SetBeginKind(static_cast<ui32>(frame.BeginKind));
                frameProto.SetBeginValue(frame.BeginValue);
                frameProto.SetEndKind(static_cast<ui32>(frame.EndKind));
                frameProto.SetEndValue(frame.EndValue);
                break;
            }
            case EOperator::Limit: {
                auto& limit = CastOperator<TOpLimit>(op);
                auto& limitProto = *proto.MutableLimit();
                limitProto.SetLimit(AddExpr(limit.LimitCond));
                if (const auto offset = limit.GetOffsetCond()) {
                    limitProto.SetOffset(AddExpr(*offset));
                }
                limitProto.SetPhase(static_cast<ui32>(limit.GetLimitPhase()));
                break;
            }
            case EOperator::Sort: {
                auto& sort = CastOperator<TOpSort>(op);
                auto& sortProto = *proto.MutableSort();
                WriteSortKeys(sort.GetSortElements(), *sortProto.MutableKeys());
                if (sort.LimitCond) {
                    sortProto.SetLimit(AddExpr(*sort.LimitCond));
                }
                sortProto.SetPhase(static_cast<ui32>(sort.GetSortPhase()));
                break;
            }
            case EOperator::UnionAll: {
                auto& unionAll = CastOperator<TOpUnionAll>(op);
                auto& unionAllProto = *proto.MutableUnionAll();
                for (const auto& [output, row] : unionAll.GetColumns().Items()) {
                    auto& column = *unionAllProto.AddColumns();
                    column.SetOutput(output);
                    WriteIds(row.Inputs, *column.MutableInputs());
                }
                unionAllProto.SetOrdered(unionAll.Ordered);
                break;
            }
            case EOperator::TableLookup:
                WriteTableLookup(CastOperator<TOpTableLookup>(op), *proto.MutableTableLookup());
                break;
            case EOperator::TableEffect:
                WriteTableEffect(CastOperator<TOpTableEffect>(op), *proto.MutableTableEffect());
                break;
            case EOperator::Replicate: {
                auto& port = CastOperator<TOpReplicate>(op);
                auto& portProto = *proto.MutableReplicate();
                portProto.SetHub(GetHub(port.GetReplicate()));
                portProto.SetIndex(port.GetIndex());
                WriteIdPairs(TPlanSnapshotAccess::GetRebindings(port), *portProto.MutableRebindings());
                break;
            }
            case EOperator::AddDependencies:
            case EOperator::DependentJoin:
            case EOperator::IndexLookupJoin:
            case EOperator::CBOTree:
            case EOperator::Root:
                ythrow yexception() << op.GetExplainName() << " is not expected before CBO";
        }

        // A Replicate port reads its hub's input.
        if (op.Kind != EOperator::Replicate) {
            for (const auto* child : op.GetChildren()) {
                proto.AddInputs(GetOperator(child));
            }
        }

        Operators.emplace(&op, Proto.OperatorsSize());
        *Proto.AddOperators() = std::move(proto);
    }

    TOpRoot& Root;
    TExprContext& Ctx;
    NKikimrKqpRbo::TPlanRoot Proto;

    THashMap<const IOperator*, ui32> Operators;
    THashMap<const TReplicate*, ui32> Hubs;
    TExprNode::TListType Expressions;
    THashMap<const TExprNode*, ui32> ExpressionIds;
    THashMap<const TTypeAnnotationNode*, ui32> TypeIds;
};

class TPlanReader {
public:
    TPlanReader(const NKikimrKqpRbo::TPlanRoot& proto, TRBOContext& ctx)
        : Proto(proto)
        , Ctx(ctx)
        , ExprCtx(ctx.ExprCtx)
    {}

    TIntrusivePtr<TOpRoot> Read() && {
        const auto pos = ReadPosition(Proto.GetPos());
        TOrderedIUs<TString> columns;
        for (const auto& column : Proto.GetColumns()) {
            columns.Append(column.GetId(), column.GetName());
        }
        TVector<TString> queryColumns(Proto.GetQueryColumns().begin(), Proto.GetQueryColumns().end());

        // Expressions bind to the final plan properties, so the root exists first;
        // its real input is attached once all operators are rebuilt.
        auto root = MakeIntrusive<TOpRoot>(MakeIntrusive<TOpEmptySource>(pos), pos, std::move(columns), std::move(queryColumns));
        Props = &root->PlanProps;
        TPlanSnapshotAccess::ReadRegistry(Proto, Props->InfoUnitRegistry);
        for (const auto id : root->GetColumns().Unordered()) {
            CheckId(id);
        }

        ReadExpressions();
        for (const auto& op : Proto.GetOperators()) {
            Operators.push_back(ReadOperator(op));
        }

        for (const auto& subplan : Proto.GetSubplans()) {
            const auto binding = CheckId(subplan.GetBinding());
            Props->Subplans.Add(binding, GetOperator(subplan.GetPlan()),
                ReadEnum(subplan.GetType(), ESubplanType::EXISTS), ReadOrderedIds(subplan.GetTuple()),
                subplan.HasResultIU() ? std::make_optional(CheckId(subplan.GetResultIU())) : std::nullopt);
            Props->Subplans.RefreshDependencies(binding);
        }

        root->SetInput(GetOperator(Proto.GetInput()));
        Props->PgSyntax = Proto.GetPgSyntax();
        Props->WithEffects = Proto.GetWithEffects();
        Props->WithReturning = Proto.GetWithReturning();
        root->ComputeParents();
        return root;
    }

private:
    TPositionHandle ReadPosition(const NKikimrKqpRbo::TPosition& proto) {
        return ExprCtx.AppendPosition(TPosition(proto.GetColumn(), proto.GetRow(), proto.GetFile()));
    }

    TInfoUnitId CheckId(ui32 id) const {
        Y_ENSURE(id < Props->InfoUnitRegistry.Size(), "Unknown information unit " << id << " in a snapshot");
        return id;
    }

    TUnorderedIUs ReadIds(const TProtoIds& ids) const {
        for (const auto id : ids) {
            CheckId(id);
        }
        TUnorderedIUs result;
        result.Assign(ids);
        return result;
    }

    TOrderedIUs<> ReadOrderedIds(const TProtoIds& ids) const {
        for (const auto id : ids) {
            CheckId(id);
        }
        return TOrderedIUs<>(ids.begin(), ids.end());
    }

    TOrderedIUs<TString> ReadNamedIds(const google::protobuf::RepeatedPtrField<NKikimrKqpRbo::TNamedIU>& ids) const {
        TOrderedIUs<TString> result;
        for (const auto& id : ids) {
            result.Append(CheckId(id.GetId()), id.GetName());
        }
        return result;
    }

    TSortIUs ReadSortKeys(const google::protobuf::RepeatedPtrField<NKikimrKqpRbo::TSortKey>& keys) const {
        TSortIUs result;
        for (const auto& key : keys) {
            result.Append(CheckId(key.GetId()), TSortOrder{key.GetAscending(), key.GetNullsFirst()});
        }
        return result;
    }

    TJoinIUs ReadJoinKeys(const google::protobuf::RepeatedPtrField<NKikimrKqpRbo::TJoinKey>& keys) const {
        TJoinIUs result;
        for (const auto& key : keys) {
            result.Add(TJoinKey(CheckId(key.GetLeft()), CheckId(key.GetRight()), key.GetEqualNulls()));
        }
        return result;
    }

    TMappedIUs<TInfoUnitId> ReadIdPairs(const google::protobuf::RepeatedPtrField<NKikimrKqpRbo::TIdPair>& pairs) const {
        TMappedIUs<TInfoUnitId> result;
        for (const auto& pair : pairs) {
            result.Add(CheckId(pair.GetKey()), CheckId(pair.GetValue()));
        }
        return result;
    }

    // Annotates a restored root unless an earlier root shared and annotated it.
    void Annotate(TExprNode::TPtr& node) {
        if (node->GetTypeAnn()) {
            return;
        }
        auto& transformer = Ctx.TypeAnnTransformer;
        transformer.Rewind();
        auto status = IGraphTransformer::TStatus(IGraphTransformer::TStatus::Ok);
        do {
            status = transformer.Transform(node, node, ExprCtx);
        } while (status == IGraphTransformer::TStatus::Repeat);
        Y_ENSURE(status == IGraphTransformer::TStatus::Ok && node->GetTypeAnn(),
            "Cannot annotate a restored expression: " << ExprCtx.IssueManager.GetIssues().ToOneLineString());
    }

    void ReadExpressions() {
        const auto list = DeserializeGraph(ReadPosition(Proto.GetPos()), Proto.GetExpressions(), ExprCtx);
        const int size = Proto.ExpressionEntriesSize();
        Y_ENSURE(list && list->IsList() && list->ChildrenSize() == static_cast<size_t>(size), "Malformed snapshot expressions");
        Expressions = list->ChildrenList();
        Types.assign(Expressions.size(), nullptr);

        // Typed lambdas take their argument types from type expressions.
        for (int i = 0; i < size; ++i) {
            if (Proto.GetExpressionEntries(i).GetIsType()) {
                Annotate(Expressions[i]);
                const TTypeAnnotationNode* type = Expressions[i]->GetTypeAnn();
                Y_ENSURE(type->GetKind() == ETypeAnnotationKind::Type, "Snapshot type entry is not a type expression");
                Types[i] = type->Cast<TTypeExprType>()->GetType();
            }
        }
        for (int i = 0; i < size; ++i) {
            const auto& entry = Proto.GetExpressionEntries(i);
            if (!entry.GetTyped() || entry.GetIsType()) {
                continue;
            }
            if (!entry.GetArgTypes().empty()) {
                std::vector<const TTypeAnnotationNode*> argTypes;
                for (const auto type : entry.GetArgTypes()) {
                    argTypes.push_back(GetType(type));
                }
                Y_ENSURE(Expressions[i]->IsLambda() && Expressions[i]->Head().ChildrenSize() == argTypes.size(),
                    "Snapshot argument types do not match the lambda");
                Y_ENSURE(UpdateLambdaAllArgumentsTypes(Expressions[i], argTypes, ExprCtx), "Cannot restore lambda argument types");
            }
            Annotate(Expressions[i]);
        }
    }

    TExprNode::TPtr GetExpr(ui32 index) const {
        Y_ENSURE(index < Expressions.size() && !Proto.GetExpressionEntries(static_cast<int>(index)).GetIsType(),
            "Unknown snapshot expression " << index);
        return Expressions[index];
    }

    TExpression GetExpression(ui32 index) const {
        return TExpression(GetExpr(index), &ExprCtx, Props);
    }

    const TTypeAnnotationNode* GetType(ui32 index) const {
        Y_ENSURE(index < Types.size() && Types[index], "Unknown snapshot type " << index);
        return Types[index];
    }

    const TStructExprType* GetStructType(ui32 index) const {
        const auto* type = GetType(index);
        Y_ENSURE(type->GetKind() == ETypeAnnotationKind::Struct, "Expected a struct type in a snapshot");
        return type->Cast<TStructExprType>();
    }

    TIntrusivePtr<IOperator> GetOperator(ui32 index) const {
        Y_ENSURE(index < Operators.size(), "Operator " << index << " is not restored before its consumer");
        return Operators[index];
    }

    TOpRead::TPointPrefix ReadPointPrefix(const NKikimrKqpRbo::TPointPrefix& proto) const {
        TOpRead::TPointPrefix prefix;
        prefix.Table = proto.GetTable();
        if (proto.HasPoints()) {
            prefix.Points = GetExpr(proto.GetPoints());
        }
        if (proto.HasPointsItemType()) {
            prefix.PointsItemType = GetStructType(proto.GetPointsItemType());
        }
        prefix.Columns.assign(proto.GetColumns().begin(), proto.GetColumns().end());
        if (proto.HasExpectedMaxPoints()) {
            prefix.ExpectedMaxPoints = proto.GetExpectedMaxPoints();
        }
        return prefix;
    }

    TIntrusivePtr<IOperator> ReadRead(const NKikimrKqpRbo::TRead& proto, TPositionHandle pos) const {
        std::optional<TOpRead::TRangeInfo> ranges;
        if (proto.HasRangeInfo()) {
            const auto& rangesProto = proto.GetRangeInfo();
            auto& info = ranges.emplace();
            if (rangesProto.HasComputeNode()) {
                info.ComputeNode = GetExpr(rangesProto.GetComputeNode());
            }
            info.KeyColumns.assign(rangesProto.GetKeyColumns().begin(), rangesProto.GetKeyColumns().end());
            info.UsedPrefixLen = rangesProto.GetUsedPrefixLen();
            info.PointPrefixLen = rangesProto.GetPointPrefixLen();
            if (rangesProto.HasExpectedMaxRanges()) {
                info.ExpectedMaxRanges = rangesProto.GetExpectedMaxRanges();
            }
            for (const auto& prefix : rangesProto.GetPointPrefixes()) {
                info.PointPrefixes.push_back(ReadPointPrefix(prefix));
            }
            info.MainTable = rangesProto.GetMainTable();
            if (rangesProto.HasLiteralRange()) {
                info.LiteralRange = GetExpr(rangesProto.GetLiteralRange());
            }
        }

        return MakeIntrusive<TOpRead>(proto.GetAlias(), ReadIds(proto.GetColumns()),
            ReadEnum(proto.GetStorageType(), NYql::EStorageType::ColumnStorage),
            GetExpr(proto.GetTableCallable()),
            proto.HasOlapFilterLambda() ? GetExpr(proto.GetOlapFilterLambda()) : nullptr,
            proto.HasLimit() ? GetExpr(proto.GetLimit()) : nullptr,
            std::move(ranges),
            proto.HasOriginalPredicate() ? std::make_optional(GetExpression(proto.GetOriginalPredicate())) : std::nullopt,
            ReadEnum(proto.GetSortDir(), ESortDir::Desc), TPhysicalOpProps{}, pos);
    }

    TIntrusivePtr<IOperator> ReadTableLookup(const NKikimrKqpRbo::TTableLookup& proto, TIntrusivePtr<IOperator> input,
        TPositionHandle pos) const
    {
        auto lookup = MakeIntrusive<TOpTableLookup>(std::move(input), pos, GetExpr(proto.GetTable()),
            ReadIds(proto.GetColumns()), ReadNamedIds(proto.GetLookupKeys()));
        lookup->JoinKind = proto.GetJoinKind();
        if (proto.HasFetchedRowFilter()) {
            lookup->FetchedRowFilter = GetExpression(proto.GetFetchedRowFilter());
        }
        if (proto.HasPrefix()) {
            const auto& prefix = proto.GetPrefix();
            lookup->Prefix = TOpTableLookup::TLookupKeyPrefix{
                .Points = GetExpr(prefix.GetPoints()),
                .PointsItemType = GetStructType(prefix.GetPointsItemType()),
                .Columns = TVector<TString>(prefix.GetColumns().begin(), prefix.GetColumns().end()),
                .Equalities = ReadNamedIds(prefix.GetEqualities()),
            };
        }
        lookup->Strategy = ReadEnum(proto.GetStrategy(), ELookupStrategy::LookupJoinRows);
        lookup->ResidualJoinKeys = ReadJoinKeys(proto.GetResidualJoinKeys());
        lookup->AllowNullKeys = proto.GetAllowNullKeys();
        lookup->KeysFromInputLookup = proto.GetKeysFromInputLookup();
        if (proto.HasSourceRead()) {
            const auto& source = proto.GetSourceRead();
            TOpTableLookup::TSourceRead sourceRead{.Predicate = GetExpression(source.GetPredicate()), .PointPrefixes = {}};
            for (const auto& prefix : source.GetPointPrefixes()) {
                sourceRead.PointPrefixes.push_back(ReadPointPrefix(prefix));
            }
            lookup->SourceRead = std::move(sourceRead);
        }
        return lookup;
    }

    TIntrusivePtr<IOperator> ReadTableEffect(const NKikimrKqpRbo::TTableEffect& proto, TIntrusivePtr<IOperator> input,
        TPositionHandle pos) const
    {
        const auto& optionsProto = proto.GetOptions();
        auto readStrings = [](bool has, const NKikimrKqpRbo::TStringList& list) -> std::optional<TVector<TString>> {
            if (!has) {
                return std::nullopt;
            }
            return TVector<TString>(list.GetValues().begin(), list.GetValues().end());
        };

        TEffectOptions options;
        options.Columns = readStrings(optionsProto.HasColumns(), optionsProto.GetColumns());
        options.ReturningColumns = readStrings(optionsProto.HasReturningColumns(), optionsProto.GetReturningColumns());
        options.DefaultColumns = readStrings(optionsProto.HasDefaultColumns(), optionsProto.GetDefaultColumns());
        if (optionsProto.HasOnConflict()) {
            options.OnConflict = optionsProto.GetOnConflict();
        }
        if (optionsProto.HasIsBatch()) {
            options.IsBatch = optionsProto.GetIsBatch();
        }
        if (optionsProto.HasSettings()) {
            auto& settings = options.Settings.emplace();
            for (const auto setting : optionsProto.GetSettings().GetValues()) {
                settings.push_back(GetExpr(setting));
            }
        }

        return MakeIntrusive<TOpTableEffect>(std::move(input), pos, GetExpr(proto.GetTable()),
            ReadEnum(proto.GetType(), EEffectType::DeleteRowsIndex), std::move(options),
            ReadNamedIds(proto.GetColumns()), ReadNamedIds(proto.GetReturning()));
    }

    TIntrusivePtr<IOperator> ReadReplicate(const NKikimrKqpRbo::TReplicatePort& proto) {
        const auto hubIndex = proto.GetHub();
        Y_ENSURE(hubIndex < static_cast<ui32>(Proto.HubsSize()), "Unknown Replicate hub " << hubIndex);
        if (Hubs.size() <= hubIndex) {
            Hubs.resize(hubIndex + 1);
        }
        auto& hub = Hubs[hubIndex];
        if (!hub) {
            const auto& hubProto = Proto.GetHubs(hubIndex);
            hub = TPlanSnapshotAccess::MakeHub(GetOperator(hubProto.GetInput()), ReadPosition(hubProto.GetPos()),
                Props->InfoUnitRegistry, hubProto.GetNextOutputIndex());
        }
        return TPlanSnapshotAccess::MakePort(hub, proto.GetIndex(), ReadIdPairs(proto.GetRebindings()));
    }

    TIntrusivePtr<IOperator> ReadOperator(const NKikimrKqpRbo::TOperator& proto) {
        const auto pos = ReadPosition(proto.GetPos());
        TVector<TIntrusivePtr<IOperator>> inputs;
        for (const auto input : proto.GetInputs()) {
            inputs.push_back(GetOperator(input));
        }
        const auto expectInputs = [&](size_t count) {
            Y_ENSURE(inputs.size() == count, "Snapshot operator expects " << count << " inputs, got " << inputs.size());
        };

        using TKind = NKikimrKqpRbo::TOperator;
        switch (proto.GetKindCase()) {
            case TKind::kEmptySource: {
                expectInputs(0);
                const auto& source = proto.GetEmptySource();
                return MakeIntrusive<TOpEmptySource>(pos, source.HasInput() ? GetExpr(source.GetInput()) : nullptr,
                    ReadIds(source.GetColumns()));
            }
            case TKind::kRead:
                expectInputs(0);
                return ReadRead(proto.GetRead(), pos);
            case TKind::kMap: {
                expectInputs(1);
                const auto& map = proto.GetMap();
                TMapIUs elements;
                for (const auto& element : map.GetElements()) {
                    elements.Add(CheckId(element.GetOutput()), GetExpression(element.GetExpression()));
                }
                return MakeIntrusive<TOpMap>(inputs[0], pos, std::move(elements), map.GetNeedToPush());
            }
            case TKind::kFilter: {
                expectInputs(1);
                const auto& filter = proto.GetFilter();
                return MakeIntrusive<TOpFilter>(inputs[0], pos, TPhysicalOpProps{}, GetExpression(filter.GetExpression()),
                    filter.GetPartiallyPushedDown());
            }
            case TKind::kJoin: {
                expectInputs(2);
                const auto& join = proto.GetJoin();
                TVector<TExpression> filters;
                for (const auto filter : join.GetFilters()) {
                    filters.push_back(GetExpression(filter));
                }
                return MakeIntrusive<TOpJoin>(inputs[0], inputs[1], pos, join.GetKind(), ReadJoinKeys(join.GetKeys()), filters);
            }
            case TKind::kAggregate: {
                expectInputs(1);
                const auto& aggregate = proto.GetAggregate();
                TAggregationIUs aggregations;
                for (const auto& aggregation : aggregate.GetAggregations()) {
                    aggregations.Add(CheckId(aggregation.GetOutput()), TOpAggregationTraits{
                        .Input = CheckId(aggregation.GetInput()),
                        .AggFunction = aggregation.GetFunction(),
                        .Distinct = aggregation.GetDistinct(),
                        .Unwrap = aggregation.GetUnwrap(),
                    });
                }
                auto result = MakeIntrusive<TOpAggregate>(inputs[0], std::move(aggregations), ReadOrderedIds(aggregate.GetKeys()),
                    ReadEnum(aggregate.GetPhase(), EOpPhase::Final), aggregate.GetDistinctAll(), pos);
                if (aggregate.GetPushedToOlap()) {
                    result->SetPushedToOlap();
                }
                return result;
            }
            case TKind::kGroupingSets: {
                expectInputs(1);
                Y_ENSURE(inputs[0]->Kind == EOperator::Aggregate, "Grouping sets read an aggregate");
                const auto& groupingSets = proto.GetGroupingSets();
                TVector<TUnorderedIUs> sets;
                for (const auto& set : groupingSets.GetSets()) {
                    sets.push_back(ReadIds(set.GetIds()));
                }
                return MakeIntrusive<TOpGroupingSets>(CastOperator<TOpAggregate>(inputs[0]), std::move(sets),
                    ReadIdPairs(groupingSets.GetColumns()), pos, ReadIdPairs(groupingSets.GetIndicators()));
            }
            case TKind::kWindow: {
                expectInputs(1);
                const auto& window = proto.GetWindow();
                TWindowIUs functions;
                for (const auto& function : window.GetFunctions()) {
                    functions.Add(CheckId(function.GetOutput()), TOpWindowFunc{
                        .Function = function.GetFunction(),
                        .Kind = ReadEnum(function.GetKind(), EWindowFuncKind::Native),
                        .Arguments = ReadOrderedIds(function.GetArguments()),
                    });
                }
                const auto& frameProto = window.GetFrame();
                TOpWindowFrame frame{
                    .Type = ReadEnum(frameProto.GetType(), EWindowFrameType::Groups),
                    .BeginKind = ReadEnum(frameProto.GetBeginKind(), EWindowFrameBound::UnboundedFollowing),
                    .BeginValue = frameProto.GetBeginValue(),
                    .EndKind = ReadEnum(frameProto.GetEndKind(), EWindowFrameBound::UnboundedFollowing),
                    .EndValue = frameProto.GetEndValue(),
                };
                return MakeIntrusive<TOpWindow>(inputs[0], pos, std::move(functions), ReadOrderedIds(window.GetPartitionKeys()),
                    ReadSortKeys(window.GetSortKeys()), frame);
            }
            case TKind::kLimit: {
                expectInputs(1);
                const auto& limit = proto.GetLimit();
                return MakeIntrusive<TOpLimit>(inputs[0], pos, TPhysicalOpProps{}, GetExpression(limit.GetLimit()),
                    limit.HasOffset() ? std::make_optional(GetExpression(limit.GetOffset())) : std::nullopt,
                    ReadEnum(limit.GetPhase(), EOpPhase::Final));
            }
            case TKind::kSort: {
                expectInputs(1);
                const auto& sort = proto.GetSort();
                return MakeIntrusive<TOpSort>(inputs[0], pos, TPhysicalOpProps{}, ReadSortKeys(sort.GetKeys()),
                    sort.HasLimit() ? std::make_optional(GetExpression(sort.GetLimit())) : std::nullopt,
                    ReadEnum(sort.GetPhase(), EOpPhase::Final));
            }
            case TKind::kUnionAll: {
                const auto& unionAll = proto.GetUnionAll();
                TUnionAllIUs columns(TUnionInputPolicy{.ChildCount = inputs.size()});
                for (const auto& column : unionAll.GetColumns()) {
                    TUnionInputRow row;
                    for (const auto input : column.GetInputs()) {
                        row.Inputs.push_back(CheckId(input));
                    }
                    columns.Add(CheckId(column.GetOutput()), std::move(row));
                }
                return MakeIntrusive<TOpUnionAll>(std::move(inputs), pos, std::move(columns), unionAll.GetOrdered());
            }
            case TKind::kTableLookup:
                expectInputs(1);
                return ReadTableLookup(proto.GetTableLookup(), inputs[0], pos);
            case TKind::kTableEffect:
                expectInputs(1);
                return ReadTableEffect(proto.GetTableEffect(), inputs[0], pos);
            case TKind::kReplicate:
                expectInputs(0);
                return ReadReplicate(proto.GetReplicate());
            case TKind::KIND_NOT_SET:
                break;
        }
        ythrow yexception() << "Snapshot operator without a kind";
    }

    const NKikimrKqpRbo::TPlanRoot& Proto;
    TRBOContext& Ctx;
    TExprContext& ExprCtx;
    TPlanProps* Props = nullptr;

    TExprNode::TListType Expressions;
    TVector<const TTypeAnnotationNode*> Types;
    TVector<TIntrusivePtr<IOperator>> Operators;
    TVector<TIntrusivePtr<TReplicate>> Hubs;
};

} // anonymous namespace

NKikimrKqpRbo::TPlanRoot SerializePlanRoot(TOpRoot& root, TExprContext& ctx) {
    return TPlanWriter(root, ctx).Write();
}

TIntrusivePtr<TOpRoot> DeserializePlanRoot(const NKikimrKqpRbo::TPlanRoot& proto, TRBOContext& ctx) {
    return TPlanReader(proto, ctx).Read();
}

} // namespace NKikimr::NKqp
