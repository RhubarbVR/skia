diagnostic(off, derivative_uniformity);
struct FSIn {
  @builtin(front_facing) sk_Clockwise: bool,
  @builtin(position) sk_FragCoord: vec4<f32>,
};
struct FSOut {
  @location(0) sk_FragColor: vec4<f32>,
};
struct _GlobalUniforms {
  testInputs: vec4<f32>,
  colorGreen: vec4<f32>,
  colorRed: vec4<f32>,
};
@binding(0) @group(0) var<uniform> _globalUniforms: _GlobalUniforms;
fn _skslMain(coords: vec2<f32>) -> vec4<f32> {
  {
    var expected: vec4<f32> = vec4<f32>(-1.0, 0.0, 1.0, 1.0);
    let _skTemp0 = sign(_globalUniforms.testInputs.x);
    let _skTemp1 = sign(_globalUniforms.testInputs.xy);
    let _skTemp2 = sign(_globalUniforms.testInputs.xyz);
    let _skTemp3 = sign(_globalUniforms.testInputs);
    return select(_globalUniforms.colorRed, _globalUniforms.colorGreen, vec4<bool>((((((((_skTemp0 == expected.x) && all(_skTemp1 == expected.xy)) && all(_skTemp2 == expected.xyz)) && all(_skTemp3 == expected)) && (-1.0 == expected.x)) && all(vec2<f32>(-1.0, 0.0) == expected.xy)) && all(vec3<f32>(-1.0, 0.0, 1.0) == expected.xyz)) && all(vec4<f32>(-1.0, 0.0, 1.0, 1.0) == expected)));
  }
}
@fragment fn main(_stageIn: FSIn) -> FSOut {
  var _stageOut: FSOut;
  _stageOut.sk_FragColor = _skslMain(_stageIn.sk_FragCoord.xy);
  return _stageOut;
}
