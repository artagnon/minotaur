; CHECK: fptrunc float %a to half
define half @test7(float %a) nounwind {
  %y = fpext float %a to double
  %z = fptrunc double %y to half
  ret half %z
}
