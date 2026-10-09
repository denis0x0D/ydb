#include "kqp_rbo_physical_source_builder.h"

#include <ydb/core/kqp/common/kqp_yql.h>
#include <ydb/core/kqp/opt/rbo/kqp_olap_expr_inspection.h>
#include <ydb/library/yql/dq/type_ann/dq_type_ann.h>

#include <yql/essentials/core/yql_expr_optimize.h>

using namespace NYql::NNodes;
using namespace NKikimr;
using namespace NKikimr::NKqp;

TString TPhysicalSourceBuilder::GetOlapAggregateField(ui32 index, TStringBuf function) const {
    return Names.GetTemporaryName(TStringBuilder() << "__kqp_olap_agg_" << function << "_" << index << "_");
}

TExprNode::TPtr TPhysicalSourceBuilder::AddOlapAggregate(TExprNode::TPtr processLambda, TVector<TString>& outputFields) const {
    outputFields.clear();
    TVector<TExprBase> aggregates;
    auto addAggregate = [&](TString field, TStringBuf function, const TString& column) {
        // clang-format off
        aggregates.push_back(Build<TKqpOlapAggOperation>(Ctx, Pos)
            .Name().Build(field)
            .Type().Build(function)
            .Column().Build(column)
        .Done());
        // clang-format on
        outputFields.push_back(std::move(field));
    };

    // Column shards return partial states. AVG returns its sum and count.
    const auto& aggregations = OlapAggregate->GetAggregationTraits();
    ui32 index = 0;
    for (const auto output : aggregations.Keys()) {
        const auto& aggFunction = aggregations.Find(output)->AggFunction;
        const auto& column = OlapAggregateInput->Columns.at(output);
        // COUNT(*) has no column argument.
        const TString storageColumn = column ? Registry.Get(*column).GetColumnName() : TString("*");
        if (aggFunction == "avg") {
            addAggregate(GetOlapAggregateField(index, "sum"), "sum", storageColumn);
            addAggregate(GetOlapAggregateField(index, "count"), "count", storageColumn);
        } else {
            addAggregate(GetOlapAggregateField(index, aggFunction), aggFunction, storageColumn);
        }
        ++index;
    }

    // Several key IDs may name one storage column; group by it once.
    TVector<TCoAtom> keys;
    THashSet<TString> keyColumns;
    for (const auto key : OlapAggregate->GetKeyColumns().Items()) {
        const auto& column = Registry.Get(key).GetColumnName();
        if (keyColumns.insert(column).second) {
            keys.push_back(Build<TCoAtom>(Ctx, Pos).Value(column).Done());
            outputFields.push_back(column);
        }
    }
    std::sort(outputFields.begin(), outputFields.end());

    // clang-format off
    auto olapAgg = Build<TKqpOlapAgg>(Ctx, Pos)
        .Input(TCoLambda(processLambda).Body())
        .Aggregates<TKqpOlapAggOperationList>()
            .Add(aggregates)
        .Build()
        .KeyColumns<TCoAtomList>()
            .Add(keys)
        .Build()
    .Done().Ptr();
    // clang-format on
    return Ctx.ChangeChild(*processLambda, TCoLambda::idx_Body, std::move(olapAgg));
}

