; CHECK: fcmp ogt float %0, 0xB6A0000000000000
define i1 @src(float %0) {
if.end35:
  %1 = fmul float %0, 2.000000e+00
  %2 = fcmp oge float %1, 0.000000e+00
  ret i1 %2
}
