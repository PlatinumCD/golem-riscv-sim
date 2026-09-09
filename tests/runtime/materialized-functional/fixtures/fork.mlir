#identity = affine_map<(d0) -> (d0)>
module {
  func.func @forward(%arg0: tensor<2500xf32>) -> tensor<2500xf32> {
    %producer_empty = tensor.empty() : tensor<2500xf32>
    %producer = linalg.generic {
      indexing_maps = [#identity, #identity],
      iterator_types = ["parallel"],
      sculptor.semantic.layer_id = 200 : i64,
      sculptor.semantic.layer_kind = "fork_producer"
    } ins(%arg0 : tensor<2500xf32>)
      outs(%producer_empty : tensor<2500xf32>) {
    ^bb0(%input: f32, %output: f32):
      %three = arith.constant 3.0 : f32
      %value = arith.addf %input, %three : f32
      linalg.yield %value : f32
    } -> tensor<2500xf32>
    %left_empty = tensor.empty() : tensor<2500xf32>
    %left = linalg.generic {
      indexing_maps = [#identity, #identity],
      iterator_types = ["parallel"],
      sculptor.semantic.layer_id = 201 : i64,
      sculptor.semantic.layer_kind = "fork_left"
    } ins(%producer : tensor<2500xf32>)
      outs(%left_empty : tensor<2500xf32>) {
    ^bb0(%input: f32, %output: f32):
      %one = arith.constant 1.0 : f32
      %value = arith.addf %input, %one : f32
      linalg.yield %value : f32
    } -> tensor<2500xf32>
    %right_empty = tensor.empty() : tensor<2500xf32>
    %right = linalg.generic {
      indexing_maps = [#identity, #identity],
      iterator_types = ["parallel"],
      sculptor.semantic.layer_id = 202 : i64,
      sculptor.semantic.layer_kind = "fork_right"
    } ins(%producer : tensor<2500xf32>)
      outs(%right_empty : tensor<2500xf32>) {
    ^bb0(%input: f32, %output: f32):
      %two = arith.constant 2.0 : f32
      %value = arith.mulf %input, %two : f32
      linalg.yield %value : f32
    } -> tensor<2500xf32>
    %join_empty = tensor.empty() : tensor<2500xf32>
    %join = linalg.generic {
      indexing_maps = [#identity, #identity, #identity],
      iterator_types = ["parallel"],
      sculptor.semantic.layer_id = 203 : i64,
      sculptor.semantic.layer_kind = "fork_join"
    } ins(%left, %right : tensor<2500xf32>, tensor<2500xf32>)
      outs(%join_empty : tensor<2500xf32>) {
    ^bb0(%lhs: f32, %rhs: f32, %output: f32):
      %value = arith.addf %lhs, %rhs : f32
      linalg.yield %value : f32
    } -> tensor<2500xf32>
    return %join : tensor<2500xf32>
  }
}
