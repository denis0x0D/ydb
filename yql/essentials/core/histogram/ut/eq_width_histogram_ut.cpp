#include "eq_width_histogram.h"

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr {
namespace NOptimizerHistograms {

Y_UNIT_TEST_SUITE(EqWidthHistogram) {
  Y_UNIT_TEST(Basic) {
    std::shared_ptr<TEqWidthHistogram> histogram(std::make_unique<TEqWidthHistogram>(10, EHistogramValueType::Uint32));
    TEqWidthHistogram::TBucketRange bucketRange;
    StoreTo<ui32>(bucketRange.start, 0);
    StoreTo<ui32>(bucketRange.end, 2);
    histogram->InitializeBuckets<ui32>(bucketRange);
    UNIT_ASSERT_VALUES_EQUAL(histogram->GetNumBuckets(), 10);
    // From range [0, 9] 10 elements.
    for (ui32 i = 0; i < 10; ++i)
        histogram->AddElement<ui32>(i);
    histogram->PrintBuckets<ui32>();
    TEqWidthHistogramEvaluator evaluator(histogram);
    UNIT_ASSERT_VALUES_EQUAL(evaluator.EvaluateLessOrEqual<ui32>(9), 10);
    UNIT_ASSERT_VALUES_EQUAL(evaluator.EvaluateGreaterOrEqual<ui32>(10), 0);
  }

  Y_UNIT_TEST(Serialization) {
    // TODO Add test.
  }
}
} // namespace NOptimizerHistograms
} // namespace NKikimr