#include <ydb/core/kqp/common/kqp_stage_cache.h>
#include <ydb/core/kqp/opt/rbo/physical_conversion/kqp_rbo_expr_serializer.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

#include <yql/essentials/ast/yql_expr.h>
#include <yql/essentials/ast/yql_type_string.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NKqp {
namespace {

using namespace NYdb;
using namespace NYql;

// A wide lambda with a nested lambda, a node shared by both bodies and a binary atom.
TExprNode::TPtr BuildGraph(TExprContext& ctx, TStringBuf firstArgName) {
    const TPositionHandle pos;
    const auto row = ctx.NewArgument(pos, firstArgName);
    const auto list = ctx.NewArgument(pos, "list");
    const auto item = ctx.NewArgument(pos, "item");
    const auto shared = ctx.NewCallable(pos, "Member", {row, ctx.NewAtom(pos, "a")});
    const auto inner = ctx.NewLambda(pos, ctx.NewArguments(pos, {item}), ctx.NewCallable(pos, "+", {item, shared}));
    const auto binary = ctx.NewAtom(pos, TStringBuf("\0\1\2", 3), TNodeFlags::BinaryContent);
    return ctx.NewLambda(pos, {
        ctx.NewArguments(pos, {row, list}),
        ctx.NewCallable(pos, "Map", {list, inner}),
        ctx.NewList(pos, {shared, shared, binary, ctx.NewWorld(pos)}),
    });
}

TVector<const TTypeAnnotationNode*> MakeTypes(TExprContext& ctx) {
    const auto* int32 = ctx.MakeType<TDataExprType>(EDataSlot::Int32);
    const auto* string = ctx.MakeType<TDataExprType>(EDataSlot::String);
    const auto* decimal = ctx.MakeType<TDataExprParamsType>(EDataSlot::Decimal, "22", "9");
    const auto* structType = ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{
        ctx.MakeType<TItemExprType>("a", int32), ctx.MakeType<TItemExprType>("b", ctx.MakeType<TOptionalExprType>(string))});
    const auto* tuple = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{int32, decimal});
    const auto* multi = ctx.MakeType<TMultiExprType>(TTypeAnnotationNode::TListType{
        ctx.MakeType<TBlockExprType>(int32), ctx.MakeType<TScalarExprType>(ctx.MakeType<TDataExprType>(EDataSlot::Uint64))});
    const auto* callable = ctx.MakeType<TCallableExprType>(int32, TVector<TCallableExprType::TArgumentInfo>{
        {int32, "x", 0}, {ctx.MakeType<TOptionalExprType>(int32), "", 0}}, 1, "payload");
    return {
        int32,
        decimal,
        structType,
        ctx.MakeType<TListExprType>(structType),
        tuple,
        ctx.MakeType<TStreamExprType>(multi),
        ctx.MakeType<TFlowExprType>(multi),
        ctx.MakeType<TDictExprType>(string, tuple),
        ctx.MakeType<TVariantExprType>(tuple),
        ctx.MakeType<TTaggedExprType>(int32, "tag"),
        ctx.MakeType<TResourceExprType>("resource"),
        ctx.MakeType<TTypeExprType>(structType),
        ctx.MakeType<TLinearExprType>(int32),
        callable,
        ctx.MakeType<TVoidExprType>(),
        ctx.MakeType<TNullExprType>(),
        ctx.MakeType<TEmptyListExprType>(),
        ctx.MakeType<TEmptyDictExprType>(),
        ctx.MakeType<TGenericExprType>(),
        ctx.MakeType<TUnitExprType>(),
    };
}

// A list of atoms, each typed with one of the types, and the list typed as a tuple of them.
TExprNode::TPtr BuildTypedGraph(TExprContext& ctx, const TVector<const TTypeAnnotationNode*>& types) {
    const TPositionHandle pos;
    TExprNode::TListType atoms;
    for (size_t i = 0; i < types.size(); ++i) {
        auto atom = ctx.NewAtom(pos, ToString(i));
        atom->SetTypeAnn(types[i]);
        atoms.push_back(std::move(atom));
    }
    auto list = ctx.NewList(pos, std::move(atoms));
    list->SetTypeAnn(ctx.MakeType<TTupleExprType>(types));
    return list;
}

