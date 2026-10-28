#include "kqp_stage_cache.h"

#include <util/generic/hash.h>
#include <util/system/mutex.h>

namespace NKikimr::NKqp {

namespace {

class TMapStageCache : public IKqpStageCache {
public:
    TStageCacheEntryPtr Find(TStringBuf key) override {
        TGuard<TMutex> guard(Lock);
        const auto it = Entries.find(key);
        if (it == Entries.end()) {
            ++Stats.Misses;
            return nullptr;
        }
        ++Stats.Hits;
        return it->second;
    }

    void Insert(const TString& key, TStageCacheEntryPtr entry) override {
        TGuard<TMutex> guard(Lock);
        const auto bytes = key.size() + entry->Program.size();
        if (Entries.emplace(key, std::move(entry)).second) {
            ++Stats.Inserts;
            ++Stats.Entries;
            Stats.Bytes += bytes;
        }
    }

    void Clear() override {
        TGuard<TMutex> guard(Lock);
        Entries.clear();
        Stats = {};
    }

    TStageCacheStats GetStats() const override {
        TGuard<TMutex> guard(Lock);
        return Stats;
    }

private:
    mutable TMutex Lock;
    THashMap<TString, TStageCacheEntryPtr> Entries;
    TStageCacheStats Stats;
};

} // anonymous namespace

IKqpStageCache::TPtr GetTestStageCache() {
    static const IKqpStageCache::TPtr cache = MakeIntrusive<TMapStageCache>();
    return cache;
}

} // namespace NKikimr::NKqp
