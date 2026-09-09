#identity1 = affine_map<(d0) -> (d0)>
module {
  func.func @forward(%arg0: tensor<2500xf32>,
                     %arg1: tensor<2500xf32>) -> tensor<5000xf32> {
    %left_empty = tensor.empty() : tensor<2500xf32>
    %left = linalg.generic {
      indexing_maps = [#identity1, #identity1],
      iterator_types = ["parallel"],
      sculptor.semantic.layer_id = 400 : i64,
      sculptor.semantic.layer_kind = "concat_left"
    } ins(%arg0 : tensor<2500xf32>) outs(%left_empty : tensor<2500xf32>) {
    ^bb0(%input: f32, %output: f32):
      %one = arith.constant 1.0 : f32
      %value = arith.addf %input, %one : f32
      linalg.yield %value : f32
    } -> tensor<2500xf32>
    %right_empty = tensor.empty() : tensor<2500xf32>
    %right = linalg.generic {
      indexing_maps = [#identity1, #identity1],
      iterator_types = ["parallel"],
      sculptor.semantic.layer_id = 401 : i64,
      sculptor.semantic.layer_kind = "concat_right"
    } ins(%arg1 : tensor<2500xf32>)
      outs(%right_empty : tensor<2500xf32>) {
    ^bb0(%input: f32, %output: f32):
      %one = arith.constant 1.0 : f32
      %value = arith.addf %input, %one : f32
      linalg.yield %value : f32
    } -> tensor<2500xf32>
    %joined = tensor.concat dim(0) %left, %right {
      sculptor.mapping.stage_id = 1 : i64,
      sculptor.mapping.stage_kind = "tile_recombine",
      sculptor.mapping.stage_name = "materialized_concat",
      sculptor.semantic.layer_id = 402 : i64,
      sculptor.semantic.layer_kind = "concat_assembly"
    } : (tensor<2500xf32>, tensor<2500xf32>) -> tensor<5000xf32>
    %result_empty = tensor.empty() : tensor<5000xf32>
    %result = linalg.generic {
      indexing_maps = [#identity1, #identity1],
      iterator_types = ["parallel"],
      sculptor.semantic.layer_id = 403 : i64,
      sculptor.semantic.layer_kind = "concat_result"
    } ins(%joined : tensor<5000xf32>)
      outs(%result_empty : tensor<5000xf32>) {
    ^bb0(%input: f32, %output: f32):
      %one = arith.constant 1.0 : f32
      %value = arith.subf %input, %one : f32
      linalg.yield %value : f32
    } -> tensor<5000xf32>
    return %result : tensor<5000xf32>
  }
}
