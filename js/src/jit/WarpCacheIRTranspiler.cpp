/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/WarpCacheIRTranspiler.h"

#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"
#include "jit/CacheIROpsGenerated.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"
#include "jit/WarpSnapshot.h"

using namespace js;
using namespace js::jit;

// The CacheIR transpiler generates MIR from Baseline CacheIR.
class MOZ_RAII WarpCacheIRTranspiler {
  TempAllocator& alloc_;
  const CacheIRStubInfo* stubInfo_;
  const uint8_t* stubData_;
  TranspilerOutput& output_;

  // Vector mapping OperandId to corresponding MDefinition.
  MDefinitionStackVector operands_;

  MBasicBlock* current;

  TempAllocator& alloc() { return alloc_; }

  // CacheIR instructions writing to the IC's result register (the *Result
  // instructions) must call this to pass the corresponding MIR node back to
  // WarpBuilder.
  void setResult(MDefinition* result) {
    MOZ_ASSERT(!output_.result, "Can't have more than one result");
    output_.result = result;
  }

  MDefinition* getOperand(OperandId id) const { return operands_[id.id()]; }

  void setOperand(OperandId id, MDefinition* def) { operands_[id.id()] = def; }

  MOZ_MUST_USE bool defineOperand(OperandId id, MDefinition* def) {
    MOZ_ASSERT(id.id() == operands_.length());
    return operands_.append(def);
  }

  uintptr_t readStubWord(uint32_t offset) {
    return stubInfo_->getStubRawWord(stubData_, offset);
  }

  Shape* shapeStubField(uint32_t offset) {
    return reinterpret_cast<Shape*>(readStubWord(offset));
  }
  JSObject* objectStubField(uint32_t offset) {
    return reinterpret_cast<JSObject*>(readStubWord(offset));
  }
  int32_t int32StubField(uint32_t offset) {
    return static_cast<int32_t>(readStubWord(offset));
  }

  MOZ_MUST_USE bool emitGuardTo(ValOperandId inputId, MIRType type);

  template <typename T>
  MOZ_MUST_USE bool emitInt32BinaryArithResult(Int32OperandId lhsId,
                                               Int32OperandId rhsId);

  MInstruction* addBoundsCheck(MDefinition* index, MDefinition* length);

  CACHE_IR_TRANSPILER_GENERATED

 public:
  WarpCacheIRTranspiler(MIRGenerator& mirGen, MBasicBlock* current,
                        const WarpCacheIR* snapshot, TranspilerOutput& output)
      : alloc_(mirGen.alloc()),
        stubInfo_(snapshot->stubInfo()),
        stubData_(snapshot->stubData()),
        output_(output),
        current(current) {}

  MOZ_MUST_USE bool transpile(const MDefinitionStackVector& inputs);
};

bool WarpCacheIRTranspiler::transpile(const MDefinitionStackVector& inputs) {
  if (!operands_.appendAll(inputs)) {
    return false;
  }

  CacheIRReader reader(stubInfo_);
  do {
    CacheOp op = reader.readOp();
    switch (op) {
#define DEFINE_OP(op, ...)   \
  case CacheOp::op:          \
    if (!emit##op(reader)) { \
      return false;          \
    }                        \
    break;
      CACHE_IR_TRANSPILER_OPS(DEFINE_OP)
#undef DEFINE_OP

      default:
        fprintf(stderr, "Unsupported op: %s\n", CacheIROpNames[size_t(op)]);
        MOZ_CRASH("Unsupported op");
    }
  } while (reader.more());

  return true;
}

