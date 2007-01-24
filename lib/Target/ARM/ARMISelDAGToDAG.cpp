//===-- ARMISelDAGToDAG.cpp - A dag to dag inst selector for ARM ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Chris Lattner and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the ARM target.
//
//===----------------------------------------------------------------------===//

#include "ARM.h"
#include "ARMISelLowering.h"
#include "ARMTargetMachine.h"
#include "ARMAddressingModes.h"
#include "llvm/CallingConv.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Intrinsics.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/SSARegMap.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Support/Debug.h"
#include <iostream>
using namespace llvm;

//===--------------------------------------------------------------------===//
/// ARMDAGToDAGISel - ARM specific code to select ARM machine
/// instructions for SelectionDAG operations.
///
namespace {
class ARMDAGToDAGISel : public SelectionDAGISel {
  ARMTargetLowering Lowering;

  /// Subtarget - Keep a pointer to the ARMSubtarget around so that we can
  /// make the right decision when generating code for different targets.
  const ARMSubtarget *Subtarget;

public:
  ARMDAGToDAGISel(ARMTargetMachine &TM)
    : SelectionDAGISel(Lowering), Lowering(TM),
    Subtarget(&TM.getSubtarget<ARMSubtarget>()) {
  }

  virtual const char *getPassName() const {
    return "ARM Instruction Selection";
  } 
  
  SDNode *Select(SDOperand Op);
  virtual void InstructionSelectBasicBlock(SelectionDAG &DAG);
  bool SelectAddrMode2(SDOperand Op, SDOperand N, SDOperand &Base,
                       SDOperand &Offset, SDOperand &Opc);
  bool SelectAddrMode2Offset(SDOperand Op, SDOperand N,
                             SDOperand &Offset, SDOperand &Opc);
  bool SelectAddrMode3(SDOperand Op, SDOperand N, SDOperand &Base,
                       SDOperand &Offset, SDOperand &Opc);
  bool SelectAddrMode3Offset(SDOperand Op, SDOperand N,
                             SDOperand &Offset, SDOperand &Opc);
  bool SelectAddrMode5(SDOperand Op, SDOperand N, SDOperand &Base,
                       SDOperand &Offset);

  bool SelectAddrModePC(SDOperand Op, SDOperand N, SDOperand &Offset,
                         SDOperand &Label);

  bool SelectThumbAddrModeRR(SDOperand Op, SDOperand N, SDOperand &Base,
                             SDOperand &Offset);
  bool SelectThumbAddrModeRI5(SDOperand Op, SDOperand N, unsigned Scale,
                              SDOperand &Base, SDOperand &Offset,
                              SDOperand &OffImm);
  bool SelectThumbAddrModeS1(SDOperand Op, SDOperand N, SDOperand &Base,
                             SDOperand &Offset, SDOperand &OffImm);
  bool SelectThumbAddrModeS2(SDOperand Op, SDOperand N, SDOperand &Base,
                             SDOperand &Offset, SDOperand &OffImm);
  bool SelectThumbAddrModeS4(SDOperand Op, SDOperand N, SDOperand &Base,
                             SDOperand &Offset, SDOperand &OffImm);
  bool SelectThumbAddrModeSP(SDOperand Op, SDOperand N, SDOperand &Base,
                             SDOperand &OffImm);

  bool SelectShifterOperandReg(SDOperand Op, SDOperand N, SDOperand &A,
                               SDOperand &B, SDOperand &C);
  
  // Include the pieces autogenerated from the target description.
#include "ARMGenDAGISel.inc"
};
}

void ARMDAGToDAGISel::InstructionSelectBasicBlock(SelectionDAG &DAG) {
  DEBUG(BB->dump());

  DAG.setRoot(SelectRoot(DAG.getRoot()));
  DAG.RemoveDeadNodes();

  ScheduleAndEmitDAG(DAG);
}