TExprNode::TPtr TPhysicalSourceBuilder::BuildOlapAggregateRow(const THashMap<TString, TExprNode::TPtr>& fields) const {
    TExprNode::TListType members;
    auto addMember = [&](TInfoUnitId id, TExprNode::TPtr value) {
        members.push_back(Ctx.NewList(Pos, {Ctx.NewAtom(Pos, Names.Get(id)), std::move(value)}));
    };

    TUnorderedIUs keys;
    for (const auto key : OlapAggregate->GetKeyColumns().Items()) {
        if (keys.Add(key)) {
            addMember(key, fields.at(Registry.Get(key).GetColumnName()));
        }
    }

    const auto& aggregations = OlapAggregate->GetAggregationTraits();
    ui32 index = 0;
    for (const auto output : aggregations.Keys()) {
        const auto& aggFunction = aggregations.Find(output)->AggFunction;
        if (aggFunction != "avg") {
            addMember(output, fields.at(GetOlapAggregateField(index++, aggFunction)));
            continue;
        }

        // Build the intermediate AVG state of TPhysicalAggregationBuilder: (sum as Double, count),
        // or nothing when an optional column has no values.
        const auto sum = fields.at(GetOlapAggregateField(index, "sum"));
        const auto count = fields.at(GetOlapAggregateField(index, "count"));
        ++index;
        const auto* columnType = Read.GetIUType(*OlapAggregateInput->Columns.at(output), Ctx);
        Y_ENSURE(columnType, "Cannot find the type of an AVG column");
        TExprNode::TPtr state;
        if (columnType->IsOptionalOrNull()) {
            // clang-format off
            state = Ctx.Builder(Pos)
                .Callable("IfPresent")
                    .Add(0, sum)
                    .Lambda(1)
                        .Param("sum")
                        .Callable(0, "Just")
                            .List(0)
                                .Callable(0, "SafeCast")
                                    .Arg(0, "sum")
                                    .Callable(1, "DataType")
                                        .Atom(0, "Double")
                                    .Seal()
                                .Seal()
                                .Add(1, count)
                            .Seal()
                        .Seal()
                    .Seal()
                    .Callable(2, "Nothing")
                        .Callable(0, "OptionalType")
                            .Callable(0, "TupleType")
                                .Callable(0, "DataType")
                                    .Atom(0, "Double")
                                .Seal()
                                .Callable(1, "DataType")
                                    .Atom(0, "Uint64")
                                .Seal()
                            .Seal()
                        .Seal()
                    .Seal()
                .Seal().Build();
            // clang-format on
        } else {
            // clang-format off
            state = Ctx.Builder(Pos)
                .List()
                    .Callable(0, "SafeCast")
                        .Add(0, sum)
                        .Callable(1, "DataType")
                            .Atom(0, "Double")
                        .Seal()
                    .Seal()
                    .Add(1, count)
                .Seal().Build();
            // clang-format on
        }
        addMember(output, std::move(state));
    }

    return Ctx.NewCallable(Pos, "AsStruct", std::move(members));
}

