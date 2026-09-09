#include "case_oracle.h"

#ifndef MITTENS_MATERIALIZED_CASE
#error "MITTENS_MATERIALIZED_CASE must select one functional fixture"
#endif

namespace mittens::materialized_test {
namespace {

#if MITTENS_MATERIALIZED_CASE == 1
constexpr CaseContract kContract{"pointwise", 1, 1, {10000, 0}, {10000}};
#elif MITTENS_MATERIALIZED_CASE == 2
constexpr CaseContract kContract{"fork", 1, 1, {10000, 0}, {10000}};
#elif MITTENS_MATERIALIZED_CASE == 3
constexpr CaseContract kContract{"pool", 1, 1, {26624, 0}, {6656}};
#elif MITTENS_MATERIALIZED_CASE == 4
constexpr CaseContract kContract{"concat", 2, 1, {10000, 10000}, {20000}};
#elif MITTENS_MATERIALIZED_CASE == 5
constexpr CaseContract kContract{"reduction", 1, 1, {16384, 0}, {15376}};
#elif MITTENS_MATERIALIZED_CASE == 6
constexpr CaseContract kContract{"layout-conversion", 1, 1, {4128, 0},
                                 {4128}};
#else
#error "MITTENS_MATERIALIZED_CASE must be in [1, 6]"
#endif

float seededValue(uint32_t input_ordinal, uint64_t element) {
  return static_cast<float>(input_ordinal * UINT32_C(100000)) +
         static_cast<float>(element);
}

} // namespace

const CaseContract &caseContract() { return kContract; }

float inputValue(uint32_t input_ordinal, uint64_t element) {
  return seededValue(input_ordinal, element);
}

bool expectedOutputValue(uint32_t output_ordinal, uint64_t element,
                         float *value) {
  if (value == nullptr || output_ordinal != 0)
    return false;

#if MITTENS_MATERIALIZED_CASE == 1
  const float input = seededValue(0, element);
  *value = (input + 1.0F) * 2.0F;
  return true;
#elif MITTENS_MATERIALIZED_CASE == 2
  const float input = seededValue(0, element);
  const float producer = input + 3.0F;
  *value = (producer + 1.0F) + producer * 2.0F;
  return true;
#elif MITTENS_MATERIALIZED_CASE == 3
  constexpr uint64_t kInputHeight = 52;
  constexpr uint64_t kInputWidth = 64;
  constexpr uint64_t kOutputHeight = 26;
  constexpr uint64_t kOutputWidth = 32;
  const uint64_t channel = element / (kOutputHeight * kOutputWidth);
  const uint64_t within = element % (kOutputHeight * kOutputWidth);
  const uint64_t row = within / kOutputWidth;
  const uint64_t column = within % kOutputWidth;
  const uint64_t maximum = channel * kInputHeight * kInputWidth +
                           (row * 2U + 1U) * kInputWidth +
                           column * 2U + 1U;
  *value = seededValue(0, maximum);
  return true;
#elif MITTENS_MATERIALIZED_CASE == 4
  constexpr uint64_t kInputElements = 2500;
  if (element < kInputElements)
    *value = seededValue(0, element);
  else
    *value = seededValue(1, element - kInputElements);
  return true;
#elif MITTENS_MATERIALIZED_CASE == 5
  constexpr uint64_t kInputHeight = 32;
  constexpr uint64_t kInputWidth = 32;
  constexpr uint64_t kOutputHeight = 31;
  constexpr uint64_t kOutputWidth = 31;
  const uint64_t channel = element / (kOutputHeight * kOutputWidth);
  const uint64_t within = element % (kOutputHeight * kOutputWidth);
  const uint64_t row = within / kOutputWidth;
  const uint64_t column = within % kOutputWidth;
  const uint64_t first = channel * kInputHeight * kInputWidth +
                         row * kInputWidth + column;
  const float top_left = seededValue(0, first);
  const float top_right = seededValue(0, first + 1U);
  const float bottom_left = seededValue(0, first + kInputWidth);
  const float bottom_right = seededValue(0, first + kInputWidth + 1U);
  *value = ((0.0F + top_left) + top_right) + bottom_left + bottom_right;
  return true;
#elif MITTENS_MATERIALIZED_CASE == 6
  constexpr uint64_t kColumns = 8;
  const uint64_t row = element / kColumns;
  const uint64_t column = element % kColumns;
  const uint64_t source_column =
      column < 4U ? column * 2U : (column - 4U) * 2U + 1U;
  *value = seededValue(0, row * kColumns + source_column) + 1.0F;
  return true;
#endif
}

bool outputValueMatches(uint32_t output_ordinal, uint64_t element,
                        float actual) {
  float expected = 0.0F;
  return expectedOutputValue(output_ordinal, element, &expected) &&
         actual == expected;
}

} // namespace mittens::materialized_test
