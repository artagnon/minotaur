// Copyright (c) 2020-present, author: Zhengyang Liu (liuz@cs.utah.edu).
// Distributed under the MIT license that can be found in the LICENSE file.
#include "util/compiler.h"
#include "util/sort.h"
#include <Slice.h>

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;
using namespace std;

static constexpr unsigned MAX_DEPTH = 5;
static constexpr unsigned DEBUG_LEVEL = 0;

using edgesTy = std::vector<std::unordered_set<unsigned>>;
// simple Tarjan topological sort ignoring loops
static vector<unsigned> top_sort(const edgesTy &edges) {
  vector<unsigned> sorted;
  vector<unsigned char> marked;
  marked.resize(edges.size());

  function<void(unsigned)> visit = [&](unsigned v) {
    if (marked[v])
      return;
    marked[v] = true;

    for (auto child : edges[v]) {
      visit(child);
    }
    sorted.emplace_back(v);
  };

  for (unsigned i = 1, e = edges.size(); i < e; ++i)
    visit(i);
  if (!edges.empty())
    visit(0);

  reverse(sorted.begin(), sorted.end());
  return sorted;
}

// place instructions within a basicblock with topology sort
static vector<Instruction*> schedule_insts(const vector<Instruction*> &iis) {
  edgesTy edges(iis.size());
  unordered_map<const Instruction*, unsigned> inst_map;

  unsigned i = 0;
  for (auto ii : iis) {
    inst_map.emplace(ii, i++);
  }

  i = 0;
  for (auto ii : iis) {
    for (auto &op : ii->operands()) {
      if (!isa<Instruction>(op))
        continue;
      auto dst_I = inst_map.find(cast<Instruction>(&op));
      if (dst_I != inst_map.end())
        edges[dst_I->second].emplace(i);
    }
    ++i;
  }

  i = 0;
  for (auto ii : iis) {
    unsigned j = 0;
    for (auto jj : iis) {
      if (isa<PHINode>(ii) && !isa<PHINode>(jj)) {
        edges[i].emplace(j);
      }
      ++j;
    }
    ++i;
  }

  vector<Instruction*> sorted_iis;
  sorted_iis.reserve(iis.size());
  for (auto v : top_sort(edges)) {
    sorted_iis.emplace_back(iis[v]);
  }

  assert(sorted_iis.size() == bbs.size());
  return sorted_iis;
}

