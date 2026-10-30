#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/kqp/ut/rbo/kqp_rbo_yql_workload_helpers.h>

#include <array>
#include <regex>

namespace NKikimr::NKqp {

using namespace NYdb;
using namespace NKikimr::NKqp::NYqlTests;

namespace {

constexpr std::array<ui32, 3> BenchmarkQueryCount{22, 99, 43};

// The new RBO continues every plan from its own pre-CBO snapshot.
const TString RoundTripPragma = "PRAGMA ydb.OptRboSnapshotRoundTrip = \"true\";\n";

struct TExplained {
    TString Plan;
    TString Ast;
};

// Stage ids that differ between any two compilations of the same query.
TString NormalizeStageIds(const TString& text) {
    static const std::regex logicalId(R"('"_logical_id" '[0-9]+)");
    static const std::regex id(R"('"_id" '"[0-9a-f-]+")");
    static const std::regex stageGuid(R"("StageGuid":"[0-9a-f-]+")");
    auto normalized = std::regex_replace(text.c_str(), logicalId, R"('"_logical_id" 'N)");
    normalized = std::regex_replace(normalized, id, R"('"_id" '"GUID")");
    return TString(std::regex_replace(normalized, stageGuid, R"("StageGuid":"GUID")"));
}

std::optional<TExplained> Explain(TKikimrRunner& kikimr, const TString& query, TString& issues) {
    auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
    auto result = session.ExecuteQuery(query, NYdb::NQuery::TTxControl::NoTx(),
        NYdb::NQuery::TExecuteQuerySettings().ExecMode(NYdb::NQuery::EExecMode::Explain)).ExtractValueSync();
    issues = result.GetIssues().ToString();
    if (!result.IsSuccess()) {
        return std::nullopt;
    }
    UNIT_ASSERT_C(result.GetStats() && result.GetStats()->GetPlan() && result.GetStats()->GetAst(), "Explain without a plan");
    return TExplained{NormalizeStageIds(TString(*result.GetStats()->GetPlan())),
                      NormalizeStageIds(TString(*result.GetStats()->GetAst()))};
}

// A query the new RBO compiles must compile to the same physical query through its snapshot.
// Returns false if the query does not compile at all.
bool AssertRoundTrips(TKikimrRunner& kikimr, const TString& name, const TString& query) {
    TString issues;
    const auto explained = Explain(kikimr, query, issues);
    if (!explained) {
        Cerr << "Not compiled, skipped: " << name << ": " << issues << Endl;
        return false;
    }

    const auto restored = Explain(kikimr, RoundTripPragma + query, issues);
    UNIT_ASSERT_C(restored, name << " fails after a snapshot round trip: " << issues);
    UNIT_ASSERT_VALUES_EQUAL_C(restored->Ast, explained->Ast, name);
    UNIT_ASSERT_VALUES_EQUAL_C(restored->Plan, explained->Plan, name);
    return true;
}

NKikimrConfig::TAppConfig MakeAppConfig(bool serviceSpilling = false) {
    NKikimrConfig::TAppConfig appConfig;
    auto& config = *appConfig.MutableTableServiceConfig();
    config.SetEnableNewRBO(true);
    config.SetEnableFallbackToYqlOptimizer(false);
    config.SetEnableInlineJoinFiltersAfterCBO(false);
    config.SetEnableFallbackOnMultipleStatements(false);
    config.SetAllowOlapDataQuery(true);
    config.SetDefaultLangVer(NYql::GetMaxLangVersion());
    config.SetBackportMode(NKikimrConfig::TTableServiceConfig_EBackportMode_All);
    config.SetEnableQueryServiceSpilling(serviceSpilling);
    return appConfig;
}

void AssertBenchmarkRoundTrips(EBenchType type, bool columnStore) {
    TKikimrRunner kikimr(TKikimrSettings(MakeAppConfig()).SetWithSampleTables(false));
    auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
    CreateTablesFromPath(session, BenchmarkSchemaPathPrefix[type], BenchmarkSchemaPath[type], columnStore);

    // Same prelude as the benchmark workload tests.
    const TString prelude = R"(
        $to_decimal = ($x) -> { return cast($x as Decimal(12, 2)); };
        $to_decimal_max_precision = ($x) -> { return cast($x as Decimal(35, 2)); };
        $round = ($x,$y) -> {return $x;};
    )";