bool ARMDAGToDAGISel::SelectAddrMode2(SDOperand Op, SDOperand N,
                                      SDOperand &Base, SDOperand &Offset,
                                      SDOperand &Opc) {
  if (N.getOpcode() != ISD::ADD && N.getOpcode() != ISD::SUB) {
    Base = N;
    if (N.getOpcode() == ISD::FrameIndex) {
      int FI = cast<FrameIndexSDNode>(N)->getIndex();
      Base = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
    } else if (N.getOpcode() == ARMISD::Wrapper) {
      Base = N.getOperand(0);
    }
    Offset = CurDAG->getRegister(0, MVT::i32);
    Opc = CurDAG->getTargetConstant(ARM_AM::getAM2Opc(ARM_AM::add, 0,
                                                      ARM_AM::no_shift),
                                    MVT::i32);
    return true;
  }
  
  // Match simple R +/- imm12 operands.
  if (N.getOpcode() == ISD::ADD)
    if (ConstantSDNode *RHS = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      int RHSC = (int)RHS->getValue();
      if (RHSC >= 0 && RHSC < 0x1000) { // 12 bits.
        Base = N.getOperand(0);
        Offset = CurDAG->getRegister(0, MVT::i32);
        Opc = CurDAG->getTargetConstant(ARM_AM::getAM2Opc(ARM_AM::add, RHSC,
                                                          ARM_AM::no_shift),
                                        MVT::i32);
        return true;
      } else if (RHSC < 0 && RHSC > -0x1000) {
        Base = N.getOperand(0);
        Offset = CurDAG->getRegister(0, MVT::i32);
        Opc = CurDAG->getTargetConstant(ARM_AM::getAM2Opc(ARM_AM::sub, -RHSC,
                                                          ARM_AM::no_shift),
                                        MVT::i32);
        return true;
      }
    }
  
  // Otherwise this is R +/- [possibly shifted] R
  ARM_AM::AddrOpc AddSub = N.getOpcode() == ISD::ADD ? ARM_AM::add:ARM_AM::sub;
  ARM_AM::ShiftOpc ShOpcVal = ARM_AM::getShiftOpcForNode(N.getOperand(1));
  unsigned ShAmt = 0;
  
  Base   = N.getOperand(0);
  Offset = N.getOperand(1);
  
  if (ShOpcVal != ARM_AM::no_shift) {
    // Check to see if the RHS of the shift is a constant, if not, we can't fold
    // it.
    if (ConstantSDNode *Sh =
           dyn_cast<ConstantSDNode>(N.getOperand(1).getOperand(1))) {
      ShAmt = Sh->getValue();
      Offset = N.getOperand(1).getOperand(0);
    } else {
      ShOpcVal = ARM_AM::no_shift;
    }
  }
  
  // Try matching (R shl C) + (R).
  if (N.getOpcode() == ISD::ADD && ShOpcVal == ARM_AM::no_shift) {
    ShOpcVal = ARM_AM::getShiftOpcForNode(N.getOperand(0));
    if (ShOpcVal != ARM_AM::no_shift) {
      // Check to see if the RHS of the shift is a constant, if not, we can't
      // fold it.
      if (ConstantSDNode *Sh =
          dyn_cast<ConstantSDNode>(N.getOperand(0).getOperand(1))) {
        ShAmt = Sh->getValue();
        Offset = N.getOperand(0).getOperand(0);
        Base = N.getOperand(1);
      } else {
        ShOpcVal = ARM_AM::no_shift;
      }
    }
  }
  
  Opc = CurDAG->getTargetConstant(ARM_AM::getAM2Opc(AddSub, ShAmt, ShOpcVal),
                                  MVT::i32);
  return true;
}

