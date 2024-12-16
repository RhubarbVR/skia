diagnostic(off, derivative_uniformity);
struct FSIn {
  @builtin(front_facing) sk_Clockwise: bool,
};
struct FSOut {
  @location(0) sk_FragColor: vec4<f32>,
};
struct _GlobalUniforms {
  src: vec4<f32>,
  dst: vec4<f32>,
};
@binding(0) @group(0) var<uniform> _globalUniforms: _GlobalUniforms;
fn _skslMain(_stageOut: ptr<function, FSOut>) {
  {
    (*_stageOut).sk_FragColor = vec4<f32>(((1.0 - _globalUniforms.src.w) * _globalUniforms.dst.xyz + (1.0 - _globalUniforms.dst.w) * _globalUniforms.src.xyz) + _globalUniforms.src.xyz * _globalUniforms.dst.xyz, _globalUniforms.src.w + (1.0 - _globalUniforms.src.w) * _globalUniforms.dst.w);
  }
}
@fragment fn main(_stageIn: FSIn) -> FSOut {
  var _stageOut: FSOut;
  _skslMain(&_stageOut);
  return _stageOut;
}