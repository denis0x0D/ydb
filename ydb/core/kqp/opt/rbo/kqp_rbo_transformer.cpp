#include "kqp_rbo_transformer.h"
#include "kqp_operator.h"
#include <yql/essentials/utils/log/log.h>

using namespace NYql;
using namespace NYql::NNodes;
using namespace NKikimr::NKqp;
using namespace NYql::NDq;

namespace {

class TJoinKeyBuilder {
    TVector<TInfoUnit> joinKeys;

public:
    TJoinKeyBuilder(const TVector<TInfoUnit> &joinKeys) : joinKeys(joinKeys) {
        Y_ENSURE(joinKeys.size() >= 2 && !(joinKeys.size() & 1));
    }

    TExprNode::TPtr BuildJoinKeysTwoTableInputs(TExprContext &ctx, TPositionHandle pos) {
        TVector<TDqJoinKeyTuple> keys;
        for (ui32 i = 0; i < joinKeys.size(); i += 2) {
            keys.push_back(Build<TDqJoinKeyTuple>(ctx, pos)
                .LeftLabel().Value(joinKeys[i].Alias).Build()
                .LeftColumn().Value(joinKeys[i].ColumnName).Build()
                .RightLabel().Value(joinKeys[i + 1].Alias).Build()
                .RightColumn().Value(joinKeys[i + 1].ColumnName).Build()
                .Done());
        }
        return Build<TDqJoinKeyTupleList>(ctx, pos).Add(keys).Done().Ptr();
    }