bool ARMDAGToDAGISel::SelectAddrMode2Offset(SDOperand Op, SDOperand N,
                                            SDOperand &Offset, SDOperand &Opc) {
  unsigned Opcode = Op.getOpcode();
  ISD::MemIndexedMode AM = (Opcode == ISD::LOAD)
    ? cast<LoadSDNode>(Op)->getAddressingMode()
    : cast<StoreSDNode>(Op)->getAddressingMode();
  ARM_AM::AddrOpc AddSub = (AM == ISD::PRE_INC || AM == ISD::POST_INC)
    ? ARM_AM::add : ARM_AM::sub;
  if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(N)) {
    int Val = (int)C->getValue();
    if (Val >= 0 && Val < 0x1000) { // 12 bits.
      Offset = CurDAG->getRegister(0, MVT::i32);
      Opc = CurDAG->getTargetConstant(ARM_AM::getAM2Opc(AddSub, Val,
                                                        ARM_AM::no_shift),
                                      MVT::i32);
      return true;
    }
  }

  Offset = N;
  ARM_AM::ShiftOpc ShOpcVal = ARM_AM::getShiftOpcForNode(N);
  unsigned ShAmt = 0;
  if (ShOpcVal != ARM_AM::no_shift) {
    // Check to see if the RHS of the shift is a constant, if not, we can't fold
    // it.
    if (ConstantSDNode *Sh = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      ShAmt = Sh->getValue();
      Offset = N.getOperand(0);
    } else {
      ShOpcVal = ARM_AM::no_shift;
    }
  }

  Opc = CurDAG->getTargetConstant(ARM_AM::getAM2Opc(AddSub, ShAmt, ShOpcVal),
                                  MVT::i32);
  return true;
}


bool ARMDAGToDAGISel::SelectAddrMode3(SDOperand Op, SDOperand N,
                                      SDOperand &Base, SDOperand &Offset,
                                      SDOperand &Opc) {
  if (N.getOpcode() == ISD::SUB) {
    // X - C  is canonicalize to X + -C, no need to handle it here.
    Base = N.getOperand(0);
    Offset = N.getOperand(1);
    Opc = CurDAG->getTargetConstant(ARM_AM::getAM3Opc(ARM_AM::sub, 0),MVT::i32);
    return true;
  }
  
  if (N.getOpcode() != ISD::ADD) {
    Base = N;
    if (N.getOpcode() == ISD::FrameIndex) {
      int FI = cast<FrameIndexSDNode>(N)->getIndex();
      Base = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
    }
    Offset = CurDAG->getRegister(0, MVT::i32);
    Opc = CurDAG->getTargetConstant(ARM_AM::getAM3Opc(ARM_AM::add, 0),MVT::i32);
    return true;
  }
  
  // If the RHS is +/- imm8, fold into addr mode.
  if (ConstantSDNode *RHS = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
    int RHSC = (int)RHS->getValue();
    if (RHSC >= 0 && RHSC < 256) {
      Base = N.getOperand(0);
      Offset = CurDAG->getRegister(0, MVT::i32);
      Opc = CurDAG->getTargetConstant(ARM_AM::getAM3Opc(ARM_AM::add, RHSC),
                                      MVT::i32);
      return true;
    } else if (RHSC < 0 && RHSC > -256) { // note -256 itself isn't allowed.
      Base = N.getOperand(0);
      Offset = CurDAG->getRegister(0, MVT::i32);
      Opc = CurDAG->getTargetConstant(ARM_AM::getAM3Opc(ARM_AM::sub, -RHSC),
                                      MVT::i32);
      return true;
    }
  }
  
  Base = N.getOperand(0);
  Offset = N.getOperand(1);
  Opc = CurDAG->getTargetConstant(ARM_AM::getAM3Opc(ARM_AM::add, 0), MVT::i32);
  return true;
}

