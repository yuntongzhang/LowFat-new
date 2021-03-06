//===-- llvm/CodeGen/GlobalISel/IRTranslator.h - IRTranslator ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
/// This file declares the IRTranslator pass.
/// This pass is responsible for translating LLVM IR into MachineInstr.
/// It uses target hooks to lower the ABI but aside from that, the pass
/// generated code is generic. This is the default translator used for
/// GlobalISel.
///
/// \todo Replace the comments with actual doxygen comments.
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_GLOBALISEL_IRTRANSLATOR_H
#define LLVM_CODEGEN_GLOBALISEL_IRTRANSLATOR_H

#include "Types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {
// Forward declarations.
class BasicBlock;
class CallLowering;
class Constant;
class Instruction;
class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class MachineRegisterInfo;
class TargetPassConfig;

// Technically the pass should run on an hypothetical MachineModule,
// since it should translate Global into some sort of MachineGlobal.
// The MachineGlobal should ultimately just be a transfer of ownership of
// the interesting bits that are relevant to represent a global value.
// That being said, we could investigate what would it cost to just duplicate
// the information from the LLVM IR.
// The idea is that ultimately we would be able to free up the memory used
// by the LLVM IR as soon as the translation is over.
class IRTranslator : public MachineFunctionPass {
public:
  static char ID;

private:
  /// Interface used to lower the everything related to calls.
  const CallLowering *CLI;
  /// Mapping of the values of the current LLVM IR function
  /// to the related virtual registers.
  ValueToVReg ValToVReg;
  // Constants are special because when we encounter one,
  // we do not know at first where to insert the definition since
  // this depends on all its uses.
  // Thus, we will insert the sequences to materialize them when
  // we know all their users.
  // In the meantime, just keep it in a set.
  // Note: Constants that end up as immediate in the related instructions,
  // do not appear in that map.
  SmallSetVector<const Constant *, 8> Constants;

  // N.b. it's not completely obvious that this will be sufficient for every
  // LLVM IR construct (with "invoke" being the obvious candidate to mess up our
  // lives.
  DenseMap<const BasicBlock *, MachineBasicBlock *> BBToMBB;

  // List of stubbed PHI instructions, for values and basic blocks to be filled
  // in once all MachineBasicBlocks have been created.
  SmallVector<std::pair<const PHINode *, MachineInstr *>, 4> PendingPHIs;

  /// Record of what frame index has been allocated to specified allocas for
  /// this function.
  DenseMap<const AllocaInst *, int> FrameIndices;

  /// Methods for translating form LLVM IR to MachineInstr.
  /// \see ::translate for general information on the translate methods.
  /// @{

  /// Translate \p Inst into its corresponding MachineInstr instruction(s).
  /// Insert the newly translated instruction(s) right where the CurBuilder
  /// is set.
  ///
  /// The general algorithm is:
  /// 1. Look for a virtual register for each operand or
  ///    create one.
  /// 2 Update the ValToVReg accordingly.
  /// 2.alt. For constant arguments, if they are compile time constants,
  ///   produce an immediate in the right operand and do not touch
  ///   ValToReg. Actually we will go with a virtual register for each
  ///   constants because it may be expensive to actually materialize the
  ///   constant. Moreover, if the constant spans on several instructions,
  ///   CSE may not catch them.
  ///   => Update ValToVReg and remember that we saw a constant in Constants.
  ///   We will materialize all the constants in finalize.
  /// Note: we would need to do something so that we can recognize such operand
  ///       as constants.
  /// 3. Create the generic instruction.
  ///
  /// \return true if the translation succeeded.
  bool translate(const Instruction &Inst);

  /// Materialize \p C into virtual-register \p Reg. The generic instructions
  /// performing this materialization will be inserted into the entry block of
  /// the function.
  ///
  /// \return true if the materialization succeeded.
  bool translate(const Constant &C, unsigned Reg);

  /// Translate an LLVM bitcast into generic IR. Either a COPY or a G_BITCAST is
  /// emitted.
  bool translateBitCast(const User &U, MachineIRBuilder &MIRBuilder);

  /// Translate an LLVM load instruction into generic IR.
  bool translateLoad(const User &U, MachineIRBuilder &MIRBuilder);

  /// Translate an LLVM store instruction into generic IR.
  bool translateStore(const User &U, MachineIRBuilder &MIRBuilder);

  bool translateMemcpy(const CallInst &CI, MachineIRBuilder &MIRBuilder);

