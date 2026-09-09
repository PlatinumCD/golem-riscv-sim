#include <stdint.h>

#include "abi_accounting.h"
#include "case_oracle.h"
#include "golem/runtime/tile_abi.h"
#include "platform.h"

namespace {

using namespace golem::runtime;
using mittens::materialized_test::CaseContract;
using mittens::materialized_test::DMAAccounting;

void printUnsigned(uint64_t value) {
  char digits[21];
  uint32_t count = 0;
  do {
    digits[count++] = static_cast<char>('0' + value % 10U);
    value /= 10U;
  } while (value != 0);
  while (count != 0)
    uart_putc(digits[--count]);
}

void printField(const char *name, uint64_t value) {
  uart_putc(' ');
  uart_puts(name);
  uart_putc('=');
  printUnsigned(value);
}

int fail(const TileABI &abi, const char *stage) {
  uart_puts("MATERIALIZED_FUNCTIONAL_PREFLIGHT_ERROR case=");
  uart_puts(mittens::materialized_test::caseContract().name);
  printField("tile", abi.core_id);
  uart_puts(" stage=");
  uart_puts(stage);
  uart_putc('\n');
  return 1;
}

bool validateCaseBuffers(const TileABI &abi, const CaseContract &contract) {
  uint32_t inputs = 0;
  uint32_t outputs = 0;
  for (uint32_t index = 0; index < abi.global_buffer_count; ++index) {
    const GlobalBuffer &buffer = abi.global_buffers[index];
    if ((buffer.flags & GlobalBufferInput) != 0) {
      if (inputs >= contract.input_count ||
          buffer.byte_size != contract.input_bytes[inputs] ||
          buffer.byte_size % sizeof(float) != 0)
        return false;
      ++inputs;
    }
    if ((buffer.flags & GlobalBufferOutput) != 0) {
      if (outputs >= contract.output_count ||
          buffer.byte_size != contract.output_bytes[outputs] ||
          buffer.byte_size % sizeof(float) != 0)
        return false;
      ++outputs;
    }
  }
  return inputs == contract.input_count && outputs == contract.output_count;
}

} // namespace

extern "C" int tile_main() {
  const TileABI abi = linkedTileABI();
  const CaseContract &contract = mittens::materialized_test::caseContract();
  if (!abi.valid() || !abi.hasDeploymentPlan() ||
      !abi.validDeploymentPlan() ||
      !mittens::materialized_test::validMaterializedABIShape(abi))
    return fail(abi, "abi");
  if (!validateCaseBuffers(abi, contract))
    return fail(abi, "case-buffers");

  DMAAccounting accounting{};
  uint64_t iterations = 0;
  if (!mittens::materialized_test::expectedDMAAccounting(abi, &accounting) ||
      !mittens::materialized_test::expectedShardIterations(abi, &iterations) ||
      accounting.full_requests == 0 || accounting.tail_requests == 0)
    return fail(abi, "accounting");

  uart_puts("MATERIALIZED_FUNCTIONAL_PREFLIGHT case=");
  uart_puts(contract.name);
  printField("tile", abi.core_id);
  printField("epoch_count", abi.epoch_count);
  printField("iterations", iterations);
  printField("full_requests", accounting.full_requests);
  printField("tail_requests", accounting.tail_requests);
  printField("runtime_requests", accounting.requests);
  printField("runtime_bytes", accounting.bytes);
  uart_putc('\n');
  return 0;
}
