
// Copyright (c) 2020-present, author: Zhengyang Liu (liuz@cs.utah.edu).
// Distributed under the MIT license that can be found in the LICENSE file.
#include "codegen.h"
#include "expr.h"

#include "ir/instr.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <iostream>

using namespace std;
using namespace llvm;

namespace minotaur {

static constexpr
std::array<llvm::Intrinsic::ID, IR::X86IntrinBinOp::numOfX86Intrinsics> IntrinsicBinOpIDs = {
#define PROCESS(NAME,A,B,C,D,E,F) llvm::Intrinsic::NAME,
#include "ir/intrinsics_binop.h"
#undef PROCESS
};

static llvm::Intrinsic::ID getIntrinsicID(IR::X86IntrinBinOp::Op op) {
  return IntrinsicBinOpIDs[op];
}

static constexpr
std::array<llvm::Intrinsic::ID, IR::X86IntrinTerOp::numOfX86Intrinsics> IntrinsicTerOpIDs = {
#define PROCESS(NAME,A,B,C,D,E,F,G,H) llvm::Intrinsic::NAME,
#include "ir/intrinsics_terop.h"
#undef PROCESS
};

static llvm::Intrinsic::ID getIntrinsicID(IR::X86IntrinTerOp::Op op) {
  return IntrinsicTerOpIDs[op];
}

llvm::Value *LLVMGen::bitcastTo(llvm::Value *V, llvm::Type *to) {
  if (auto BC = dyn_cast<BitCastInst>(V)) {
    V = BC->getOperand(0);
  }
  return b.CreateBitCast(V, to);
}

llvm::Value*
LLVMGen::codeGenImpl(Inst *I, ValueToValueMapTy &VMap, ConstMap &CMap) {
  if (auto V = dynamic_cast<Var*>(I)) {
    if (VMap.empty()) {
      return V->V();
    } else {
      if (VMap.count(V->V()))
        return VMap[V->V()];
      else {
        cerr<<*V<<endl;
        llvm::report_fatal_error("Value is not found in VMap");
      }
    }
  } else if (auto RC = dynamic_cast<ReservedConst*>(I)) {
    if (CMap.count(RC)) {
      return CMap[RC];
    } else {
      return RC->getA();
    }
  } else if (auto U = dynamic_cast<UnaryInst*>(I)) {
    type workty = U->getWorkTy();
    auto op0 = codeGenImpl(U->Op0(), VMap, CMap);
    if(U->Op0()->getWidth() != workty.getWidth())
      report_fatal_error("operand width mismatch");
    op0 = bitcastTo(op0, workty.toLLVM(C));
    Intrinsic::ID iid = 0;
    auto K = U->K();
    switch (K) {
    case UnaryInst::bitreverse: iid = Intrinsic::bitreverse; break;
    case UnaryInst::bswap:      iid = Intrinsic::bswap;      break;
    case UnaryInst::ctpop:      iid = Intrinsic::ctpop;      break;
    case UnaryInst::ctlz:       iid = Intrinsic::ctlz;       break;
    case UnaryInst::cttz:       iid = Intrinsic::cttz;       break;
    }

    if (K == UnaryInst::ctlz || K == UnaryInst::cttz) {
      llvm::Function *F = Intrinsic::getDeclaration(M, iid, {workty.toLLVM(C), Type::getInt1Ty(C)});
      IntrinsicDecls.insert(F);
      return b.CreateCall(F, { op0, ConstantInt::getFalse(C) });
    } else {
      llvm::Function *F = Intrinsic::getDeclaration(M, iid, {workty.toLLVM(C)});
      IntrinsicDecls.insert(F);
      return b.CreateCall(F, op0);
    }
  } else if (auto U = dynamic_cast<CopyInst*>(I)) {
    auto op0 = codeGenImpl(U->Op0(), VMap, CMap);
    return op0;
  } else if (auto CI = dynamic_cast<ConversionInst*>(I)) {
    auto op0 = codeGenImpl(CI->V(), VMap, CMap);
    op0 = bitcastTo(op0, CI->getPrevTy().toLLVM(C));
    Type *new_type = CI->getNewTy().toLLVM(C);
    llvm::Value *r = nullptr;
    switch (CI->K()) {
    case ConversionInst::sext:
      r = b.CreateSExt(op0, new_type);
      break;
    case ConversionInst::zext:
      r = b.CreateZExt(op0, new_type);
      break;
    case ConversionInst::trunc:
      r = b.CreateTrunc(op0, new_type);
      break;
    }
    return r;
  } else if (auto B = dynamic_cast<BinaryInst*>(I)) {
    type workty = B->getWorkTy();
    auto op0 = codeGenImpl(B->L(), VMap, CMap);
    if(B->L()->getWidth() != workty.getWidth())
      report_fatal_error("left operand width mismatch");
    op0 = bitcastTo(op0, workty.toLLVM(C));

    auto op1 = codeGenImpl(B->R(), VMap, CMap);
    if(B->R()->getWidth() != workty.getWidth())
      report_fatal_error("left operand width mismatch");
    op1 = bitcastTo(op1, workty.toLLVM(C));

    llvm::Value *r = nullptr;
    switch (B->K()) {
    case BinaryInst::band:
      r = b.CreateAnd(op0, op1, "and");
      break;
    case BinaryInst::bor:
      r = b.CreateOr(op0, op1, "or");
      break;
    case BinaryInst::bxor:
      r = b.CreateXor(op0, op1, "xor");
      break;
    case BinaryInst::add:
      r = b.CreateAdd(op0, op1, "add");
      break;
    case BinaryInst::sub:
      r = b.CreateSub(op0, op1, "sub");
      break;
    case BinaryInst::mul:
      r = b.CreateMul(op0, op1, "mul");
      break;
    case BinaryInst::sdiv:
      r = b.CreateSDiv(op0, op1, "sdiv");
      break;
    case BinaryInst::udiv:
      r = b.CreateUDiv(op0, op1, "udiv");
      break;
    case BinaryInst::lshr:
      r = b.CreateLShr(op0, op1, "lshr");
      break;
    case BinaryInst::ashr:
      r = b.CreateAShr(op0, op1, "ashr");
      break;
    case BinaryInst::shl:
      r = b.CreateShl(op0, op1, "shl");
      break;
    default:
      UNREACHABLE();
    }
    return r;
  } else if (auto IC = dynamic_cast<ICmpInst*>(I)) {
    auto op0 = codeGenImpl(IC->L(), VMap, CMap);
    auto workty = type(IC->getWidth(), IC->L()->getWidth()/IC->getWidth(),
                       false);
    op0 = bitcastTo(op0, workty.toLLVM(C));

    auto op1 = codeGenImpl(IC->R(), VMap, CMap);
    op1 = bitcastTo(op1, workty.toLLVM(C));
    llvm::Value *r = nullptr;
    switch (IC->K()) {
    case ICmpInst::eq:
      r = b.CreateICmp(CmpInst::ICMP_EQ, op0, op1, "ieq");
      break;
    case ICmpInst::ne:
      r = b.CreateICmp(CmpInst::ICMP_NE, op0, op1, "ine");
      break;
    case ICmpInst::ult:
      r = b.CreateICmp(CmpInst::ICMP_ULT, op0, op1, "iult");
      break;
    case ICmpInst::ule:
      r = b.CreateICmp(CmpInst::ICMP_ULE, op0, op1, "iule");
      break;
    case ICmpInst::slt:
      r = b.CreateICmp(CmpInst::ICMP_SLT, op0, op1, "islt");
      break;
    case ICmpInst::sle:
      r = b.CreateICmp(CmpInst::ICMP_SLE, op0, op1, "isle");
      break;
    }
    return r;

  } else if (auto B = dynamic_cast<SIMDBinOpInst*>(I)) {
    type op0_ty = type::getIntrinsicOp0Ty(B->K());
    type op1_ty = type::getIntrinsicOp1Ty(B->K());
    auto op0 = codeGenImpl(B->L(), VMap, CMap);
    if(B->L()->getWidth() != op0_ty.getWidth())
      report_fatal_error("left operand width mismatch");
    op0 = bitcastTo(op0, op0_ty.toLLVM(C));

    auto op1 = codeGenImpl(B->R(), VMap, CMap);
    if(B->R()->getWidth() != op1_ty.getWidth())
      report_fatal_error("right operand width mismatch");
    op1 = bitcastTo(op1, op1_ty.toLLVM(C));

    llvm::Function *decl = Intrinsic::getDeclaration(M, getIntrinsicID(B->K()));
    IntrinsicDecls.insert(decl);

    llvm::Value *CI = CallInst::Create(decl, ArrayRef<llvm::Value *>({op0, op1}),
                                       "intr",
                                       cast<Instruction>(b.GetInsertPoint()));
    return CI;
  // TODO: handle terop
  } else if (auto FSV = dynamic_cast<FakeShuffleInst*>(I)) {
    auto op0 = codeGenImpl(FSV->L(), VMap, CMap);
    op0 = bitcastTo(op0, FSV->getInputTy().toLLVM(C));
    llvm::Value *op1 = nullptr;
    if (FSV->R()) {
      op1 = bitcastTo(codeGenImpl(FSV->R(), VMap, CMap), op0->getType());
    } else {
      op1 = llvm::PoisonValue::get(op0->getType());
    }

    auto mask = codeGenImpl(FSV->M(), VMap, CMap);
    llvm::Value *SV = nullptr;
    if (isa<Constant>(mask)) {
      ConstantVector *CV = cast<ConstantVector>(mask);
      auto zm = b.CreateZExt(mask,
        FixedVectorType::get(b.getInt32Ty(), CV->getType()->getNumElements()));
      SV = b.CreateShuffleVector(op0, op1, zm, "sv");
    } else {
      unsigned elem_bits = FSV->getElementBits();
      auto op_ty = type(FSV->L()->getWidth()/elem_bits, elem_bits, false);
      std::vector<llvm::Type*> Args(2, op_ty.toLLVM(C));
      Args.push_back(FSV->M()->getType().toLLVM(C));
      FunctionType *FT = FunctionType::get(FSV->getRetTy().toLLVM(C), Args, false);
      llvm::Function *F =
        llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "__fksv", M);
      IntrinsicDecls.insert(F);
      SV = b.CreateCall(F, { op0, op1, mask }, "sv");
    }

    return SV;
  }
  llvm::report_fatal_error("[ERROR] unknown instruction found in LLVMGen");
}

llvm::Value *LLVMGen::codeGen(Rewrite &R, ValueToValueMapTy &VMap) {
  return codeGenImpl(R.I, VMap, R.Consts);
}

}