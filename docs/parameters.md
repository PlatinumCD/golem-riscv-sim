# Simulation parameters

Generated from [tile/CPU composition](../src/configuration.py),
[mesh settings](../src/components/mordred/configuration.py), and
[NIU settings](../src/components/mordred/tiles.py).

These are standalone defaults. The complete-tile mesh helper defaults to four
SPM banks with all banks accessible to the CPU and the highest two to the router.
Bank lists select physical addresses and shared resources; they are not bandwidth quotas.

Parameter meanings and constraints: [tile and array parameters](../src/README.md#architecture-parameters),
[CPU](../src/components/riscv-qemu/README.md), [mesh](../src/components/mordred/README.md),
[router-facing SPM service](../src/components/mordred/spm-interface.md).

Regenerate with `python3 tools/hardware/parameter-reference.py > docs/parameters.md`.

## Tile, scratchpad and accelerator

| Parameter | Default |
|---|---|
| `cost_per_array_program_cycles` | `0` |
| `array_program_delay_scope` | `"per_command"` |
| `cost_per_mvm_cycles` | `100` |
| `arrays_per_tile` | `1` |
| `array_rows` | `32` |
| `array_cols` | `32` |
| `riscv_vector_length_bits` | `256` |
| `spm_capacity_bytes` | `2097152` |
| `spm_banks` | `8` |
| `cpu_spm_banks` | `null` |
| `router_spm_banks` | `null` |
| `spm_bank_width` | `4` |
| `spm_read_ports_per_bank` | `1` |
| `spm_write_ports_per_bank` | `1` |
| `spm_channels` | `2` |
| `spm_channel_width` | `32` |
| `spm_request_bytes` | `32` |
| `array_inflight_bytes` | `64` |
| `array_link_duplex` | `"shared"` |
| `array_pipeline_enabled` | `true` |

## CPU

| Parameter | Default |
|---|---|
| `instruction_budget` | `256` |
| `issue_width` | `1` |
| `instruction_fetch_width` | `0` |
| `integer_issue_units` | `2` |
| `memory_issue_units` | `1` |
| `integer_latency_cycles` | `1` |
| `integer_initiation_interval` | `1` |
| `floating_latency_cycles` | `3` |
| `floating_initiation_interval` | `1` |
| `vector_latency_cycles` | `1` |
| `vector_initiation_interval` | `1` |
| `multiply_latency_cycles` | `3` |
| `divide_latency_cycles` | `16` |
| `load_store_queue_depth` | `1` |
| `scalar_load_store_queue_depth` | `8` |
| `analog_command_queue_depth` | `4` |
| `analog_command_queue_bytes` | `16384` |
| `instruction_cache_enabled` | `true` |
| `instruction_cache_bytes` | `8192` |
| `instruction_cache_line_bytes` | `64` |
| `instruction_cache_ways` | `2` |
| `instruction_cache_hit_cycles` | `1` |
| `riscv_vector_enabled` | `true` |
| `riscv_vector_length_bits` | `256` |
| `riscv_vector_element_bits` | `64` |
| `host_timeout_seconds` | `30` |
| `serial_output` | `""` |

## Router-facing SPM interface

| Parameter | Default |
|---|---|
| `request_window` | `4` |
| `max_request_bytes` | `256` |
| `memory_queue_depth` | `8` |
| `posted_receive_slots_per_source` | `16` |
| `posted_credit_batch` | `4` |
| `posted_credit_delay_cycles` | `4` |
| `net_command_queue_depth` | `4` |
| `net_ticket_capacity` | `16` |

## Mesh and NIC

| Parameter | Default |
|---|---|
| `x_dim` | `2` |
| `y_dim` | `2` |
| `local_ports` | `1` |
| `num_vns` | `1` |
| `num_vcs` | `1` |
| `clock` | `"1GHz"` |
| `link_latency` | `"1ns"` |
| `flit_size_bits` | `128` |
| `router_input_buffer_flits` | `8` |
| `router_output_buffer_flits` | `8` |
| `nic_input_buffer_bytes` | `4096` |
| `nic_output_buffer_bytes` | `4096` |
| `verbose` | `0` |

`array_link_width` is derived as `riscv_vector_length_bits / 8`.
The standalone default resolves to 32 bytes/cycle.
