; CHECK: fmul float %t1, %t23
define half @test2(half %x, half %y) nounwind  {
  %t1 = fpext half %x to float
  %t23 = fpext half %y to float
  %t5 = fmul float %t1, %t23
  %t56 = fptrunc float %t5 to half
  ret half %t56
}