NQuery::TExecuteQueryResult RunQuery(NQuery::TQueryClient& client, const TString& pragmas, const TString& query, ui32 run) {
    // A different comment makes a different query text, so the compile cache doesn't skip the optimizer.
    const auto text = TStringBuilder() << pragmas << "-- run " << run << "\n" << query;
    auto result = client.ExecuteQuery(text, NQuery::TTxControl::NoTx()).GetValueSync();
    UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString() << "\n" << text);
    return result;
}

const TVector<TString> Queries = {
    "SELECT a, b FROM `/Root/t1` WHERE b > 10 ORDER BY a;",
    "SELECT b, COUNT(*) AS rows, SUM(a) AS sum_a FROM `/Root/t1` GROUP BY b ORDER BY b;",
    "SELECT a FROM `/Root/t1` ORDER BY a LIMIT 2 OFFSET 1;",
    "SELECT a FROM `/Root/t1` ORDER BY a LIMIT 2;",
    "SELECT t1.a AS a, t2.a AS a2 FROM `/Root/t1` AS t1 JOIN `/Root/t2` AS t2 ON t1.b = t2.b ORDER BY a, a2;",
    "SELECT t1.a AS a, t2.b AS b2 FROM `/Root/t1` AS t1 JOIN `/Root/t2` AS t2 ON t1.a = t2.a ORDER BY a;",
};

NKikimrConfig::TAppConfig MakeAppConfig() {
    NKikimrConfig::TAppConfig appConfig;
    appConfig.MutableTableServiceConfig()->SetEnableNewRBO(true);
    appConfig.MutableTableServiceConfig()->SetEnableFallbackToYqlOptimizer(false);
    return appConfig;
}

