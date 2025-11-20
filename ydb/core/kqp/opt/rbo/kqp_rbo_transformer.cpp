#include "kqp_rbo_transformer.h"
#include "kqp_operator.h"
#include "kqp_plan_conversion_utils.h"

#include <yql/essentials/utils/log/log.h>

using namespace NYql;
using namespace NYql::NNodes;
using namespace NKikimr::NKqp;
using namespace NYql::NDq;

namespace {
struct TJoinTableAliases {
    THashSet<TString> LeftSideAliases;
    THashSet<TString> RightSideAliases;
};

struct TAggregationTraits {
    TVector<TExprNode::TPtr> AggTraitsList;
    TVector<TInfoUnit> KeyColumns;
};

TString GetColumnNameFromPgGroupRef(TExprNode::TPtr pgGroupRef, const TVector<std::pair<TInfoUnit, TExprNode::TPtr>>& groupByKeysExpressionsMap) {
    TString colName;
    if (pgGroupRef->ChildrenSize() == 4) {
        colName = TString(pgGroupRef->ChildPtr(3)->Content());
    } else if (pgGroupRef->ChildrenSize() == 3) {
        // In this case we can get a column name from group expr map
        const auto groupByKeyExprId = FromString<uint32_t>(TString(pgGroupRef->ChildPtr(2)->Content()));
        Y_ENSURE(groupByKeysExpressionsMap.size() > groupByKeyExprId);
        colName = groupByKeysExpressionsMap[groupByKeyExprId].first.GetFullName();
    } else {
        Y_ENSURE(false, "Invalid children size for `pgGroupRef`");
    }
    return colName;
}

THashSet<TString> SupportedAggregationFunctions{"sum", "min", "max", "count"};
ui64 KqpUniqueAggColumnId{0};

bool IsExpression(TExprNode::TPtr node) {
    return !(node->IsCallable("Member") || (node->IsCallable("ToPg") && node->ChildPtr(0)->IsCallable("Member")));
}

TExprNode::TPtr GetPgCallable(TExprNode::TPtr input, const TString& callableName) {
    auto isPgCallable = [&](const TExprNode::TPtr& node) -> bool {
        if (node->IsCallable(callableName)) {
            return true;
        }
        return false;
    };

    return FindNode(input, isPgCallable);
}

bool IsAggregation(TExprNode::TPtr node) { return node->IsCallable("PgAgg"); }

void CollectAggregationsImpl(TExprNode::TPtr node, TVector<TExprNode::TPtr>& aggregations) {
    if (IsAggregation(node)) {
        Y_ENSURE(node->ChildrenSize() == 3, "Invalid children size for PgAgg");
        // This error should be checked in compiler front-end.
        Y_ENSURE(!IsAggregation(node->ChildPtr(2)), "Nested aggregation is not supported, aka f(g(a))");
        if (!!GetSetting(*node->Child(1), "distinct")) {
            Y_ENSURE(!IsExpression(node->ChildPtr(2)), "Nested distinct on expression is not supported, aka f(distinct a x b)");
        }
        Y_ENSURE(SupportedAggregationFunctions.count(node->ChildPtr(0)->Content()),
                 "Aggregation function " + TString(node->ChildPtr(0)->Content()) + " is not supported ");

        aggregations.push_back(node);
        return;
    }

    for (ui32 i = 0; i < node->ChildrenSize(); ++i) {
        CollectAggregationsImpl(node->ChildPtr(i), aggregations);
    }
}

TVector<TExprNode::TPtr> CollectAggregations(TExprNode::TPtr node) {
    TVector<TExprNode::TPtr> aggregations;
    CollectAggregationsImpl(node, aggregations);
    return aggregations;
}

TJoinTableAliases GatherJoinAliasesLeftSideMultiInputs(const TVector<TInfoUnit> &joinKeys, const THashSet<TString> &processedInputs) {
    TJoinTableAliases joinAliases;
    for (const auto &joinKey : joinKeys) {
        if (processedInputs.count(joinKey.Alias)) {
            joinAliases.LeftSideAliases.insert(joinKey.Alias);
        } else {
            joinAliases.RightSideAliases.insert(joinKey.Alias);
        }
    }
    Y_ENSURE(joinAliases.LeftSideAliases.size(), "Left side of the join inputs are empty");
    Y_ENSURE(joinAliases.RightSideAliases.size() == 1, "Right side of the join should have only one input");
    return joinAliases;
}

TJoinTableAliases GatherJoinAliasesTwoInputs(const TVector<TInfoUnit> &joinKeys) {
    TJoinTableAliases joinAliases;
    for (ui32 i = 0; i < joinKeys.size(); i += 2) {
        joinAliases.LeftSideAliases.insert(joinKeys[i].Alias);
        joinAliases.RightSideAliases.insert(joinKeys[i + 1].Alias);
    }

    Y_ENSURE(joinAliases.LeftSideAliases.size() == 1, "Left side of the join should have only one input");
    Y_ENSURE(joinAliases.RightSideAliases.size() == 1, "Right side of the join should have only one input");
    return joinAliases;
}

TExprNode::TPtr BuildJoinKeys(const TVector<TInfoUnit> &joinKeys, const TJoinTableAliases &joinAliases, THashSet<TString> &processedInputs,
                              TExprContext &ctx, TPositionHandle pos) {
    Y_ENSURE(joinKeys.size() >= 2 && !(joinKeys.size() & 1), "Invalid join key size");
    TVector<TDqJoinKeyTuple> keys;
    for (ui32 i = 0; i < joinKeys.size(); i += 2) {
        auto leftSideKey = joinKeys[i];
        auto rightSideKey = joinKeys[i + 1];
        if (joinAliases.LeftSideAliases.count(rightSideKey.Alias)) {
            std::swap(leftSideKey, rightSideKey);
        }
        // clang-format off
        keys.push_back(Build<TDqJoinKeyTuple>(ctx, pos)
                           .LeftLabel()
                               .Value(leftSideKey.Alias)
                           .Build()
                           .LeftColumn()
                               .Value(leftSideKey.ColumnName)
                           .Build()
                           .RightLabel()
                               .Value(rightSideKey.Alias)
                           .Build()
                           .RightColumn()
                               .Value(rightSideKey.ColumnName)
                           .Build()
                      .Done());
        // clang-format on
        processedInputs.insert(leftSideKey.Alias);
        processedInputs.insert(rightSideKey.Alias);
    }
    return Build<TDqJoinKeyTupleList>(ctx, pos).Add(keys).Done().Ptr();
}

TExprNode::TPtr BuildAggregationTraits(const TString& originalColName, const TString& resultColName, const TString& aggFunction,
                                       const TTypeAnnotationNode* resultType, TExprContext &ctx, TPositionHandle pos) {
    // clang-format off
    return Build<TKqpOpAggregationTraits>(ctx, pos)
        .OriginalColName<TCoAtom>()
            .Value(originalColName)
        .Build()
        .AggregationFunction<TCoAtom>()
            .Value(aggFunction)
        .Build()
        .ResultColName<TCoAtom>()
            .Value(resultColName)
        .Build()
        .AggregationFunctionResultType(ExpandType(pos, *resultType, ctx))
    .Done().Ptr();
    // clang-format on
}

TExprNode::TPtr BuildAggregate(TExprNode::TPtr resultExpr, const TVector<TExprNode::TPtr>& aggTraitsList, const TVector<TInfoUnit> &keys,
                               bool distinctAll,
                               TExprContext& ctx, TPositionHandle pos) {
    TVector<TCoAtom> keyColumns;
    for (const auto& column : keys) {
        // clang-format off
        auto keyColumn = Build<TCoAtom>(ctx, pos)
            .Value(column.GetFullName())
        .Done();
        // clang-format on
        keyColumns.push_back(keyColumn);
    }

    // clang-format off
    return Build<TKqpOpAggregate>(ctx, pos)
        .Input(resultExpr)
        .AggregationTraitsList<TKqpOpAggregationTraitsList>()
            .Add(aggTraitsList)
        .Build()
        .KeyColumns<TCoAtomList>()
            .Add(keyColumns)
        .Build()
        .DistinctAll<TCoAtom>()
            .Value(distinctAll ? "True" : "False")
        .Build()
    .Done().Ptr();
    // clang-format on
}

TVector<std::pair<TInfoUnit, TExprNode::TPtr>> BuildExpressionsFromColumns(const TVector<TInfoUnit>& colNames, TExprContext& ctx,
                                                                           TPositionHandle pos) {
    TVector<std::pair<TInfoUnit, TExprNode::TPtr>> renameExprMap;
    for (const auto& colName : colNames) {
        // clang-format off
        auto lambda = Build<TCoLambda>(ctx, pos)
            .Args({"arg"})
            .Body<TCoMember>()
                .Struct("arg")
                .Name<TCoAtom>()
                    .Value(colName.GetFullName())
                .Build()
            .Build()
        .Done().Ptr();
        // clang-format on
        renameExprMap.push_back({colName, lambda});
    }

    return renameExprMap;
}

TExprNode::TPtr BuildAggregateExpressionMap(TExprNode::TPtr resultExpr,
                                            const TVector<std::pair<TInfoUnit, TExprNode::TPtr>>& aggFieldsExpressionsMap,
                                            const TVector<std::pair<TInfoUnit, TExprNode::TPtr>>& groupByKeysExpressionsMap,
                                            TExprContext& ctx, TPositionHandle pos) {
    // Add expressions
    TVector<TExprNode::TPtr> mapElements;
    for (const auto& [colName, expr] : aggFieldsExpressionsMap) {
        // clang-format off
        mapElements.push_back(Build<TKqpOpMapElementLambda>(ctx, pos)
            .Input(resultExpr)
            .Variable()
                .Value(colName.GetFullName())
            .Build()
            .Lambda(expr)
        .Done().Ptr());
        // clang-format on
    }

    // Add expressions for group by keys.
    for (const auto& [colName, expr] : groupByKeysExpressionsMap) {
        // clang-format off
        mapElements.push_back(Build<TKqpOpMapElementLambda>(ctx, pos)
            .Input(resultExpr)
            .Variable()
                .Value(colName.GetFullName())
            .Build()
            .Lambda(expr)
        .Done().Ptr());
        // clang-format on
    }

    // clang-format off
    return Build<TKqpOpMap>(ctx, pos)
        .Input(resultExpr)
        .MapElements()
            .Add(mapElements)
        .Build()
        .Project()
            .Value("true")
        .Build()
    .Done().Ptr();
    // clang-format on
}

TString GenerateUniqueColumnName(const TString &colName) {
    TStringBuilder strBuilder;
    strBuilder << "_kqp_agg_input_";
    strBuilder << colName;
    strBuilder << "_";
    strBuilder << ToString(KqpUniqueAggColumnId++);
    return strBuilder;
}

void ToCamelCase(std::string& s) {
    char previous = ' ';
    auto f = [&](char current) {
        char result = (std::isblank(previous) && std::isalpha(current)) ? std::toupper(current) : std::tolower(current);
        previous = current;
        return result;
    };
    std::transform(s.begin(), s.end(), s.begin(), f);
}

TExprNode::TPtr ReplacePgOps(TExprNode::TPtr input, TExprContext &ctx) {
    if (input->IsLambda()) {
        auto lambda = TCoLambda(input);

        // clang-format off
            return Build<TCoLambda>(ctx, input->Pos())
                .Args(lambda.Args())
                .Body(ReplacePgOps(lambda.Body().Ptr(), ctx))
            .Done().Ptr();
        // clang-format on
    } else if (input->IsCallable("PgAnd")) {
        // clang-format off
            return ctx.Builder(input->Pos())
                .Callable("ToPg")
                    .Callable(0, "And")
                        .Callable(0, "FromPg")
                            .Add(0, ReplacePgOps(input->ChildPtr(0), ctx))
                        .Seal()
                        .Callable(1, "FromPg")
                            .Add(0, ReplacePgOps(input->ChildPtr(1), ctx))
                        .Seal()
                    .Seal()
                .Seal()
            .Build();
        // clang-format on

    } else if (input->IsCallable("PgOr")) {
        // clang-format off
            return ctx.Builder(input->Pos())
                .Callable("ToPg")
                    .Callable(0, "Or")
                        .Callable(0, "FromPg")
                            .Add(0, ReplacePgOps(input->ChildPtr(0), ctx))
                        .Seal()
                        .Callable(1, "FromPg")
                            .Add(0, ReplacePgOps(input->ChildPtr(1), ctx))
                        .Seal()
                    .Seal()
                .Seal()
            .Build();
            // clnag-format on
    } else if (input->IsCallable()){
            TVector<TExprNode::TPtr> newChildren;
            for (auto c : input->Children()) {
                newChildren.push_back(ReplacePgOps(c, ctx));
            }
            // clang-format off
            return ctx.Builder(input->Pos())
                .Callable(input->Content())
                    .Add(std::move(newChildren))
                .Seal()
            .Build();
        // clang-format on
    } else if (input->IsList()) {
        TVector<TExprNode::TPtr> newChildren;
        for (auto c : input->Children()) {
            newChildren.push_back(ReplacePgOps(c, ctx));
        }
        // clang-format off
            return ctx.Builder(input->Pos())
                .List()
                    .Add(std::move(newChildren))
                .Seal()
            .Build();
        // clang-format on
    } else {
        return input;
    }
}

TExprNode::TPtr BuildSort(TExprNode::TPtr input, TExprNode::TPtr sort, TExprContext &ctx) {
    TVector<TExprNode::TPtr> sortElements;

    for (auto sortItem : sort->Child(1)->Children()) {
        auto sortLambda = sortItem->Child(1);
        auto direction = sortItem->Child(2);
        auto nullsFirst = sortItem->Child(3);

        // clang-format off
        sortElements.push_back(Build<TKqpOpSortElement>(ctx, input->Pos())
            .Input(input)
            .Direction(direction)
            .NullsFirst(nullsFirst)
            .Lambda(sortLambda)
            .Done().Ptr());
        // clang-format on
    }

    // clang-format off
    return Build<TKqpOpSort>(ctx, input->Pos())
        .Input(input)
        .SortExpressions().Add(sortElements).Build()
        .Done().Ptr();
    // clang-format off
}

TExprNode::TPtr RewritePgSelect(const TExprNode::TPtr &node, TExprContext &ctx, const TTypeAnnotationContext &typeCtx) {
    Y_UNUSED(typeCtx);

    TVector<TString> finalColumnOrder;

    auto setItems = GetSetting(node->Head(), "set_items")->TailPtr();
    TVector<TExprNode::TPtr> setItemsResults;
    for (ui32 i = 0; i < setItems->ChildrenSize(); ++i) {
        auto setItem = setItems->ChildPtr(i);

        TVector<TExprNode::TPtr> resultElements;
        // In pg syntax duplicate attributes are allowed in the results, but we need to rename them
        // We use the counters for this purpose
        THashMap<TString, int> resultElementCounters;

        TExprNode::TPtr joinExpr;
        TExprNode::TPtr filterExpr;
        TExprNode::TPtr lastAlias;

        auto from = GetSetting(setItem->Tail(), "from");
        THashMap<TString, TExprNode::TPtr> aliasToInputMap;
        TVector<TExprNode::TPtr> inputsInOrder;

        if (from) {
            for (auto fromItem : from->Child(1)->Children()) {
                // From item can be a table read with an alias or a subquery with an alias
                // In case of a subquery, we have already translated PgSelect of the nested subquery
                // so we just need to remove TKqpOpRoot and plug in the translated subquery

                auto childExpr = fromItem->ChildPtr(0);
                auto alias = fromItem->Child(1);
                TExprNode::TPtr fromExpr;

                if (TKqpOpRoot::Match(childExpr.Get())) {
                    auto opRoot = TKqpOpRoot(childExpr);

                    TVector<TExprNode::TPtr> subqueryElements;

                    // We need to rename all the IUs in the subquery to reflect the new alias
                    auto subqueryType = childExpr->GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
                    for (auto item : subqueryType->GetItems()) {
                        auto orig = TString(item->GetName());
                        auto unit = TInfoUnit(orig);
                        auto renamedUnit = TInfoUnit(TString(alias->Content()), unit.ColumnName);

                        // clang-format off
                        subqueryElements.push_back(Build<TKqpOpMapElementRename>(ctx, node->Pos())
                            .Input(opRoot.Input())
                            .Variable().Value(renamedUnit.GetFullName()).Build()
                            .From().Value(unit.GetFullName()).Build()
                        .Done().Ptr());
                        // clang-format on
                    }

                    // clang-format off
                    fromExpr = Build<TKqpOpMap>(ctx, node->Pos())
                        .Input(opRoot.Input())
                        .MapElements().Add(subqueryElements).Build()
                        .Project().Value("true").Build()
                    .Done().Ptr();
                    // clang-format on
                }

                else {
                    auto readExpr = TKqlReadTableRanges(childExpr);

                    // clang-format off
                    fromExpr = Build<TKqpOpRead>(ctx, node->Pos())
                        .Table(readExpr.Table())
                        .Alias(alias)
                        .Columns(readExpr.Columns())
                    .Done().Ptr();
                    // clang-format on
                }

                aliasToInputMap.insert({TString(alias->Content()), fromExpr});
                inputsInOrder.push_back(fromExpr);
                lastAlias = alias;
            }
        }

        THashSet<TString> processedInputs;
        auto joinOps = GetSetting(setItem->Tail(), "join_ops");
        if (joinOps) {
            for (ui32 i = 0; i < joinOps->Tail().ChildrenSize(); ++i) {
                ui32 tableInputsCount = 0;
                auto tuple = joinOps->Tail().Child(i);
                for (ui32 j = 0; j < tuple->ChildrenSize(); ++j) {
                    auto join = tuple->Child(j);
                    auto joinType = join->Child(0)->Content();
                    if (joinType == "push") {
                        ++tableInputsCount;
                        continue;
                    }

                    Y_ENSURE(join->ChildrenSize() > 1 && join->Child(1)->ChildrenSize() > 1);
                    auto pgResolvedOps = FindNodes(join->Child(1)->Child(1)->TailPtr(), [](const TExprNode::TPtr &node) {
                        if (node->IsCallable("PgResolvedOp")) {
                            return true;
                        } else {
                            return false;
                        }
                    });

                    // FIXME: join on clause may include expressions, we need to handle this case
                    TVector<TInfoUnit> joinKeys;
                    for (const auto &pgResolvedOp : pgResolvedOps) {
                        TVector<TInfoUnit> keys;
                        GetAllMembers(pgResolvedOp, keys);
                        joinKeys.insert(joinKeys.end(), keys.begin(), keys.end());
                    }

                    TJoinTableAliases joinAliases;
                    TExprNode::TPtr leftInput;
                    TExprNode::TPtr rightInput;

                    if (tableInputsCount == 2) {
                        joinAliases = GatherJoinAliasesTwoInputs(joinKeys);
                        const auto leftSideAlias = *joinAliases.LeftSideAliases.begin();
                        const auto rightSideAlias = *joinAliases.RightSideAliases.begin();
                        Y_ENSURE(aliasToInputMap.count(leftSideAlias), "Left side alias is not present in input tables");
                        Y_ENSURE(aliasToInputMap.count(rightSideAlias), "Right sided alias is not present input tables");
                        leftInput = aliasToInputMap[leftSideAlias];
                        rightInput = aliasToInputMap[rightSideAlias];
                    } else if (tableInputsCount == 1) {
                        joinAliases = GatherJoinAliasesLeftSideMultiInputs(joinKeys, processedInputs);
                        const auto rightSideAlias = *joinAliases.RightSideAliases.begin();
                        Y_ENSURE(aliasToInputMap.contains(rightSideAlias), "Right side alias is not present in input tables");
                        leftInput = joinExpr;
                        rightInput = aliasToInputMap[rightSideAlias];
                    }

                    auto joinKind = TString(joinType);
                    ToCamelCase(joinKind.MutRef());

                    // clang-format off
                    joinExpr = Build<TKqpOpJoin>(ctx, node->Pos())
                        .LeftInput(leftInput)
                        .RightInput(rightInput)
                        .JoinKind()
                            .Value(joinKind)
                        .Build()
                        .JoinKeys(BuildJoinKeys(joinKeys, joinAliases, processedInputs, ctx, node->Pos()))
                    .Done().Ptr();
                    // clang-format on
                    tableInputsCount = 0;
                }
            }

            // Build in order
            if (!joinExpr) {
                ui32 inputIndex = 0;
                if (inputsInOrder.size() > 1) {
                    while (inputIndex < inputsInOrder.size()) {
                        auto leftTableInput = inputIndex == 0 ? inputsInOrder[inputIndex] : joinExpr;
                        auto rightTableInput = inputIndex == 0 ? inputsInOrder[inputIndex + 1] : inputsInOrder[inputIndex];
                        auto joinKeys = Build<TDqJoinKeyTupleList>(ctx, node->Pos()).Done();
                        // clang-format off
                        joinExpr = Build<TKqpOpJoin>(ctx, node->Pos())
                            .LeftInput(leftTableInput)
                            .RightInput(rightTableInput)
                            .JoinKind()
                                .Value("Cross")
                            .Build()
                            .JoinKeys(joinKeys)
                        .Done().Ptr();
                        // clang-format on
                        inputIndex += (inputIndex == 0 ? 2 : 1);
                    }
                } else {
                    joinExpr = inputsInOrder.front();
                }
            }
        }

        filterExpr = joinExpr;

        auto where = GetSetting(setItem->Tail(), "where");

        if (where) {
            TExprNode::TPtr lambda = where->Child(1)->Child(1);
            lambda = ReplacePgOps(lambda, ctx);
            // clang-format off
            filterExpr = Build<TKqpOpFilter>(ctx, node->Pos())
                .Input(filterExpr)
                .Lambda(lambda)
            .Done().Ptr();
            // clang-format on
        }

        if (!filterExpr) {
            filterExpr = Build<TKqpOpEmptySource>(ctx, node->Pos()).Done().Ptr();
        }

        // Group by fields for renames or expressions.
        TVector<std::pair<TInfoUnit, TInfoUnit>> groupByKeysRenamesMap;
        TVector<std::pair<TInfoUnit, TExprNode::TPtr>> groupByKeysExpressionsMap;
        // Aggregate.
        TAggregationTraits aggTraits;
        // Pre/Post distinct aggregations.
        TAggregationTraits distinctAggregationTraitsPreAggregate;
        TAggregationTraits distinctAggregationTraitsPostAggregate;
        auto groupOps = GetSetting(setItem->Tail(), "group_exprs");
        THashSet<TString> groupByUniqueKeyNames;
        if (groupOps) {
            const auto groupByList = groupOps->TailPtr();
            for (ui32 i = 0; i < groupByList->ChildrenSize(); ++i) {
                auto pgGroup = groupByList->ChildPtr(i);
                auto lambda = TCoLambda(ctx.DeepCopyLambda(*(pgGroup->Child(1))));
                auto body = lambda.Body().Ptr();
                TInfoUnit groupByKeyName;
                TExprNode::TPtr newBody;

                // Expression for group by keys.
                if (IsExpression(body)) {
                    // For exression we use map.
                    // For example: f(a) group by b + c => map(a -> a, b + c -> d) -> f(a) group by d
                    newBody = ctx.NewCallable(node->Pos(), "FromPg", {body});
                    groupByKeyName = TInfoUnit(GenerateUniqueColumnName("group_expr"));
               } else {
                    Y_ENSURE(body->IsCallable("Member"), "Invalid callable for PgGroup: " + TString(body->Content()));
                    auto member = TCoMember(body);
                    groupByKeyName = TInfoUnit(member.Name().StringValue());
                    Y_ENSURE(!groupByUniqueKeyNames.contains(groupByKeyName.GetFullName()), "Not unique key name for group by kyes is not supported.");
                    groupByUniqueKeyNames.insert(groupByKeyName.GetFullName());
                    newBody = member.Ptr();
                }

                // clang-format off
                auto groupExprLambda = Build<TCoLambda>(ctx, node->Pos())
                    .Args(lambda.Args())
                    .Body(newBody)
                .Done().Ptr();
                // clang-format on

                groupByKeysExpressionsMap.push_back(std::make_pair(groupByKeyName, groupExprLambda));
                aggTraits.KeyColumns.push_back(groupByKeyName);
            }
        }

        const bool distinctAll = !!GetSetting(setItem->Tail(), "distinct_all");
        auto result = GetSetting(setItem->Tail(), "result");
        Y_ENSURE(result);
        auto finalType = node->GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();

        // Aggregations.
        TVector<std::pair<TInfoUnit, TExprNode::TPtr>> expressionsMapPreAgg;
        TVector<std::pair<TInfoUnit, TExprNode::TPtr>> expressionsMapPostAgg;
        THashSet<TString> uniqueColNames;
        bool distinctPreAggregate = false;
        for (ui32 i = 0; i < result->Child(1)->ChildrenSize(); ++i) {
            const auto resultItem = result->Child(1)->ChildPtr(i);
            auto lambda = TCoLambda(ctx.DeepCopyLambda(*(resultItem->Child(2))));
            auto resultColName = TString(resultItem->Child(0)->Content());
            const auto* aggFuncResultType = finalType->FindItemType(resultColName);
            Y_ENSURE(aggFuncResultType, "Cannot find type for aggregation result.");
            THashMap<TExprNode::TPtr, TString> aggregationsForReplacement;

            if (auto aggregations = CollectAggregations(lambda.Body().Ptr()); !aggregations.empty()) {
                for (const auto &aggregation : aggregations) {
                    auto aggInput = aggregation->ChildPtr(2);
                    TInfoUnit aggColName;
                    TExprNode::TPtr exprBody;

                    if (IsExpression(aggInput)) {
                        // Aggregation on expression f(a x b).
                        // We pull expression outside a given aggregation and rename result of a given expression with unique name
                        // to later process result with aggregate function.
                        // For example: f(a x b) => map((a x b) -> c) -> f(c)
                        exprBody = ctx.NewCallable(aggInput->Pos(), "FromPg", {aggInput});
                        aggColName = TInfoUnit(GenerateUniqueColumnName("expr"));
                    } else {
                        // Pure aggregation f(a).
                        // Here we want to get just a column name for aggregation.
                        // For example: f(a) -> map(a -> a) -> f(a).
                        // This is needed to simplify logic for translation from PgSelect to KqpOp.
                        Y_ENSURE(aggInput->IsCallable("ToPg") && aggInput->ChildPtr(0)->IsCallable("Member"), "PgAgg not a member");
                        auto member = TCoMember(aggInput->ChildPtr(0));
                        exprBody = member.Ptr();
                        // f(a), g(a) => map(a -> a, a -> b) -> f(a), g(b)
                        TString colName = member.Name().StringValue();
                        if (uniqueColNames.contains(colName)) {
                            colName = GenerateUniqueColumnName("expr");
                        }
                        uniqueColNames.insert(colName);
                        aggColName = TInfoUnit(colName);
                    }

                    // clang-format off
                    auto exprLambda = Build<TCoLambda>(ctx, node->Pos())
                        .Args(lambda.Args())
                        .Body(exprBody)
                    .Done().Ptr();
                    // clang-format on

                   expressionsMapPreAgg.push_back({aggColName, exprLambda});
                   aggregationsForReplacement[aggregation] = aggColName.GetFullName();

                    // Distinct for column or expression f(distinct a) => (distinct a) as b -> f(b).
                    if (!!GetSetting(*aggregation->Child(1), "distinct")) {
                        const auto colName = aggColName.GetFullName();
                        auto distinctAggTraits = BuildAggregationTraits(colName, colName, "distinct", aggFuncResultType, ctx, node->Pos());
                        distinctAggregationTraitsPreAggregate.AggTraitsList.push_back(distinctAggTraits);
                        distinctAggregationTraitsPreAggregate.KeyColumns.push_back(aggColName);
                        distinctPreAggregate = true;
                    }

                    const TString aggFuncName = TString(aggregation->ChildPtr(0)->Content());
                    // Build an aggregation traits.
                    auto aggregationTraits =
                        BuildAggregationTraits(aggColName.GetFullName(), aggColName.GetFullName(), aggFuncName, aggFuncResultType, ctx, node->Pos());
                    aggTraits.AggTraitsList.push_back(aggregationTraits);
                }

                TNodeOnNodeOwnedMap nodeReplacementMap;
                for (const auto& [aggregation, colName] : aggregationsForReplacement) {
                    // clang-format off
                    auto member = Build<TCoMember>(ctx, node->Pos())
                        .Struct(lambda.Args().Arg(0))
                        .Name<TCoAtom>()
                            .Value(colName)
                        .Build()
                    .Done().Ptr();
                    // clang-format on

                    // Do not need convertion to pg, because input of projection map is aggregation.
                    if (!distinctAll) {
                        auto toPg = ctx.NewCallable(node->Pos(), "ToPg", {member});
                        auto pgType = ctx.NewCallable(
                            node->Pos(), "PgType",
                            {ctx.NewAtom(node->Pos(), NPg::LookupType(aggFuncResultType->Cast<TPgExprType>()->GetId()).Name)});
                        member = ctx.NewCallable(node->Pos(), "PgCast", {toPg, pgType});
                    }

                    nodeReplacementMap[aggregation.Get()] = member;
                }

                auto newBody = ctx.ReplaceNodes(lambda.Body().Ptr(), nodeReplacementMap);
                // clang-format off
                auto exprLambda = Build<TCoLambda>(ctx, node->Pos())
                    .Args(lambda.Args())
                    .Body(newBody)
                .Done().Ptr();
                // clang-format on

                auto colName = TInfoUnit(resultColName);
                expressionsMapPostAgg.push_back({colName, exprLambda});

                // Case for distinct after aggregation.
                if (distinctAll) {
                    auto distinctAggTraits =
                        BuildAggregationTraits(resultColName, resultColName, "distinct", aggFuncResultType, ctx, node->Pos());
                    distinctAggregationTraitsPostAggregate.AggTraitsList.push_back(distinctAggTraits);
                    distinctAggregationTraitsPostAggregate.KeyColumns.push_back(TInfoUnit(resultColName));
                }
            } else if (distinctAll) {
                // This case covers distinct all on just columns without aggregation functions.
                auto pgGroupRef = GetPgCallable(lambda.Body().Ptr(), "PgGroupRef");
                TInfoUnit colName;
                if (pgGroupRef) {
                    colName = TInfoUnit(GetColumnNameFromPgGroupRef(pgGroupRef, groupByKeysExpressionsMap));
                } else {
                    auto body = lambda.Body().Ptr();
                    Y_ENSURE(body->IsCallable("Member"), "Distinct on expression is not supported");
                    auto member = TCoMember(body);
                    colName = TInfoUnit(member.Name().StringValue());
                }

                auto distinctAggTraits =
                    BuildAggregationTraits(colName.GetFullName(), colName.GetFullName(), "distinct", aggFuncResultType, ctx, node->Pos());
                distinctAggregationTraitsPostAggregate.AggTraitsList.push_back(distinctAggTraits);
                distinctAggregationTraitsPostAggregate.KeyColumns.push_back(colName);
            }
        }

        // Distinct pre aggregate fro group by keys.
        if (distinctPreAggregate) {
            for (const auto& key : aggTraits.KeyColumns) {
                const auto colName = key.GetFullName();
                const auto* aggFuncResultType = finalType->FindItemType(key.ColumnName);
                Y_ENSURE(aggFuncResultType, "Cannot find type for aggregation result");
                auto distinctAggTraits = BuildAggregationTraits(colName, colName, "distinct", aggFuncResultType, ctx, node->Pos());
                distinctAggregationTraitsPreAggregate.AggTraitsList.push_back(distinctAggTraits);
                distinctAggregationTraitsPreAggregate.KeyColumns.push_back(colName);
            }
        }

        // Distinct post aggregate for group by keys.
        if (distinctAll) {
            for (const auto& key : aggTraits.KeyColumns) {
                const auto colName = key.GetFullName();
                const auto* aggFuncResultType = finalType->FindItemType(key.ColumnName);
                // agg key in result set.
                if (aggFuncResultType) {
                    auto distinctAggTraits = BuildAggregationTraits(colName, colName, "distinct", aggFuncResultType, ctx, node->Pos());
                    distinctAggregationTraitsPostAggregate.AggTraitsList.push_back(distinctAggTraits);
                    distinctAggregationTraitsPostAggregate.KeyColumns.push_back(colName);
                }
            }
        }

        TExprNode::TPtr resultExpr = filterExpr;
        // In case we have an expression for aggregation - f(a + b ...) or group by.
        if (!expressionsMapPreAgg.empty() || !groupByKeysExpressionsMap.empty()) {
            resultExpr = BuildAggregateExpressionMap(resultExpr, expressionsMapPreAgg, groupByKeysExpressionsMap, ctx, node->Pos());
        }
        // Build distinct aggregate pre aggregate.
        if (!distinctAggregationTraitsPreAggregate.AggTraitsList.empty()) {
            resultExpr = BuildAggregate(resultExpr, distinctAggregationTraitsPreAggregate.AggTraitsList,
                                        distinctAggregationTraitsPreAggregate.KeyColumns, /*distinct=*/ true, ctx, node->Pos());
        }
        // Build Aggreegate.
        if (!aggTraits.AggTraitsList.empty()) {
            resultExpr = BuildAggregate(resultExpr, aggTraits.AggTraitsList, aggTraits.KeyColumns, /*distinct=*/ false, ctx, node->Pos());
        }
        // In case we have an expression on aggregation - f(...) x b.
        if (!expressionsMapPostAgg.empty()) {
            resultExpr = BuildAggregateExpressionMap(resultExpr, expressionsMapPostAgg,
                                                     BuildExpressionsFromColumns(aggTraits.KeyColumns, ctx, node->Pos()), ctx, node->Pos());
        }
        // Build distinct aggregate post aggregate.
        if (!distinctAggregationTraitsPostAggregate.AggTraitsList.empty()) {
            resultExpr = BuildAggregate(resultExpr, distinctAggregationTraitsPostAggregate.AggTraitsList,
                                        distinctAggregationTraitsPostAggregate.KeyColumns, /*distinct=*/ true, ctx, node->Pos());
        }

        finalColumnOrder.clear();

        for (auto resultItem : result->Child(1)->Children()) {
            auto column = resultItem->Child(0);
            TString columnName = TString(column->Content());

            const auto expectedTypeNode = finalType->FindItemType(columnName);
            Y_ENSURE(expectedTypeNode);
            const auto expectedType = expectedTypeNode->Cast<TPgExprType>();
            const auto actualTypeNode = resultItem->GetTypeAnn();

            YQL_CLOG(TRACE, CoreDq) << "Actual type for column: " << columnName << " is: " << *actualTypeNode;
            YQL_CLOG(TRACE, CoreDq) << "Expected type for column: " << columnName << " is: " << *expectedTypeNode;

            ui32 actualPgTypeId;
            bool convertToPg;
            Y_ENSURE(ExtractPgType(actualTypeNode, actualPgTypeId, convertToPg, node->Pos(), ctx));

            bool needPgCast = (expectedType->GetId() != actualPgTypeId);
            auto lambda = TCoLambda(ctx.DeepCopyLambda(*(resultItem->Child(2))));
            bool needPgCastForAgg = distinctAll;

            auto pgAgg = GetPgCallable(lambda.Body().Ptr(), "PgAgg");
            auto pgGroupRef = GetPgCallable(lambda.Body().Ptr(), "PgGroupRef");
            // Eliminate aggregation or reference to a group by expression from result lambda.
            auto aggColName = columnName;
            if (pgAgg || pgGroupRef) {
                if (pgGroupRef) {
                    aggColName = GetColumnNameFromPgGroupRef(pgGroupRef, groupByKeysExpressionsMap);
                }

                // clang-format off
                lambda = Build<TCoLambda>(ctx, node->Pos())
                    .Args(lambda.Args())
                    .Body<TCoMember>()
                        .Struct(lambda.Args().Arg(0))
                        .Name<TCoAtom>()
                            .Value(aggColName)
                        .Build()
                    .Build()
                .Done();
                // clang-format on

                needPgCastForAgg = true;
            }

            if (convertToPg && !needPgCastForAgg) {
                Y_ENSURE(!needPgCast,
                         TStringBuilder() << "Conversion to PG type is different at typization (" << expectedType->GetId()
                                          << ") and optimization (" << actualPgTypeId << ") stages.");

                TExprNode::TPtr lambdaBody = lambda.Body().Ptr();
                lambdaBody = ReplacePgOps(lambdaBody, ctx);
                auto toPg = ctx.NewCallable(node->Pos(), "ToPg", {lambdaBody});

                // clang-format off
                lambda = Build<TCoLambda>(ctx, node->Pos())
                    .Args(lambda.Args())
                    .Body(toPg)
                .Done();
                // clang-format on
            } else if ((needPgCast || needPgCastForAgg)) {

                auto pgType =
                    ctx.NewCallable(node->Pos(), "PgType", {ctx.NewAtom(node->Pos(), NPg::LookupType(expectedType->GetId()).Name)});
                TExprNode::TPtr lambdaBody = lambda.Body().Ptr();
                lambdaBody = ReplacePgOps(lambdaBody, ctx);
                auto pgCast = ctx.NewCallable(node->Pos(), "PgCast", {lambdaBody, pgType});

                // clang-format off
                lambda = Build<TCoLambda>(ctx, node->Pos())
                    .Args(lambda.Args())
                    .Body(pgCast)
                .Done();
                // clang-format on
            }

            if (resultElementCounters.contains(columnName)) {
                resultElementCounters[columnName] += 1;
                columnName = columnName + "_generated_" + std::to_string(resultElementCounters.at(columnName));
            } else {
                resultElementCounters[columnName] = 1;
            }

            finalColumnOrder.push_back(columnName);
            auto variable = Build<TCoAtom>(ctx, node->Pos()).Value(columnName).Done();

            // clang-format off
            resultElements.push_back(Build<TKqpOpMapElementLambda>(ctx, node->Pos())
                .Input(resultExpr)
                .Variable(variable)
                .Lambda(lambda)
            .Done().Ptr());
            // clang-format on
        }

        // clang-format off
        auto setItemPtr = Build<TKqpOpMap>(ctx, node->Pos())
            .Input(resultExpr)
            .MapElements()
                .Add(resultElements)
            .Build()
            .Project()
                .Value("true")
            .Build()
        .Done().Ptr();
        // clang-format onto

        auto sort = GetSetting(setItem->Tail(), "sort");
        if (sort) {
            setItemPtr = BuildSort(setItemPtr, sort, ctx);
        }

        setItemsResults.push_back(setItemPtr);
    }

    auto setOps = GetSetting(node->Head(), "set_ops");
    Y_ENSURE(setOps && setItemsResults.size());

    auto setOpsList = setOps->TailPtr();
    TExprNode::TPtr opResult = setItemsResults.front();
    for (ui32 i = 0, end = setOpsList->ChildrenSize(), setItemsIndex = 0, opsInputCount = 0; i < end; ++i) {
        if (setOpsList->ChildPtr(i)->Content() == "push") {
            ++opsInputCount;
            continue;
        }
        Y_ENSURE(setOpsList->ChildPtr(i)->Content() == "union_all");
        Y_ENSURE(opsInputCount <= 2);

        TExprNode::TPtr leftInput;
        TExprNode::TPtr rightInput;
        if (opsInputCount == 2) {
            Y_ENSURE(setItemsIndex + 1 < end);
            leftInput = setItemsResults[setItemsIndex++];
            rightInput = setItemsResults[setItemsIndex++];
        } else {
            Y_ENSURE(setItemsIndex < end);
            leftInput = opResult;
            rightInput = setItemsResults[setItemsIndex++];
        }

        // clang-format off
        opResult = Build<TKqpOpUnionAll>(ctx, node->Pos())
            .LeftInput(leftInput)
            .RightInput(rightInput)
        .Done().Ptr();
        // clang-format on

        // Count again.
        opsInputCount = 0;
    }

    auto sort = GetSetting(node->Head(), "sort");
    if (sort) {
        opResult = BuildSort(opResult, sort, ctx);
    }

    TVector<TCoAtom> columnAtomList;
    for (auto c : finalColumnOrder) {
        columnAtomList.push_back(Build<TCoAtom>(ctx, node->Pos()).Value(c).Done());
    }
    auto columnOrder = Build<TCoAtomList>(ctx, node->Pos()).Add(columnAtomList).Done().Ptr();

    // clang-format off
    return Build<TKqpOpRoot>(ctx, node->Pos())
        .Input(opResult)
        .ColumnOrder(columnOrder)
    .Done().Ptr();
    // clang-format on

}

TExprNode::TPtr PushTakeIntoPlan(const TExprNode::TPtr &node, TExprContext &ctx, const TTypeAnnotationContext &typeCtx) {
    Y_UNUSED(typeCtx);
    auto take = TCoTake(node);
    if (auto root = take.Input().Maybe<TKqpOpRoot>()) {
        // clang-format off
        return Build<TKqpOpRoot>(ctx, node->Pos())
            .Input<TKqpOpLimit>()
                .Input(root.Cast().Input())
                .Count(take.Count())
            .Build()
            .ColumnOrder(root.Cast().ColumnOrder())
        .Done().Ptr();
        // clang-format on
    } else {
        return node;
    }
}

TExprNode::TPtr RewritePgSublink(const TExprNode::TPtr &node, TExprContext &ctx) {
    if (node->Child(0)->Content() != "expr") {
        return node;
    }

    // clang-format off
    return Build<TKqpPgExprSublink>(ctx, node->Pos())
        .Expr(node->Child(4))
        .Done().Ptr();
    // clang-format on
}

TExprNode::TPtr RemoveRootFromSublink(const TExprNode::TPtr &node, TExprContext &ctx) {
    auto sublink = TKqpPgExprSublink(node);
    if (auto root = sublink.Expr().Maybe<TKqpOpRoot>()) {
        // clang-format off
        return Build<TKqpPgExprSublink>(ctx, node->Pos())
            .Expr(root.Cast().Input())
            .Done().Ptr();
        // clang-format on
    }
    return node;
}
} // namespace

