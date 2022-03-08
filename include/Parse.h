// Copyright (c) 2020-present, author: Zhengyang Liu (liuz@cs.utah.edu).
// Distributed under the MIT license that can be found in the LICENSE file.
#pragma once
#include "IR.h"
#include "llvm/IR/Function.h"

namespace minotaur {

Inst* parse_rewrite(const llvm::Function &F, std::string rewrite);

}