  void getStackGuard(unsigned DstReg, MachineIRBuilder &MIRBuilder);

  bool translateOverflowIntrinsic(const CallInst &CI, unsigned Op,
                                  MachineIRBuilder &MIRBuilder);

  bool translateKnownIntrinsic(const CallInst &CI, Intrinsic::ID ID,
                               MachineIRBuilder &MIRBuilder);

  /// Translate call instruction.
  /// \pre \p U is a call instruction.
  bool translateCall(const User &U, MachineIRBuilder &MIRBuilder);

  bool translateInvoke(const User &U, MachineIRBuilder &MIRBuilder);

  bool translateLandingPad(const User &U, MachineIRBuilder &MIRBuilder);

  /// Translate one of LLVM's cast instructions into MachineInstrs, with the
  /// given generic Opcode.
  bool translateCast(unsigned Opcode, const User &U,
                     MachineIRBuilder &MIRBuilder);

  /// Translate static alloca instruction (i.e. one  of constant size and in the
  /// first basic block).
  bool translateStaticAlloca(const AllocaInst &Inst,
                             MachineIRBuilder &MIRBuilder);

  /// Translate a phi instruction.
  bool translatePHI(const User &U, MachineIRBuilder &MIRBuilder);

  /// Translate a comparison (icmp or fcmp) instruction or constant.
  bool translateCompare(const User &U, MachineIRBuilder &MIRBuilder);

  /// Translate an integer compare instruction (or constant).
  bool translateICmp(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateCompare(U, MIRBuilder);
  }

  /// Translate a floating-point compare instruction (or constant).
  bool translateFCmp(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateCompare(U, MIRBuilder);
  }


  /// Add remaining operands onto phis we've translated. Executed after all
  /// MachineBasicBlocks for the function have been created.
  void finishPendingPhis();

  /// Translate \p Inst into a binary operation \p Opcode.
  /// \pre \p U is a binary operation.
  bool translateBinaryOp(unsigned Opcode, const User &U,
                         MachineIRBuilder &MIRBuilder);

  /// Translate branch (br) instruction.
  /// \pre \p U is a branch instruction.
  bool translateBr(const User &U, MachineIRBuilder &MIRBuilder);

  bool translateSwitch(const User &U, MachineIRBuilder &MIRBuilder);

  bool translateExtractValue(const User &U, MachineIRBuilder &MIRBuilder);

  bool translateInsertValue(const User &U, MachineIRBuilder &MIRBuilder);

  bool translateSelect(const User &U, MachineIRBuilder &MIRBuilder);

  bool translateGetElementPtr(const User &U, MachineIRBuilder &MIRBuilder);

  /// Translate return (ret) instruction.
  /// The target needs to implement CallLowering::lowerReturn for
  /// this to succeed.
  /// \pre \p U is a return instruction.
  bool translateRet(const User &U, MachineIRBuilder &MIRBuilder);

