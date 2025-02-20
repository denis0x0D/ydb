#pragma once

//#include <memory>
#include <util/generic/strbuf.h>
#include <util/generic/vector.h>
#include <util/stream/output.h>
#include <util/system/types.h>

namespace NKikimr {
namespace NOptimizerHistograms {

// Helpers methods.
template <typename T> T LoadFrom(const ui8 *storage) {
  T val;
  std::memcpy(&val, storage, sizeof(T));
  return val;
}

template <typename T> void StoreTo(ui8 *storage, T value) {
  std::memcpy(storage, &value, sizeof(T));
}

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

// This class represents an `Equal-width` histogram.
// Each bucket represents a range of contiguous values of equal width, and the
// aggregate summary stored in the bucket is the number of rows whose value lies
// within that range.
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
  template <typename T> void AddElement(T val) {
    const auto index = FindBucketIndex(val);
    // The given `index` in range [0, numBuckets - 1].
    if (!index || (LoadFrom<T>(buckets[index].start) <= val)) {
      buckets[index].count++;
    } else {
      buckets[index - 1].count++;
    }
  }

  // Returns an index of the bucket which stores the given `val`.
  // Returned index in range [0, numBuckets - 1].
  template <typename T> ui32 FindBucketIndex(T val) const {
    ui32 start = 0;
    ui32 end = numBuckets - 1;
    while (start < end) {
      auto it = start + (end - start) / 2;
      if (LoadFrom<T>(buckets[it].start) < val) {
        start = it + 1;
      } else {
        end = it;
      }
    }
    return start;
  }

  // FIXME: Remove before publish
  template <typename T> void PrintBuckets() {
    for (uint32_t i = 0; i < numBuckets; ++i) {
      T start = LoadFrom<T>(buckets[i].start);
      uint64_t count = buckets[i].count;
      Cout << "[" << count << " , " << start << "]\n ";
    }
  }

  // Returns a number of buckets in a histogram.
  ui64 GetNumBuckets() const { return numBuckets; }
  // Returns histogram type.
  EHistogramValueType GetType() const { return valueType; }
  // Returns a number of elements in a bucket by the given `index`.
  ui64 GetNumElementsInBucket(ui32 index) const { return buckets[index].count; }

  // Initializes buckets with a given `range`.
  template <typename T> void InitializeBuckets(const TBucketRange &range) {
    // TODO: Proper diff calculation for types like `string`, `datetime`, etc.
    T rangeLen = LoadFrom<T>(range.end) - LoadFrom<T>(range.start);
    std::memcpy(buckets[0].start, range.start, sizeof(range.start));
    for (ui32 i = 1; i < numBuckets; ++i) {
      auto prevStart = LoadFrom<T>(buckets[i - 1].start);
      StoreTo<T>(buckets[i].start, prevStart + rangeLen);
    }
  }

  // Seriailizes to a binary representation.
  std::unique_ptr<char> Serialize() const;
  // Helper methods.
  ui64 GetStaticSize(ui64 size) const;
  // Returns buckets.
  TVector<TBucket> &GetBuckets() { return buckets; }

private:
  ui32 numBuckets;
  EHistogramValueType valueType;
  TVector<TBucket> buckets;
};

class TEqWidthHistogramEvaluator {
public:
  TEqWidthHistogramEvaluator(std::shared_ptr<TEqWidthHistogram> histogram);

  template <typename T> ui64 EvaluateLessOrEqual(T val) {
    const auto index = histogram->FindBucketIndex(val);
    if (!index || (LoadFrom<T>(histogram->GetBuckets()[index].start) <= val))
      return prefixSum[index];
    return prefixSum[index - 1];
  }

  template <typename T> ui64 EvaluateGreaterOrEqual(T val) {
    const auto index = histogram->FindBucketIndex(val);
    if (!index || (LoadFrom<T>(histogram->GetBuckets()[index].start) <= val))
      return suffixSum[index];
    return suffixSum[index - 1];
  }

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