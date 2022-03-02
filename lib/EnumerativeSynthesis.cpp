// Copyright (c) 2020-present, author: Zhengyang Liu (liuz@cs.utah.edu).
// Distributed under the MIT license that can be found in the LICENSE file.
#include "EnumerativeSynthesis.h"
#include "ConstantSynthesis.h"
#include "IR.h"
#include "LLVMGen.h"
#include "MachineCost.h"
#include "Utils.h"

#include "Type.h"
#include "ir/globals.h"
#include "ir/instr.h"
#include "smt/smt.h"
#include "tools/transform.h"
#include "util/compiler.h"
#include "util/symexec.h"
#include "util/config.h"
#include "util/dataflow.h"
#include "util/version.h"
#include "llvm_util/llvm2alive.h"

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <iostream>
#include <queue>
#include <vector>
#include <set>
#include <map>

using namespace tools;
using namespace util;
using namespace std;
using namespace IR;

void calculateAndInitConstants(Transform &t);

namespace minotaur {

static void findInputs(llvm::Value *Root,
                       set<unique_ptr<Var>> &Cands,
                       set<unique_ptr<Addr>> &Pointers,
                       unsigned Max) {
  // breadth-first search
  unordered_set<llvm::Value *> Visited;
  queue<llvm::Value *> Q;
  Q.push(Root);

  while (!Q.empty()) {
    llvm::Value *V = Q.front();
    Q.pop();
    if (Visited.insert(V).second) {
      if (auto I = llvm::dyn_cast<llvm::Instruction>(V)) {
        for (auto &Op : I->operands()) {
          Q.push(Op);
        }
      }

      if (llvm::isa<llvm::Constant>(V))
        continue;
      if (V == Root)
        continue;
      if (V->getType()->isIntOrIntVectorTy())
        Cands.insert(make_unique<Var>(V));
      else if (V->getType()->isPointerTy())
        Pointers.insert(make_unique<Addr>(V));
      if (Cands.size() >= Max)
        return;
    }
  }
}

static bool getSketches(llvm::Value *V,
                        set<unique_ptr<Var>> &Inputs,
                        set<unique_ptr<Addr>> &Pointers,
                        vector<pair<unique_ptr<Inst>,
                        set<unique_ptr<ReservedConst>>>> &R) {
  vector<Inst*> Comps;
  for (auto &I : Inputs) {
    Comps.emplace_back(I.get());
  }

  auto RC1 = make_unique<ReservedConst>(type(-1, -1, false));
  Comps.emplace_back(RC1.get());

  // handle Memory in other function.
  if (V->getType()->isPointerTy())
    return true;

  unsigned expected = V->getType()->getPrimitiveSizeInBits();

  /*
  // Unary operators
  for (unsigned K = UnaryInst::Op::copy; K <= UnaryInst::Op::copy; ++K) {
    for (auto Op = Comps.begin(); Op != Comps.end(); ++Op) {
      set<unique_ptr<ReservedConst>> RCs;
      Inst *I = nullptr;
      if (dynamic_cast<ReservedConst *>(*Op)) {
        auto T = make_unique<ReservedConst>(expected);
        I = T.get();
        RCs.insert(move(T));
      } else if (dynamic_cast<Var *>(*Op)) {
        // TODO;
        continue;
      }
      UnaryInst::Op op = static_cast<UnaryInst::Op>(K);
      auto UO = make_unique<UnaryInst>(op, *I);
      R.push_back(make_pair(move(UO), move(RCs)));
    }
  }*/

  for (unsigned K = BinaryInst::Op::band; K <= BinaryInst::Op::mul; ++K) {
    BinaryInst::Op Op = static_cast<BinaryInst::Op>(K);
    for (auto Op0 = Comps.begin(); Op0 != Comps.end(); ++Op0) {
      auto Op1 = BinaryInst::isCommutative(Op) ? Op0 : Comps.begin();
      for (; Op1 != Comps.end(); ++Op1) {
        vector<type> tys;
        if (BinaryInst::isLaneIndependent(Op)) {
          tys.push_back(type(1, expected, false));
        } else {
          tys = type::getVectorTypes(expected);
        }
        for (auto workty : tys) {
          Inst *I = nullptr, *J = nullptr;
          set<unique_ptr<ReservedConst>> RCs;

          // (op rc, var)
          if (dynamic_cast<ReservedConst *>(*Op0)) {
            if (auto R = dynamic_cast<Var *>(*Op1)) {
              // ignore icmp temporarily
              if (R->getWidth() != expected)
                continue;
              auto T = make_unique<ReservedConst>(workty);
              I = T.get();
              RCs.insert(move(T));
              J = R;
              if (BinaryInst::isCommutative(Op)) {
                swap(I, J);
              }
            } else continue;
          }
          // (op var, rc)
          else if (dynamic_cast<ReservedConst *>(*Op1)) {
            if (auto L = dynamic_cast<Var *>(*Op0)) {
              // do not generate (- x 3) which can be represented as (+ x -3)
              if (Op == BinaryInst::Op::sub)
                continue;
              if (L->getWidth() != expected)
                continue;
              I = L;
              auto T = make_unique<ReservedConst>(workty);
              J = T.get();
              RCs.insert(move(T));
            } else continue;
          }
          // (op var, var)
          else {
            if (auto L = dynamic_cast<Var *>(*Op0)) {
              if (auto R = dynamic_cast<Var *>(*Op1)) {
                if (L->getWidth() != expected || R->getWidth() != expected)
                  continue;
              };
            };
            I = *Op0;
            J = *Op1;
          }
          auto BO = make_unique<BinaryInst>(Op, *I, *J, workty);
          R.push_back(make_pair(move(BO), move(RCs)));
        }
      }
    }
  }

  //icmps
  if (expected <= 64) {
    for (auto Op0 = Comps.begin(); Op0 != Comps.end(); ++Op0) {
      for (auto Op1 = Comps.begin(); Op1 != Comps.end(); ++Op1) {
        // skip (icmp rc, rc)
        if (dynamic_cast<ReservedConst*>(*Op0) &&
            dynamic_cast<ReservedConst*>(*Op1))
          continue;
        // skip (icmp rc, var)
        if (dynamic_cast<ReservedConst*>(*Op0) && dynamic_cast<Var*>(*Op1))
          continue;
        for (unsigned C = ICmpInst::Cond::eq; C <= ICmpInst::Cond::sle; ++C) {
          ICmpInst::Cond Cond = static_cast<ICmpInst::Cond>(C);
          set<unique_ptr<ReservedConst>> RCs;
          Inst *I = nullptr, *J = nullptr;

          if (auto L = dynamic_cast<Var*>(*Op0)) {
            if (L->getWidth() % expected)
              continue;
            // (icmp var, rc)
            if (dynamic_cast<ReservedConst*>(*Op1)) {
              if (Cond == ICmpInst::sle || Cond == ICmpInst::ule)
                continue;
              I = L;
              auto jty = type(expected, L->getWidth() / expected, false);
              auto T = make_unique<ReservedConst>(jty);
              J = T.get();
              RCs.insert(move(T));
            // (icmp var, var)
            } else if (auto R = dynamic_cast<Var*>(*Op1)) {
              if (L->getWidth() != R->getWidth())
                continue;
              I = *Op0;
              J = *Op1;
            } else UNREACHABLE();
          } else UNREACHABLE();
          auto BO = make_unique<ICmpInst>(Cond, *I, *J, expected);
          R.push_back(make_pair(move(BO), move(RCs)));
        }
      }
    }
  }

  // BinaryIntrinsics
  for (unsigned K = 0; K < X86IntrinBinOp::numOfX86Intrinsics; ++K) {
    // typecheck for return val
    X86IntrinBinOp::Op op = static_cast<X86IntrinBinOp::Op>(K);
    type ret_ty = type::getIntrinsicRetTy(op);
    type op0_ty = type::getIntrinsicOp0Ty(op);
    type op1_ty = type::getIntrinsicOp1Ty(op);

    if (ret_ty.getWidth() != expected)
      continue;

    for (auto Op0 = Comps.begin(); Op0 != Comps.end(); ++Op0) {
      for (auto Op1 = Comps.begin(); Op1 != Comps.end(); ++Op1) {
        if (dynamic_cast<ReservedConst *>(*Op0) && dynamic_cast<ReservedConst *>(*Op1))
          continue;

        Inst *I = nullptr;
        set<unique_ptr<ReservedConst>> RCs;

        if (auto L = dynamic_cast<Var *> (*Op0)) {
          // typecheck for op0
          if (L->getWidth() != op0_ty.getWidth())
            continue;
          I = L;
        } else if (dynamic_cast<ReservedConst *>(*Op0)) {
          auto T = make_unique<ReservedConst>(op0_ty);
          I = T.get();
          RCs.insert(move(T));
        }
        Inst *J = nullptr;
        if (auto R = dynamic_cast<Var *>(*Op1)) {
          // typecheck for op1
          if (R->getWidth() != op1_ty.getWidth())
            continue;
          J = R;
        } else if (dynamic_cast<ReservedConst *>(*Op1)) {
          auto T = make_unique<ReservedConst>(op1_ty);
          J = T.get();
          RCs.insert(move(T));
        }
        auto B = make_unique<SIMDBinOpInst>(op, *I, *J, expected);
        R.push_back(make_pair(move(B), move(RCs)));
      }
    }
  }
/*
  // shufflevector
  if (V->getType()->isVectorTy()) {
    for (auto Op0 = Comps.begin(); Op0 != Comps.end(); ++Op0) {
      for (auto Op1 = Comps.begin(); Op1 != Comps.end(); ++Op1) {
        auto vty = llvm::cast<llvm::VectorType>(V->getType());

        Inst *I = nullptr, *J = nullptr;
        set<unique_ptr<ReservedConst>> RCs;

        // (shufflevecttor rc, *, *), skip
        if (dynamic_cast<ReservedConst *>(*Op0)) {
            continue;
        }
        // (shufflevector var, rc, mask)
        else if (dynamic_cast<ReservedConst *>(*Op1)) {
          if (auto L = dynamic_cast<Var *>(*Op0)) {
            if (!L->V()->getType()->isVectorTy())
              continue;
            auto lvty = llvm::cast<llvm::VectorType>(L->V()->getType());
            if (lvty->getElementType() != vty->getElementType())
              continue;
            I = L;
            auto T = make_unique<ReservedConst>(L->V()->getType());
            J = T.get();
            RCs.insert(move(T));
          } else continue;
        }
        // (shufflevector, var, var, mask)
        else {
          if (auto L = dynamic_cast<Var *>(*Op0)) {
            if (auto R = dynamic_cast<Var *>(*Op1)) {
              if (L->getType() != R->getType())
                continue;
              if (!L->getType().isVector())
                continue;
              auto lvty = llvm::cast<llvm::VectorType>(L->V()->getType());
              if (lvty->getElementType() != vty->getElementType())
                continue;
            };
          };
          I = *Op0;
          J = *Op1;
        }
        auto mty = llvm::VectorType::get(
          llvm::Type::getInt32Ty(V->getContext()), vty->getElementCount());
        auto mask = make_unique<ReservedConst>(mty);
        auto SVI = make_unique<ShuffleVectorInst>(*I, *J, *mask.get());
        RCs.insert(move(mask));
        R.push_back(make_pair(move(SVI), move(RCs)));
      }
    }
  }
*/
/*

  for (auto &P : Pointers) {
    auto elemTy = P->getType();
    if (elemTy != expected)
      continue;
    set<unique_ptr<ReservedConst>> RCs;
    auto V = make_unique<Load>(*P);
    R.push_back(make_pair(move(V), move(RCs)));
  }*/
  return true;
}

static optional<smt::smt_initializer> smt_init;
static bool compareFunctions(IR::Function &Func1, IR::Function &Func2,
                             unsigned &goodCount,
                             unsigned &badCount, unsigned &errorCount) {
  TransformPrintOpts print_opts;
  smt_init->reset();
  Transform t;
  t.src = move(Func1);
  t.tgt = move(Func2);

  t.preprocess();
  t.tgt.syncDataWithSrc(t.src);
  calculateAndInitConstants(t);
  TransformVerify verifier(t, false);
  t.print(cout, print_opts);

  {
    auto types = verifier.getTypings();
    if (!types) {
      cerr << "Transformation doesn't verify!\n"
              "ERROR: program doesn't type check!\n\n";
      ++errorCount;
      return true;
    }
    assert(types.hasSingleTyping());
  }

  Errors errs = verifier.verify();
  bool result(errs);
  if (result) {
    if (errs.isUnsound()) {
      cerr << "Transformation doesn't verify!\n" << errs << endl;
      ++badCount;
    } else {
      cerr << errs << endl;
      ++errorCount;
    }
  } else {
    cerr << "Transformation seems to be correct!\n\n";
    ++goodCount;
  }

  return result;
}

static bool
constantSynthesis(IR::Function &Func1, IR::Function &Func2,
                  unsigned &goodCount, unsigned &badCount, unsigned &errorCount,
                  unordered_map<const IR::Value *, llvm::Argument *> &inputMap,
                  unordered_map<llvm::Argument *, llvm::Constant *> &constMap) {
  TransformPrintOpts print_opts;
  smt_init->reset();
  Transform t;
  t.src = move(Func1);
  t.tgt = move(Func2);

  t.preprocess();
  t.tgt.syncDataWithSrc(t.src);
  ::calculateAndInitConstants(t);

  ConstantSynthesis S(t);
  t.print(cout, print_opts);
  // assume type verifies
  std::unordered_map<const IR::Value *, smt::expr> result;
  Errors errs = S.synthesize(result);

  bool ret(errs);
  if (result.empty()) {
    llvm::errs()<<"failed to synthesize constants\n";
    return ret;
  }

  for (auto p : inputMap) {
    auto &ty = p.first->getType();
    auto lty = p.second->getType();

    if (ty.isIntType()) {
      // TODO, fix, do not use numeral_string()
      constMap[p.second] =
        llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(lty),
                               result[p.first].numeral_string(), 10);
    } else if (ty.isFloatType()) {
      //TODO
      UNREACHABLE();
    } else if (ty.isVectorType()) {
      auto trunk = result[p.first];
      llvm::FixedVectorType *vty = llvm::cast<llvm::FixedVectorType>(lty);
      llvm::IntegerType *ety =
        llvm::cast<llvm::IntegerType>(vty->getElementType());
      vector<llvm::Constant *> v;
      for (int i = vty->getElementCount().getKnownMinValue()-1; i >= 0; i --) {
        unsigned bits = ety->getBitWidth();
        auto elem = trunk.extract((i + 1) * bits - 1, i * bits);
        // TODO: support undef
        if (!elem.isConst())
          return ret;
        v.push_back(llvm::ConstantInt::get(ety, elem.numeral_string(), 10));
      }
      constMap[p.second] = llvm::ConstantVector::get(v);
    }
  }