bool ARMDAGToDAGISel::SelectAddrMode3Offset(SDOperand Op, SDOperand N,
                                            SDOperand &Offset, SDOperand &Opc) {
  unsigned Opcode = Op.getOpcode();
  ISD::MemIndexedMode AM = (Opcode == ISD::LOAD)
    ? cast<LoadSDNode>(Op)->getAddressingMode()
    : cast<StoreSDNode>(Op)->getAddressingMode();
  ARM_AM::AddrOpc AddSub = (AM == ISD::PRE_INC || AM == ISD::POST_INC)
    ? ARM_AM::add : ARM_AM::sub;
  if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(N)) {
    int Val = (int)C->getValue();
    if (Val >= 0 && Val < 256) {
      Offset = CurDAG->getRegister(0, MVT::i32);
      Opc = CurDAG->getTargetConstant(ARM_AM::getAM3Opc(AddSub, Val), MVT::i32);
      return true;
    }
  }

  Offset = N;
  Opc = CurDAG->getTargetConstant(ARM_AM::getAM3Opc(AddSub, 0), MVT::i32);
  return true;
}


bool ARMDAGToDAGISel::SelectAddrMode5(SDOperand Op, SDOperand N,
                                      SDOperand &Base, SDOperand &Offset) {
  if (N.getOpcode() != ISD::ADD) {
    Base = N;
    if (N.getOpcode() == ISD::FrameIndex) {
      int FI = cast<FrameIndexSDNode>(N)->getIndex();
      Base = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
    } else if (N.getOpcode() == ARMISD::Wrapper) {
      Base = N.getOperand(0);
    }
    Offset = CurDAG->getTargetConstant(ARM_AM::getAM5Opc(ARM_AM::add, 0),
                                       MVT::i32);
    return true;
  }
  
  // If the RHS is +/- imm8, fold into addr mode.
  if (ConstantSDNode *RHS = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
    int RHSC = (int)RHS->getValue();
    if ((RHSC & 3) == 0) {  // The constant is implicitly multiplied by 4.
      RHSC >>= 2;
      if (RHSC >= 0 && RHSC < 256) {
        Base = N.getOperand(0);
        Offset = CurDAG->getTargetConstant(ARM_AM::getAM5Opc(ARM_AM::add, RHSC),
                                           MVT::i32);
        return true;
      } else if (RHSC < 0 && RHSC > -256) { // note -256 itself isn't allowed.
        Base = N.getOperand(0);
        Offset = CurDAG->getTargetConstant(ARM_AM::getAM5Opc(ARM_AM::sub,-RHSC),
                                           MVT::i32);
        return true;
      }
    }
  }
  
  Base = N;
  Offset = CurDAG->getTargetConstant(ARM_AM::getAM5Opc(ARM_AM::add, 0),
                                     MVT::i32);
  return true;
}

bool ARMDAGToDAGISel::SelectAddrModePC(SDOperand Op, SDOperand N,
                                        SDOperand &Offset, SDOperand &Label) {
  if (N.getOpcode() == ARMISD::PIC_ADD && N.hasOneUse()) {
    Offset = N.getOperand(0);
    SDOperand N1 = N.getOperand(1);
    Label  = CurDAG->getTargetConstant(cast<ConstantSDNode>(N1)->getValue(),
                                       MVT::i32);
    return true;
  }
  return false;
}

bool ARMDAGToDAGISel::SelectThumbAddrModeRR(SDOperand Op, SDOperand N,
                                            SDOperand &Base, SDOperand &Offset){
  if (N.getOpcode() != ISD::ADD) {
    Base = N;
    // We must materialize a zero in a reg! Returning an constant here won't
    // work since its node is -1 so it won't get added to the selection queue.
    // Explicitly issue a tMOVri8 node!
    Offset = SDOperand(CurDAG->getTargetNode(ARM::tMOVri8, MVT::i32,
                                    CurDAG->getTargetConstant(0, MVT::i32)), 0);
    return true;
  }

  Base = N.getOperand(0);
  Offset = N.getOperand(1);
  return true;
}

