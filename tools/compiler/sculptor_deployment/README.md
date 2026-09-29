# Single-tile execution support

`single_tile_runtime.cc` implements the compiler's fixed tensor connection ABI
using bounded copies within local SPM. `connection_abi.h` defines the descriptor
layout and `transfer_view.h` implements tensor-region selection. The compiler
generates all computation, including the vector-register analog instructions.

The maintained integration test is
`bash tests/run-all.sh --suite compiler`. It compiles this support afresh with
each generated program. External tensors are embedded in the tile's SPM image;
network sends and multi-tile deployments are outside this runtime's support.
