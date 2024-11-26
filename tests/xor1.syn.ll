; CHECK: call i1 @llvm.ctlz.i1(i1 %C, i1 false)
define i1 @test5(i1 %C) {
  %V = select i1 %C, i1 false, i1 true
  ret i1 %V
}
