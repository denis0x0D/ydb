#pragma once

#include <memory>
#include <util/generic/strbuf.h>
#include <util/generic/vector.h>
#include <util/system/types.h>

namespace NKikimr {
namespace NOptimizerHistograms {

// Represents value types supported by histogram.
enum class EHistogramValueType : ui8 {
  Int32,
  Int64,
  Uint32,
  Uint64,
  NotSupported
};

// Represents histogram types.
enum class EHistogramType : ui8 { EqualWidth, Unknown };

// This class represents an `equal width` histogram.
class TEqWidthHistogram {
public:
  struct TBucket {
    // The number of values in a bucket.
    ui64 count{0};
    // The `start` value of a bucket, the `end` of the bucket is a next start.
    // [start = start[i], end = start[i + 1])
    ui8 start[8];
  };
  struct TBucketRange {
    ui8 start[8];
    ui8 end[8];
  };

  TEqWidthHistogram(ui32 numBuckets = 1,
                    EHistogramValueType type = EHistogramValueType::Int32);
  // From serialized data.
  TEqWidthHistogram(const char *str, ui64 size);

  // Adds the given `val` to a histogram.
  template <typename T> void AddElement(T val);
  // Returns an index of the bucket which stores the given `val`.
  template <typename T> ui32 FindBucketIndex(T val) const;
  // Returns a number of buckets in a histogram.
  ui64 GetNumBuckets() const { return numBuckets; }
  // Returns histogram type.
  EHistogramValueType GetType() const { return valueType; }
  // Returns a number of elements in a bucket by the given `index`.
  ui64 GetNumElementsInBucket(ui32 index) const;
  // Initializes buckets with a given `range`.
  template <typename T> void InitializeBuckets(const TBucketRange &range);
  // Seriailizes to a binary representation.
  std::unique_ptr<char> Serialize() const;

private:
  // Helper methods.
  ui64 GetStaticSize(ui64 size) const;
  template <typename T> static inline T LoadFrom(uint8_t storage[8]);
  template <typename T> static inline void StoreTo(uint8_t storage[8], T value);

  ui32 numBuckets;
  EHistogramValueType valueType;
  TVector<TBucket> buckets;
};

class TEqWidthHistogramEvaluator {
public:
  TEqWidthHistogramEvaluator(std::shared_ptr<TEqWidthHistogram> histogram);

  template <typename T> ui64 EvaluateLessOrEqual(T val);
  template <typename T> ui64 EvaluateGreaterOrEqual(T val);
  template <typename T> ui64 EvaluateLessThan(T val);
  template <typename T> ui64 EvaluateGreaterThan(T val);

private:
  void CreatePrefixSum();
  void CreateSuffixSum();
  std::shared_ptr<TEqWidthHistogram> histogram;
  TVector<ui64> prefixSum;
  TVector<ui64> suffixSum;
  ui32 numBuckets;
};
} // namespace NOptimizerHistograms
} // namespace NKikimr