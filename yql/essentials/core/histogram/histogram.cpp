#include "histogram.h"

#include <stdlib.h>
#include <util/system/compiler.h>

namespace NKikimr {
namespace OptStatistics {

KHistogram::KHistogram(ui32 numBuckets, ui8 type)
    : numBuckets(numBuckets), type(type) {
  buckets = new KBucket[numBuckets];
}

KHistogram::KHistogram(const char *str, ui64 size) {
  Y_ASSERT(str && size);
  numBuckets = *reinterpret_cast<const ui32 *>(str);
  Y_ABORT_UNLESS(GetStaticSize(numBuckets) == size);
  type = *reinterpret_cast<const uint8_t *>(str + sizeof(numBuckets));
  buckets = new KBucket[numBuckets];
  ui32 offset = sizeof(numBuckets) + sizeof(type);
  for (ui32 i = 0; i < numBuckets; ++i) {
    std::memcpy(&buckets[i], reinterpret_cast<const uint8_t *>(str + offset),
                sizeof(KBucket));
    offset += sizeof(KBucket);
  }
}

template <typename T>
void KHistogram::Initialize(const KHistogram::KBucketRange &range) {
  T rangeLen = LoadFrom<T>(range.end) - LoadFrom<T>(range.start);
  std::memcpy(buckets[0].start, range.start, sizeof(range.start));
  for (ui32 i = 1; i < numBuckets; ++i) {
    auto prevStart = LoadFrom<T>(buckets[i - 1].start);
    storeTo<T>(buckets[i].start, prevStart + rangeLen);
  }
}

ui64 KHistogram::GetStaticSize(ui64 nBuckets) {
  return sizeof(numBuckets) + sizeof(type) + sizeof(KBucket) * nBuckets;
}

std::unique_ptr<char> KHistogram::Serialize() {
  std::unique_ptr<char> data(new char[GetStaticSize(numBuckets)]);
  ui32 offset = 0;
  std::memcpy(data.get(), &numBuckets, sizeof(numBuckets));
  offset += sizeof(numBuckets);
  std::memcpy(data.get() + offset, &type, sizeof(type));
  offset += sizeof(type);
  for (ui32 i = 0; i < numBuckets; ++i) {
    std::memcpy(data.get() + offset, &buckets[i], sizeof(KBucket));
    offset += sizeof(KBucket);
  }
  return data;
}

template <typename T> ui32 KHistogram::FindBucketIndex(T val) {
  ui32 start = 0;
  ui32 end = numBuckets;
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

template <typename T> void KHistogram::AddElement(T val) {
  auto index = findBucketIndex(val);
  if (index < numBuckets) {
    buckets[index].count++;
  } else {
    buckets[numBuckets - 1].count++;
  }
}

ui64 KHistogram::GetNumElementsInBucket(ui32 index) {
  return buckets[index].count;
}

ui64 KHistogram::GetNumBuckets() { return numBuckets; }

template <typename T> inline T KHistogram::LoadFrom(uint8_t storage[8]) {
  T val;
  std::memcpy(&val, storage, sizeof(T));
  return val;
}

template <typename T>
inline void KHistogram::StoreTo(uint8_t storage[8], T value) {
  std::memcpy(storage, &value, sizeof(T));
}

KHistogramEvaluator::KHistogramEvaluator(std::shared_ptr<KHistogram> histogram)
    : histogram(histogram), numBuckets(histogram->GetNumBuckets()) {
  prefixSum = TVector<ui64>(numBuckets);
  suffixSum = TVector<ui64>(numBuckets);
  CreatePrefixSum();
  CreateSuffixSum();
}

void KHistogramEvaluator::CreatePrefixSum() {
  prefixSum[0] = histogram->GetNumElementsInBucket(0);
  for (ui32 i = 1; i < numBuckets; ++i) {
    prefixSum[i] = prefixSum[i - 1] + histogram->GetNumElementsInBucket(i);
  }
}

void KHistogramEvaluator::CreateSuffixSum() {
  suffixSum[numBuckets - 1] = histogram->GetNumElementsInBucket(numBuckets - 1);
  for (i32 i = numBuckets - 2; i >= 0; --i) {
    suffixSum[i] = suffixSum[i + 1] + histogram->GetNumElementsInBucket(i);
  }
}

template <typename T> ui64 KHistogramEvaluator::EvaluateLessOrEqual(T val) {
  const auto index = histogram->FindBucketIndex(val);
  if (index < numBuckets) {
    return prefixSum[index];
  }
  return prefixSum[numBuckets - 1];
}

template <typename T> ui64 KHistogramEvaluator::EvaluateGreaterOrEqual(T val) {
  const auto index = histogram->FindBucketIndex(val);
  if (index < numBuckets) {
    return suffixSum[index];
  }
  return suffixSum[numBuckets - 1];
}

template <typename T> ui64 KHistogramEvaluator::EvaluateLessThan(T val) {
  (void)val;
  return 1;
}

template <typename T> uint64_t KHistogramEvaluator::EvaluateGreaterThan(T val) {
  (void)val;
  return 1;
}
} // namespace OptStatistics
} // namespace NKikimr