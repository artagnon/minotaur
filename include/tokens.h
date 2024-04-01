// Copyright (c) 2020-present, author: Zhengyang Liu (liuz@cs.utah.edu).
// Distributed under the MIT license that can be found in the LICENSE file.

#include "lexer.h"
TOKEN(END)
TOKEN(COMMA)
TOKEN(LPAREN)
TOKEN(RPAREN)
TOKEN(LCURLY)
TOKEN(RCURLY)

TOKEN(COPY)

TOKEN(BITREVERSE)
TOKEN(BSWAP)
TOKEN(CTPOP)
TOKEN(CTLZ)
TOKEN(CTTZ)
TOKEN(FNEG)
TOKEN(FABS)
TOKEN(FCEIL)
TOKEN(FFLOOR)
TOKEN(FRINT)
TOKEN(FNEARBYINT)
TOKEN(FROUND)
TOKEN(FROUNDEVEN)
TOKEN(FTRUNC)

TOKEN(BAND)
TOKEN(BOR)
TOKEN(BXOR)
TOKEN(ADD)
TOKEN(SUB)
TOKEN(MUL)
TOKEN(SDIV)
TOKEN(UDIV)
TOKEN(LSHR)
TOKEN(ASHR)
TOKEN(SHL)
TOKEN(SMAX)
TOKEN(SMIN)
TOKEN(UMAX)
TOKEN(UMIN)
TOKEN(FADD)
TOKEN(FSUB)
TOKEN(FMUL)
TOKEN(FDIV)
TOKEN(FMAXNUM)
TOKEN(FMINNUM)
TOKEN(FMAXIMUM)
TOKEN(FMINIMUM)
TOKEN(COPYSIGN)

TOKEN(EQ)
TOKEN(NE)
TOKEN(ULT)
TOKEN(ULE)
TOKEN(UGT)
TOKEN(UGE)
TOKEN(SLT)
TOKEN(SLE)
TOKEN(SGT)
TOKEN(SGE)

TOKEN(FCMP_TRUE)
TOKEN(FCMP_OEQ)
TOKEN(FCMP_ONE)
TOKEN(FCMP_OLT)
TOKEN(FCMP_OLE)
TOKEN(FCMP_OGT)
TOKEN(FCMP_OGE)
TOKEN(FCMP_ORD)
TOKEN(FCMP_UEQ)
TOKEN(FCMP_UNE)
TOKEN(FCMP_ULT)
TOKEN(FCMP_ULE)
TOKEN(FCMP_UGT)
TOKEN(FCMP_UGE)
TOKEN(FCMP_UNO)
TOKEN(FCMP_FALSE)

TOKEN(SHUFFLE)
TOKEN(BLEND)
TOKEN(SELECT)
TOKEN(INSERTELEMENT)
TOKEN(EXTRACTELEMENT)

TOKEN(X86BINARY)

TOKEN(CONV_ZEXT)
TOKEN(CONV_SEXT)
TOKEN(CONV_TRUNC)

TOKEN(CONV_FPTRUNC)
TOKEN(CONV_FPEXT)
TOKEN(CONV_FPTOUI)
TOKEN(CONV_FPTOSI)
TOKEN(CONV_UITOFP)
TOKEN(CONV_SITOFP)

TOKEN(VAR)
TOKEN(CONST)
TOKEN(LITERAL)

TOKEN(NUM_STR)
TOKEN(BITS)

TOKEN(REGISTER)
TOKEN(INT_TYPE)
TOKEN(HALF)
TOKEN(FLOAT)
TOKEN(DOUBLE)
TOKEN(FP128)
TOKEN(VECTOR_TYPE_PREFIX)
TOKEN(CSGT)