#pragma once

#include <memory>
#include <util/generic/strbuf.h>
#include <util/generic/vector.h>
#include <util/system/types.h>

namespace NKikimr {

class KHistogram {
public:
  struct KBucket {
    ui64 count{0};
    ui8 start[8];
  };
  struct KBucketRange {
    ui8 start[8];
    ui8 end[8];
  };

  KHistogram(ui32 numBuckets = 200, ui8 type = 0);
  KHistogram(const char *str, ui64 size);
  ~KHistogram() { delete[] buckets; }

  template <typename T> void Initialize(const KBucketRange &range);
  template <typename T> ui32 FindBucketIndex(T val);
  template <typename T> void AddElement(T val);
  ui64 GetNumElementsInBucket(uint32_t index);
  ui64 GetNumBuckets();
  std::unique_ptr<char> Serialize();

private:
  ui64 GetStaticSize(ui64 size);
  template <typename T> static inline T LoadFrom(uint8_t storage[8]);
  template <typename T> static inline void StoreTo(uint8_t storage[8], T value);

  ui32 numBuckets;
  ui8 type;
  KBucket *buckets;
};

class KHistogramEvaluator {
public:
  KHistogramEvaluator(std::shared_ptr<KHistogram> histogram);

  template <typename T> ui64 EvaluateLessOrEqual(T val);
  template <typename T> ui64 EvaluateGreaterOrEqual(T val);
  template <typename T> ui64 EvaluateLessThan(T val);
  template <typename T> ui64 EvaluateGreaterThan(T val);

private:
  void CreatePrefixSum();
  void CreateSuffixSum();
  std::shared_ptr<KHistogram> histogram;
  TVector<ui64> prefixSum;
  TVector<ui64> suffixSum;
  ui32 numBuckets;
};
} // namespace NKikimr