    ui32 compiled = 0;
    for (ui32 id = 1; id <= BenchmarkQueryCount[type]; ++id) {
        const auto query = prelude + GetFullPath(BenchmarkQueryPath[type], ToString(id) + ".yql");
        compiled += AssertRoundTrips(kikimr, TStringBuilder() << "Query " << id, query);
    }
    UNIT_ASSERT_C(compiled > 0, "No benchmark query compiled");
}

// Shapes the benchmarks do not cover: effects, subqueries, grouping sets, windows,
// parameters, index lookups and shared subqueries.
void AssertQueriesRoundTrip(bool spilling) {
    NKikimrKqp::TKqpSetting spillingSetting;
    spillingSetting.SetName("_KqpEnableSpilling");
    spillingSetting.SetValue(spilling ? "true" : "false");
    TKikimrRunner kikimr(TKikimrSettings(MakeAppConfig(spilling)).SetKqpSettings({spillingSetting}).SetWithSampleTables(false));

    auto client = kikimr.GetQueryClient();
    auto created = client.ExecuteQuery(R"(
        CREATE TABLE `/Root/t1` (a Int64 NOT NULL, b String, c Int64, PRIMARY KEY (a), INDEX idx_c GLOBAL ON (c));
        CREATE TABLE `/Root/t2` (a Int64 NOT NULL, b String, c Int64, PRIMARY KEY (a));
        CREATE TABLE `/Root/t3` (a Int64 NOT NULL, b String, c Int64, PRIMARY KEY (a)) WITH (STORE = COLUMN);
    )", NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
    UNIT_ASSERT_C(created.IsSuccess(), created.GetIssues().ToString());

    const TVector<std::pair<TString, TString>> queries = {
        {"join order limit", R"(
            SELECT t1.a, t2.b FROM `/Root/t1` AS t1 JOIN `/Root/t2` AS t2 ON t1.a = t2.a
            WHERE t1.c > 10 ORDER BY t1.a LIMIT 5 OFFSET 2;
        )"},
        {"group by having", R"(
            SELECT c, COUNT(*) AS cnt, SUM(a) AS s FROM `/Root/t1` GROUP BY c HAVING COUNT(*) > 1;
        )"},
        {"in and exists", R"(
            SELECT a FROM `/Root/t1` AS t1
            WHERE a IN (SELECT c FROM `/Root/t2`) AND EXISTS (SELECT 1 FROM `/Root/t3` AS t3 WHERE t3.a = t1.a);
        )"},
        {"scalar subquery", R"(
            SELECT a, (SELECT MAX(c) FROM `/Root/t2` AS t2 WHERE t2.a = t1.a) AS m FROM `/Root/t1` AS t1;
        )"},
        {"union all", R"(
            SELECT a, b FROM `/Root/t1` UNION ALL SELECT a, b FROM `/Root/t2`;
        )"},
        {"window", R"(
            SELECT a, SUM(c) OVER (PARTITION BY b ORDER BY a) AS s FROM `/Root/t1`;
        )"},
        {"rollup", R"(
            SELECT b, c, COUNT(*) AS cnt FROM `/Root/t1` GROUP BY ROLLUP(b, c);
        )"},
        {"distinct", R"(
            SELECT DISTINCT b FROM `/Root/t1`;
        )"},
        {"index lookup", R"(
            SELECT a, b FROM `/Root/t1` WHERE c = 5;
        )"},
        {"key range", R"(
            SELECT b FROM `/Root/t1` WHERE a BETWEEN 3 AND 7;
        )"},
        {"parameter", R"(
            DECLARE $p AS Int64;
            SELECT a FROM `/Root/t1` WHERE a = $p;
        )"},
        {"column store aggregate", R"(
            SELECT b, COUNT(DISTINCT c) AS d FROM `/Root/t3` GROUP BY b;
        )"},
        {"column store filter", R"(
            SELECT a FROM `/Root/t3` WHERE b LIKE '%x%' AND c > 3;
        )"},
        {"shared subquery", R"(
            $shared = (SELECT c, COUNT(*) AS n FROM `/Root/t1` GROUP BY c);
            SELECT l.c, l.n + r.n AS n FROM $shared AS l JOIN $shared AS r ON l.c = r.c;
        )"},
        {"upsert select", R"(
            UPSERT INTO `/Root/t2` SELECT a, b, c FROM `/Root/t1` WHERE c > 100;
        )"},
        {"insert values", R"(
            INSERT INTO `/Root/t2` (a, b, c) VALUES (1000, 'z', 1);
        )"},
        {"update", R"(
            UPDATE `/Root/t2` SET b = 'x' WHERE a = 1;
        )"},
        {"delete", R"(
            DELETE FROM `/Root/t2` WHERE a > 5;
        )"},
    };

    ui32 compiled = 0;
    for (const auto& [name, query] : queries) {
        compiled += AssertRoundTrips(kikimr, name, "PRAGMA YqlSelect = 'force';\n" + query);
    }
    UNIT_ASSERT_C(compiled > 0, "No query compiled");
}

} // anonymous namespace

Y_UNIT_TEST_SUITE(KqpRboSnapshot) {
    Y_UNIT_TEST_TWIN(TPCH, ColumnStore) {
        AssertBenchmarkRoundTrips(EBenchType::TPCH, ColumnStore);
    }

    Y_UNIT_TEST_TWIN(TPCDS, ColumnStore) {
        AssertBenchmarkRoundTrips(EBenchType::TPCDS, ColumnStore);
    }

    Y_UNIT_TEST_TWIN(ClickBench, ColumnStore) {
        AssertBenchmarkRoundTrips(EBenchType::CLICKBENCH, ColumnStore);
    }

    Y_UNIT_TEST_TWIN(Queries, Spilling) {
        AssertQueriesRoundTrip(Spilling);
    }
}

} // namespace NKikimr::NKqp
