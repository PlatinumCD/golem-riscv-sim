#identity4 = affine_map<(d0, d1, d2, d3) -> (d0, d1, d2, d3)>
module {
  func.func @forward(%arg0: tensor<1x2x52x64xf32>)
      -> tensor<1x2x26x32xf32> {
    %producer_empty = tensor.empty() : tensor<1x2x52x64xf32>
    %producer = linalg.generic {
      indexing_maps = [#identity4, #identity4],
      iterator_types = ["parallel", "parallel", "parallel", "parallel"],
      sculptor.semantic.layer_id = 300 : i64,
      sculptor.semantic.layer_kind = "pool_source"
    } ins(%arg0 : tensor<1x2x52x64xf32>)
      outs(%producer_empty : tensor<1x2x52x64xf32>) {
    ^bb0(%input: f32, %output: f32):
      %one = arith.constant 1.0 : f32
      %value = arith.addf %input, %one : f32
      linalg.yield %value : f32
    } -> tensor<1x2x52x64xf32>
    %kernel = tensor.empty() : tensor<2x2xf32>
    %output_empty = tensor.empty() : tensor<1x2x26x32xf32>
    %negative_infinity = arith.constant 0xFF800000 : f32
    %initial = linalg.fill ins(%negative_infinity : f32)
      outs(%output_empty : tensor<1x2x26x32xf32>)
      -> tensor<1x2x26x32xf32>
    %pooled = linalg.pooling_nchw_max {
      dilations = dense<1> : vector<2xi64>,
      sculptor.semantic.layer_id = 301 : i64,
      sculptor.semantic.layer_kind = "pool_max",
      strides = dense<2> : vector<2xi64>
    } ins(%producer, %kernel : tensor<1x2x52x64xf32>, tensor<2x2xf32>)
      outs(%initial : tensor<1x2x26x32xf32>)
      -> tensor<1x2x26x32xf32>
    %final_empty = tensor.empty() : tensor<1x2x26x32xf32>
    %final = linalg.generic {
      indexing_maps = [#identity4, #identity4],
      iterator_types = ["parallel", "parallel", "parallel", "parallel"],
      sculptor.semantic.layer_id = 302 : i64,
      sculptor.semantic.layer_kind = "pool_result"
    } ins(%pooled : tensor<1x2x26x32xf32>)
      outs(%final_empty : tensor<1x2x26x32xf32>) {
    ^bb0(%input: f32, %output: f32):
      %one = arith.constant 1.0 : f32
      %value = arith.subf %input, %one : f32
      linalg.yield %value : f32
    } -> tensor<1x2x26x32xf32>
    return %final : tensor<1x2x26x32xf32>
  }
}
