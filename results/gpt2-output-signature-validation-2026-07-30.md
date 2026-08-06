# GPT-2 Output Signature Validation

**Date:** 2026-07-30  
**Scope:** All 112 backend-aware scheduling deployments  
**Result:** Signature-level sanity check passed

## Purpose

This is a quick functional validation of the GPT-2 scheduling sweep against
the original PyTorch fixture. It tests every analog and digital deployment at
token counts 4, 8, 16, and 32.

The existing deployment logs preserve:

- output element count;
- finite element count;
- the bit pattern of the first output value; and
- the bit pattern of a sequential float32 checksum.

They do not preserve the complete output tensor. This study can detect gross
corruption and quantify the available numerical signatures, but it cannot
calculate full-tensor maximum error, RMSE, or cosine similarity.

## Method

For each token count:

1. Reconstruct the exact `GPT2LikeTransformer` PyTorch fixture.
2. Set `torch.manual_seed(0)`.
3. Generate the same deterministic input used by the tile runtime:
   `float(index + 1) / 100`.
4. Execute the PyTorch model in float32.
5. Reproduce the tile runtime's sequential float32 checksum operation.
6. Compare the reference signatures with all fourteen analog and all fourteen
   digital scheduler deployments.

## Results

| Tokens | Backend | Maximum first-value absolute error | Maximum first-value relative error | Maximum first-value ULP difference | Worst checksum absolute drift | Checksum drift per output element | Finite outputs |
|---:|---|---:|---:|---:|---:|---:|---:|
| 4 | Analog | 1.3113e-5 | 7.5807e-6 | 110 | 0.02308 | 7.5123e-6 | 14/14 |
| 4 | Digital | 1.4424e-5 | 8.3388e-6 | 121 | 0.02702 | 8.7967e-6 | 14/14 |
| 8 | Analog | 1.4544e-5 | 8.4077e-6 | 122 | 0.08317 | 1.3536e-5 | 14/14 |
| 8 | Digital | 1.4424e-5 | 8.3388e-6 | 121 | 0.08531 | 1.3886e-5 | 14/14 |
| 16 | Analog | 1.2159e-5 | 7.0294e-6 | 102 | 0.22413 | 1.8239e-5 | 14/14 |
| 16 | Digital | 1.4424e-5 | 8.3388e-6 | 121 | 0.23261 | 1.8929e-5 | 14/14 |
| 32 | Analog | 1.1563e-5 | 6.6848e-6 | 97 | 0.18006 | 7.3267e-6 | 14/14 |
| 32 | Digital | 1.7762e-5 | 1.0268e-5 | 149 | 0.12937 | 5.2639e-6 | 14/14 |

Every deployment produced the expected number of output elements, and every
output element was finite.

The largest observed first-value error was `1.7762e-5`, or approximately
`1.03e-5` relative error. The largest checksum drift, normalized by output
element count, was `1.8929e-5` per element.

## Interpretation

The result provides no evidence of gross dataflow corruption. The observed
signature differences are small and consistent with changes in float32
operation or reduction ordering.

The raw checksum has poor relative-error behavior because the final LayerNorm
output contains positive and negative values whose sum is close to zero.
Small per-element rounding differences therefore produce a large relative
change in the checksum. Absolute drift and drift per element are more useful
for this quick check.

## Limitation and Required Acceptance Test

This result is a **signature-level sanity check**, not the final numerical
correctness gate. A checksum can hide offsetting element-wise errors.

The final acceptance test must preserve the complete output tensor for
representative and eventually all deployments, then report:

- maximum absolute error;
- maximum relative error with an explicit near-zero policy;
- mean absolute error;
- RMSE;
- cosine similarity;
- NaN and infinity counts; and
- agreement with a declared PyTorch tolerance.

Until that test exists, the appropriate claim is:

> All 112 deployments produce finite, correctly sized outputs whose available
> numerical signatures remain close to the PyTorch reference; full-tensor
> equivalence has not yet been established.
