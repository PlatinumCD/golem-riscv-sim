module attributes {
  sculptor.arch.streaming = {
    fixed_shard_bytes = 4096 : i64,
    global_ram_bytes = 34359738368 : i64,
    max_in_flight = 2 : i64,
    noc_word_bytes = 4 : i64,
    scratchpad_bytes = 2097152 : i64,
    version = 1 : i64
  },
  sculptor.deployment.active_tile_ids = [0, 2],
  sculptor.deployment.kind = "tile_routines",
  sculptor.deployment.model_inputs = [],
  sculptor.deployment.model_outputs = [],
  sculptor.deployment.routes = [],
  sculptor.materialization.boundaries = [{
    boundary_id = 99 : i64,
    source_operation_id = 0 : i64,
    source_work_unit_id = 0 : i64,
    source_result_number = 0 : i64,
    target_operation_id = 1 : i64,
    target_work_unit_id = 1 : i64
  }],
  sculptor.materialization.exact_ram_readiness_enabled = false,
  sculptor.materialization.epoch_count = 3 : i64
} {
  module attributes {
    sculptor.deployment.incoming_routes = [],
    sculptor.deployment.local_bindings = [],
    sculptor.deployment.outgoing_routes = [],
    sculptor.deployment.physical_tile_id = 0 : i64,
    sculptor.memory.assemblies = [],
    sculptor.memory.bindings = [],
    sculptor.memory.capacity = #sculptor.tile_memory_capacity<tile = 0 : i64, externalBytes = 0 : i64, persistentBytes = 0 : i64, workspaceBytes = 0 : i64, scratchpadBytes = 0 : i64, routeInputBytes = 0 : i64, routeOutputBytes = 0 : i64, assemblyBytes = 0 : i64, intermediateBytes = 0 : i64, routineTemporaryPeakBytes = 0 : i64, routineTemporaryTotalBytes = 0 : i64, peakLiveBytes = 0 : i64, requiredLocalBytes = 0 : i64, reusableBytes = 0 : i64, complete = true>,
    sculptor.memory.completion_events = [
      #sculptor.tile_memory_completion<id = 0 : i64, kind = routine_start, tile = 0 : i64, routine = 0 : i64, routeId = -1 : i64, ownerId = -1 : i64, viewId = -1 : i64>,
      #sculptor.tile_memory_completion<id = 1 : i64, kind = routine_complete, tile = 0 : i64, routine = 0 : i64, routeId = -1 : i64, ownerId = -1 : i64, viewId = -1 : i64>
    ],
    sculptor.memory.event_edges = [
      #sculptor.tile_memory_event_edge<id = 0 : i64, sourceEventId = 0 : i64, targetEventId = 1 : i64, kind = routine_execution>
    ],
    sculptor.memory.in_place_aliases = [],
    sculptor.memory.interferences = [],
    sculptor.memory.lifetimes = [],
    sculptor.memory.movements = [],
    sculptor.memory.owners = [],
    sculptor.memory.parametric_bindings = [],
    sculptor.memory.parametric_completions = [],
    sculptor.memory.parametric_owners = [],
    sculptor.memory.parametric_views = [],
    sculptor.memory.plan_version = 4 : i64,
    sculptor.memory.segments = [],
    sculptor.memory.views = []
  } {
  }
  module attributes {
    sculptor.deployment.incoming_routes = [],
    sculptor.deployment.local_bindings = [],
    sculptor.deployment.outgoing_routes = [],
    sculptor.deployment.physical_tile_id = 2 : i64,
    sculptor.memory.assemblies = [],
    sculptor.memory.bindings = [],
    sculptor.memory.capacity = #sculptor.tile_memory_capacity<tile = 2 : i64, externalBytes = 0 : i64, persistentBytes = 0 : i64, workspaceBytes = 0 : i64, scratchpadBytes = 0 : i64, routeInputBytes = 0 : i64, routeOutputBytes = 0 : i64, assemblyBytes = 0 : i64, intermediateBytes = 0 : i64, routineTemporaryPeakBytes = 0 : i64, routineTemporaryTotalBytes = 0 : i64, peakLiveBytes = 0 : i64, requiredLocalBytes = 0 : i64, reusableBytes = 0 : i64, complete = true>,
    sculptor.memory.completion_events = [
      #sculptor.tile_memory_completion<id = 0 : i64, kind = routine_start, tile = 2 : i64, routine = 0 : i64, routeId = -1 : i64, ownerId = -1 : i64, viewId = -1 : i64>,
      #sculptor.tile_memory_completion<id = 1 : i64, kind = routine_complete, tile = 2 : i64, routine = 0 : i64, routeId = -1 : i64, ownerId = -1 : i64, viewId = -1 : i64>
    ],
    sculptor.memory.event_edges = [
      #sculptor.tile_memory_event_edge<id = 0 : i64, sourceEventId = 0 : i64, targetEventId = 1 : i64, kind = routine_execution>
    ],
    sculptor.memory.in_place_aliases = [],
    sculptor.memory.interferences = [],
    sculptor.memory.lifetimes = [],
    sculptor.memory.movements = [],
    sculptor.memory.owners = [],
    sculptor.memory.parametric_bindings = [],
    sculptor.memory.parametric_completions = [],
    sculptor.memory.parametric_owners = [],
    sculptor.memory.parametric_views = [],
    sculptor.memory.plan_version = 4 : i64,
    sculptor.memory.segments = [],
    sculptor.memory.views = []
  } {
  }
}