bool WarpCacheIRTranspiler::emitGuardClass(ObjOperandId objId,
                                           GuardClassKind kind) {
  MDefinition* def = getOperand(objId);

  const JSClass* classp = nullptr;
  switch (kind) {
    case GuardClassKind::Array:
      classp = &ArrayObject::class_;
      break;
    default:
      MOZ_CRASH("not yet supported");
  }

  auto* ins = MGuardToClass::New(alloc(), def, classp);
  current->add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardShape(ObjOperandId objId,
                                           uint32_t shapeOffset) {
  MDefinition* def = getOperand(objId);
  Shape* shape = shapeStubField(shapeOffset);

  auto* ins = MGuardShape::New(alloc(), def, shape, Bailout_ShapeGuard);
  current->add(ins);

  setOperand(objId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardTo(ValOperandId inputId, MIRType type) {
  MDefinition* def = getOperand(inputId);
  if (def->type() == type) {
    return true;
  }

  auto* ins = MUnbox::New(alloc(), def, type, MUnbox::Fallible);
  current->add(ins);

  setOperand(inputId, ins);
  return true;
}

bool WarpCacheIRTranspiler::emitGuardToObject(ValOperandId inputId) {
  return emitGuardTo(inputId, MIRType::Object);
}

bool WarpCacheIRTranspiler::emitGuardToString(ValOperandId inputId) {
  return emitGuardTo(inputId, MIRType::String);
}

bool WarpCacheIRTranspiler::emitGuardToInt32(ValOperandId inputId,
                                             Int32OperandId resultId) {
  MDefinition* input = getOperand(inputId);
  if (input->type() == MIRType::Int32) {
    return defineOperand(resultId, input);
  }

  auto* ins = MUnbox::New(alloc(), input, MIRType::Int32, MUnbox::Fallible);
  current->add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitGuardToInt32Index(ValOperandId inputId,
                                                  Int32OperandId resultId) {
  MDefinition* input = getOperand(inputId);
  auto* ins =
      MToNumberInt32::New(alloc(), input, IntConversionInputKind::NumbersOnly);
  current->add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitGuardToTypedArrayIndex(
    ValOperandId inputId, Int32OperandId resultId) {
  MDefinition* input = getOperand(inputId);
  auto* ins = MTypedArrayIndexToInt32::New(alloc(), input);
  current->add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitLoadInt32Result(Int32OperandId valId) {
  MDefinition* val = getOperand(valId);
  MOZ_ASSERT(val->type() == MIRType::Int32);
  setResult(val);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadObjectResult(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);
  MOZ_ASSERT(obj->type() == MIRType::Object);
  setResult(obj);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadEnclosingEnvironment(
    ObjOperandId objId, ObjOperandId resultId) {
  MDefinition* env = getOperand(objId);
  auto* ins = MEnclosingEnvironment::New(alloc(), env);
  current->add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitLoadObject(ObjOperandId resultId,
                                           uint32_t objOffset) {
  JSObject* obj = objectStubField(objOffset);

  auto* ins = MConstant::NewConstraintlessObject(alloc(), obj);
  current->add(ins);

  return defineOperand(resultId, ins);
}

bool WarpCacheIRTranspiler::emitLoadDynamicSlotResult(ObjOperandId objId,
                                                      uint32_t offsetOffset) {
  int32_t offset = int32StubField(offsetOffset);

  MDefinition* obj = getOperand(objId);
  size_t slotIndex = NativeObject::getDynamicSlotIndexFromOffset(offset);

  auto* slots = MSlots::New(alloc(), obj);
  current->add(slots);

  auto* load = MLoadSlot::New(alloc(), slots, slotIndex);
  current->add(load);

  setResult(load);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadFixedSlotResult(ObjOperandId objId,
                                                    uint32_t offsetOffset) {
  int32_t offset = int32StubField(offsetOffset);

  MDefinition* obj = getOperand(objId);
  uint32_t slotIndex = NativeObject::getFixedSlotIndexFromOffset(offset);

  auto* load = MLoadFixedSlot::New(alloc(), obj, slotIndex);
  current->add(load);

  setResult(load);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadEnvironmentFixedSlotResult(
    ObjOperandId objId, uint32_t offsetOffset) {
  int32_t offset = int32StubField(offsetOffset);

  MDefinition* obj = getOperand(objId);
  uint32_t slotIndex = NativeObject::getFixedSlotIndexFromOffset(offset);

  auto* load = MLoadFixedSlot::New(alloc(), obj, slotIndex);
  current->add(load);

  auto* lexicalCheck = MLexicalCheck::New(alloc(), load);
  current->add(lexicalCheck);

  setResult(lexicalCheck);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadEnvironmentDynamicSlotResult(
    ObjOperandId objId, uint32_t offsetOffset) {
  int32_t offset = int32StubField(offsetOffset);

  MDefinition* obj = getOperand(objId);
  size_t slotIndex = NativeObject::getDynamicSlotIndexFromOffset(offset);

  auto* slots = MSlots::New(alloc(), obj);
  current->add(slots);

  auto* load = MLoadSlot::New(alloc(), slots, slotIndex);
  current->add(load);

  auto* lexicalCheck = MLexicalCheck::New(alloc(), load);
  current->add(lexicalCheck);

  setResult(lexicalCheck);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadInt32ArrayLengthResult(ObjOperandId objId) {
  MDefinition* obj = getOperand(objId);

  auto* elements = MElements::New(alloc(), obj);
  current->add(elements);

  auto* length = MArrayLength::New(alloc(), elements);
  current->add(length);

  setResult(length);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadStringLengthResult(StringOperandId strId) {
  MDefinition* str = getOperand(strId);

  auto* length = MStringLength::New(alloc(), str);
  current->add(length);

  setResult(length);
  return true;
}

MInstruction* WarpCacheIRTranspiler::addBoundsCheck(MDefinition* index,
                                                    MDefinition* length) {
  MInstruction* check = MBoundsCheck::New(alloc(), index, length);
  current->add(check);

  if (JitOptions.spectreIndexMasking) {
    // Use a separate MIR instruction for the index masking. Doing this as
    // part of MBoundsCheck would be unsound because bounds checks can be
    // optimized or eliminated completely. Consider this:
    //
    //   for (var i = 0; i < x; i++)
    //        res = arr[i];
    //
    // If we can prove |x < arr.length|, we are able to eliminate the bounds
    // check, but we should not get rid of the index masking because the
    // |i < x| branch could still be mispredicted.
    //
    // Using a separate instruction lets us eliminate the bounds check
    // without affecting the index masking.
    check = MSpectreMaskIndex::New(alloc(), check, length);
    current->add(check);
  }

  return check;
}

bool WarpCacheIRTranspiler::emitLoadDenseElementResult(ObjOperandId objId,
                                                       Int32OperandId indexId) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  auto* elements = MElements::New(alloc(), obj);
  current->add(elements);

  auto* length = MInitializedLength::New(alloc(), elements);
  current->add(length);

  index = addBoundsCheck(index, length);

  bool needsHoleCheck = true;
  bool loadDouble = false;  // TODO: Ion-only optimization.
  auto* load =
      MLoadElement::New(alloc(), elements, index, needsHoleCheck, loadDouble);
  current->add(load);

  setResult(load);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadTypedArrayElementResult(
    ObjOperandId objId, Int32OperandId indexId, Scalar::Type elementType,
    bool handleOOB) {
  MDefinition* obj = getOperand(objId);
  MDefinition* index = getOperand(indexId);

  if (handleOOB) {
    bool allowDouble = true;
    auto* load = MLoadTypedArrayElementHole::New(alloc(), obj, index,
                                                 elementType, allowDouble);
    current->add(load);

    setResult(load);
    return true;
  }

  auto* length = MTypedArrayLength::New(alloc(), obj);
  current->add(length);

  index = addBoundsCheck(index, length);

  auto* elements = MTypedArrayElements::New(alloc(), obj);
  current->add(elements);

  auto* load = MLoadUnboxedScalar::New(alloc(), elements, index, elementType);
  // TODO: Uint32 always loaded as double.
  load->setResultType(MIRTypeForTypedArrayRead(elementType, true));
  current->add(load);

  setResult(load);
  return true;
}

bool WarpCacheIRTranspiler::emitLoadStringCharResult(StringOperandId strId,
                                                     Int32OperandId indexId) {
  MDefinition* str = getOperand(strId);
  MDefinition* index = getOperand(indexId);

  auto* length = MStringLength::New(alloc(), str);
  current->add(length);

  index = addBoundsCheck(index, length);

  auto* charCode = MCharCodeAt::New(alloc(), str, index);
  current->add(charCode);

  auto* fromCharCode = MFromCharCode::New(alloc(), charCode);
  current->add(fromCharCode);

  setResult(fromCharCode);
  return true;
}

bool WarpCacheIRTranspiler::emitInt32IncResult(Int32OperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* constOne = MConstant::New(alloc(), Int32Value(1));
  current->add(constOne);

  auto* ins = MAdd::New(alloc(), input, constOne, MIRType::Int32);
  current->add(ins);

  setResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitInt32DecResult(Int32OperandId inputId) {
  MDefinition* input = getOperand(inputId);

  auto* constOne = MConstant::New(alloc(), Int32Value(1));
  current->add(constOne);

  auto* ins = MSub::New(alloc(), input, constOne, MIRType::Int32);
  current->add(ins);

  setResult(ins);
  return true;
}

template <typename T>
bool WarpCacheIRTranspiler::emitInt32BinaryArithResult(Int32OperandId lhsId,
                                                       Int32OperandId rhsId) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = T::New(alloc(), lhs, rhs, MIRType::Int32);
  current->add(ins);

  setResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitInt32AddResult(Int32OperandId lhsId,
                                               Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MAdd>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32SubResult(Int32OperandId lhsId,
                                               Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MSub>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32BitOrResult(Int32OperandId lhsId,
                                                 Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MBitOr>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32BitXorResult(Int32OperandId lhsId,
                                                  Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MBitXor>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32BitAndResult(Int32OperandId lhsId,
                                                  Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MBitAnd>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32LeftShiftResult(Int32OperandId lhsId,
                                                     Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MLsh>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitInt32RightShiftResult(Int32OperandId lhsId,
                                                      Int32OperandId rhsId) {
  return emitInt32BinaryArithResult<MRsh>(lhsId, rhsId);
}

bool WarpCacheIRTranspiler::emitCompareInt32Result(JSOp op,
                                                   Int32OperandId lhsId,
                                                   Int32OperandId rhsId) {
  MDefinition* lhs = getOperand(lhsId);
  MDefinition* rhs = getOperand(rhsId);

  auto* ins = MCompare::New(alloc(), lhs, rhs, op);
  ins->setCompareType(MCompare::Compare_Int32);
  current->add(ins);

  setResult(ins);
  return true;
}

bool WarpCacheIRTranspiler::emitTypeMonitorResult() {
  MOZ_ASSERT(output_.result, "Didn't set result MDefinition");
  return true;
}

bool WarpCacheIRTranspiler::emitReturnFromIC() { return true; }

bool jit::TranspileCacheIRToMIR(MIRGenerator& mirGen, MBasicBlock* current,
                                const WarpCacheIR* snapshot,
                                const MDefinitionStackVector& inputs,
                                TranspilerOutput& output) {
  WarpCacheIRTranspiler transpiler(mirGen, current, snapshot, output);
  return transpiler.transpile(inputs);
}
