#pragma once

#include <stdint.h>

namespace mittens::materialized_test {

struct CaseContract {
  const char *name;
  uint32_t input_count;
  uint32_t output_count;
  uint64_t input_bytes[2];
  uint64_t output_bytes[1];
};

const CaseContract &caseContract();
float inputValue(uint32_t input_ordinal, uint64_t element);
bool expectedOutputValue(uint32_t output_ordinal, uint64_t element,
                         float *value);
bool outputValueMatches(uint32_t output_ordinal, uint64_t element,
                        float actual);

} // namespace mittens::materialized_test
