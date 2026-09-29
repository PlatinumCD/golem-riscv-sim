# Guest platform support

These retained files provide program startup, a parameterized SPM linker script,
UART/exit helpers and freestanding memory/math routines. They do not implement a
guest network-send API. The current component tests compile their guests directly;
Sculptor integration uses the linker here and its bounded single-tile runtime.
See [guest programming](../../docs/platform.md) and [migration](../../docs/migration.md).