    TString GetLeftInputAlias() { return joinKeys[0].Alias; }
    TString GetRightInputAlias() { return joinKeys[1].Alias; }
};

TExprNode::TPtr RewritePgSelect(const TExprNode::TPtr& node, TExprContext& ctx, const TTypeAnnotationContext& typeCtx) {
    Y_UNUSED(typeCtx);
    auto setItems = GetSetting(node->Head(), "set_items");
    
    TVector<TExprNode::TPtr> resultElements;

    TExprNode::TPtr joinExpr;
    TExprNode::TPtr filterExpr;
    TExprNode::TPtr lastAlias;

    auto setItem = setItems->Tail().ChildPtr(0);

    auto from = GetSetting(setItem->Tail(), "from");
    THashMap<TString, TExprNode::TPtr> aliasToInputMap;

    if (from) {
        for (auto fromItem : from->Child(1)->Children()) {
            auto readExpr = TKqlReadTableRanges(fromItem->Child(0));
            auto alias = fromItem->Child(1);

            auto opRead = Build<TKqpOpRead>(ctx, node->Pos())
                .Table(readExpr.Table())
                .Alias(alias)
                .Columns(readExpr.Columns())
                .Done().Ptr();
            aliasToInputMap.insert({TString(alias->Content()), opRead});
            /*

            if (!joinExpr) {
                joinExpr = opRead;
            } 
            else {
                auto joinKeys = Build<TDqJoinKeyTupleList>(ctx, node->Pos()).Done();

                joinExpr = Build<TKqpOpJoin>(ctx, node->Pos())
                    .LeftInput(joinExpr)
                    .RightInput(opRead)
                    .JoinKind().Value("Cross").Build()
                    .JoinKeys(joinKeys)
                    .Done().Ptr();
            }
                    */
            lastAlias = alias;
        }
    }

    auto joinOps = GetSetting(setItem->Tail(), "join_ops");
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

            auto pgResolvedOps = FindNodes(join->Child(1)->Child(1)->TailPtr(), [] (const TExprNode::TPtr &node) {
                if (node->IsCallable("PgResolvedOp")) {
                    return true;
                } else {
                    return false;
                }
            });

            TVector<TInfoUnit> joinKeys;
            for (const auto &pgResolvedOp : pgResolvedOps) {
                TVector<TInfoUnit> keys;
                GetAllMembers(pgResolvedOp, keys);
                joinKeys.insert(joinKeys.end(), keys.begin(), keys.end());
            }

            TSet<TString> processedTables;
            if (tableInputsCount == 2) {
                if (joinKeys.empty()) {
                    // Cross join
                } else {
                    TJoinKeyBuilder joinKeyBuilder(joinKeys);
                    auto leftLabel = joinKeyBuilder.GetLeftInputAlias();
                    auto rightLabel = joinKeyBuilder.GetRightInputAlias();
                    Y_ENSURE(aliasToInputMap.count(leftLabel));
                    Y_ENSURE(aliasToInputMap.count(rightLabel));
                    auto leftInputTable = aliasToInputMap[leftLabel];
                    auto rightInputTable = aliasToInputMap[rightLabel];
                    joinExpr = Build<TKqpOpJoin>(ctx, node->Pos())
                                   .LeftInput(leftInputTable)
                                   .RightInput(rightInputTable)
                                   .JoinKind().Value(joinType).Build()
                                   .JoinKeys(joinKeyBuilder.BuildJoinKeysTwoTableInputs(ctx, node->Pos()))
                                   .Done().Ptr();
                }
            } else if (tableInputsCount == 1) {
                if (joinKeys.empty()) {
                    // Cross join
                } else {
                    TVector<TDqJoinKeyTuple> keys;
                    for (ui32 i = 0; i < joinKeys.size(); i += 2) {
                        keys.push_back(Build<TDqJoinKeyTuple>(ctx, node->Pos())
                                           .LeftLabel().Value(joinKeys[i].Alias).Build()
                                           .LeftColumn().Value(joinKeys[i].ColumnName).Build()
                                           .RightLabel().Value(joinKeys[i + 1].Alias).Build()
                                           .RightColumn().Value(joinKeys[i + 1].ColumnName)
                                           .Build()
                                           .Done());
                    }

                    auto dqJoinKeys = Build<TDqJoinKeyTupleList>(ctx, node->Pos()).Add(keys).Done();
                    auto leftLabel = joinKeys[0].Alias;
                    auto rightLabel = joinKeys[1].Alias;
                    Cerr << "RIGHT LABEL " << rightLabel << Endl;
                    Y_ENSURE(aliasToInputMap.contains(rightLabel), "Label not contains ");
                    auto rightInputTable = aliasToInputMap[rightLabel];
                    joinExpr = Build<TKqpOpJoin>(ctx, node->Pos())
                                   .LeftInput(joinExpr)
                                   .RightInput(rightInputTable)
                                   .JoinKind()
                                   .Value(joinType)
                                   .Build()
                                   .JoinKeys(dqJoinKeys)
                                   .Done()
                                   .Ptr();
                }
            }
            tableInputsCount = 0;
        }
    }

    filterExpr = joinExpr;

    auto where = GetSetting(setItem->Tail(), "where");

    if (where) {
        auto lambda = where->Child(1)->Child(1);
        filterExpr = Build<TKqpOpFilter>(ctx, node->Pos())
            .Input(filterExpr)
            .Lambda(lambda)
            .Done().Ptr();
    }

    if (!filterExpr) {
        filterExpr = Build<TKqpOpEmptySource>(ctx, node->Pos()).Done().Ptr();
    }

    auto result = GetSetting(setItem->Tail(), "result");
    auto finalType = node->GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();

    TExprNode::TPtr resultExpr = filterExpr;

    for (auto resultItem : result->Child(1)->Children()) {
        auto column = resultItem->Child(0);
        auto columnName = column->Content();
        auto variable = Build<TCoAtom>(ctx, node->Pos()).Value(columnName).Done();

        const auto expectedTypeNode = finalType->FindItemType(columnName);
        Y_ENSURE(expectedTypeNode);
        const auto expectedType = expectedTypeNode->Cast<TPgExprType>();
        const auto actualTypeNode = resultItem->GetTypeAnn();

        YQL_CLOG(TRACE, CoreDq) << "Actual type for column: " << columnName << " is: " << *actualTypeNode;

        ui32 actualPgTypeId;
        bool convertToPg;
        Y_ENSURE(ExtractPgType(actualTypeNode, actualPgTypeId, convertToPg, node->Pos(), ctx));

        auto needPgCast = (expectedType->GetId() != actualPgTypeId);
        auto lambda = TCoLambda(ctx.DeepCopyLambda(*(resultItem->Child(2))));

        if (convertToPg) {
            Y_ENSURE(!needPgCast, TStringBuilder()
                 << "Conversion to PG type is different at typization (" << expectedType->GetId()
                 << ") and optimization (" << actualPgTypeId << ") stages.");

            auto toPg = ctx.NewCallable(node->Pos(), "ToPg", {lambda.Body().Ptr()});

            lambda = Build<TCoLambda>(ctx, node->Pos())
                .Args(lambda.Args())
                .Body(toPg)
                .Done();
        }
        else if (needPgCast) {
            auto pgType = ctx.NewCallable(node->Pos(), "PgType", {ctx.NewAtom(node->Pos(), NPg::LookupType(expectedType->GetId()).Name)});
            auto pgCast = ctx.NewCallable(node->Pos(), "PgCast", {lambda.Body().Ptr(), pgType});

            lambda = Build<TCoLambda>(ctx, node->Pos())
                .Args(lambda.Args())
                .Body(pgCast)
                .Done();
        }

        resultElements.push_back(Build<TKqpOpMapElement>(ctx, node->Pos())
            .Input(resultExpr)
            .Variable(variable)
            .Lambda(lambda)
            .Done().Ptr());
    }

    return Build<TKqpOpRoot>(ctx, node->Pos())
            .Input<TKqpOpMap>()
                .Input(resultExpr)
                .MapElements()
                    .Add(resultElements)
                .Build()
            .Build()
            .Done().Ptr();
}