TExprNode::TPtr TPhysicalSourceBuilder::BuildPhysicalOp() {
    TExprNode::TPtr source;
    TVector<TString> storageColumns;
    TVector<std::pair<TString, TString>> renames;
    THashMap<TString, TString> olapNames;
    for (const auto id : Read.GetColumns()) {
        const auto column = Registry.Get(id).GetColumnName();
        storageColumns.push_back(column);
        renames.emplace_back(column, Names.Get(id));
        olapNames.emplace(Ctx.GetIndexAsString(id), column);
    }
    // OLAP execution needs a nonempty storage projection to retain row counts.
    // The carrier has no logical ID and is dropped by the NarrowMap below.
    if (storageColumns.empty() && Read.GetTableStorageType() == NYql::EStorageType::ColumnStorage) {
        Y_ENSURE(!CarrierColumn.empty(), "An empty OLAP payload needs a storage carrier column");
        storageColumns.push_back(CarrierColumn);
    }
    // Block reads expose TStructExprType's lexical field order, not ID order.
    // Fetch a storage field once even when multiple logical IDs refer to it.
    std::sort(storageColumns.begin(), storageColumns.end());
    storageColumns.erase(std::unique(storageColumns.begin(), storageColumns.end()), storageColumns.end());
    TVector<TExprNode::TPtr> columns;
    for (const auto& column : storageColumns) {
        columns.push_back(Ctx.NewAtom(Pos, column));
    }
    // Extract ranges.
    TExprNode::TPtr ranges = Read.GetRanges() ? Read.GetRanges() : Build<TCoVoid>(Ctx, Pos).Done().Ptr();

    switch (Read.GetTableStorageType()) {
        case NYql::EStorageType::RowStorage: {
            // A literal range is passed to the source as is, so it needs no materialization.
            if (const auto literalRange = Read.GetLiteralRange()) {
                ranges = literalRange;
            }

            TKqpReadTableSettings settings;
            if (Read.SortDir != ESortDir::None) {
                settings.SetSorting(Read.SortDir == ESortDir::Asc ? ERequestSorting::ASC : ERequestSorting::DESC);
                if (Read.Limit) {
                    settings.SetItemsLimit(Read.Limit);
                }
            }

            TExprNode::TPtr sourceSettings;
            // System views have no datashard partitions and need their own reader.
            if (IsSysView) {
                // clang-format off
                sourceSettings = Build<TKqpReadSysViewSourceSettings>(Ctx, Pos)
                    .Table(Read.TableCallable)
                    .Columns().Add(columns).Build()
                    .Settings(settings.BuildNode(Ctx, Pos))
                    .RangesExpr(ranges)
                .Done().Ptr();
                // clang-format on
            } else {
                // clang-format off
                sourceSettings = Build<TKqpReadRangesSourceSettings>(Ctx, Pos)
                    .Table(Read.TableCallable)
                    .Columns()
                        .Add(columns)
                    .Build()
                    .Settings(settings.BuildNode(Ctx, Pos))
                    .RangesExpr(ranges)
                    .ExplainPrompt<TCoNameValueTupleList>().Build()
                .Done().Ptr();
                // clang-format on
            }

            // clang-format off
            source = Build<TDqSource>(Ctx, Pos)
                .DataSource<TCoDataSource>()
                    .Category<TCoAtom>().Build(IsSysView ? NYql::KqpSysViewSourceName : NYql::KqpReadRangesSourceName)
                .Build()
                .Settings(sourceSettings)
            .Done().Ptr();
            // clang-format on

            const auto programArg = Build<TCoArgument>(Ctx, Pos).Name("program_arg").Done().Ptr();
            const auto renameMap = NPhysicalConvertionUtils::BuildRenameMap(programArg, renames, Ctx);
            // clang-format off
            source = Build<TDqPhyStage>(Ctx, Pos)
                .Inputs()
                    .Add({source})
                .Build()
                .Program()
                    .Args({programArg})
                    .Body(renameMap)
                .Build()
                .Settings(NYql::NDq::TDqStageSettings::New(StageGUID).BuildNode(Ctx, Pos))
            .Done().Ptr();
            // clang-format on
            break;
        }
        case NYql::EStorageType::ColumnStorage: {
            // clang-format off
            auto processLambda = Build<TCoLambda>(Ctx, Pos)
                .Args({"arg"})
                .Body("arg")
            .Done().Ptr();
            // clang-format on

            if (Read.OlapFilterLambda) {
                // Do not carry the optimizer's ID-keyed argument type across the
                // storage-name boundary. The read annotator supplies the storage row.
                processLambda = Ctx.DeepCopyLambda(*NOpt::TOlapFilterInspector::RenameColumns(
                    Read.OlapFilterLambda, olapNames, Ctx));
            }

            // Block reads expose the program row in lexical field order.
            TVector<TString> outputFields = storageColumns;
            if (OlapAggregate) {
                processLambda = AddOlapAggregate(processLambda, outputFields);
            }

            TKqpReadTableSettings settings;
            if (Read.Limit) {
                settings.SetItemsLimit(Read.Limit);
            }

            if (Read.SortDir != ESortDir::None) {
                const auto sortDirection = Read.SortDir == ESortDir::Asc ? ERequestSorting::ASC : ERequestSorting::DESC;
                settings.SetSorting(sortDirection);
            } else if (Read.Limit) {
                // Limit without sort.
                settings.SequentialInFlight = 1;
            }

            // clang-format off
            auto olapRead = Build<TKqpBlockReadOlapTableRanges>(Ctx, Pos)
                .Table(Read.TableCallable)
                .Ranges(ranges)
                .Columns().Add(columns).Build()
                .Settings(settings.BuildNode(Ctx, Pos))
                .ExplainPrompt<TCoNameValueTupleList>().Build()
                .Process(processLambda)
            .Done().Ptr();

            // From blocks.
            auto flowNonBlockRead = Build<TCoToFlow>(Ctx, Pos)
                .Input<TCoWideFromBlocks>()
                    .Input<TCoFromFlow>()
                        .Input(olapRead)
                    .Build()
                .Build()
            .Done().Ptr();
            // clang-format on

            TExprNode::TListType args;
            THashMap<TString, TExprNode::TPtr> fieldArgs;
            for (ui32 i = 0; i < outputFields.size(); ++i) {
                args.push_back(Ctx.NewArgument(Pos, "column_" + ToString(i)));
                fieldArgs.emplace(outputFields[i], args.back());
            }
            TExprNode::TPtr row;
            if (OlapAggregate) {
                row = BuildOlapAggregateRow(fieldArgs);
            } else {
                TExprNode::TListType fields;
                for (const auto& [column, name] : renames) {
                    fields.push_back(Ctx.NewList(Pos, {Ctx.NewAtom(Pos, name), fieldArgs.at(column)}));
                }
                row = Ctx.NewCallable(Pos, "AsStruct", std::move(fields));
            }
            auto lambda = Ctx.NewLambda(Pos, Ctx.NewArguments(Pos, std::move(args)), std::move(row));
            auto narrowMap = Build<TCoNarrowMap>(Ctx, Pos).Input(flowNonBlockRead).Lambda(lambda).Done().Ptr();

            // clang-format off
            source = Build<TCoFromFlow>(Ctx, Pos)
                .Input(narrowMap)
            .Done().Ptr();
            // clang-format on
            break;
        }
        default:
            Y_ENSURE(false, "Unsupported table source type.");
    }

    YQL_CLOG(TRACE, CoreDq) << "[NEW RBO Physical source] " << KqpExprToPrettyString(TExprBase(source), Ctx);

    return source;
}
