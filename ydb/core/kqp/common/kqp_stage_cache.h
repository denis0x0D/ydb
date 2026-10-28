#pragma once

#include <util/generic/ptr.h>
#include <util/generic/string.h>
#include <util/generic/strbuf.h>

#include <memory>

namespace NKikimr::NKqp {

// A physical stage program after peephole, serialized independently of any TExprContext.
struct TStageCacheEntry {
    ui32 FormatVersion = 0;
    TString Program;
};
using TStageCacheEntryPtr = std::shared_ptr<const TStageCacheEntry>;

struct TStageCacheStats {
    ui64 Hits = 0;
    ui64 Misses = 0;
    ui64 Inserts = 0;
    ui64 Entries = 0;
    ui64 Bytes = 0;
};

// Physical stage programs after peephole, keyed by the program before peephole, its argument types and the
// settings peephole reads.
// Shared by queries that compile concurrently, so implementations must be thread safe.
class IKqpStageCache : public TThrRefBase {
public:
    using TPtr = TIntrusivePtr<IKqpStageCache>;

    // nullptr on a miss. Entries are immutable and stay valid after eviction while referenced.
    virtual TStageCacheEntryPtr Find(TStringBuf key) = 0;
    // If the key is already present, the existing entry is kept.
    virtual void Insert(const TString& key, TStageCacheEntryPtr entry) = 0;
    virtual void Clear() = 0;
    virtual TStageCacheStats GetStats() const = 0;
};

// Process-wide cache backed by a plain map, without eviction or size limits.
// Used for tests until the real cache is passed in from the compile service.
IKqpStageCache::TPtr GetTestStageCache();

} // namespace NKikimr::NKqp
