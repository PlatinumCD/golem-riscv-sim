#identity = affine_map<(d0) -> (d0)>
module {
  func.func @forward(%arg0: tensor<2500xf32>) -> tensor<2500xf32> {
    %empty0 = tensor.empty() : tensor<2500xf32>
    %first = linalg.generic {
      indexing_maps = [#identity, #identity],
      iterator_types = ["parallel"],
      sculptor.semantic.layer_id = 100 : i64,
      sculptor.semantic.layer_kind = "pointwise_add"
    } ins(%arg0 : tensor<2500xf32>) outs(%empty0 : tensor<2500xf32>) {
    ^bb0(%input: f32, %output: f32):
      %one = arith.constant 1.0 : f32
      %value = arith.addf %input, %one : f32
      linalg.yield %value : f32
    } -> tensor<2500xf32>
    %empty1 = tensor.empty() : tensor<2500xf32>
    %second = linalg.generic {
      indexing_maps = [#identity, #identity],
      iterator_types = ["parallel"],
      sculptor.semantic.layer_id = 101 : i64,
      sculptor.semantic.layer_kind = "pointwise_multiply"
    } ins(%first : tensor<2500xf32>) outs(%empty1 : tensor<2500xf32>) {
    ^bb0(%input: f32, %output: f32):
      %two = arith.constant 2.0 : f32
      %value = arith.mulf %input, %two : f32
      linalg.yield %value : f32
    } -> tensor<2500xf32>
    return %second : tensor<2500xf32>
  }
}
