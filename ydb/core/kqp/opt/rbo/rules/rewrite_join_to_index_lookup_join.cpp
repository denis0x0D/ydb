#include <ydb/core/kqp/opt/rbo/kqp_rbo_lookup_join.h>
#include <ydb/core/kqp/opt/rbo/rules/kqp_rules_include.h>

namespace NKikimr {
namespace NKqp {

namespace {

using namespace NYql;
using namespace NYql::NNodes;

const TTypeAnnotationNode* StripOptional(const TTypeAnnotationNode* type) {
    return type && type->GetKind() == ETypeAnnotationKind::Optional ? type->Cast<TOptionalExprType>()->GetItemType() : type;
}

struct TLookupKey {
    TInfoUnit LeftIU;
    TInfoUnit RightIU;
    TString Column;
};

struct TKeyMatch {
    // Represents a constant prefix keys.
    TVector<TLookupKey> PrefixKeys;
    // Represents a lookup keys.
    TVector<TLookupKey> LookupKeys;
    // Represents a join keys which are not present in the right side index.
    TVector<TLookupKey> ResidualKeys;
};

std::optional<TKeyMatch> MatchKeyPrefix(const TOpJoin& join, const TOpRead& read, const TVector<TString>& keyColumnNames,
                                        size_t pointPrefixLen) {
    Y_ENSURE(pointPrefixLen < keyColumnNames.size());

    THashMap<TInfoUnit, TString, TInfoUnit::THashFunction> readColumnByIU;
    for (size_t i = 0; i < read.OutputIUs.size(); ++i) {
        readColumnByIU[read.OutputIUs[i]] = read.Columns[i];
    }

    THashMap<TString, TLookupKey> keyByColumn;
    for (const auto& joinKey : join.JoinKeys) {
        const auto& leftIU = joinKey.Left;
        const auto& rightIU = joinKey.Right;
        const auto it = readColumnByIU.find(rightIU);
        Y_ENSURE(it != readColumnByIU.end(), "Cannot find a join key in input columns.");
        const auto column = it->second;
        if (!keyByColumn.emplace(column, TLookupKey{leftIU, rightIU, column}).second) {
            return std::nullopt;
        }
    }

    TKeyMatch match;
    THashSet<TString> takenKeys;
    const auto end = keyByColumn.end();
    for (size_t i = 0; i < keyColumnNames.size(); ++i) {
        const auto it = keyByColumn.find(keyColumnNames[i]);
        if (i < pointPrefixLen) {
            if (it != end) {
                const auto key = it->second;
                match.PrefixKeys.push_back(key);
                takenKeys.insert(key.Column);
            }
            continue;
        }

        if (it == end) {
            break;
        }

        const auto key = it->second;
        match.LookupKeys.push_back(key);
        takenKeys.insert(key.Column);
    }

    for (const auto& [column, key] : keyByColumn) {
        if (!takenKeys.contains(column)) {
            match.ResidualKeys.push_back(key);
        }
    }

    if (match.LookupKeys.empty()) {
        return std::nullopt;
    }

    return match;
}

bool KeyTypesMatch(IOperator& leftInput, IOperator& rightInput, const TVector<TLookupKey>& keys) {
    for (const auto& key : keys) {
        const auto* leftType = StripOptional(leftInput.GetIUType(key.LeftIU));
        const auto* rightType = StripOptional(rightInput.GetIUType(key.RightIU));
        // TODO: Add support key with different types.
        if (!leftType || !rightType || leftType != rightType) {
            return false;
        }
    }
    return true;
}

bool KeyTypesMatch(IOperator& leftInput, IOperator& rightInput, const TKeyMatch& keys) {
    return KeyTypesMatch(leftInput, rightInput, keys.LookupKeys) && KeyTypesMatch(leftInput, rightInput, keys.PrefixKeys)
        && KeyTypesMatch(leftInput, rightInput, keys.ResidualKeys);
}

} // anonymous namespace

bool TRewriteJoinToIndexLookupJoinRule::QuickMatch(const TIntrusivePtr<IOperator>& input) const {
    return input->Kind == EOperator::Join;
}

TIntrusivePtr<IOperator> TRewriteJoinToIndexLookupJoinRule::SimpleMatchAndApply(const TIntrusivePtr<IOperator>& input, TRBOContext& ctx,
                                                                               TPlanProps& props) {
    Y_UNUSED(props);

    if (!ctx.KqpCtx.Config->GetEnableKqpDataQueryStreamIdxLookupJoin()) {
        return input;
    }
    if (!ctx.KqpCtx.IsDataQuery() && !ctx.KqpCtx.IsGenericQuery()) {
        return input;
    }

    auto join = CastOperator<TOpJoin>(input);

    if (join->Props.JoinAlgo.has_value() && *join->Props.JoinAlgo != EJoinAlgoType::LookupJoin){
        return input;
    }

    const auto joinKind = GetValidJoinKind(join->JoinKind);
    if (joinKind != "Inner" && joinKind != "Left" && joinKind != "LeftSemi" && joinKind != "LeftOnly") {
        return input;
    }

    if (HasEqualNullsKey(join->JoinKeys)) {
        return input;
    }

    // Not supported for join with join filters.
    if (join->JoinKeys.empty() || !join->JoinFilters.empty()) {
        return input;
    }

    // We transform left side into special form: tuple(left row, key to lookup).
    if (!join->GetLeftInput()->IsSingleConsumer()) {
        return input;
    }

    // We want to find Read or Read -> Filter for the right side.
    auto rightInput = join->GetRightInput();
    TIntrusivePtr<TOpFilter> rightFilter;
    if (rightInput->Kind == EOperator::Filter) {
        if (!rightInput->IsSingleConsumer()) {
            return input;
        }
        rightFilter = CastOperator<TOpFilter>(rightInput);
        rightInput = rightFilter->GetInput();
    }

    if (rightInput->Kind != EOperator::Source || !rightInput->IsSingleConsumer()) {
        return input;
    }

    auto read = CastOperator<TOpRead>(rightInput);

    TVector<TInfoUnit> rightJoinKeys;
    rightJoinKeys.reserve(join->JoinKeys.size());
    for (const auto& joinKey : join->JoinKeys) {
        rightJoinKeys.push_back(joinKey.Right);
    }

    const auto joinKeyColumns = GetLookupJoinKeyColumns(*read, rightJoinKeys);
    if (!joinKeyColumns) {
        return input;
    }

    bool filterSupported = true;
    const auto fetchedRowFilter = BuildFetchedRowFilter(*read, rightFilter, filterSupported);
    if (!filterSupported) {
        return input;
    }

    const auto target = ChooseLookupJoinTarget(*read, *joinKeyColumns, joinKind == "Inner", ctx.KqpCtx);
    if (!target) {
        return input;
    }

    const size_t pointPrefixLen = target->PointPrefix ? target->PointPrefix->Columns.size() : 0;
    const auto keys = MatchKeyPrefix(*join, *read, target->Metadata->KeyColumnNames, pointPrefixLen);
    if (!keys) {
        return input;
    }

    // Different types for keys are not supported.
    if (!KeyTypesMatch(*join->GetLeftInput(), *read, *keys)) {
        // This check is missing in CBO, so we need to change join implementation in this case
        join->Props.JoinAlgo = EJoinAlgoType::MapJoin;
        return input;
    }

    TVector<TInfoUnit> lookupKeys;
    TVector<TString> lookupKeyColumns;
    lookupKeys.reserve(keys->LookupKeys.size());
    lookupKeyColumns.reserve(keys->LookupKeys.size());
    for (const auto& key : keys->LookupKeys) {
        lookupKeys.push_back(key.LeftIU);
        lookupKeyColumns.push_back(key.Column);
    }

    std::optional<TOpTableLookup::TLookupKeyPrefix> prefix;
    if (target->PointPrefix) {
        TOpTableLookup::TLookupKeyPrefix keyPrefix;
        keyPrefix.Points = target->PointPrefix->Points;
        keyPrefix.PointsItemType = target->PointPrefix->PointsItemType;
        keyPrefix.Columns = target->PointPrefix->Columns;
        for (const auto& key : keys->PrefixKeys) {
            keyPrefix.Equalities.emplace_back(key.Column, key.LeftIU);
        }
        prefix = std::move(keyPrefix);
    }

    TVector<TJoinKey> residualJoinKeys;
    residualJoinKeys.reserve(keys->ResidualKeys.size());
    for (const auto& key : keys->ResidualKeys) {
        residualJoinKeys.emplace_back(key.LeftIU, key.RightIU);
    }

    // The lookup join can probe a covering index or the main table instead of the read table.
    auto lookupTable = read->GetTable();
    if (target->Metadata->Name != TKqpTable(lookupTable).Path().Value()) {
        lookupTable = BuildTableCallable(*target->Metadata, read->Pos, ctx.ExprCtx);
    }

    YQL_CLOG(TRACE, ProviderKqp) << "[NEW RBO] Rewriting a " << joinKind << " join into an index lookup join of "
                                 << target->Metadata->Name;

    auto lookup = MakeIntrusive<TOpTableLookup>(join->GetLeftInput(), join->Pos, lookupTable, read->Columns, read->OutputIUs,
                                                lookupKeys, lookupKeyColumns, joinKind, fetchedRowFilter, prefix, residualJoinKeys);
    return MakeIntrusive<TOpIndexLookupJoin>(lookup, join->Pos, joinKind, join->JoinKeys);
}

} // namespace NKqp
} // namespace NKikimr