  bool translateAdd(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_ADD, U, MIRBuilder);
  }
  bool translateSub(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_SUB, U, MIRBuilder);
  }
  bool translateAnd(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_AND, U, MIRBuilder);
  }
  bool translateMul(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_MUL, U, MIRBuilder);
  }
  bool translateOr(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_OR, U, MIRBuilder);
  }
  bool translateXor(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_XOR, U, MIRBuilder);
  }

  bool translateUDiv(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_UDIV, U, MIRBuilder);
  }
  bool translateSDiv(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_SDIV, U, MIRBuilder);
  }
  bool translateURem(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_UREM, U, MIRBuilder);
  }
  bool translateSRem(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_SREM, U, MIRBuilder);
  }
  bool translateAlloca(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateStaticAlloca(cast<AllocaInst>(U), MIRBuilder);
  }
  bool translateIntToPtr(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateCast(TargetOpcode::G_INTTOPTR, U, MIRBuilder);
  }
  bool translatePtrToInt(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateCast(TargetOpcode::G_PTRTOINT, U, MIRBuilder);
  }
  bool translateTrunc(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateCast(TargetOpcode::G_TRUNC, U, MIRBuilder);
  }
  bool translateFPTrunc(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateCast(TargetOpcode::G_FPTRUNC, U, MIRBuilder);
  }
  bool translateFPExt(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateCast(TargetOpcode::G_FPEXT, U, MIRBuilder);
  }
  bool translateFPToUI(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateCast(TargetOpcode::G_FPTOUI, U, MIRBuilder);
  }
  bool translateFPToSI(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateCast(TargetOpcode::G_FPTOSI, U, MIRBuilder);
  }
  bool translateUIToFP(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateCast(TargetOpcode::G_UITOFP, U, MIRBuilder);
  }
  bool translateSIToFP(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateCast(TargetOpcode::G_SITOFP, U, MIRBuilder);
  }
  bool translateUnreachable(const User &U, MachineIRBuilder &MIRBuilder) {
    return true;
  }
  bool translateSExt(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateCast(TargetOpcode::G_SEXT, U, MIRBuilder);
  }

  bool translateZExt(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateCast(TargetOpcode::G_ZEXT, U, MIRBuilder);
  }

  bool translateShl(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_SHL, U, MIRBuilder);
  }
  bool translateLShr(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_LSHR, U, MIRBuilder);
  }
  bool translateAShr(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_ASHR, U, MIRBuilder);
  }

  bool translateFAdd(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_FADD, U, MIRBuilder);
  }
  bool translateFSub(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_FSUB, U, MIRBuilder);
  }
  bool translateFMul(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_FMUL, U, MIRBuilder);
  }
  bool translateFDiv(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_FDIV, U, MIRBuilder);
  }
  bool translateFRem(const User &U, MachineIRBuilder &MIRBuilder) {
    return translateBinaryOp(TargetOpcode::G_FREM, U, MIRBuilder);
  }

  // Stubs to keep the compiler happy while we implement the rest of the
  // translation.
  bool translateIndirectBr(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateResume(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateCleanupRet(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateCatchRet(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateCatchSwitch(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateFence(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateAtomicCmpXchg(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateAtomicRMW(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateAddrSpaceCast(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateCleanupPad(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateCatchPad(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateUserOp1(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateUserOp2(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateVAArg(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateExtractElement(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateInsertElement(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }
  bool translateShuffleVector(const User &U, MachineIRBuilder &MIRBuilder) {
    return false;
  }

  /// @}

  // Builder for machine instruction a la IRBuilder.
  // I.e., compared to regular MIBuilder, this one also inserts the instruction
  // in the current block, it can creates block, etc., basically a kind of
  // IRBuilder, but for Machine IR.
  MachineIRBuilder CurBuilder;

  // Builder set to the entry block (just after ABI lowering instructions). Used
  // as a convenient location for Constants.
  MachineIRBuilder EntryBuilder;

  // The MachineFunction currently being translated.
  MachineFunction *MF;

  /// MachineRegisterInfo used to create virtual registers.
  MachineRegisterInfo *MRI;

  const DataLayout *DL;

  /// Current target configuration. Controls how the pass handles errors.
  const TargetPassConfig *TPC;

  // * Insert all the code needed to materialize the constants
  // at the proper place. E.g., Entry block or dominator block
  // of each constant depending on how fancy we want to be.
  // * Clear the different maps.
  void finalizeFunction();

  /// Get the VReg that represents \p Val.
  /// If such VReg does not exist, it is created.
  unsigned getOrCreateVReg(const Value &Val);

  /// Get the frame index that represents \p Val.
  /// If such VReg does not exist, it is created.
  int getOrCreateFrameIndex(const AllocaInst &AI);

  /// Get the alignment of the given memory operation instruction. This will
  /// either be the explicitly specified value or the ABI-required alignment for
  /// the type being accessed (according to the Module's DataLayout).
  unsigned getMemOpAlignment(const Instruction &I);

  /// Get the MachineBasicBlock that represents \p BB.
  /// If such basic block does not exist, it is created.
  MachineBasicBlock &getOrCreateBB(const BasicBlock &BB);


public:
  // Ctor, nothing fancy.
  IRTranslator();

  StringRef getPassName() const override { return "IRTranslator"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  // Algo:
  //   CallLowering = MF.subtarget.getCallLowering()
  //   F = MF.getParent()
  //   MIRBuilder.reset(MF)
  //   MIRBuilder.getOrCreateBB(F.getEntryBB())
  //   CallLowering->translateArguments(MIRBuilder, F, ValToVReg)
  //   for each bb in F
  //     MIRBuilder.getOrCreateBB(bb)
  //     for each inst in bb
  //       if (!translate(MIRBuilder, inst, ValToVReg, ConstantToSequence))
  //         report_fatal_error(???Don???t know how to translate input");
  //   finalize()
  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // End namespace llvm.
#endif
