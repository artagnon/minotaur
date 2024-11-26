; CHECK: call float @llvm.minimum.f32(float %x.ext, float %y.ext)
define half @reduce_precision(half %x, half %y) {
  %x.ext = fpext half %x to float
  %y.ext = fpext half %y to float
  %minimum = call float @llvm.minimum.f32(float %x.ext, float %y.ext)
  %trunc = fptrunc float %minimum to half
  ret half %trunc
}

declare float @llvm.minimum.f32(float, float)
