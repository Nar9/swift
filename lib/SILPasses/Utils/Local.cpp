//===--- Local.cpp - Functions that perform local SIL transformations. ---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===---------------------------------------------------------------------===//
#include "swift/SILPasses/Utils/Local.h"
#include "swift/SILAnalysis/Analysis.h"
#include "swift/SIL/CallGraph.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILModule.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Intrinsics.h"
#include <deque>

using namespace swift;

bool
swift::isSideEffectFree(BuiltinFunctionRefInst *FR) {

  // First, check if we are dealing with a swift builtin.
  const BuiltinInfo &BInfo = FR->getBuiltinInfo();
  if (BInfo.ID != BuiltinValueKind::None) {
    return BInfo.isReadNone();
  }

  // Second, specialcase llvm intrinsic.
  const IntrinsicInfo & IInfo = FR->getIntrinsicInfo();
  if (IInfo.ID != llvm::Intrinsic::not_intrinsic) {
    return ( (IInfo.hasAttribute(llvm::Attribute::ReadNone) ||
              IInfo.hasAttribute(llvm::Attribute::ReadOnly)) &&
            IInfo.hasAttribute(llvm::Attribute::NoUnwind) );
  }

  llvm_unreachable("All cases are covered.");
}

bool swift::isReadNone(BuiltinFunctionRefInst *FR) {
  // First, check if we are dealing with a swift builtin.
  const BuiltinInfo &BInfo = FR->getBuiltinInfo();
  if (BInfo.ID != BuiltinValueKind::None)
    return BInfo.isReadNone();

  // Second, specialcase llvm intrinsic.
  const IntrinsicInfo & IInfo = FR->getIntrinsicInfo();
  if (IInfo.ID != llvm::Intrinsic::not_intrinsic)
    return IInfo.hasAttribute(llvm::Attribute::ReadNone) &&
      IInfo.hasAttribute(llvm::Attribute::NoUnwind);

  llvm_unreachable("All cases are covered.");
}

/// \brief Perform a fast local check to see if the instruction is dead.
///
/// This routine only examines the state of the instruction at hand.
bool
swift::isInstructionTriviallyDead(SILInstruction *I) {
  if (!I->use_empty() || isa<TermInst>(I))
    return false;

  // We know that some calls do not have side effects.
  if (const ApplyInst *AI = dyn_cast<ApplyInst>(I)) {
    if (BuiltinFunctionRefInst *FR =
        dyn_cast<BuiltinFunctionRefInst>(AI->getCallee().getDef())) {
      return isSideEffectFree(FR);
    }
  }

  // condfail instructions that obviously can't fail are dead.
  if (auto *CFI = dyn_cast<CondFailInst>(I))
    if (auto *ILI = dyn_cast<IntegerLiteralInst>(CFI->getOperand()))
      if (!ILI->getValue())
        return true;

  // mark_uninitialized is never dead.
  if (isa<MarkUninitializedInst>(I))
    return false;
  
  if (!I->mayHaveSideEffects())
    return true;

  return false;
}

/// \brief For each of the given instructions, if they are dead delete them
/// along with their dead operands.
///
/// \param IA The instruction to be deleted.
/// \param Force If Force is set, don't check if the top level instructions
///        are considered dead - delete them regardless.
bool
swift::recursivelyDeleteTriviallyDeadInstructions(ArrayRef<SILInstruction*> IA,
                                                  bool Force) {
  // Delete these instruction and others that become dead after it's deleted.
  llvm::SmallPtrSet<SILInstruction*, 8> DeadInsts;
  for (auto I : IA) {
    // If the instruction is not dead or force is false, there is nothing to do.
    if (Force || isInstructionTriviallyDead(I))
      DeadInsts.insert(I);
  }
  llvm::SmallPtrSet<SILInstruction*, 8> NextInsts;
  while (!DeadInsts.empty()) {
    for (auto I : DeadInsts) {
      // Check if any of the operands will become dead as well.
      MutableArrayRef<Operand> Ops = I->getAllOperands();
      for (Operand &Op : Ops) {
        SILValue OpVal = Op.get();
        if (!OpVal) continue;

        // Remove the reference from the instruction being deleted to this
        // operand.
        Op.drop();

        // If the operand is an instruction that is only used by the instruction
        // being deleted, delete it.
        if (SILInstruction *OpValInst = dyn_cast<SILInstruction>(OpVal))
          if (!DeadInsts.count(OpValInst) &&
             isInstructionTriviallyDead(OpValInst))
            NextInsts.insert(OpValInst);
      }
    }

    for (auto I : DeadInsts) {
      // This will remove this instruction and all its uses.
      I->eraseFromParent();
    }

    NextInsts.swap(DeadInsts);
    NextInsts.clear();
  }

  return true;
}

/// \brief If the given instruction is dead, delete it along with its dead
/// operands.
///
/// \param I The instruction to be deleted.
/// \param Force If Force is set, don't check if the top level instruction is
///        considered dead - delete it regardless.
/// \return Returns true if any instructions were deleted.
bool
swift::recursivelyDeleteTriviallyDeadInstructions(SILInstruction *I,
                                                       bool Force) {
  return recursivelyDeleteTriviallyDeadInstructions(
           ArrayRef<SILInstruction*>(I), Force);
}