TExprNode::TPtr PushTakeIntoPlan(const TExprNode::TPtr& node, TExprContext& ctx, const TTypeAnnotationContext& typeCtx) {
    Y_UNUSED(typeCtx);
    auto take = TCoTake(node);
    if (auto root = take.Input().Maybe<TKqpOpRoot>()){
        return Build<TKqpOpRoot>(ctx, node->Pos())
            .Input<TKqpOpLimit>()
                .Input(root.Cast().Input())
                .Count(take.Count())
            .Build()
            .Done().Ptr();
    }
    else {
        return node;
    }
}
}

namespace NKikimr {
namespace NKqp {

IGraphTransformer::TStatus TKqpPgRewriteTransformer::DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) {
    output = input;
    TOptimizeExprSettings settings(&TypeCtx);

    auto status = OptimizeExpr(output, output, [this] (const TExprNode::TPtr& node, TExprContext& ctx) -> TExprNode::TPtr {
        if (TCoPgSelect::Match(node.Get())) {
            return RewritePgSelect(node, ctx, TypeCtx);
        } if (TCoTake::Match(node.Get())) {
            return PushTakeIntoPlan(node, ctx, TypeCtx);
        }
        else {
            return node;
        }}, ctx, settings);

    return status;
}

void TKqpPgRewriteTransformer::Rewind() {
}


IGraphTransformer::TStatus TKqpNewRBOTransformer::DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) {
    output = input;
    TOptimizeExprSettings settings(&TypeCtx);

    auto status = OptimizeExpr(output, output, [this] (const TExprNode::TPtr& node, TExprContext& ctx) -> TExprNode::TPtr {
        if (TKqpOpRoot::Match(node.Get())) {
            auto root = TOpRoot(node);
            return RBO.Optimize(root, ctx);
        } else {
            return node;
        }}, ctx, settings);

    if (status != IGraphTransformer::TStatus::Ok) {
        return status;
    }

    return IGraphTransformer::TStatus::Ok;
}

void TKqpNewRBOTransformer::Rewind() {
}

TAutoPtr<IGraphTransformer> CreateKqpPgRewriteTransformer(const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx, TTypeAnnotationContext& typeCtx) {
    return new TKqpPgRewriteTransformer(kqpCtx, typeCtx);
}

TAutoPtr<IGraphTransformer> CreateKqpNewRBOTransformer(const TIntrusivePtr<TKqpOptimizeContext>& kqpCtx, TTypeAnnotationContext& typeCtx, const TKikimrConfiguration::TPtr& config) {
    return new TKqpNewRBOTransformer(kqpCtx, typeCtx, config);
}

}
}