bool
ARMDAGToDAGISel::SelectThumbAddrModeRI5(SDOperand Op, SDOperand N,
                                        unsigned Scale, SDOperand &Base,
                                        SDOperand &Offset, SDOperand &OffImm) {
  if (Scale == 4) {
    SDOperand TmpBase, TmpOffImm;
    if (SelectThumbAddrModeSP(Op, N, TmpBase, TmpOffImm))
      return false;  // We want to select tLDRspi / tSTRspi instead.
  }

  if (N.getOpcode() != ISD::ADD) {
    Base = (N.getOpcode() == ARMISD::Wrapper) ? N.getOperand(0) : N;
    Offset = CurDAG->getRegister(0, MVT::i32);
    OffImm = CurDAG->getTargetConstant(0, MVT::i32);
    return true;
  }

  // If the RHS is + imm5 * scale, fold into addr mode.
  if (ConstantSDNode *RHS = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
    int RHSC = (int)RHS->getValue();
    if ((RHSC & (Scale-1)) == 0) {  // The constant is implicitly multiplied.
      RHSC /= Scale;
      if (RHSC >= 0 && RHSC < 32) {
        Base = N.getOperand(0);
        Offset = CurDAG->getRegister(0, MVT::i32);
        OffImm = CurDAG->getTargetConstant(RHSC, MVT::i32);
        return true;
      }
    }
  }

  Base = N.getOperand(0);
  Offset = N.getOperand(1);
  OffImm = CurDAG->getTargetConstant(0, MVT::i32);
  return true;
}

bool ARMDAGToDAGISel::SelectThumbAddrModeS1(SDOperand Op, SDOperand N,
                                            SDOperand &Base, SDOperand &Offset,
                                            SDOperand &OffImm) {
  return SelectThumbAddrModeRI5(Op, N, 1, Base, Offset, OffImm);
}

bool ARMDAGToDAGISel::SelectThumbAddrModeS2(SDOperand Op, SDOperand N,
                                            SDOperand &Base, SDOperand &Offset,
                                            SDOperand &OffImm) {
  return SelectThumbAddrModeRI5(Op, N, 2, Base, Offset, OffImm);
}

bool ARMDAGToDAGISel::SelectThumbAddrModeS4(SDOperand Op, SDOperand N,
                                            SDOperand &Base, SDOperand &Offset,
                                            SDOperand &OffImm) {
  return SelectThumbAddrModeRI5(Op, N, 4, Base, Offset, OffImm);
}

bool ARMDAGToDAGISel::SelectThumbAddrModeSP(SDOperand Op, SDOperand N,
                                           SDOperand &Base, SDOperand &OffImm) {
  if (N.getOpcode() == ISD::FrameIndex) {
    int FI = cast<FrameIndexSDNode>(N)->getIndex();
    Base = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
    OffImm = CurDAG->getTargetConstant(0, MVT::i32);
    return true;
  }

  if (N.getOpcode() == ISD::ADD &&
      N.getOperand(0).getOpcode() == ISD::FrameIndex) {
    // If the RHS is + imm8 * scale, fold into addr mode.
    if (ConstantSDNode *RHS = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      int RHSC = (int)RHS->getValue();
      if ((RHSC & 3) == 0) {  // The constant is implicitly multiplied.
        RHSC >>= 2;
        if (RHSC >= 0 && RHSC < 256) {
          int FI = cast<FrameIndexSDNode>(N.getOperand(0))->getIndex();
          Base = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
          OffImm = CurDAG->getTargetConstant(RHSC, MVT::i32);
          return true;
        }
      }
    }
  }
  
  return false;
}

