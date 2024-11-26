; CHECK: shufflevector <4 x i16> %1, <4 x i16> <i16 0, i16 3, i16 4, i16 0>, <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 5, i32 7, i32 6, i32 7>
define <4 x i32> @extractelement_out_of_range(<4 x i32> %x, i64 %i) {
  %f = trunc i64 %i to i32
  %g = lshr i64 %i, 32
  %h = trunc i64 %g to i32
  %b = insertelement <4 x i32> <i32 1, i32 2, i32 3, i32 4>, i32 %f, i32 0
  %c = insertelement <4 x i32> %b, i32 %h, i32 1
  ret <4 x i32> %c
}