void CreateTables(TKikimrRunner& kikimr) {
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();
    const auto schemeResult = session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/t1` (a Int64 NOT NULL, b Int64, c String, PRIMARY KEY (a));
        CREATE TABLE `/Root/t2` (a Int64 NOT NULL, b Int64, PRIMARY KEY (a));
    )").GetValueSync();
    UNIT_ASSERT_C(schemeResult.IsSuccess(), schemeResult.GetIssues().ToString());

    TValueBuilder t1;
    t1.BeginList();
    const TVector<std::tuple<i64, std::optional<i64>, std::string>> t1Rows = {
        {1, 10, "x"}, {2, 20, "y"}, {3, 10, "z"}, {4, std::nullopt, "x"}, {5, 30, "y"}};
    for (const auto& [a, b, c] : t1Rows) {
        t1.AddListItem().BeginStruct()
            .AddMember("a").Int64(a)
            .AddMember("b").OptionalInt64(b)
            .AddMember("c").OptionalString(c)
        .EndStruct();
    }
    t1.EndList();
    auto upsertResult = db.BulkUpsert("/Root/t1", t1.Build()).GetValueSync();
    UNIT_ASSERT_C(upsertResult.IsSuccess(), upsertResult.GetIssues().ToString());

    TValueBuilder t2;
    t2.BeginList();
    for (const auto& [a, b] : TVector<std::pair<i64, i64>>{{1, 10}, {2, 20}, {3, 40}}) {
        t2.AddListItem().BeginStruct().AddMember("a").Int64(a).AddMember("b").OptionalInt64(b).EndStruct();
    }
    t2.EndList();
    upsertResult = db.BulkUpsert("/Root/t2", t2.Build()).GetValueSync();
    UNIT_ASSERT_C(upsertResult.IsSuccess(), upsertResult.GetIssues().ToString());
}

} // anonymous namespace

Y_UNIT_TEST_SUITE(KqpRboStageCache) {
    Y_UNIT_TEST(SerializeGraph) {
        TExprContext ctx;
        const auto graph = BuildGraph(ctx, "row");
        const auto bytes = SerializeExprGraph(*graph, ESerializedTypes::None);
        UNIT_ASSERT(bytes);

        // Argument names are not stored, so equal graphs give equal bytes.
        TExprContext otherCtx;
        UNIT_ASSERT_VALUES_EQUAL(*SerializeExprGraph(*BuildGraph(otherCtx, "renamed"), ESerializedTypes::None), *bytes);

        TExprContext loadCtx;
        const auto loaded = DeserializeExprGraph(*bytes, loadCtx, TPositionHandle());
        UNIT_ASSERT(loaded);
        UNIT_ASSERT_VALUES_EQUAL(*SerializeExprGraph(*loaded, ESerializedTypes::None), *bytes);

        UNIT_ASSERT(loaded->IsLambda());
        UNIT_ASSERT_VALUES_EQUAL(loaded->ChildrenSize(), 3);
        const auto& list = *loaded->Child(2);
        UNIT_ASSERT_EQUAL(list.Child(0), list.Child(1));
        UNIT_ASSERT_VALUES_EQUAL(list.Child(2)->Content(), TStringBuf("\0\1\2", 3));
        UNIT_ASSERT_VALUES_EQUAL(list.Child(2)->Flags(), static_cast<ui32>(TNodeFlags::BinaryContent));
        UNIT_ASSERT(list.Child(3)->IsWorld());
        // The member access in the nested lambda refers to the argument of the outer lambda.
        const auto& map = *loaded->Child(1);
        UNIT_ASSERT_EQUAL(map.Child(1)->Child(1)->Child(1), list.Child(0));
        UNIT_ASSERT_EQUAL(list.Child(0)->Child(0), loaded->Head().Child(0));
    }

    Y_UNIT_TEST(SerializeTypes) {
        TExprContext ctx;
        const auto types = MakeTypes(ctx);
        const auto graph = BuildTypedGraph(ctx, types);
        const auto bytes = SerializeExprGraph(*graph, ESerializedTypes::Required);
        UNIT_ASSERT(bytes);

        // Types are interned, so loading into the same context gives the same type objects.
        const auto sameCtxLoaded = DeserializeExprGraph(*bytes, ctx, TPositionHandle());
        UNIT_ASSERT(sameCtxLoaded);
        UNIT_ASSERT_EQUAL(sameCtxLoaded->GetTypeAnn(), graph->GetTypeAnn());
        for (size_t i = 0; i < types.size(); ++i) {
            UNIT_ASSERT_EQUAL(sameCtxLoaded->Child(i)->GetTypeAnn(), types[i]);
        }

        TExprContext loadCtx;
        const auto loaded = DeserializeExprGraph(*bytes, loadCtx, TPositionHandle());
        UNIT_ASSERT(loaded);
        UNIT_ASSERT_VALUES_EQUAL(FormatType(loaded->GetTypeAnn()), FormatType(graph->GetTypeAnn()));
        for (size_t i = 0; i < types.size(); ++i) {
            UNIT_ASSERT_VALUES_EQUAL(FormatType(loaded->Child(i)->GetTypeAnn()), FormatType(types[i]));
            UNIT_ASSERT_EQUAL(loaded->Child(i)->GetState(), TExprNode::EState::TypeComplete);
        }
        UNIT_ASSERT_VALUES_EQUAL(*SerializeExprGraph(*loaded, ESerializedTypes::Required), *bytes);
    }

    Y_UNIT_TEST(SerializeRefusesWhatItCannotRestore) {
        TExprContext ctx;
        const auto untyped = ctx.NewList(TPositionHandle(), {ctx.NewAtom(TPositionHandle(), "a")});
        untyped->SetTypeAnn(ctx.MakeType<TUnitExprType>());
        UNIT_ASSERT(!SerializeExprGraph(*untyped, ESerializedTypes::Required));
        UNIT_ASSERT(SerializeExprGraph(*untyped, ESerializedTypes::None));

        auto error = ctx.NewAtom(TPositionHandle(), "a");
        error->SetTypeAnn(ctx.MakeType<TErrorExprType>(TIssue("error")));
        UNIT_ASSERT(!SerializeExprGraph(*error, ESerializedTypes::Required));
    }

    // The key of a stage program has untyped nodes, made by block propagation right before peephole.
    Y_UNIT_TEST(SerializeTypesIfPresent) {
        TExprContext ctx;
        const auto* int32 = ctx.MakeType<TDataExprType>(EDataSlot::Int32);
        auto typed = ctx.NewAtom(TPositionHandle(), "typed");
        typed->SetTypeAnn(int32);
        const auto graph = ctx.NewList(TPositionHandle(), {typed, ctx.NewAtom(TPositionHandle(), "untyped")});
        UNIT_ASSERT(!SerializeExprGraph(*graph, ESerializedTypes::Required));
        const auto bytes = SerializeExprGraph(*graph, ESerializedTypes::IfPresent);
        UNIT_ASSERT(bytes);

        // The types are a part of the bytes: the same graph with another type gives other bytes.
        TExprContext otherCtx;
        auto otherTyped = otherCtx.NewAtom(TPositionHandle(), "typed");
        otherTyped->SetTypeAnn(otherCtx.MakeType<TDataExprType>(EDataSlot::Int64));
        const auto otherGraph = otherCtx.NewList(TPositionHandle(), {otherTyped, otherCtx.NewAtom(TPositionHandle(), "untyped")});
        UNIT_ASSERT_VALUES_UNEQUAL(*SerializeExprGraph(*otherGraph, ESerializedTypes::IfPresent), *bytes);
        UNIT_ASSERT_VALUES_EQUAL(*SerializeExprGraph(*otherGraph, ESerializedTypes::None), *SerializeExprGraph(*graph, ESerializedTypes::None));

        const auto loaded = DeserializeExprGraph(*bytes, ctx, TPositionHandle());
        UNIT_ASSERT(loaded);
        UNIT_ASSERT(!loaded->GetTypeAnn());
        UNIT_ASSERT_EQUAL(loaded->Child(0)->GetTypeAnn(), int32);
        UNIT_ASSERT(!loaded->Child(1)->GetTypeAnn());
    }

    Y_UNIT_TEST(DeserializeRejectsBadData) {
        TExprContext ctx;
        const auto bytes = *SerializeExprGraph(*BuildTypedGraph(ctx, MakeTypes(ctx)), ESerializedTypes::Required);
        for (size_t size = 0; size < bytes.size(); ++size) {
            UNIT_ASSERT_C(!DeserializeExprGraph(TStringBuf(bytes).Head(size), ctx, TPositionHandle()), size);
        }
        UNIT_ASSERT(!DeserializeExprGraph(bytes + "x", ctx, TPositionHandle()));

        auto otherVersion = bytes;
        otherVersion[4] = 2;
        UNIT_ASSERT(!DeserializeExprGraph(otherVersion, ctx, TPositionHandle()));
    }

    Y_UNIT_TEST(DisabledByDefault) {
        auto cache = GetTestStageCache();
        cache->Clear();
        TKikimrRunner kikimr(TKikimrSettings(MakeAppConfig()).SetWithSampleTables(false));
        CreateTables(kikimr);
        auto client = kikimr.GetQueryClient();
        for (const auto& query : Queries) {
            RunQuery(client, "", query, 1);
        }
        const auto stats = cache->GetStats();
        UNIT_ASSERT_VALUES_EQUAL(stats.Inserts, 0);
        UNIT_ASSERT_VALUES_EQUAL(stats.Hits + stats.Misses, 0);
    }

    Y_UNIT_TEST_TWIN(ReuseStages, Verify) {
        auto cache = GetTestStageCache();
        cache->Clear();
        TKikimrRunner kikimr(TKikimrSettings(MakeAppConfig()).SetWithSampleTables(false));
        CreateTables(kikimr);
        auto client = kikimr.GetQueryClient();

        TString pragmas = "PRAGMA ydb.EnableNewRBOStageCache = \"true\";\n";
        if (Verify) {
            pragmas += "PRAGMA ydb.NewRBOStageCacheVerify = \"true\";\n";
        }

        TVector<TString> expected;
        for (const auto& query : Queries) {
            expected.push_back(FormatResultSetYson(RunQuery(client, "", query, 0).GetResultSet(0)));
        }

        for (size_t i = 0; i < Queries.size(); ++i) {
            const auto result = RunQuery(client, pragmas, Queries[i], 1);
            UNIT_ASSERT_VALUES_EQUAL_C(FormatResultSetYson(result.GetResultSet(0)), expected[i], Queries[i]);
        }
        const auto afterFirstRun = cache->GetStats();
        UNIT_ASSERT_GT(afterFirstRun.Inserts, 0);

        // The same plans again: every stage that was stored is now taken from the cache.
        for (size_t i = 0; i < Queries.size(); ++i) {
            const auto result = RunQuery(client, pragmas, Queries[i], 2);
            UNIT_ASSERT_VALUES_EQUAL_C(FormatResultSetYson(result.GetResultSet(0)), expected[i], Queries[i]);
        }
        const auto afterSecondRun = cache->GetStats();
        UNIT_ASSERT_VALUES_EQUAL(afterSecondRun.Inserts, afterFirstRun.Inserts);
        UNIT_ASSERT_GE(afterSecondRun.Hits - afterFirstRun.Hits, afterFirstRun.Inserts);
        Cerr << "Stage cache: " << afterSecondRun.Inserts << " stored, " << afterSecondRun.Hits << " hits, "
             << afterSecondRun.Misses << " misses, " << afterSecondRun.Bytes << " bytes" << Endl;
    }
}

} // namespace NKikimr::NKqp