  goodCount++;

  return ret;
}

static void removeUnusedDecls(unordered_set<llvm::Function *> IntrinsicDecls) {
  for (auto Intr : IntrinsicDecls) {
    if (Intr->isDeclaration() && Intr->use_empty()) {
      Intr->eraseFromParent();
    }
  }
}

unordered_map<llvm::Function*, unsigned> cost_cache;
unsigned get_cost_with_cache(llvm::Function *f) {
  if (cost_cache.contains(f)) {
    return cost_cache[f];
  } else {
    unsigned cost = get_machine_cost(f);
    cost_cache[f] = cost;
    return cost;
  }
}

bool mc_cmp(tuple<llvm::Function*, llvm::Function*, Inst*, bool> f1,
            tuple<llvm::Function*, llvm::Function*, Inst*, bool> f2) {
  return get_cost_with_cache(get<0>(f1)) < get_cost_with_cache(get<0>(f2));
}

bool synthesize(llvm::Function &F, llvm::TargetLibraryInfo *TLI) {
  unsigned origLatency = get_cost_with_cache(&F);
  config::disable_undef_input = true;
  config::disable_poison_input = true;
  config::src_unroll_cnt = 2;
  config::tgt_unroll_cnt = 2;

  bool changed = false;

  smt_init.emplace();
  Inst *R = nullptr;
  bool result = false;
  std::unordered_set<llvm::Function *> IntrinsicDecls;

  for (auto &BB : F) {
    auto T = BB.getTerminator();
    if(!llvm::isa<llvm::ReturnInst>(T))
      continue;
    llvm::Value *S = llvm::cast<llvm::ReturnInst>(T)->getReturnValue();
    if (!llvm::isa<llvm::Instruction>(S))
      continue;
    llvm::Instruction *I = cast<llvm::Instruction>(S);
    unordered_map<llvm::Argument *, llvm::Constant *> constMap;
    set<unique_ptr<Var>> Inputs;
    set<unique_ptr<Addr>> Pointers;
    findInputs(&*I, Inputs, Pointers, 20);

    vector<pair<unique_ptr<Inst>,set<unique_ptr<ReservedConst>>>> Sketches;

    // immediate constant synthesis
    if (!I->getType()->isPointerTy()) {
      set<unique_ptr<ReservedConst>> RCs;
      auto RC = make_unique<ReservedConst>(type(I->getType()));
      auto CI = make_unique<CopyInst>(*RC.get());
      RCs.insert(move(RC));
      Sketches.push_back(make_pair(move(CI), move(RCs)));
    }
    // nops
    {
      for (auto &V : Inputs) {
        auto vty = V->V()->getType();
        if (vty->isPointerTy())
          continue;
        if (V->getWidth() != I->getType()->getPrimitiveSizeInBits())
          continue;
        set<unique_ptr<ReservedConst>> RCs;
        auto VA = make_unique<Var>(V->V());
        Sketches.push_back(make_pair(move(VA), move(RCs)));
      }
    }
    getSketches(&*I, Inputs, Pointers, Sketches);

    cout<<"---------Sketches------------"<<endl;
    for (auto &Sketch : Sketches) {
      cout<<*Sketch.first<<endl;
    }
    cout<<"-----------------------------"<<endl;

    /*
    struct MCComparator {
      bool operator()(tuple<llvm::Function*, llvm::Function*, Inst*, bool>& a,
                      tuple<llvm::Function*, llvm::Function*, Inst*, bool> &b) {
        return get_cost_with_cache(get<0>(a)) > get_cost_with_cache(get<0>(b));
      }
    };
    struct IRComparator {
      bool operator()(tuple<llvm::Function*, llvm::Function*, Inst*, bool>& a,
                      tuple<llvm::Function*, llvm::Function*, Inst*, bool>& b) {
        return
          get<0>(a)->getInstructionCount() > get<0>(b)->getInstructionCount();
      }
    };*/

    unordered_map<string, llvm::Argument *> constants;
    unsigned CI = 0;
    /*
    priority_queue<tuple<llvm::Function*, llvm::Function*, Inst*, bool>,
                   vector<tuple<llvm::Function*, llvm::Function*, Inst*, bool>>,
                   MCComparator> Fns;*/
    vector<tuple<llvm::Function*, llvm::Function*, Inst*, bool>> Fns;
    auto FT = F.getFunctionType();
    // sketches -> llvm functions
    for (auto &Sketch : Sketches) {
      bool HaveC = !Sketch.second.empty();
      auto &G = Sketch.first;
      llvm::ValueToValueMapTy VMap;


      llvm::SmallVector<llvm::Type *, 8> Args;
      for (auto I: FT->params()) {
        Args.push_back(I);
      }

      for (auto &C : Sketch.second) {
        Args.push_back(C->getType().toLLVM(F.getContext()));
      }

      auto nFT =
        llvm::FunctionType::get(FT->getReturnType(), Args, FT->isVarArg());

      llvm::Function *Tgt =
        llvm::Function::Create(nFT, F.getLinkage(), F.getName(), F.getParent());

      llvm::SmallVector<llvm::ReturnInst *, 8> TgtReturns;
      llvm::Function::arg_iterator TgtArgI = Tgt->arg_begin();

      for (auto I = F.arg_begin(), E = F.arg_end(); I != E; ++I, ++TgtArgI) {
        VMap[I] = TgtArgI;
        TgtArgI->setName(I->getName());
      }

      // sketches with constants, duplicate F
      for (auto &C : Sketch.second) {
        string arg_name = "_reservedc_" + std::to_string(CI);
        TgtArgI->setName(arg_name);
        constants[arg_name] = TgtArgI;
        C->setA(TgtArgI);
        ++CI;
        ++TgtArgI;
      }

      llvm::CloneFunctionInto(Tgt, &F, VMap,
        llvm::CloneFunctionChangeType::LocalChangesOnly, TgtReturns);

      llvm::Function *Src;
      if (HaveC) {
        llvm::ValueToValueMapTy _vs;
        Src = llvm::CloneFunction(Tgt, _vs);
      } else {
        Src = &F;
      }

      llvm::Instruction *PrevI = llvm::cast<llvm::Instruction>(VMap[&*I]);
      llvm::Value *V =
         LLVMGen(PrevI, IntrinsicDecls).codeGen(G.get(), VMap, nullptr);
      V = llvm::IRBuilder<>(PrevI).CreateBitCast(V, PrevI->getType());
      PrevI->replaceAllUsesWith(V);

      eliminate_dead_code(*Tgt);
      /*if (Tgt->getInstructionCount() >= F.getInstructionCount()) {
        Tgt->dump();
        if (HaveC)
          Src->eraseFromParent();
        Tgt->eraseFromParent();
        llvm::errs()<<"foo\n";

        continue;
      }*/

      Fns.push_back(make_tuple(Tgt, Src, G.get(), !Sketch.second.empty()));
    }
    std::stable_sort(Fns.begin(), Fns.end(), mc_cmp);
    // llvm functions -> alive2 functions
    auto iter = Fns.begin();
    for (;iter != Fns.end();) {
      auto [Tgt, Src, G, HaveC] = *iter;
      iter = Fns.erase(iter);
      Tgt->dump();
      llvm::errs()<<"latency: " << get_cost_with_cache(Tgt);
      auto Func1 = llvm_util::llvm2alive(*Src, *TLI);
      auto Func2 = llvm_util::llvm2alive(*Tgt, *TLI);
      unsigned goodCount = 0, badCount = 0, errorCount = 0;
      if (!HaveC) {
        result |= compareFunctions(*Func1, *Func2,
                                    goodCount, badCount, errorCount);
      } else {
        unordered_map<const IR::Value *, llvm::Argument *> inputMap;
        for (auto &I : Func2->getInputs()) {
          string input_name = I.getName();
          // remove "%"
          input_name.erase(0, 1);
          if (constants.count(input_name)) {
            inputMap[&I] = constants[input_name];
          }
        }
        constMap.clear();
        result |= constantSynthesis(*Func1, *Func2,
                                    goodCount, badCount, errorCount,
                                    inputMap, constMap);

        Src->eraseFromParent();
      }
      Tgt->eraseFromParent();
      if (goodCount) {
        R = G;
        break;
      }
    }

    for (;iter != Fns.end(); ++iter) {
      auto &[Tgt, Src, G, HaveC] = *iter;
      (void) G;
      if (HaveC)
        Src->eraseFromParent();
      Tgt->eraseFromParent();
    }

    // replace
    if (R) {
      llvm::ValueToValueMapTy VMap;
      llvm::Value *V = LLVMGen(&*I, IntrinsicDecls).codeGen(R, VMap, &constMap);
      V = llvm::IRBuilder<>(I).CreateBitCast(V, I->getType());
      I->replaceAllUsesWith(V);
      eliminate_dead_code(F);
      changed = true;
      break;
    }
  }
  if (changed) {
    llvm::errs()<<"\n\n--successfully infered RHS--"<<"\n";
    llvm::errs()<<"previous latency: "<<origLatency<<"\n";
    llvm::errs()<<"optimized latency: "<<get_machine_cost(&F)<<"\n\n";
  }
  else
    llvm::errs()<<"\n\n--no solution found--\n\n";

  removeUnusedDecls(IntrinsicDecls);
  return changed;
}

};
