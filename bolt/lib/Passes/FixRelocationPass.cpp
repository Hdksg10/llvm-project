//===- bolt/Passes/FixRelocationPass.cpp ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/FixRelocationPass.h"
#include "bolt/Core/ParallelUtilities.h"
#include <stack>

using namespace llvm;

namespace llvm {
namespace bolt {

void FixRelocations::runOnFunction(BinaryFunction &BF) {
  BinaryContext &BC = BF.getBinaryContext();
  // Skip if function not in .text section
  if (BF.getOriginSection()->getName() != ".text") {
    BC.outs() << "Function " << BF.getOneName() << " not in .text section, skip\n";
    return;
  }
  // Skip if function name starts with "__kvm_nvhe_"
  if (BF.forEachName(
          [](StringRef Name) { return Name.starts_with("__kvm_nvhe_"); })
          .has_value()) {
    BC.outs() << "Function " << BF.getOneName() << " has __kvm_nvhe_, skip\n";
    return;
  }
  std::unordered_map<MCInst *, int64_t>
      ModifiedAdds; 
  BC.outs()<<BF.getOneName()<<"\n";
  for (BinaryBasicBlock &BB : BF) {
    for (auto I = BB.begin(); I != BB.end(); ++I) {
      MCInst &Adrp = *I;

      if (!BC.MIB->isADRP(Adrp) || !Adrp.getOperand(0).isReg() ||
          !Adrp.getOperand(1).isImm())
        continue;

      const uint64_t Offset = *BC.MIB->getOffset(Adrp);
      uint64_t instrBaseAddr = (BF.getAddress() + Offset) & 0xFFFFFFFFFFFFF000;
      int64_t Imm = Adrp.getOperand(1).getImm() * 4 * 1024;
      uint64_t baseAddr = instrBaseAddr + Imm;
      unsigned baseReg = Adrp.getOperand(0).getReg();

      bool findAdrpAddPattern = false;
      bool baseRegIsUsed = false;
      int64_t immValue = 0;
      MCSymbol *TargetSymbol = nullptr;

      std::stack<BinaryBasicBlock *> Stack;
      std::unordered_set<BinaryBasicBlock *> Visited;
      Stack.push(&BB);
      Visited.insert(&BB);

      while (!Stack.empty()) {
        BinaryBasicBlock *CurBB = Stack.top();
        Stack.pop();

        bool shouldSkip = false;
        bool shouldTerminate = false;
        // bool foundAddInBB = false;

        for (auto II = (CurBB == &BB) ? std::next(I) : CurBB->begin();
             II != CurBB->end(); ++II) {
          MCInst &Add = *II;

          if ((!BC.MIB->isAddXri(Add) || baseRegIsUsed) &&
              Add.getOperand(0).isReg() &&
              Add.getOperand(0).getReg() == baseReg) {
            shouldSkip = true;
            break;
          }

          if (!BC.MIB->isPseudo(Add) && BC.MIB->isAddXri(Add) &&
              Add.getOperand(0).isReg() && Add.getOperand(1).isReg() &&
              (Add.getOperand(2).isImm() || Add.getOperand(2).isExpr()) &&
              Add.getOperand(1).getReg() == baseReg) {

            int64_t addImm =
                BC.MIB->getAnnotationWithDefault<int64_t>(Add, "AddImm");

            if (findAdrpAddPattern && (addImm != immValue)) {
              continue;
            }

            BinaryFunction *TargetBF =
                BC.getBinaryFunctionAtAddress(baseAddr + addImm);
            BinaryFunction *IncludedBF =
                BC.getBinaryFunctionContainingAddress(baseAddr + addImm);

            if (TargetBF || (IncludedBF && !IncludedBF->isInConstantIsland(baseAddr + addImm))) {
              if (!findAdrpAddPattern) {
                findAdrpAddPattern = true;
                //BC.outs()<<"0x"<<Twine::utohexstr(baseAddr + addImm) <<":isInConstantIsland(Address):" <<IncludedBF->isInConstantIsland(baseAddr + addImm)<<"\n";
                immValue = addImm;
                TargetSymbol =
                    TargetBF ? TargetBF->getSymbol()
                             : BC.getOrCreateGlobalSymbol(baseAddr + addImm, "SYMat");
                int64_t Val;
                //bool hasTargetBF = (TargetBF != nullptr);
                //BC.outs()<<"hasTargetBF:"<< hasTargetBF <<";Adrp/Add Symbol:"<< TargetSymbol->getName()<<"\n";
                BC.MIB->replaceImmWithSymbolRef(
                    Adrp, TargetSymbol, 0, BC.Ctx.get(), Val,
                    ELF::R_AARCH64_ADR_PREL_PG_HI21);
              }
              if (TargetSymbol) {
                if (Add.getOperand(2).isImm()) {
                  int64_t Val;
                  BC.MIB->replaceImmWithSymbolRef(
                      Add, TargetSymbol, 0, BC.Ctx.get(), Val,
                      ELF::R_AARCH64_ADD_ABS_LO12_NC);
                  ModifiedAdds[&Add] = addImm;
                  if (Add.getOperand(0).getReg() == baseReg) {
                    baseRegIsUsed = true;
                  }
                } else {
                  const MCSymbol *OldSymbol = BC.MIB->getTargetSymbol(Add, 2);
                  if (OldSymbol != TargetSymbol) {
                    BC.outs() << "Skip conflicting ADD at 0x"
                              << Twine::utohexstr(*BC.MIB->getOffset(Add))
                              << " oldSym=" << OldSymbol->getName()
                              << " newSym=" << TargetSymbol->getName() << "\n";
                  }
                }
              }
            } else {
              shouldTerminate = true;
              break;
            }
          }
        }

        if (shouldTerminate)
          break;

        if (shouldSkip)
          continue;

        // if (!foundAddInBB) {
        for (BinaryBasicBlock *Succ : CurBB->successors()) {
          if (!Succ)
            continue;
          if (Visited.insert(Succ).second)
            Stack.push(Succ);
        }
        //}
      }

      if (!findAdrpAddPattern) {
        int64_t Val;
        uint64_t Addend1 = baseAddr;
        MCSymbol *Symbol1 =
            BC.registerNameAtAddress("__BOLT_zero_addr", 0, 0, 0);
        BC.MIB->replaceImmWithSymbolRef(Adrp, Symbol1, Addend1, BC.Ctx.get(),
                                        Val, ELF::R_AARCH64_ADR_PREL_PG_HI21);
      }
    }
  }

  for (BinaryBasicBlock &BB : BF) {
    for (auto II = BB.begin(); II != BB.end(); ++II) {
      MCInst &Instr = *II;
      if (BC.MIB->isADRP(Instr))
        BC.MIB->clearOffset(Instr);
      if (BC.MIB->isAddXri(Instr)) {
        BC.MIB->stripAnnotations(Instr);
        BC.MIB->clearOffset(Instr);
      }
    }
  }

  /*
  BC.outs() << "Modified ADD count: " << ModifiedAdds.size() << "\n";
  for (auto &P : ModifiedAdds) {
    MCInst *Add = P.first;
    int64_t Imm = P.second;
    BC.outs() << "Modified ADD Imm: " << Imm << " ";
    Add->dump_pretty(BC.outs(), BC.InstPrinter.get());
    BC.outs() << "\n";
  }
  */
}

void FixRelocations::FixDataRelocation(BinaryContext &BC){

  if (BC.isAArch64() && !BC.getUniqueSectionByName(".rela" + std::string(BC.getMainCodeSectionName()))){
    for (BinarySection &Section : BC.sections()) {
      auto Name = Section.getName();
      //if (Name != ".data.rel.ro")
      if (Name != ".init_array" && Name != ".fini_array")
        continue;
      //BC.outs() << "Found Section: " << Name << ";Address:"<<Twine::utohexstr(Section.getAddress()) <<";endAddress:"<< Twine::utohexstr(Section.getEndAddress())<< "\n";
      auto *InitBytes = reinterpret_cast<const uint8_t *>(Section.getContents().data());
      ArrayRef<uint8_t> SecBytes = ArrayRef<uint8_t>(InitBytes, Section.getSize()); 
      DataExtractor SECDE(SecBytes, BC.AsmInfo->isLittleEndian(), BC.AsmInfo->getCodePointerSize());
      //BC.printData(BC.outs(),BDBytes , Section.getAddress());
      uint64_t SecDataOffset = 0;
      while (SecDataOffset + 8 <= Section.getSize()) {
        uint64_t SecDataAddress = Section.getAddress() + SecDataOffset;
        uint64_t SecTargetValue = (uint64_t)SECDE.getUnsigned(&SecDataOffset, 8);
        BinaryFunction *SecTargetBF = BC.getBinaryFunctionAtAddress(SecTargetValue);
        if (SecTargetBF)
        {
          MCSymbol *SecTargetSymbol = SecTargetBF->getSymbol();
         BC.addRelocation(SecDataAddress, SecTargetSymbol, ELF::R_AARCH64_ABS64, 0,SecTargetValue);
        }
      }
    }
    for (const auto &[Address, BD] : BC.getBinaryData()) {
      // Filter out all symbols that are not vtables.
      //&& !BD->getName().starts_with("_ZN")

      //if (!BD->getName().starts_with("_ZTV") && !BD->getName().starts_with("_ZL") && !BD->getName().starts_with("_ZTCN"))
      //  continue;
      
      ErrorOr<BinarySection &> Section = BC.getSectionForAddress(Address);
      if (!Section)
        continue;
      if (Section->getName() != ".data.rel.ro" && Section->getName() != ".data" && Section->getName() != ".rodata")
        continue;
      //BC.outs()<<"Section:"<<Section->getName()<<";BD->getName():"<<BD->getName()<<"\n";
      auto *Bytes = reinterpret_cast<const uint8_t *>(Section->getContents().data());
      ArrayRef<uint8_t> BDBytes = ArrayRef<uint8_t>(Bytes + BD->getAddress() - Section->getAddress(), BD->getSize());
      //BC.printData(BC.outs(),BDBytes , BD->getAddress());
      DataExtractor DE(BDBytes, BC.AsmInfo->isLittleEndian(), BC.AsmInfo->getCodePointerSize());
      uint64_t DataOffset = 0;
      while (DataOffset + 8 <= BD->getSize()) {
        uint64_t dataAddress = BD->getAddress() + DataOffset;
        uint64_t targetValue = (uint64_t)DE.getUnsigned(&DataOffset, 8);
        BinaryFunction *TargetBF = BC.getBinaryFunctionAtAddress(targetValue);
        if (TargetBF)
        {
          ErrorOr<BinarySection &> TargetSection = BC.getSectionForAddress(targetValue);
          MCSymbol *TargetSymbol = TargetBF->getSymbol();
          BC.addRelocation(dataAddress, TargetSymbol, ELF::R_AARCH64_ABS64, 0,targetValue);
          //BC.outs()<<"BD->getName():"<<BD->getName()<<"\n";
          //BC.outs()<<"dataAddress:"<<Twine::utohexstr(dataAddress) <<";targetValue:"<< Twine::utohexstr(targetValue)<< ";";
          //BC.outs()<< "Section:"<<Section->getName() <<";TargetSection:"<<TargetSection->getName() <<";TargetSymbol->getName():"<< TargetSymbol->getName()<< "\n";
        }
      }
    }
  }
}

Error FixRelocations::runOnFunctions(BinaryContext &BC) {
  if (!BC.isAArch64())
    return Error::success();

  // ParallelUtilities::WorkFuncTy WorkFun = [&](BinaryFunction &BF) {
  //   runOnFunction(BF);
  // };

  for (auto &BFI : BC.getBinaryFunctions()) {
    BinaryFunction &Function = BFI.second;
    runOnFunction(Function);
  }
  FixDataRelocation(BC);
  // ParallelUtilities::runOnEachFunction(
  //     BC, ParallelUtilities::SchedulingPolicy::SP_INST_LINEAR, WorkFun,
  //     nullptr, "FixRelocations");
  // BC.HasRelocations = true;
  return Error::success();
}

} // namespace bolt
} // namespace llvm