void swift::eraseUsesOfInstruction(SILInstruction *Inst) {
  for (auto UI : Inst->getUses()) {
    auto *User = UI->getUser();

    // If the instruction itself has any uses, recursively zap them so that
    // nothing uses this instruction.
    eraseUsesOfInstruction(User);

    // Walk through the operand list and delete any random instructions that
    // will become trivially dead when this instruction is removed.

    for (auto &Op : User->getAllOperands()) {
      if (auto *OpI = dyn_cast<SILInstruction>(Op.get().getDef())) {
        // Don't recursively delete the pointer we're getting in.
        if (OpI != Inst) {
          Op.drop();
          recursivelyDeleteTriviallyDeadInstructions(OpI);
        }
      }
    }

    User->eraseFromParent();
  }
}

void swift::bottomUpCallGraphOrder(SILModule *M,
                                   std::vector<SILFunction*> &order) {
  CallGraphSorter<SILFunction*> sorter;
  for (auto &Caller : *M)
    for (auto &BB : Caller)
      for (auto &I : BB)
        if (FunctionRefInst *FRI = dyn_cast<FunctionRefInst>(&I)) {
          SILFunction *Callee = FRI->getReferencedFunction();
          sorter.addEdge(&Caller, Callee);
        }

  sorter.sort(order);
}

void swift::replaceWithSpecializedFunction(ApplyInst *AI, SILFunction *NewF) {
  SILLocation Loc = AI->getLoc();
  ArrayRef<Substitution> Subst;

  SmallVector<SILValue, 4> Arguments;
  for (auto &Op : AI->getArgumentOperands()) {
    Arguments.push_back(Op.get());
  }

  SILBuilder Builder(AI);
  FunctionRefInst *FRI = Builder.createFunctionRef(Loc, NewF);

  ApplyInst *NAI =
      Builder.createApply(Loc, FRI, Arguments, AI->isTransparent());
  SILValue(AI, 0).replaceAllUsesWith(SILValue(NAI, 0));
  recursivelyDeleteTriviallyDeadInstructions(AI, true);
}

static bool operandEscapesApply(Operand *O) {
  auto *Apply = cast<ApplyInst>(O->getUser());

  auto Type = Apply->getSubstCalleeType();

  // TODO: We do not yet handle generics.
  if (Apply->hasSubstitutions() || Type->isPolymorphic())
    return true;

  // It's not a direct call? Bail out.
  auto Callee = Apply->getCallee();
  if (!isa<FunctionRefInst>(Callee))
    return true;

  auto *FRI = cast<FunctionRefInst>(Callee);

  // We don't have a function body to examine?
  SILFunction *F = FRI->getReferencedFunction();
  if (F->empty())
    return true;

  // The applied function is the first operand.
  auto ParamIndex = O->getOperandNumber() - 1;
  auto &Entry = F->front();
  auto *Box = Entry.getBBArg(ParamIndex);

  // Check the uses of the operand, but do not recurse down into other
  // apply instructions.
  return !canValueEscape(SILValue(Box), /* examineApply = */ false);
}

/// \brief Returns True if the operand or one of its users is captured.
static bool useCaptured(Operand *UI) {
  auto *User = UI->getUser();

  // These instructions do not cause the address to escape.
  if (isa<CopyAddrInst>(User) ||
      isa<LoadInst>(User) ||
      isa<ProtocolMethodInst>(User) ||
      isa<DebugValueInst>(User) ||
      isa<DebugValueAddrInst>(User))
    return false;

  if (auto *Store = dyn_cast<StoreInst>(User)) {
    if (Store->getDest() == UI->get())
      return false;
  } else if (auto *Assign = dyn_cast<AssignInst>(User)) {
    if (Assign->getDest() == UI->get())
      return false;
  }

  return true;
}

bool
swift::canValueEscape(SILValue V, bool examineApply) {
  for (auto UI : V.getUses()) {
    auto *User = UI->getUser();

    // These instructions do not cause the address to escape.
    if (!useCaptured(UI))
      continue;

    // These instructions only cause the value to escape if they are used in
    // a way that escapes.  Recursively check that the uses of the instruction
    // don't escape and collect all of the uses of the value.
    if (isa<StructElementAddrInst>(User) || isa<TupleElementAddrInst>(User) ||
        isa<ProjectExistentialInst>(User) || isa<OpenExistentialInst>(User) ||
        isa<MarkUninitializedInst>(User) || isa<AddressToPointerInst>(User) ||
        isa<PointerToAddressInst>(User)) {
      if (canValueEscape(User, examineApply))
        return true;
      continue;
    }

    if (auto apply = dyn_cast<ApplyInst>(User)) {
      // Applying a function does not cause the function to escape.
      if (UI->getOperandNumber() == 0)
        continue;

      // apply instructions do not capture the pointer when it is passed
      // indirectly
      if (apply->getSubstCalleeType()
          ->getInterfaceParameters()[UI->getOperandNumber()-1].isIndirect())
        continue;

      // Optionally drill down into an apply to see if the operand is
      // captured in or returned from the apply.
      if (examineApply && !operandEscapesApply(UI))
        continue;
    }

    // partial_apply instructions do not allow the pointer to escape
    // when it is passed indirectly, unless the partial_apply itself
    // escapes
    if (auto partialApply = dyn_cast<PartialApplyInst>(User)) {
      auto args = partialApply->getArguments();
      auto params = partialApply->getSubstCalleeType()
        ->getInterfaceParameters();
      params = params.slice(params.size() - args.size(), args.size());
      if (params[UI->getOperandNumber()-1].isIndirect()) {
        if (canValueEscape(User, examineApply))
          return true;
        continue;
      }
    }

    return true;
  }

  return false;
}

bool swift::hasUnboundGenericTypes(Type T) {
    return T.findIf([](Type type) ->bool {
      return isa<ArchetypeType>(type.getPointer());
    });
 }
