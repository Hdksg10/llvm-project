//===- bolt/Passes/FixRelocationPass.h --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the FixRelocations class, which rewrites ADRP/ADD
// relocations and synthesizes data relocations when needed.
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_FIXRELOCATIONPASS_H
#define BOLT_PASSES_FIXRELOCATIONPASS_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class FixRelocations : public BinaryFunctionPass {
  void runOnFunction(BinaryFunction &Function);
  void FixDataRelocation(BinaryContext &BC);

public:
  explicit FixRelocations(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}

  const char *getName() const override { return "fix-relocations"; }

  /// Pass entry point
  Error runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif

