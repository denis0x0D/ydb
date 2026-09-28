#include "kqp_rbo_lookup_join.h"
#include "kqp_rbo_utils.h"

namespace NKikimr::NKqp {

using namespace NYql;
using namespace NYql::NNodes;

namespace {

bool IsValidIndex(const TIndexDescription& index) {
    return index.Type != TIndexDescription::EType::GlobalAsync
        && index.Type != TIndexDescription::EType::GlobalJson
        && index.Type != TIndexDescription::EType::GlobalJsonCompact
        && index.Type != TIndexDescription::EType::LocalMinMax
        && index.Type != TIndexDescription::EType::LocalBloomFilter
        && index.Type != TIndexDescription::EType::LocalBloomNgramFilter
        && index.State == TIndexDescription::EIndexState::Ready;
}

bool IsCoveringIndex(const TVector<TString>& readColumns, const TVector<TString>& keyColumns, const TVector<TString>& dataColumns) {
    THashSet<TString> indexColumnSet(keyColumns.begin(), keyColumns.end());
    indexColumnSet.insert(dataColumns.begin(), dataColumns.end());
    for (const auto& column : readColumns) {
        if (!indexColumnSet.contains(column)) {
            return false;
        }
    }
    return true;
}

TIntrusivePtr<TKikimrTableMetadata> TryToFindBestIndexForRightSide(const TKikimrTableDescription& mainTableDesc, const TVector<TString>& readColumns,
                                                                   const THashSet<TString>& rightJoinKeys) {
    const auto& meta = *mainTableDesc.Metadata;
    std::optional<TString> bestIndexName;
    ui32 bestPrefix = 0;

    for (const auto& index : meta.Indexes) {
        if (!IsValidIndex(index) || !IsCoveringIndex(readColumns, index.KeyColumns, index.DataColumns)) {
            continue;
        }

        ui32 currentPrefix = 0;
        for (const auto& keyCol : index.KeyColumns) {
            if (!rightJoinKeys.contains(keyCol)) {
                break;
            }
            ++currentPrefix;
        }

        // Better prefix wins and ties broken alphabetically by index name.
        if (currentPrefix > bestPrefix || (currentPrefix == bestPrefix && currentPrefix > 0 && index.Name < *bestIndexName)) {
            bestPrefix = currentPrefix;
            bestIndexName = index.Name;
        }
    }

    if (bestIndexName.has_value()) {
        return meta.GetIndexMetadata(*bestIndexName).first;
    }

    return nullptr;
}

bool IsLookupTable(const TKikimrTableMetadata& meta) {
    // Can't lookup in system views: a read of one is not a datashard read even though it is
    // described as a row storage read.
    return meta.SysView.empty() && meta.Kind != EKikimrTableKind::SysView && !meta.KeyColumnNames.empty();
}

bool IsUsablePointPrefix(const TOpRead::TPointPrefix& prefix, const TVector<TString>& keyColumnNames, bool innerJoin, size_t pointsLimit) {
    if (!prefix.Points || !prefix.PointsItemType || prefix.Columns.empty()) {
        return false;
    }

    if (prefix.Columns.size() >= keyColumnNames.size()) {
        return false;
    }

    for (size_t i = 0; i < prefix.Columns.size(); ++i) {
        if (prefix.Columns[i] != keyColumnNames[i]) {
            return false;
        }
    }

    // For left, left only, left semi joins we cannot support more than 1 point lookup.
    if (!innerJoin) {
        pointsLimit = std::min<size_t>(pointsLimit, 1);
    }

    return prefix.ExpectedMaxPoints.Defined() && *prefix.ExpectedMaxPoints <= pointsLimit;
}

// Returns how many leading key columns a lookup fixes: the point prefix and the join keys after it.
size_t GetLookupKeyPrefixLen(const TVector<TString>& keyColumnNames, size_t pointPrefixLen, const THashSet<TString>& joinKeyColumns) {
    size_t len = pointPrefixLen;
    while (len < keyColumnNames.size() && joinKeyColumns.contains(keyColumnNames[len])) {
        ++len;
    }
    return len;
}

} // anonymous namespace

std::optional<THashSet<TString>> GetLookupJoinKeyColumns(const TOpRead& read, const TVector<TInfoUnit>& rightJoinKeys) {
    THashMap<TInfoUnit, TString, TInfoUnit::THashFunction> readColumnByIU;
    for (size_t i = 0; i < read.OutputIUs.size(); ++i) {
        readColumnByIU[read.OutputIUs[i]] = read.Columns[i];
    }

    THashSet<TString> columns;
    for (const auto& key : rightJoinKeys) {
        const auto it = readColumnByIU.find(key);
        if (it == readColumnByIU.end()) {
            return std::nullopt;
        }
        columns.insert(it->second);
    }
    return columns;
}

std::optional<TExpression> BuildFetchedRowFilter(const TOpRead& read, const TIntrusivePtr<TOpFilter>& filter, bool& supported) {
    TVector<TExpression> conjuncts;
    if (read.RangeInfo.has_value()) {
        if (!read.OriginalPredicate.has_value()) {
            supported = false;
            return std::nullopt;
        }
        const auto original = read.OriginalPredicate->SplitConjunct();
        conjuncts.insert(conjuncts.end(), original.begin(), original.end());
    }
    if (filter) {
        const auto filters = filter->GetFilterExpression().SplitConjunct();
        conjuncts.insert(conjuncts.end(), filters.begin(), filters.end());
    }

    if (conjuncts.empty()) {
        return std::nullopt;
    }

    // The filter is evaluated on a fetched row, so it can only refer to the fetched columns.
    const auto readOutputs = MakeInfoUnitSet(read.OutputIUs);
    for (const auto& conjunct : conjuncts) {
        for (const auto& iu : conjunct.GetInputIUs(/*includeSubplanVars=*/true, /*includeCorrelatedDeps=*/true)) {
            if (!readOutputs.contains(iu)) {
                supported = false;
                return std::nullopt;
            }
        }
    }

    return MakeConjunction(conjuncts);
}

std::optional<TLookupJoinTarget> ChooseLookupJoinTarget(const TOpRead& read, const THashSet<TString>& joinKeyColumns, bool innerJoin,
                                                        const NOpt::TKqpOptimizeContext& kqpCtx) {
    // Only supports row storage tables.
    if (read.GetTableStorageType() != NYql::EStorageType::RowStorage) {
        return std::nullopt;
    }

    const auto table = TKqpTable(read.GetTable());
    if (table.PathId().Value().empty()) {
        return std::nullopt;
    }

    const auto& tableDesc = kqpCtx.Tables->ExistingTable(kqpCtx.Cluster, table.Path().Value());
    Y_ENSURE(tableDesc.Metadata);
    const bool autoIndexSelection = kqpCtx.Config->IsAutoIndexSelectionForIndexLookupJoinEnabled();
    const bool allPointPrefixes = kqpCtx.Config->GetEnableNewRBOLookupJoinPointPrefixes();

    // Without a pushed predicate the read can be redirected to a covering index whose key starts with join keys.
    if (autoIndexSelection && !read.RangeInfo.has_value()) {
        if (auto index = TryToFindBestIndexForRightSide(tableDesc, read.Columns, joinKeyColumns); index && IsLookupTable(*index)) {
            return TLookupJoinTarget{index, nullptr};
        }
    }

    const size_t pointsLimit = kqpCtx.Config->GetIdxLookupJoinPointsLimit();
    std::optional<TLookupJoinTarget> best;
    size_t bestLen = 0;
    // Candidates are considered in the order of preference, so the first one wins a tie.
    auto consider = [&](const TIntrusivePtr<TKikimrTableMetadata>& meta, const TOpRead::TPointPrefix* prefix) {
        if (!IsLookupTable(*meta)) {
            return;
        }
        if (prefix && !IsUsablePointPrefix(*prefix, meta->KeyColumnNames, innerJoin, pointsLimit)) {
            return;
        }

        const size_t pointPrefixLen = prefix ? prefix->Columns.size() : 0;
        const size_t len = GetLookupKeyPrefixLen(meta->KeyColumnNames, pointPrefixLen, joinKeyColumns);
        // At least one join key is needed to lookup by.
        if (len == pointPrefixLen || len <= bestLen) {
            return;
        }

        best = TLookupJoinTarget{meta, prefix};
        bestLen = len;
    };

    // The read table is preferred, so the read is not redirected without a reason.
    TVector<const TOpRead::TPointPrefix*> otherPrefixes;
    if (read.RangeInfo.has_value()) {
        for (const auto& prefix : read.RangeInfo->PointPrefixes) {
            if (prefix.Table == table.Path().Value()) {
                consider(tableDesc.Metadata, &prefix);
            } else if (autoIndexSelection && allPointPrefixes) {
                otherPrefixes.push_back(&prefix);
            }
        }
    }
    consider(tableDesc.Metadata, nullptr);

    // Ties are broken deterministically, independently of the order indexes are declared.
    std::sort(otherPrefixes.begin(), otherPrefixes.end(), [](const auto* lhs, const auto* rhs) { return lhs->Table < rhs->Table; });
    for (const auto* prefix : otherPrefixes) {
        const auto& otherTableDesc = kqpCtx.Tables->ExistingTable(kqpCtx.Cluster, prefix->Table);
        Y_ENSURE(otherTableDesc.Metadata);
        consider(otherTableDesc.Metadata, prefix);
    }

    return best;
}

TExprNode::TPtr BuildTableCallable(const TKikimrTableMetadata& meta, TPositionHandle pos, TExprContext& ctx) {
    // clang-format off
    return Build<TKqpTable>(ctx, pos)
        .Path().Build(meta.Name)
        .PathId().Build(meta.PathId.ToString())
        .SysView().Build(meta.SysView)
        .Version().Build(meta.SchemaVersion)
    .Done().Ptr();
    // clang-format on
}

} // namespace NKikimr::NKqp
