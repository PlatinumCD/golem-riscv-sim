# LLVM RVV correctness regression

`bash tests/run-all.sh --case platform/llvm-rvv` compiles fresh copy, addition,
checksum and stencil kernels with LLVM. It exercises VLEN 256/1024 and LSQ depth
1/4, using a small fixed input and eight banks. All 16 cases check outputs,
guards, byte traffic and queue/bank accounting. Copy and addition at depth 4 must
actually enter the asynchronous queue. Kernels, linker scripts and validators
live in this directory. No studies or saved binaries are required.
