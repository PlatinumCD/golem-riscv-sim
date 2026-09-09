#include <stdint.h>

#include "case_oracle.h"

int main() {
  const auto &contract = mittens::materialized_test::caseContract();
  if (contract.input_count == 0 || contract.output_count != 1 ||
      contract.output_bytes[0] == 0 ||
      contract.output_bytes[0] % sizeof(float) != 0)
    return 1;
  const uint64_t elements = contract.output_bytes[0] / sizeof(float);
  const uint64_t samples[3] = {0, elements / 2, elements - 1};
  for (uint64_t sample : samples) {
    float expected = 0.0F;
    if (!mittens::materialized_test::expectedOutputValue(
            0, sample, &expected) ||
        !mittens::materialized_test::outputValueMatches(0, sample, expected) ||
        mittens::materialized_test::outputValueMatches(
            0, sample, expected + 1.0F))
      return 2;
  }
  return 0;
}