namespace NKikimr {
namespace NKqp {

IGraphTransformer::TStatus TKqpPgRewriteTransformer::DoTransform(TExprNode::TPtr input, TExprNode::TPtr &output, TExprContext &ctx) {
    output = input;
    TOptimizeExprSettings settings(&TypeCtx);

    auto status = OptimizeExpr(
        output, output,
        [](const TExprNode::TPtr &node, TExprContext &ctx) -> TExprNode::TPtr {
            if (node->IsCallable("PgSubLink")) {
                return RewritePgSublink(node, ctx);
            } else {
                return node;
            }
        },
        ctx, settings);
    
    if (status != TStatus::Ok) {
        return status;
    }

    status = OptimizeExpr(
        output, output,
        [this](const TExprNode::TPtr &node, TExprContext &ctx) -> TExprNode::TPtr {
            if (TCoPgSelect::Match(node.Get())) {
                return RewritePgSelect(node, ctx, TypeCtx);
            } else if (TKqpPgExprSublink::Match(node.Get())) {
                return RemoveRootFromSublink(node, ctx);
            } else if (TCoTake::Match(node.Get())) {
                return PushTakeIntoPlan(node, ctx, TypeCtx);
            } else {
                return node;
            }
        },
        ctx, settings);

    return status;
}

void TKqpPgRewriteTransformer::Rewind() {}

IGraphTransformer::TStatus TKqpNewRBOTransformer::DoTransform(TExprNode::TPtr input, TExprNode::TPtr &output, TExprContext &ctx) {
    output = input;
    TOptimizeExprSettings settings(&TypeCtx);

    auto status = OptimizeExpr(
        output, output,
        [this](const TExprNode::TPtr &node, TExprContext &ctx) -> TExprNode::TPtr {
            Y_UNUSED(ctx);
            if (TKqpOpRoot::Match(node.Get())) {
                auto root = PlanConverter(TypeCtx, ctx).ConvertRoot(node);
                root.ComputeParents();
                return RBO.Optimize(root, ctx);
            } else {
                return node;
            }
        },
        ctx, settings);

    if (status != IGraphTransformer::TStatus::Ok) {
        return status;
    }

    return IGraphTransformer::TStatus::Ok;
}

void TKqpNewRBOTransformer::Rewind() {}

IGraphTransformer::TStatus TKqpRBOCleanupTransformer::DoTransform(TExprNode::TPtr input, TExprNode::TPtr &output, TExprContext &ctx) {
    output = input;
    TOptimizeExprSettings settings(&TypeCtx);

    Y_UNUSED(ctx);

    YQL_CLOG(TRACE, CoreDq) << "Cleanup input plan: " << KqpExprToPrettyString(TExprBase(output), ctx) << Endl;


    if (output->IsList() && output->ChildrenSize() >= 1) {
        auto child_level_1 = output->Child(0);
        YQL_CLOG(TRACE, CoreDq) << "Matched level 0";

        if (child_level_1->IsList() && child_level_1->ChildrenSize() >= 1) {
            auto child_level_2 = child_level_1->Child(0);
            YQL_CLOG(TRACE, CoreDq) << "Matched level 1";

            if (child_level_2->IsList() && child_level_2->ChildrenSize() >= 1) {
                auto child_level_3 = child_level_2->Child(0);
                YQL_CLOG(TRACE, CoreDq) << "Matched level 2";

                if (child_level_3->IsList() && child_level_2->ChildrenSize() >= 1) {
                    auto maybeQuery = child_level_3->Child(0);

                    if (TKqpPhysicalQuery::Match(maybeQuery)) {
                        YQL_CLOG(TRACE, CoreDq) << "Found query node";
                        output = maybeQuery;
                    }
                }
            }
        }
    }

    return IGraphTransformer::TStatus::Ok;
}

void TKqpRBOCleanupTransformer::Rewind() {}

TAutoPtr<IGraphTransformer> CreateKqpPgRewriteTransformer(const TIntrusivePtr<TKqpOptimizeContext> &kqpCtx,
                                                          TTypeAnnotationContext &typeCtx) {
    return new TKqpPgRewriteTransformer(kqpCtx, typeCtx);
}

TAutoPtr<IGraphTransformer> CreateKqpNewRBOTransformer(const TIntrusivePtr<TKqpOptimizeContext> &kqpCtx, TTypeAnnotationContext &typeCtx,
                                                       TAutoPtr<IGraphTransformer> rboTypeAnnTransformer,
                                                       TAutoPtr<IGraphTransformer> typeAnnTransformer,
                                                       TAutoPtr<IGraphTransformer> peephole,
                                                       const NMiniKQL::IFunctionRegistry& funcRegistry) {
    return new TKqpNewRBOTransformer(kqpCtx, typeCtx, rboTypeAnnTransformer, typeAnnTransformer, peephole, funcRegistry);
}

TAutoPtr<IGraphTransformer> CreateKqpRBOCleanupTransformer(TTypeAnnotationContext &typeCtx) {
    return new TKqpRBOCleanupTransformer(typeCtx);
}

} // namespace NKqp
} // namespace NKikimr