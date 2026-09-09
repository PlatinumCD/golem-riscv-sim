#identity2 = affine_map<(d0, d1) -> (d0, d1)>
module {
  func.func @forward(%input: tensor<129x8xf32>) -> tensor<129x8xf32> {
    %even_source_empty = tensor.empty() : tensor<129x8xf32>
    %even_source = linalg.generic {
      indexing_maps = [#identity2, #identity2],
      iterator_types = ["parallel", "parallel"],
      sculptor.semantic.layer_id = 600 : i64,
      sculptor.semantic.layer_kind = "layout_even_source"
    } ins(%input : tensor<129x8xf32>)
      outs(%even_source_empty : tensor<129x8xf32>) {
    ^bb0(%value: f32, %output: f32):
      %one = arith.constant 1.0 : f32
      %shifted = arith.addf %value, %one : f32
      linalg.yield %shifted : f32
    } -> tensor<129x8xf32>
    %odd_source_empty = tensor.empty() : tensor<129x8xf32>
    %odd_source = linalg.generic {
      indexing_maps = [#identity2, #identity2],
      iterator_types = ["parallel", "parallel"],
      sculptor.semantic.layer_id = 601 : i64,
      sculptor.semantic.layer_kind = "layout_odd_source"
    } ins(%input : tensor<129x8xf32>)
      outs(%odd_source_empty : tensor<129x8xf32>) {
    ^bb0(%value: f32, %output: f32):
      %one = arith.constant 1.0 : f32
      %shifted = arith.addf %value, %one : f32
      linalg.yield %shifted : f32
    } -> tensor<129x8xf32>
    %even = tensor.extract_slice %even_source[0, 0] [129, 4] [1, 2]
      : tensor<129x8xf32> to tensor<129x4xf32>
    %odd = tensor.extract_slice %odd_source[0, 1] [129, 4] [1, 2]
      : tensor<129x8xf32> to tensor<129x4xf32>
    %interleaved = tensor.concat dim(1) %even, %odd
      : (tensor<129x4xf32>, tensor<129x4xf32>) -> tensor<129x8xf32>
    return %interleaved : tensor<129x8xf32>
  }
}
