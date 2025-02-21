#include "eq_width_histogram.h"

namespace NKikimr {
namespace NOptimizerHistograms {

TEqWidthHistogram::TEqWidthHistogram(ui32 numBuckets, EHistogramValueType valueType)
    : numBuckets(numBuckets), valueType(valueType), buckets(numBuckets) {
  // Exptected at least one bucket for histogram.
  Y_ASSERT(numBuckets >= 1);
}

TEqWidthHistogram::TEqWidthHistogram(const char *str, ui64 size) {
  Y_ASSERT(str && size);
  numBuckets = *reinterpret_cast<const ui32 *>(str);
  Y_ABORT_UNLESS(GetStaticSize(numBuckets) == size);
  valueType = *reinterpret_cast<const EHistogramValueType *>(str + sizeof(numBuckets));
  buckets = TVector<TBucket>(numBuckets);
  ui32 offset = sizeof(numBuckets) + sizeof(valueType);
  for (ui32 i = 0; i < numBuckets; ++i) {
    std::memcpy(&buckets[i], reinterpret_cast<const char *>(str + offset), sizeof(TBucket));
    offset += sizeof(TBucket);
  }
}

ui64 TEqWidthHistogram::GetStaticSize(ui32 nBuckets) const {
  return sizeof(numBuckets) + sizeof(valueType) + sizeof(TBucket) * nBuckets;
}

// Binary layout:
// [4 byte: number of buckets][1 byte: value type]
// [sizeof(Bucket)[0]... sizeof(Bucket)[n]]
std::unique_ptr<char> TEqWidthHistogram::Serialize() const {
  const auto binarySize = GetStaticSize(numBuckets);
  std::unique_ptr<char> binaryData(new char[binarySize]);
  ui32 offset = 0;
  std::memcpy(binaryData.get(), &numBuckets, sizeof(numBuckets));
  offset += sizeof(numBuckets);
  std::memcpy(binaryData.get() + offset, &valueType, sizeof(valueType));
  offset += sizeof(valueType);
  for (ui32 i = 0; i < numBuckets; ++i) {
    std::memcpy(binaryData.get() + offset, &buckets[i], sizeof(TBucket));
    offset += sizeof(TBucket);
  }
  return binaryData;
}

TEqWidthHistogramEvaluator::TEqWidthHistogramEvaluator(std::shared_ptr<TEqWidthHistogram> histogram)
    : histogram(histogram), numBuckets(histogram->GetNumBuckets()) {
  prefixSum = TVector<ui64>(numBuckets);
  suffixSum = TVector<ui64>(numBuckets);
  CreatePrefixSum();
  CreateSuffixSum();
}

void TEqWidthHistogramEvaluator::CreatePrefixSum() {
  prefixSum[0] = histogram->GetNumElementsInBucket(0);
  for (ui32 i = 1; i < numBuckets; ++i) {
    prefixSum[i] = prefixSum[i - 1] + histogram->GetNumElementsInBucket(i);
  }
}

void TEqWidthHistogramEvaluator::CreateSuffixSum() {
  suffixSum[numBuckets - 1] = histogram->GetNumElementsInBucket(numBuckets - 1);
  for (i32 i = static_cast<i32>(numBuckets) - 2; i >= 0; --i) {
    suffixSum[i] = suffixSum[i + 1] + histogram->GetNumElementsInBucket(i);
  }
}
}  // namespace NOptimizerHistograms
}  // namespace NKikimr