bool ARMDAGToDAGISel::SelectShifterOperandReg(SDOperand Op,
                                              SDOperand N, 
                                              SDOperand &BaseReg,
                                              SDOperand &ShReg,
                                              SDOperand &Opc) {
  ARM_AM::ShiftOpc ShOpcVal = ARM_AM::getShiftOpcForNode(N);

  // Don't match base register only case. That is matched to a separate
  // lower complexity pattern with explicit register operand.
  if (ShOpcVal == ARM_AM::no_shift) return false;
  
  BaseReg = N.getOperand(0);
  unsigned ShImmVal = 0;
  if (ConstantSDNode *RHS = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
    ShReg = CurDAG->getRegister(0, MVT::i32);
    ShImmVal = RHS->getValue() & 31;
  } else {
    ShReg = N.getOperand(1);
  }
  Opc = CurDAG->getTargetConstant(ARM_AM::getSORegOpc(ShOpcVal, ShImmVal),
                                  MVT::i32);
  return true;
}


SDNode *ARMDAGToDAGISel::Select(SDOperand Op) {
  SDNode *N = Op.Val;
  unsigned Opcode = N->getOpcode();

  if (Opcode >= ISD::BUILTIN_OP_END && Opcode < ARMISD::FIRST_NUMBER)
    return NULL;   // Already selected.

  switch (N->getOpcode()) {
  default: break;
  case ISD::Constant: {
    unsigned Val = cast<ConstantSDNode>(N)->getValue();
    bool UseCP = true;
    if (Subtarget->isThumb())
      UseCP = (Val > 255 &&                          // MOV
               ~Val > 255 &&                         // MOV + MVN
               !ARM_AM::isThumbImmShiftedVal(Val));  // MOV + LSL
    else
      UseCP = (ARM_AM::getSOImmVal(Val) == -1 &&     // MOV
               ARM_AM::getSOImmVal(~Val) == -1 &&    // MVN
               !ARM_AM::isSOImmTwoPartVal(Val));     // two instrs.
    if (UseCP) {
      SDOperand CPIdx =
        CurDAG->getTargetConstantPool(ConstantInt::get(Type::Int32Ty, Val),
                                      TLI.getPointerTy());
      SDOperand Ops[] = {
        CPIdx, 
        CurDAG->getRegister(0, MVT::i32),
        CurDAG->getTargetConstant(0, MVT::i32),
        CurDAG->getEntryNode()
      };
      SDNode *ResNode = 
        CurDAG->getTargetNode(ARM::LDR, MVT::i32, MVT::Other, Ops, 4);
      ReplaceUses(Op, SDOperand(ResNode, 0));
      return NULL;
    }
      
    // Other cases are autogenerated.
    break;
  }
  case ISD::FrameIndex: {
    // Selects to ADDri FI, 0 which in turn will become ADDri SP, imm.
    int FI = cast<FrameIndexSDNode>(N)->getIndex();
    unsigned Opc = Subtarget->isThumb() ? ARM::tADDrSPi : ARM::ADDri;
    SDOperand TFI = CurDAG->getTargetFrameIndex(FI, TLI.getPointerTy());
    return CurDAG->SelectNodeTo(N, Opc, MVT::i32, TFI,
                                CurDAG->getTargetConstant(0, MVT::i32));
  }
  case ISD::MUL:
    if (Subtarget->isThumb())
      break;
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op.getOperand(1))) {
      unsigned RHSV = C->getValue();
      if (!RHSV) break;
      if (isPowerOf2_32(RHSV-1)) {  // 2^n+1?
        SDOperand V = Op.getOperand(0);
        AddToISelQueue(V);
        unsigned ShImm = ARM_AM::getSORegOpc(ARM_AM::lsl, Log2_32(RHSV-1));
        SDOperand Ops[] = { V, V, CurDAG->getRegister(0, MVT::i32),
          CurDAG->getTargetConstant(ShImm, MVT::i32)
        };
        return CurDAG->SelectNodeTo(N, ARM::ADDrs, MVT::i32, Ops, 4);
      }
      if (isPowerOf2_32(RHSV+1)) {  // 2^n-1?
        SDOperand V = Op.getOperand(0);
        AddToISelQueue(V);
        unsigned ShImm = ARM_AM::getSORegOpc(ARM_AM::lsl, Log2_32(RHSV+1));
        SDOperand Ops[] = { V, V, CurDAG->getRegister(0, MVT::i32),
          CurDAG->getTargetConstant(ShImm, MVT::i32)
        };
        return CurDAG->SelectNodeTo(N, ARM::RSBrs, MVT::i32, Ops, 4);
      }
    }
    break;
  case ARMISD::FMRRD:
    AddToISelQueue(Op.getOperand(0));
    return CurDAG->getTargetNode(ARM::FMRRD, MVT::i32, MVT::i32,
                                 Op.getOperand(0));
  case ARMISD::MULHILOU:
    AddToISelQueue(Op.getOperand(0));
    AddToISelQueue(Op.getOperand(1));
    return CurDAG->getTargetNode(ARM::UMULL, MVT::i32, MVT::i32,
                                 Op.getOperand(0), Op.getOperand(1));
  case ARMISD::MULHILOS:
    AddToISelQueue(Op.getOperand(0));
    AddToISelQueue(Op.getOperand(1));
    return CurDAG->getTargetNode(ARM::SMULL, MVT::i32, MVT::i32,
                                 Op.getOperand(0), Op.getOperand(1));
  case ISD::LOAD: {
    LoadSDNode *LD = cast<LoadSDNode>(Op);
    ISD::MemIndexedMode AM = LD->getAddressingMode();
    MVT::ValueType LoadedVT = LD->getLoadedVT();
    if (AM != ISD::UNINDEXED) {
      SDOperand Offset, AMOpc;
      bool isPre = (AM == ISD::PRE_INC) || (AM == ISD::PRE_DEC);
      unsigned Opcode = 0;
      bool Match = false;
      if (LoadedVT == MVT::i32 &&
          SelectAddrMode2Offset(Op, LD->getOffset(), Offset, AMOpc)) {
        Opcode = isPre ? ARM::LDR_PRE : ARM::LDR_POST;
        Match = true;
      } else if (LoadedVT == MVT::i16 &&
                 SelectAddrMode3Offset(Op, LD->getOffset(), Offset, AMOpc)) {
        Match = true;
        Opcode = (LD->getExtensionType() == ISD::SEXTLOAD)
          ? (isPre ? ARM::LDRSH_PRE : ARM::LDRSH_POST)
          : (isPre ? ARM::LDRH_PRE : ARM::LDRH_POST);
      } else if (LoadedVT == MVT::i8 || LoadedVT == MVT::i1) {
        if (LD->getExtensionType() == ISD::SEXTLOAD) {
          if (SelectAddrMode3Offset(Op, LD->getOffset(), Offset, AMOpc)) {
            Match = true;
            Opcode = isPre ? ARM::LDRSB_PRE : ARM::LDRSB_POST;
          }
        } else {
          if (SelectAddrMode2Offset(Op, LD->getOffset(), Offset, AMOpc)) {
            Match = true;
            Opcode = isPre ? ARM::LDRB_PRE : ARM::LDRB_POST;
          }
        }
      }

      if (Match) {
        SDOperand Chain = LD->getChain();
        SDOperand Base = LD->getBasePtr();
        AddToISelQueue(Chain);
        AddToISelQueue(Base);
        AddToISelQueue(Offset);
        SDOperand Ops[] = { Base, Offset, AMOpc, Chain };
        return CurDAG->getTargetNode(Opcode, MVT::i32, MVT::i32,
                                     MVT::Other, Ops, 4);
      }
    }
    // Other cases are autogenerated.
    break;
  }
  }

  return SelectCode(Op);
}

/// createARMISelDag - This pass converts a legalized DAG into a
/// ARM-specific DAG, ready for instruction scheduling.
///
FunctionPass *llvm::createARMISelDag(ARMTargetMachine &TM) {
  return new ARMDAGToDAGISel(TM);
}
