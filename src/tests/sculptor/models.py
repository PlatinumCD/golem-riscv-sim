"""Small tensor models with independent numerical references for compiler migration."""
import json
import math


def activation(value, shape, kind):
    dims = ','.join(f'd{i}' for i in range(len(shape)))
    identity = f'affine_map<({dims})->({dims})>'
    typ = 'tensor<' + 'x'.join(map(str, shape)) + 'xf32>'
    body = '''%zero = arith.constant 0.0 : f32
      %positive = arith.cmpf ogt, %x, %zero : f32
      %v = arith.select %positive, %x, %zero : f32'''
    if kind == 'sigmoid':
        body = '''%one = arith.constant 1.0 : f32
      %negative = arith.negf %x : f32
      %e = math.exp %negative : f32
      %denominator = arith.addf %one, %e : f32
      %v = arith.divf %one, %denominator : f32'''
    return f'''%empty = tensor.empty() : {typ}
    %activated = linalg.generic {{indexing_maps=[{identity},{identity}],
      iterator_types=[{','.join('"parallel"' for _ in shape)}]}}
      ins({value}:{typ}) outs(%empty:{typ}) {{
      ^bb0(%x:f32,%unused:f32):
      {body}
      linalg.yield %v : f32
    }} -> {typ}
    return %activated : {typ}'''


def fixture(name, epochs):
    if name == 'conv_relu':
        weights = [[[[float((r * 5 + k) % 9 - 4) / 8 for k in range(y*3,y*3+3)] for y in range(3)]] for r in range(2)]
        inputs = [[float((i * 3 + epoch * 5) % 17 - 8) / 4 for i in range(25)] for epoch in range(epochs)]
        outputs = []
        for x in inputs:
            outputs.append([max(0., sum(weights[r][0][kh][kw]*x[(oh+kh)*5+ow+kw]
                               for kh in range(3) for kw in range(3)))
                            for r in range(2) for oh in range(3) for ow in range(3)])
        source = f'''module {{ func.func @forward(%input:tensor<1x1x5x5xf32>)->tensor<1x2x3x3xf32> {{
          %weights = arith.constant dense<{json.dumps(weights)}> : tensor<2x1x3x3xf32>
          %result = sculptor.nn.conv2d %input, %weights {{has_bias=false,
            stride=[1 : i64,1 : i64],padding=[0 : i64,0 : i64],dilation=[1 : i64,1 : i64]}}
            : (tensor<1x1x5x5xf32>,tensor<2x1x3x3xf32>)->tensor<1x2x3x3xf32>
          {activation('%result',(1,2,3,3),'relu')}
        }} }}'''
        return dict(source=source, inputs=inputs, expected=outputs,
                    array_rows=8,array_cols=8,arrays=2,mvms_per_input=18)
    batch, rows, cols = 2, 9, 13
    ar, ac = (8, 8) if name == 'linear_blocks' else (17, 19)
    kind = 'sigmoid' if name == 'linear_sigmoid' else 'relu'
    weights = [[float((r*3+c*5)%11-5)/8 for c in range(cols)] for r in range(rows)]
    inputs = [[float((i*7+epoch*3)%13-6)/4 for i in range(batch*cols)] for epoch in range(epochs)]
    outputs = []
    for x in inputs:
        y = [sum(weights[r][c]*x[b*cols+c] for c in range(cols)) for b in range(batch) for r in range(rows)]
        outputs.append([1/(1+math.exp(-v)) if kind=='sigmoid' else max(0.,v) for v in y])
    source = f'''module {{ func.func @forward(%input:tensor<{batch}x{cols}xf32>)->tensor<{batch}x{rows}xf32> {{
      %weights = arith.constant dense<{json.dumps(weights)}> : tensor<{rows}x{cols}xf32>
      %result = sculptor.nn.linear %input,%weights {{has_bias=false}}
        : (tensor<{batch}x{cols}xf32>,tensor<{rows}x{cols}xf32>)->tensor<{batch}x{rows}xf32>
      {activation('%result',(batch,rows),kind)}
    }} }}'''
    arrays = ((rows+ar-1)//ar)*((cols+ac-1)//ac)
    return dict(source=source,inputs=inputs,expected=outputs,array_rows=ar,array_cols=ac,
                arrays=arrays,mvms_per_input=batch*arrays)


CASES = ('linear_relu','linear_sigmoid','linear_blocks','conv_relu')