namespace minotaur {

//  * if a external value is outside the loop, and it does not dominates v,
//    do not extract it
optional<reference_wrapper<Function>> Slice::extractExpr(Value &v) {
  if(DEBUG_LEVEL > 0) {
    llvm::errs() << ">>> slicing value " << v << ">>>\n";
  }

  assert(isa<Instruction>(&v) && "Expr to be extracted must be a Instruction");
  Instruction *vi = cast<Instruction>(&v);
  BasicBlock *vbb = vi->getParent();

  Loop *loopv = LI.getLoopFor(vbb);
  if (loopv) {
    if(DEBUG_LEVEL > 0) {
      llvm::errs() << "[INFO] value is in " << *loopv;
    }
    if (!loopv->isLoopSimplifyForm()) {
      // TODO: continue harvesting within loop boundary, even loop is not in
      // normal form.
      if(DEBUG_LEVEL > 0) {
        llvm::errs() << "[INFO] loop is not in normal form\n";
      }
      return nullopt;
    }
  }

  LLVMContext &ctx = m->getContext();
  unordered_set<Value *> visited;

  queue<pair<Value *, unsigned>> worklist;

  ValueToValueMapTy vmap;
  vector<Instruction *> insts;
  unordered_map<BasicBlock *, vector<Instruction *>> bb_insts;
  unordered_set<BasicBlock *> blocks;

  // set of predecessor bb a bb depends on
  unordered_map<BasicBlock *, unordered_set<BasicBlock *>> bb_deps;

  worklist.push({&v, 0});

  bool havePhi = false;
  // pass 1;
  // + duplicate instructions, leave the operands untouched
  // + if there are intrinsic calls, create declares in the new module
  // * if the def of a use is not copied, the use will be treated as unknown,
  //   we will create an function argument for the def and replace the use
  //   with the argument.
  while (!worklist.empty()) {
    auto &[w, depth] = worklist.front();
    worklist.pop();

    if (isa<LandingPadInst>(w))
      continue;

    if (!visited.insert(w).second)
      continue;

    if (Instruction *i = dyn_cast<Instruction>(w)) {
      // do not handle function operands.
      bool haveUnknownOperand = false;
      for (auto &op : i->operands()) {
        if (isa<ConstantExpr>(op)) {
          haveUnknownOperand = true;
          break;
        }
        auto ot = op->getType();
        if (ot->isPointerTy() && ot->getPointerElementType()->isFunctionTy()) {
          haveUnknownOperand = true;
          break;
        }
      }

      if (haveUnknownOperand) {
        continue;
      }


      // do not harvest instructions beyond loop boundry.
      BasicBlock *ibb = i->getParent();
      Loop *loopi = LI.getLoopFor(ibb);

      if (loopi != loopv)
        continue;

      // handle callsites
      if (CallInst *ci = dyn_cast<CallInst>(i)) {
        Function *callee = ci->getCalledFunction();
        if (!callee) {
          if(DEBUG_LEVEL > 0)
            llvm::errs() << "[INFO] indirect call found" << "\n";
          continue;
        }
        if (!callee->isIntrinsic()) {
          if(DEBUG_LEVEL > 0)
            llvm::errs() << "[INFO] unknown callee found "
                         << callee->getName() << "\n";
          continue;
        }
        FunctionCallee intrindecl =
            m->getOrInsertFunction(callee->getName(), callee->getFunctionType(),
                                   callee->getAttributes());

        vmap[callee] = intrindecl.getCallee();
      } else if (auto phi = dyn_cast<PHINode>(i)) {
        bool phiHasUnknownIncome = false;

        unsigned incomes = phi->getNumIncomingValues();
        for (unsigned i = 0; i < incomes; i ++) {
          BasicBlock *block = phi->getIncomingBlock(i);

          if (!isa<Instruction>(phi->getIncomingValue(i))) {
            phiHasUnknownIncome = true;
            break;
          }

          Loop *loopbb = LI.getLoopFor(block);
          if (loopbb != loopv) {
            phiHasUnknownIncome = true;
            break;
          }
        }

        // if a phi node has unknown income, do not harvest
        if (phiHasUnknownIncome) {
          if(DEBUG_LEVEL > 0) {
            llvm::errs()<<"[INFO]"<<*phi<<" has external income\n";
          }
          continue;
        }

        unsigned e = phi->getNumIncomingValues();
        for (unsigned pi = 0 ; pi != e ; ++pi) {
          auto vi = phi->getIncomingValue(pi);
          BasicBlock *income = phi->getIncomingBlock(pi);
          blocks.insert(income);
          if (!isa<Instruction>(vi))
            continue;

          BasicBlock *bb_i = cast<Instruction>(vi)->getParent();
          auto inc_pds = predecessors(income);
          if (find(inc_pds.begin(), inc_pds.end(), bb_i) != inc_pds.end())
             continue;
          bb_deps[income].insert(bb_i);
        }

        havePhi = true;
      }

      insts.push_back(i);
      bb_insts[ibb].push_back(i);

      // BB->getInstList().push_front(c);

      bool never_visited = blocks.insert(ibb).second;

      if (depth > MAX_DEPTH)
        continue;

      // add condition to worklist
      if (ibb != vbb && never_visited) {
        Instruction *term = ibb->getTerminator();
        if(!isa<BranchInst>(term))
          return nullopt;
        BranchInst *bi = cast<BranchInst>(term);
        if (bi->isConditional()) {
          if (Instruction *c = dyn_cast<Instruction>(bi->getCondition())) {
            BasicBlock *cbb = cast<Instruction>(c)->getParent();
            auto pds = predecessors(ibb);
            if (cbb != ibb && find(pds.begin(), pds.end(), cbb) == pds.end())
              bb_deps[ibb].insert(cbb);
            worklist.push({c, depth + 1});
          }
        }
      }

      for (auto &op : i->operands()) {
        if (!isa<Instruction>(op))
          continue;

        auto preds = predecessors(i->getParent());
        BasicBlock *bb_i = cast<Instruction>(op)->getParent();
        if (find(preds.begin(), preds.end(), bb_i) != preds.end())
          continue;

        bb_deps[i->getParent()].insert(bb_i);
        worklist.push({op, depth + 1});
      }
    } else {
      llvm::report_fatal_error("[ERROR] Unknown value:" + w->getName() + "\n");
    }
  }

  // if no instructions satisfied the criteria of cloning, return null.
  if (insts.empty()) {
    if(DEBUG_LEVEL > 0) {
      llvm::errs()<<"[INFO] no instruction can be harvested\n";
    }
    return nullopt;
  }

  // pass 2
  // + find missed intermidiate blocks
  // For example,
  /*
         S
        / \
       A   B
       |   |
       |   I
        \  /
         T
  */
  // Suppose an instruction in T uses values defined in A and B, if we harvest
  // values by simply backward-traversing def/use tree, Block I will be missed.
  // To solve this issue,  we identify all such missed block by searching.
  // TODO: better object management.
  for (auto &[bb, deps] : bb_deps) {
    unordered_set<Value *> visited;
    queue<pair<unordered_set<BasicBlock *>, BasicBlock *>> worklist;
    worklist.push({{bb}, bb});

    while (!worklist.empty()) {
      auto [path, ibb] = worklist.front();
      worklist.pop();

      if (deps.contains(ibb)) {
        blocks.insert(path.begin(), path.end());
        if(visited.insert(ibb).second) {
          path.clear();
          path.insert(ibb);
        } else {
          continue;
        }
      }

      for (BasicBlock *pred : predecessors(ibb)) {
        // do not allow loop
        if (path.count(pred))
          return nullopt;

        path.insert(pred);
        worklist.push({path, pred});
      }
    }
  }

  // FIXME: Do not handle switch for now
  for (BasicBlock *orig_bb : blocks) {
    Instruction *term = orig_bb->getTerminator();
    if (!isa<BranchInst>(term))
      return nullopt;
  }

  // clone instructions
  vector<Instruction *> cloned_insts;
  unordered_set<Value *> inst_set(insts.begin(), insts.end());
  for (auto inst : insts) {
    Instruction *c = inst->clone();
    vmap[inst] = c;
    c->setValueName(nullptr);
    SmallVector<std::pair<unsigned, MDNode *>, 8> ClonedMeta;
    c->getAllMetadata(ClonedMeta);
    for (size_t i = 0; i < ClonedMeta.size(); ++i) {
      c->setMetadata(ClonedMeta[i].first, NULL);
    }
    cloned_insts.push_back(c);
  }

  // pass 3
  // + duplicate blocks
  BasicBlock *sinkbb = BasicBlock::Create(ctx, "sink");
  new UnreachableInst(ctx, sinkbb);

  unordered_set<BasicBlock *> cloned_blocks;
  unordered_map<BasicBlock *, BasicBlock *> bmap;
  if (havePhi) {
    // pass 3.1.1;
    // + duplicate BB;
    for (BasicBlock *orig_bb : blocks) {
      BasicBlock *bb = BasicBlock::Create(ctx, orig_bb->getName());
      bmap[orig_bb] = bb;
      vmap[orig_bb] = bb;
      cloned_blocks.insert(bb);
    }

    // pass 3.1.2:
    // + put in instructions
    for (auto bis : bb_insts) {
      auto is = schedule_insts(bis.second);
      for (Instruction *inst : is) {
        if (isa<BranchInst>(inst))
          continue;
        bmap.at(bis.first)->getInstList().push_back(cast<Instruction>(vmap[inst]));
      }
    }
    // pass 3.1.2:
    // + wire branch
    for (BasicBlock *orig_bb : blocks) {
      if (orig_bb == vbb)
        continue;
      BranchInst *bi = cast<BranchInst>(orig_bb->getTerminator());

      BranchInst *cloned_bi = nullptr;
      if (bi->isConditional()) {
        BasicBlock *truebb = nullptr, *falsebb = nullptr;
        if(bmap.count(bi->getSuccessor(0)))
          truebb = bmap.at(bi->getSuccessor(0));
        else
          truebb = sinkbb;
        if(bmap.count(bi->getSuccessor(1)))
          falsebb = bmap.at(bi->getSuccessor(1));
        else
          falsebb = sinkbb;
        cloned_bi = BranchInst::Create(truebb, falsebb,
                                       bi->getCondition(), bmap.at(orig_bb));
      } else {
        cloned_bi =
            BranchInst::Create( bmap.at(bi->getSuccessor(0)), bmap.at(orig_bb));
      }
      insts.push_back(bi);
      cloned_insts.push_back(cloned_bi);
      //bb_insts[orig_bb].push_back(bi);
      vmap[bi] = cloned_bi;
    }

    // create ret
    ReturnInst *ret = ReturnInst::Create(ctx, vmap[&v]);
    bmap.at(vbb)->getInstList().push_back(ret);
  } else {
    // pass 3.2
    // + phi free
    BasicBlock *bb = BasicBlock::Create(ctx, "entry");
    auto is = schedule_insts(insts);
    for (auto inst : is) {
      bb->getInstList().push_back(cast<Instruction>(vmap[inst]));
    }
    ReturnInst *ret = ReturnInst::Create(ctx, vmap[&v]);
    bb->getInstList().push_back(ret);
    cloned_blocks.insert(bb);
  }

  // pass 4;
  // + remap the operands of duplicated instructions with vmap from pass 1
  // + if a operand value is unknown, reserve a function parameter for it
  SmallVector<Type *, 4> argTys;
  DenseMap<Value *, unsigned> argMap;
  unsigned idx = 0;
  for (auto &i : cloned_insts) {
    RemapInstruction(i, vmap, RF_IgnoreMissingLocals);
    for (auto &op : i->operands()) {
      if (isa<Argument>(op) || isa<GlobalVariable>(op)) {
        argTys.push_back(op->getType());
        argMap[op.get()] = idx++;
      } else if (isa<Constant>(op)) {
        continue;
      } else if (Instruction *op_i = dyn_cast<Instruction>(op)) {
        auto unknown = find(cloned_insts.begin(), cloned_insts.end(), op_i);
        if (unknown != cloned_insts.end())
          continue;
        if (argMap.count(op.get()))
          continue;

        argTys.push_back(op->getType());
        argMap[op.get()] = idx++;
      }
    }
  }
  // argument for switch
  argTys.push_back(Type::getInt8Ty(ctx));
  // create function
  auto func_name = "sliced_" + v.getName();
  Function *F = Function::Create(FunctionType::get(v.getType(), argTys, false),
                                 GlobalValue::ExternalLinkage, func_name, *m);

  // pass 5:
  // + replace the use of unknown value with the function parameter
  for (auto &i : cloned_insts) {
    for (auto &op : i->operands()) {
      if (argMap.count(op.get())) {
        op.set(F->getArg(argMap[op.get()]));
      }
    }
  }

  unordered_set<BasicBlock *> block_without_preds;
  for (auto block : cloned_blocks) {
    auto preds = predecessors(block);
    if (preds.empty()) {
      block_without_preds.insert(block);
    }
  }
  if (block_without_preds.size() == 0) {
    llvm::report_fatal_error("[ERROR] no entry block found");
  } if (block_without_preds.size() == 1) {
    BasicBlock *entry = *block_without_preds.begin();
    entry->insertInto(F);
    for (auto block : cloned_blocks) {
      if (block == entry)
        continue;
      block->insertInto(F);
    }
  } else {
    BasicBlock *entry =  BasicBlock::Create(ctx, "entry");
    SwitchInst *sw = SwitchInst::Create(F->getArg(idx), sinkbb, 1, entry);
    unsigned idx  = 0;
    for (BasicBlock *no_pred : block_without_preds) {
      sw->addCase(ConstantInt::get(IntegerType::get(ctx, 8), idx ++), no_pred);
    }
    entry->insertInto(F);
    for (auto block : cloned_blocks) {
      block->insertInto(F);
    }
  }
  sinkbb->insertInto(F);

  DominatorTree FDT = DominatorTree();
  FDT.recalculate(*F);
  auto FLI = new LoopInfoBase<BasicBlock, Loop>();
  FLI->analyze(FDT);

  // make sure sliced function is loop free.
  if (!FLI->empty())
    llvm::report_fatal_error("[ERROR] why a loop is generated?");

  // validate the created function
  string err;
  llvm::raw_string_ostream err_stream(err);
  bool illformed = llvm::verifyFunction(*F, &err_stream);
  if (illformed) {
    llvm::errs() << "[ERROR] found errors in the generated function\n";
    F->dump();
    llvm::errs() << err << "\n";
    llvm::report_fatal_error("illformed function generated");
  }
  if (DEBUG_LEVEL > 0) {
    llvm::errs() << "<<< end of %" << v.getName() << " <<<\n";
  }
  return optional<reference_wrapper<Function>>(*F);
}

} // namespace minotaur