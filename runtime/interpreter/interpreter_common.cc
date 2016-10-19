/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "interpreter_common.h"

#include <cmath>

#include "base/enums.h"
#include "debugger.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "jit/jit.h"
#include "jvalue.h"
#include "method_handles.h"
#include "method_handles-inl.h"
#include "mirror/array-inl.h"
#include "mirror/class.h"
#include "mirror/method_handle_impl.h"
#include "reflection.h"
#include "reflection-inl.h"
#include "stack.h"
#include "unstarted_runtime.h"
#include "verifier/method_verifier.h"

namespace art {
namespace interpreter {

void ThrowNullPointerExceptionFromInterpreter() {
  ThrowNullPointerExceptionFromDexPC();
}

template<FindFieldType find_type, Primitive::Type field_type, bool do_access_check>
bool DoFieldGet(Thread* self, ShadowFrame& shadow_frame, const Instruction* inst,
                uint16_t inst_data) {
  const bool is_static = (find_type == StaticObjectRead) || (find_type == StaticPrimitiveRead);
  const uint32_t field_idx = is_static ? inst->VRegB_21c() : inst->VRegC_22c();
  ArtField* f =
      FindFieldFromCode<find_type, do_access_check>(field_idx, shadow_frame.GetMethod(), self,
                                                    Primitive::ComponentSize(field_type));
  if (UNLIKELY(f == nullptr)) {
    CHECK(self->IsExceptionPending());
    return false;
  }
  ObjPtr<Object> obj;
  if (is_static) {
    obj = f->GetDeclaringClass();
  } else {
    obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
    if (UNLIKELY(obj == nullptr)) {
      ThrowNullPointerExceptionForFieldAccess(f, true);
      return false;
    }
  }
  f->GetDeclaringClass()->AssertInitializedOrInitializingInThread(self);
  // Report this field access to instrumentation if needed.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldReadListeners())) {
    StackHandleScope<1> hs(self);
    // Wrap in handle wrapper in case the listener does thread suspension.
    HandleWrapperObjPtr<mirror::Object> h(hs.NewHandleWrapper(&obj));
    ObjPtr<Object> this_object;
    if (!f->IsStatic()) {
      this_object = obj;
    }
    instrumentation->FieldReadEvent(self,
                                    this_object.Ptr(),
                                    shadow_frame.GetMethod(),
                                    shadow_frame.GetDexPC(),
                                    f);
  }
  uint32_t vregA = is_static ? inst->VRegA_21c(inst_data) : inst->VRegA_22c(inst_data);
  switch (field_type) {
    case Primitive::kPrimBoolean:
      shadow_frame.SetVReg(vregA, f->GetBoolean(obj));
      break;
    case Primitive::kPrimByte:
      shadow_frame.SetVReg(vregA, f->GetByte(obj));
      break;
    case Primitive::kPrimChar:
      shadow_frame.SetVReg(vregA, f->GetChar(obj));
      break;
    case Primitive::kPrimShort:
      shadow_frame.SetVReg(vregA, f->GetShort(obj));
      break;
    case Primitive::kPrimInt:
      shadow_frame.SetVReg(vregA, f->GetInt(obj));
      break;
    case Primitive::kPrimLong:
      shadow_frame.SetVRegLong(vregA, f->GetLong(obj));
      break;
    case Primitive::kPrimNot:
      shadow_frame.SetVRegReference(vregA, f->GetObject(obj).Ptr());
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
      UNREACHABLE();
  }
  return true;
}

// Explicitly instantiate all DoFieldGet functions.
#define EXPLICIT_DO_FIELD_GET_TEMPLATE_DECL(_find_type, _field_type, _do_check) \
  template bool DoFieldGet<_find_type, _field_type, _do_check>(Thread* self, \
                                                               ShadowFrame& shadow_frame, \
                                                               const Instruction* inst, \
                                                               uint16_t inst_data)

#define EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(_find_type, _field_type)  \
    EXPLICIT_DO_FIELD_GET_TEMPLATE_DECL(_find_type, _field_type, false);  \
    EXPLICIT_DO_FIELD_GET_TEMPLATE_DECL(_find_type, _field_type, true);

// iget-XXX
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimBoolean)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimByte)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimChar)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimShort)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimInt)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimLong)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstanceObjectRead, Primitive::kPrimNot)

// sget-XXX
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimBoolean)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimByte)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimChar)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimShort)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimInt)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimLong)
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticObjectRead, Primitive::kPrimNot)

#undef EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL
#undef EXPLICIT_DO_FIELD_GET_TEMPLATE_DECL

// Handles iget-quick, iget-wide-quick and iget-object-quick instructions.
// Returns true on success, otherwise throws an exception and returns false.
template<Primitive::Type field_type>
bool DoIGetQuick(ShadowFrame& shadow_frame, const Instruction* inst, uint16_t inst_data) {
  Object* obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
  if (UNLIKELY(obj == nullptr)) {
    // We lost the reference to the field index so we cannot get a more
    // precised exception message.
    ThrowNullPointerExceptionFromDexPC();
    return false;
  }
  MemberOffset field_offset(inst->VRegC_22c());
  // Report this field access to instrumentation if needed. Since we only have the offset of
  // the field from the base of the object, we need to look for it first.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldReadListeners())) {
    ArtField* f = ArtField::FindInstanceFieldWithOffset(obj->GetClass(),
                                                        field_offset.Uint32Value());
    DCHECK(f != nullptr);
    DCHECK(!f->IsStatic());
    instrumentation->FieldReadEvent(Thread::Current(), obj, shadow_frame.GetMethod(),
                                    shadow_frame.GetDexPC(), f);
  }
  // Note: iget-x-quick instructions are only for non-volatile fields.
  const uint32_t vregA = inst->VRegA_22c(inst_data);
  switch (field_type) {
    case Primitive::kPrimInt:
      shadow_frame.SetVReg(vregA, static_cast<int32_t>(obj->GetField32(field_offset)));
      break;
    case Primitive::kPrimBoolean:
      shadow_frame.SetVReg(vregA, static_cast<int32_t>(obj->GetFieldBoolean(field_offset)));
      break;
    case Primitive::kPrimByte:
      shadow_frame.SetVReg(vregA, static_cast<int32_t>(obj->GetFieldByte(field_offset)));
      break;
    case Primitive::kPrimChar:
      shadow_frame.SetVReg(vregA, static_cast<int32_t>(obj->GetFieldChar(field_offset)));
      break;
    case Primitive::kPrimShort:
      shadow_frame.SetVReg(vregA, static_cast<int32_t>(obj->GetFieldShort(field_offset)));
      break;
    case Primitive::kPrimLong:
      shadow_frame.SetVRegLong(vregA, static_cast<int64_t>(obj->GetField64(field_offset)));
      break;
    case Primitive::kPrimNot:
      shadow_frame.SetVRegReference(vregA, obj->GetFieldObject<mirror::Object>(field_offset));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
      UNREACHABLE();
  }
  return true;
}

// Explicitly instantiate all DoIGetQuick functions.
#define EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(_field_type) \
  template bool DoIGetQuick<_field_type>(ShadowFrame& shadow_frame, const Instruction* inst, \
                                         uint16_t inst_data)

EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimInt);      // iget-quick.
EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimBoolean);  // iget-boolean-quick.
EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimByte);     // iget-byte-quick.
EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimChar);     // iget-char-quick.
EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimShort);    // iget-short-quick.
EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimLong);     // iget-wide-quick.
EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimNot);      // iget-object-quick.
#undef EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL

template<Primitive::Type field_type>
static JValue GetFieldValue(const ShadowFrame& shadow_frame, uint32_t vreg)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  JValue field_value;
  switch (field_type) {
    case Primitive::kPrimBoolean:
      field_value.SetZ(static_cast<uint8_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimByte:
      field_value.SetB(static_cast<int8_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimChar:
      field_value.SetC(static_cast<uint16_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimShort:
      field_value.SetS(static_cast<int16_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimInt:
      field_value.SetI(shadow_frame.GetVReg(vreg));
      break;
    case Primitive::kPrimLong:
      field_value.SetJ(shadow_frame.GetVRegLong(vreg));
      break;
    case Primitive::kPrimNot:
      field_value.SetL(shadow_frame.GetVRegReference(vreg));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
      UNREACHABLE();
  }
  return field_value;
}

template<FindFieldType find_type, Primitive::Type field_type, bool do_access_check,
         bool transaction_active>
bool DoFieldPut(Thread* self, const ShadowFrame& shadow_frame, const Instruction* inst,
                uint16_t inst_data) {
  bool do_assignability_check = do_access_check;
  bool is_static = (find_type == StaticObjectWrite) || (find_type == StaticPrimitiveWrite);
  uint32_t field_idx = is_static ? inst->VRegB_21c() : inst->VRegC_22c();
  ArtField* f =
      FindFieldFromCode<find_type, do_access_check>(field_idx, shadow_frame.GetMethod(), self,
                                                    Primitive::ComponentSize(field_type));
  if (UNLIKELY(f == nullptr)) {
    CHECK(self->IsExceptionPending());
    return false;
  }
  ObjPtr<Object> obj;
  if (is_static) {
    obj = f->GetDeclaringClass();
  } else {
    obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
    if (UNLIKELY(obj == nullptr)) {
      ThrowNullPointerExceptionForFieldAccess(f, false);
      return false;
    }
  }
  f->GetDeclaringClass()->AssertInitializedOrInitializingInThread(self);
  uint32_t vregA = is_static ? inst->VRegA_21c(inst_data) : inst->VRegA_22c(inst_data);
  // Report this field access to instrumentation if needed. Since we only have the offset of
  // the field from the base of the object, we need to look for it first.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldWriteListeners())) {
    StackHandleScope<1> hs(self);
    // Wrap in handle wrapper in case the listener does thread suspension.
    HandleWrapperObjPtr<mirror::Object> h(hs.NewHandleWrapper(&obj));
    JValue field_value = GetFieldValue<field_type>(shadow_frame, vregA);
    ObjPtr<Object> this_object = f->IsStatic() ? nullptr : obj;
    instrumentation->FieldWriteEvent(self, this_object.Ptr(),
                                     shadow_frame.GetMethod(),
                                     shadow_frame.GetDexPC(),
                                     f,
                                     field_value);
  }
  switch (field_type) {
    case Primitive::kPrimBoolean:
      f->SetBoolean<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimByte:
      f->SetByte<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimChar:
      f->SetChar<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimShort:
      f->SetShort<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimInt:
      f->SetInt<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimLong:
      f->SetLong<transaction_active>(obj, shadow_frame.GetVRegLong(vregA));
      break;
    case Primitive::kPrimNot: {
      Object* reg = shadow_frame.GetVRegReference(vregA);
      if (do_assignability_check && reg != nullptr) {
        // FieldHelper::GetType can resolve classes, use a handle wrapper which will restore the
        // object in the destructor.
        ObjPtr<Class> field_class;
        {
          StackHandleScope<2> hs(self);
          HandleWrapper<mirror::Object> h_reg(hs.NewHandleWrapper(&reg));
          HandleWrapperObjPtr<mirror::Object> h_obj(hs.NewHandleWrapper(&obj));
          field_class = f->GetType<true>();
        }
        if (!reg->VerifierInstanceOf(field_class.Ptr())) {
          // This should never happen.
          std::string temp1, temp2, temp3;
          self->ThrowNewExceptionF("Ljava/lang/VirtualMachineError;",
                                   "Put '%s' that is not instance of field '%s' in '%s'",
                                   reg->GetClass()->GetDescriptor(&temp1),
                                   field_class->GetDescriptor(&temp2),
                                   f->GetDeclaringClass()->GetDescriptor(&temp3));
          return false;
        }
      }
      f->SetObj<transaction_active>(obj, reg);
      break;
    }
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
      UNREACHABLE();
  }
  return true;
}

// Explicitly instantiate all DoFieldPut functions.
#define EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, _do_check, _transaction_active) \
  template bool DoFieldPut<_find_type, _field_type, _do_check, _transaction_active>(Thread* self, \
      const ShadowFrame& shadow_frame, const Instruction* inst, uint16_t inst_data)

#define EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(_find_type, _field_type)  \
    EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, false, false);  \
    EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, true, false);  \
    EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, false, true);  \
    EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, true, true);

// iput-XXX
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimBoolean)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimByte)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimChar)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimShort)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimInt)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimLong)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstanceObjectWrite, Primitive::kPrimNot)

// sput-XXX
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimBoolean)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimByte)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimChar)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimShort)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimInt)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimLong)
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticObjectWrite, Primitive::kPrimNot)

#undef EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL
#undef EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL

template<Primitive::Type field_type, bool transaction_active>
bool DoIPutQuick(const ShadowFrame& shadow_frame, const Instruction* inst, uint16_t inst_data) {
  Object* obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
  if (UNLIKELY(obj == nullptr)) {
    // We lost the reference to the field index so we cannot get a more
    // precised exception message.
    ThrowNullPointerExceptionFromDexPC();
    return false;
  }
  MemberOffset field_offset(inst->VRegC_22c());
  const uint32_t vregA = inst->VRegA_22c(inst_data);
  // Report this field modification to instrumentation if needed. Since we only have the offset of
  // the field from the base of the object, we need to look for it first.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldWriteListeners())) {
    ArtField* f = ArtField::FindInstanceFieldWithOffset(obj->GetClass(),
                                                        field_offset.Uint32Value());
    DCHECK(f != nullptr);
    DCHECK(!f->IsStatic());
    JValue field_value = GetFieldValue<field_type>(shadow_frame, vregA);
    instrumentation->FieldWriteEvent(Thread::Current(), obj, shadow_frame.GetMethod(),
                                     shadow_frame.GetDexPC(), f, field_value);
  }
  // Note: iput-x-quick instructions are only for non-volatile fields.
  switch (field_type) {
    case Primitive::kPrimBoolean:
      obj->SetFieldBoolean<transaction_active>(field_offset, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimByte:
      obj->SetFieldByte<transaction_active>(field_offset, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimChar:
      obj->SetFieldChar<transaction_active>(field_offset, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimShort:
      obj->SetFieldShort<transaction_active>(field_offset, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimInt:
      obj->SetField32<transaction_active>(field_offset, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimLong:
      obj->SetField64<transaction_active>(field_offset, shadow_frame.GetVRegLong(vregA));
      break;
    case Primitive::kPrimNot:
      obj->SetFieldObject<transaction_active>(field_offset, shadow_frame.GetVRegReference(vregA));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
      UNREACHABLE();
  }
  return true;
}

// Explicitly instantiate all DoIPutQuick functions.
#define EXPLICIT_DO_IPUT_QUICK_TEMPLATE_DECL(_field_type, _transaction_active) \
  template bool DoIPutQuick<_field_type, _transaction_active>(const ShadowFrame& shadow_frame, \
                                                              const Instruction* inst, \
                                                              uint16_t inst_data)

#define EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(_field_type)   \
  EXPLICIT_DO_IPUT_QUICK_TEMPLATE_DECL(_field_type, false);     \
  EXPLICIT_DO_IPUT_QUICK_TEMPLATE_DECL(_field_type, true);

EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimInt)      // iput-quick.
EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimBoolean)  // iput-boolean-quick.
EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimByte)     // iput-byte-quick.
EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimChar)     // iput-char-quick.
EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimShort)    // iput-short-quick.
EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimLong)     // iput-wide-quick.
EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimNot)      // iput-object-quick.
#undef EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL
#undef EXPLICIT_DO_IPUT_QUICK_TEMPLATE_DECL

// We accept a null Instrumentation* meaning we must not report anything to the instrumentation.
uint32_t FindNextInstructionFollowingException(
    Thread* self, ShadowFrame& shadow_frame, uint32_t dex_pc,
    const instrumentation::Instrumentation* instrumentation) {
  self->VerifyStack();
  StackHandleScope<2> hs(self);
  Handle<mirror::Throwable> exception(hs.NewHandle(self->GetException()));
  if (instrumentation != nullptr && instrumentation->HasExceptionCaughtListeners()
      && self->IsExceptionThrownByCurrentMethod(exception.Get())) {
    instrumentation->ExceptionCaughtEvent(self, exception.Get());
  }
  bool clear_exception = false;
  uint32_t found_dex_pc = shadow_frame.GetMethod()->FindCatchBlock(
      hs.NewHandle(exception->GetClass()), dex_pc, &clear_exception);
  if (found_dex_pc == DexFile::kDexNoIndex && instrumentation != nullptr) {
    // Exception is not caught by the current method. We will unwind to the
    // caller. Notify any instrumentation listener.
    instrumentation->MethodUnwindEvent(self, shadow_frame.GetThisObject(),
                                       shadow_frame.GetMethod(), dex_pc);
  } else {
    // Exception is caught in the current method. We will jump to the found_dex_pc.
    if (clear_exception) {
      self->ClearException();
    }
  }
  return found_dex_pc;
}

void UnexpectedOpcode(const Instruction* inst, const ShadowFrame& shadow_frame) {
  LOG(FATAL) << "Unexpected instruction: "
             << inst->DumpString(shadow_frame.GetMethod()->GetDexFile());
  UNREACHABLE();
}

void AbortTransactionF(Thread* self, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  AbortTransactionV(self, fmt, args);
  va_end(args);
}

void AbortTransactionV(Thread* self, const char* fmt, va_list args) {
  CHECK(Runtime::Current()->IsActiveTransaction());
  // Constructs abort message.
  std::string abort_msg;
  StringAppendV(&abort_msg, fmt, args);
  // Throws an exception so we can abort the transaction and rollback every change.
  Runtime::Current()->AbortTransactionAndThrowAbortError(self, abort_msg);
}

// Separate declaration is required solely for the attributes.
template <bool is_range, bool do_assignability_check>
    REQUIRES_SHARED(Locks::mutator_lock_)
static inline bool DoCallCommon(ArtMethod* called_method,
                                Thread* self,
                                ShadowFrame& shadow_frame,
                                JValue* result,
                                uint16_t number_of_inputs,
                                uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                uint32_t vregC) ALWAYS_INLINE;

// Separate declaration is required solely for the attributes.
template <bool is_range> REQUIRES_SHARED(Locks::mutator_lock_)
static inline bool DoCallPolymorphic(ArtMethod* called_method,
                                     Handle<mirror::MethodType> callsite_type,
                                     Handle<mirror::MethodType> target_type,
                                     Thread* self,
                                     ShadowFrame& shadow_frame,
                                     JValue* result,
                                     uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                     uint32_t vregC) ALWAYS_INLINE;

void ArtInterpreterToCompiledCodeBridge(Thread* self,
                                        ArtMethod* caller,
                                        const DexFile::CodeItem* code_item,
                                        ShadowFrame* shadow_frame,
                                        JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ArtMethod* method = shadow_frame->GetMethod();
  // Ensure static methods are initialized.
  if (method->IsStatic()) {
    mirror::Class* declaringClass = method->GetDeclaringClass();
    if (UNLIKELY(!declaringClass->IsInitialized())) {
      self->PushShadowFrame(shadow_frame);
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> h_class(hs.NewHandle(declaringClass));
      if (UNLIKELY(!Runtime::Current()->GetClassLinker()->EnsureInitialized(self, h_class, true,
                                                                            true))) {
        self->PopShadowFrame();
        DCHECK(self->IsExceptionPending());
        return;
      }
      self->PopShadowFrame();
      CHECK(h_class->IsInitializing());
      // Reload from shadow frame in case the method moved, this is faster than adding a handle.
      method = shadow_frame->GetMethod();
    }
  }
  uint16_t arg_offset = (code_item == nullptr)
                            ? 0
                            : code_item->registers_size_ - code_item->ins_size_;
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit != nullptr && caller != nullptr) {
    jit->NotifyInterpreterToCompiledCodeTransition(self, caller);
  }
  method->Invoke(self, shadow_frame->GetVRegArgs(arg_offset),
                 (shadow_frame->NumberOfVRegs() - arg_offset) * sizeof(uint32_t),
                 result, method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty());
}

void SetStringInitValueToAllAliases(ShadowFrame* shadow_frame,
                                    uint16_t this_obj_vreg,
                                    JValue result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  Object* existing = shadow_frame->GetVRegReference(this_obj_vreg);
  if (existing == nullptr) {
    // If it's null, we come from compiled code that was deoptimized. Nothing to do,
    // as the compiler verified there was no alias.
    // Set the new string result of the StringFactory.
    shadow_frame->SetVRegReference(this_obj_vreg, result.GetL());
    return;
  }
  // Set the string init result into all aliases.
  for (uint32_t i = 0, e = shadow_frame->NumberOfVRegs(); i < e; ++i) {
    if (shadow_frame->GetVRegReference(i) == existing) {
      DCHECK_EQ(shadow_frame->GetVRegReference(i),
                reinterpret_cast<mirror::Object*>(shadow_frame->GetVReg(i)));
      shadow_frame->SetVRegReference(i, result.GetL());
      DCHECK_EQ(shadow_frame->GetVRegReference(i),
                reinterpret_cast<mirror::Object*>(shadow_frame->GetVReg(i)));
    }
  }
}

template<bool is_range, bool do_access_check>
    REQUIRES_SHARED(Locks::mutator_lock_)
inline bool DoInvokePolymorphic(Thread* self, ShadowFrame& shadow_frame,
                                const Instruction* inst, uint16_t inst_data,
                                JValue* result) {
  // Invoke-polymorphic instructions always take a receiver. i.e, they are never static.
  const uint32_t vRegC = (is_range) ? inst->VRegC_4rcc() : inst->VRegC_45cc();

  // The method_idx here is the name of the signature polymorphic method that
  // was symbolically invoked in bytecode (say MethodHandle.invoke or MethodHandle.invokeExact)
  // and not the method that we'll dispatch to in the end.
  //
  // TODO(narayan) We'll have to check in the verifier that this is in fact a
  // signature polymorphic method so that we disallow calls via invoke-polymorphic
  // to non sig-poly methods. This would also have the side effect of verifying
  // that vRegC really is a reference type.
  StackHandleScope<6> hs(self);
  Handle<mirror::MethodHandleImpl> method_handle(hs.NewHandle(
      reinterpret_cast<mirror::MethodHandleImpl*>(shadow_frame.GetVRegReference(vRegC))));
  if (UNLIKELY(method_handle.Get() == nullptr)) {
    const int method_idx = (is_range) ? inst->VRegB_4rcc() : inst->VRegB_45cc();
    // Note that the invoke type is kVirtual here because a call to a signature
    // polymorphic method is shaped like a virtual call at the bytecode level.
    ThrowNullPointerExceptionForMethodAccess(method_idx, InvokeType::kVirtual);

    result->SetJ(0);
    return false;
  }

  // The vRegH value gives the index of the proto_id associated with this
  // signature polymorphic callsite.
  const uint32_t callsite_proto_id = (is_range) ? inst->VRegH_4rcc() : inst->VRegH_45cc();

  // Call through to the classlinker and ask it to resolve the static type associated
  // with the callsite. This information is stored in the dex cache so it's
  // guaranteed to be fast after the first resolution.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::Class> caller_class(hs.NewHandle(shadow_frame.GetMethod()->GetDeclaringClass()));
  Handle<mirror::MethodType> callsite_type(hs.NewHandle(class_linker->ResolveMethodType(
      caller_class->GetDexFile(), callsite_proto_id,
      hs.NewHandle<mirror::DexCache>(caller_class->GetDexCache()),
      hs.NewHandle<mirror::ClassLoader>(caller_class->GetClassLoader()))));

  // This implies we couldn't resolve one or more types in this method handle.
  if (UNLIKELY(callsite_type.Get() == nullptr)) {
    CHECK(self->IsExceptionPending());
    result->SetJ(0);
    return false;
  }

  // Get the method we're actually invoking along with the kind of
  // invoke that is desired. We don't need to perform access checks at this
  // point because they would have been performed on our behalf at the point
  // of creation of the method handle.
  ArtMethod* called_method = method_handle->GetTargetMethod();
  const MethodHandleKind handle_kind = method_handle->GetHandleKind();
  Handle<mirror::MethodType> handle_type(hs.NewHandle(method_handle->GetMethodType()));
  CHECK(called_method != nullptr);
  CHECK(handle_type.Get() != nullptr);

  // We now have to massage the number of inputs to the target function.
  // It's always one less than the number of inputs to the signature polymorphic
  // invoke, the first input being a reference to the MethodHandle itself.
  const uint16_t number_of_inputs =
      ((is_range) ? inst->VRegA_4rcc(inst_data) : inst->VRegA_45cc(inst_data)) - 1;

  uint32_t arg[Instruction::kMaxVarArgRegs] = {};
  uint32_t receiver_vregC = 0;
  if (is_range) {
    receiver_vregC = (inst->VRegC_4rcc() + 1);
  } else {
    inst->GetVarArgs(arg, inst_data);
    arg[0] = arg[1];
    arg[1] = arg[2];
    arg[2] = arg[3];
    arg[3] = arg[4];
    arg[4] = 0;
    receiver_vregC = arg[0];
  }

  if (IsInvoke(handle_kind)) {
    if (handle_kind == kInvokeVirtual || handle_kind == kInvokeInterface) {
      mirror::Object* receiver = shadow_frame.GetVRegReference(receiver_vregC);
      mirror::Class* declaring_class = called_method->GetDeclaringClass();
      // Verify that _vRegC is an object reference and of the type expected by
      // the receiver.
      called_method = receiver->GetClass()->FindVirtualMethodForVirtualOrInterface(
          called_method, kRuntimePointerSize);
      if (!VerifyObjectIsClass(receiver, declaring_class)) {
        return false;
      }
    } else if (handle_kind == kInvokeDirect) {
      if (called_method->IsConstructor()) {
        // TODO(narayan) : We need to handle the case where the target method is a
        // constructor here.
        UNIMPLEMENTED(FATAL) << "Direct invokes for constructors are not implemented yet.";
        return false;
      }

      // Nothing special to do in the case where we're not dealing with a
      // constructor. It's a private method, and we've already access checked at
      // the point of creating the handle.
    } else if (handle_kind == kInvokeSuper) {
      mirror::Class* declaring_class = called_method->GetDeclaringClass();

      // Note that we're not dynamically dispatching on the type of the receiver
      // here. We use the static type of the "receiver" object that we've
      // recorded in the method handle's type, which will be the same as the
      // special caller that was specified at the point of lookup.
      mirror::Class* referrer_class = handle_type->GetPTypes()->Get(0);
      if (!declaring_class->IsInterface()) {
        mirror::Class* super_class = referrer_class->GetSuperClass();
        uint16_t vtable_index = called_method->GetMethodIndex();
        DCHECK(super_class != nullptr);
        DCHECK(super_class->HasVTable());
        // Note that super_class is a super of referrer_class and called_method
        // will always be declared by super_class (or one of its super classes).
        DCHECK_LT(vtable_index, super_class->GetVTableLength());
        called_method = super_class->GetVTableEntry(vtable_index, kRuntimePointerSize);
      } else {
        called_method = referrer_class->FindVirtualMethodForInterfaceSuper(
            called_method, kRuntimePointerSize);
      }

      CHECK(called_method != nullptr);
    }

    // NOTE: handle_kind == kInvokeStatic needs no special treatment here. We
    // can directly make the call. handle_kind == kInvokeSuper doesn't have any
    // particular use and can probably be dropped.

    if (callsite_type->IsExactMatch(handle_type.Get())) {
      return DoCallCommon<is_range, do_access_check>(
          called_method, self, shadow_frame, result, number_of_inputs,
          arg, receiver_vregC);
    } else {
      return DoCallPolymorphic<is_range>(
          called_method, callsite_type, handle_type, self, shadow_frame,
          result, arg, receiver_vregC);
    }
  } else {
    // TODO(narayan): Implement field getters and setters.
    UNIMPLEMENTED(FATAL) << "Field references in method handles are not implemented yet.";
    return false;
  }
}

// Calculate the number of ins for a proxy or native method, where we
// can't just look at the code item.
static inline size_t GetInsForProxyOrNativeMethod(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(method->IsNative() || method->IsProxyMethod());

  method = method->GetInterfaceMethodIfProxy(kRuntimePointerSize);
  size_t num_ins = 0;
  // Separate accounting for the receiver, which isn't a part of the
  // shorty.
  if (!method->IsStatic()) {
    ++num_ins;
  }

  uint32_t shorty_len = 0;
  const char* shorty = method->GetShorty(&shorty_len);
  for (size_t i = 1; i < shorty_len; ++i) {
    const char c = shorty[i];
    ++num_ins;
    if (c == 'J' || c == 'D') {
      ++num_ins;
    }
  }

  return num_ins;
}

template <bool is_range>
static inline bool DoCallPolymorphic(ArtMethod* called_method,
                                     Handle<mirror::MethodType> callsite_type,
                                     Handle<mirror::MethodType> target_type,
                                     Thread* self,
                                     ShadowFrame& shadow_frame,
                                     JValue* result,
                                     uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                     uint32_t vregC) {
  // TODO(narayan): Wire in the String.init hacks.

  // Compute method information.
  const DexFile::CodeItem* code_item = called_method->GetCodeItem();

  // Number of registers for the callee's call frame. Note that for non-exact
  // invokes, we always derive this information from the callee method. We
  // cannot guarantee during verification that the number of registers encoded
  // in the invoke is equal to the number of ins for the callee. This is because
  // some transformations (such as boxing a long -> Long or wideining an
  // int -> long will change that number.
  uint16_t num_regs;
  size_t first_dest_reg;
  if (LIKELY(code_item != nullptr)) {
    num_regs = code_item->registers_size_;
    first_dest_reg = num_regs - code_item->ins_size_;
    // Parameter registers go at the end of the shadow frame.
    DCHECK_NE(first_dest_reg, (size_t)-1);
  } else {
    // No local regs for proxy and native methods.
    DCHECK(called_method->IsNative() || called_method->IsProxyMethod());
    num_regs = GetInsForProxyOrNativeMethod(called_method);
    first_dest_reg = 0;
  }

  // Allocate shadow frame on the stack.
  ShadowFrameAllocaUniquePtr shadow_frame_unique_ptr =
      CREATE_SHADOW_FRAME(num_regs, &shadow_frame, called_method, /* dex pc */ 0);
  ShadowFrame* new_shadow_frame = shadow_frame_unique_ptr.get();

  // Thread might be suspended during PerformArgumentConversions due to the
  // allocations performed during boxing.
  {
    ScopedStackedShadowFramePusher pusher(
        self, new_shadow_frame, StackedShadowFrameType::kShadowFrameUnderConstruction);
    if (!PerformArgumentConversions<is_range>(self, callsite_type, target_type,
                                              shadow_frame, vregC, first_dest_reg,
                                              arg, new_shadow_frame, result)) {
      DCHECK(self->IsExceptionPending());
      result->SetL(0);
      return false;
    }
  }

  // Do the call now.
  if (LIKELY(Runtime::Current()->IsStarted())) {
    ArtMethod* target = new_shadow_frame->GetMethod();
    if (ClassLinker::ShouldUseInterpreterEntrypoint(
        target,
        target->GetEntryPointFromQuickCompiledCode())) {
      ArtInterpreterToInterpreterBridge(self, code_item, new_shadow_frame, result);
    } else {
      ArtInterpreterToCompiledCodeBridge(
          self, shadow_frame.GetMethod(), code_item, new_shadow_frame, result);
    }
  } else {
    UnstartedRuntime::Invoke(self, code_item, new_shadow_frame, result, first_dest_reg);
  }

  // TODO(narayan): Perform return value conversions.

  return !self->IsExceptionPending();
}

template <bool is_range,
          bool do_assignability_check>
static inline bool DoCallCommon(ArtMethod* called_method,
                                Thread* self,
                                ShadowFrame& shadow_frame,
                                JValue* result,
                                uint16_t number_of_inputs,
                                uint32_t (&arg)[Instruction::kMaxVarArgRegs],
                                uint32_t vregC) {
  bool string_init = false;
  // Replace calls to String.<init> with equivalent StringFactory call.
  if (UNLIKELY(called_method->GetDeclaringClass()->IsStringClass()
               && called_method->IsConstructor())) {
    called_method = WellKnownClasses::StringInitToStringFactory(called_method);
    string_init = true;
  }

  // Compute method information.
  const DexFile::CodeItem* code_item = called_method->GetCodeItem();

  // Number of registers for the callee's call frame.
  uint16_t num_regs;
  if (LIKELY(code_item != nullptr)) {
    num_regs = code_item->registers_size_;
    DCHECK_EQ(string_init ? number_of_inputs - 1 : number_of_inputs, code_item->ins_size_);
  } else {
    DCHECK(called_method->IsNative() || called_method->IsProxyMethod());
    num_regs = number_of_inputs;
  }

  // Hack for String init:
  //
  // Rewrite invoke-x java.lang.String.<init>(this, a, b, c, ...) into:
  //         invoke-x StringFactory(a, b, c, ...)
  // by effectively dropping the first virtual register from the invoke.
  //
  // (at this point the ArtMethod has already been replaced,
  // so we just need to fix-up the arguments)
  //
  // Note that FindMethodFromCode in entrypoint_utils-inl.h was also special-cased
  // to handle the compiler optimization of replacing `this` with null without
  // throwing NullPointerException.
  uint32_t string_init_vreg_this = is_range ? vregC : arg[0];
  if (UNLIKELY(string_init)) {
    DCHECK_GT(num_regs, 0u);  // As the method is an instance method, there should be at least 1.

    // The new StringFactory call is static and has one fewer argument.
    if (code_item == nullptr) {
      DCHECK(called_method->IsNative() || called_method->IsProxyMethod());
      num_regs--;
    }  // else ... don't need to change num_regs since it comes up from the string_init's code item
    number_of_inputs--;

    // Rewrite the var-args, dropping the 0th argument ("this")
    for (uint32_t i = 1; i < arraysize(arg); ++i) {
      arg[i - 1] = arg[i];
    }
    arg[arraysize(arg) - 1] = 0;

    // Rewrite the non-var-arg case
    vregC++;  // Skips the 0th vreg in the range ("this").
  }

  // Parameter registers go at the end of the shadow frame.
  DCHECK_GE(num_regs, number_of_inputs);
  size_t first_dest_reg = num_regs - number_of_inputs;
  DCHECK_NE(first_dest_reg, (size_t)-1);

  // Allocate shadow frame on the stack.
  const char* old_cause = self->StartAssertNoThreadSuspension("DoCallCommon");
  ShadowFrameAllocaUniquePtr shadow_frame_unique_ptr =
      CREATE_SHADOW_FRAME(num_regs, &shadow_frame, called_method, /* dex pc */ 0);
  ShadowFrame* new_shadow_frame = shadow_frame_unique_ptr.get();

  // Initialize new shadow frame by copying the registers from the callee shadow frame.
  if (do_assignability_check) {
    // Slow path.
    // We might need to do class loading, which incurs a thread state change to kNative. So
    // register the shadow frame as under construction and allow suspension again.
    ScopedStackedShadowFramePusher pusher(
        self, new_shadow_frame, StackedShadowFrameType::kShadowFrameUnderConstruction);
    self->EndAssertNoThreadSuspension(old_cause);

    // ArtMethod here is needed to check type information of the call site against the callee.
    // Type information is retrieved from a DexFile/DexCache for that respective declared method.
    //
    // As a special case for proxy methods, which are not dex-backed,
    // we have to retrieve type information from the proxy's method
    // interface method instead (which is dex backed since proxies are never interfaces).
    ArtMethod* method =
        new_shadow_frame->GetMethod()->GetInterfaceMethodIfProxy(kRuntimePointerSize);

    // We need to do runtime check on reference assignment. We need to load the shorty
    // to get the exact type of each reference argument.
    const DexFile::TypeList* params = method->GetParameterTypeList();
    uint32_t shorty_len = 0;
    const char* shorty = method->GetShorty(&shorty_len);

    // Handle receiver apart since it's not part of the shorty.
    size_t dest_reg = first_dest_reg;
    size_t arg_offset = 0;

    if (!method->IsStatic()) {
      size_t receiver_reg = is_range ? vregC : arg[0];
      new_shadow_frame->SetVRegReference(dest_reg, shadow_frame.GetVRegReference(receiver_reg));
      ++dest_reg;
      ++arg_offset;
      DCHECK(!string_init);  // All StringFactory methods are static.
    }

    // Copy the caller's invoke-* arguments into the callee's parameter registers.
    for (uint32_t shorty_pos = 0; dest_reg < num_regs; ++shorty_pos, ++dest_reg, ++arg_offset) {
      // Skip the 0th 'shorty' type since it represents the return type.
      DCHECK_LT(shorty_pos + 1, shorty_len) << "for shorty '" << shorty << "'";
      const size_t src_reg = (is_range) ? vregC + arg_offset : arg[arg_offset];
      switch (shorty[shorty_pos + 1]) {
        // Handle Object references. 1 virtual register slot.
        case 'L': {
          Object* o = shadow_frame.GetVRegReference(src_reg);
          if (do_assignability_check && o != nullptr) {
            PointerSize pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
            Class* arg_type =
                method->GetClassFromTypeIndex(
                    params->GetTypeItem(shorty_pos).type_idx_, true /* resolve */, pointer_size);
            if (arg_type == nullptr) {
              CHECK(self->IsExceptionPending());
              return false;
            }
            if (!o->VerifierInstanceOf(arg_type)) {
              // This should never happen.
              std::string temp1, temp2;
              self->ThrowNewExceptionF("Ljava/lang/VirtualMachineError;",
                                       "Invoking %s with bad arg %d, type '%s' not instance of '%s'",
                                       new_shadow_frame->GetMethod()->GetName(), shorty_pos,
                                       o->GetClass()->GetDescriptor(&temp1),
                                       arg_type->GetDescriptor(&temp2));
              return false;
            }
          }
          new_shadow_frame->SetVRegReference(dest_reg, o);
          break;
        }
        // Handle doubles and longs. 2 consecutive virtual register slots.
        case 'J': case 'D': {
          uint64_t wide_value =
              (static_cast<uint64_t>(shadow_frame.GetVReg(src_reg + 1)) << BitSizeOf<uint32_t>()) |
               static_cast<uint32_t>(shadow_frame.GetVReg(src_reg));
          new_shadow_frame->SetVRegLong(dest_reg, wide_value);
          // Skip the next virtual register slot since we already used it.
          ++dest_reg;
          ++arg_offset;
          break;
        }
        // Handle all other primitives that are always 1 virtual register slot.
        default:
          new_shadow_frame->SetVReg(dest_reg, shadow_frame.GetVReg(src_reg));
          break;
      }
    }
  } else {
    size_t arg_index = 0;

    // Fast path: no extra checks.
    if (is_range) {
      uint16_t first_src_reg = vregC;

      for (size_t src_reg = first_src_reg, dest_reg = first_dest_reg; dest_reg < num_regs;
          ++dest_reg, ++src_reg) {
        AssignRegister(new_shadow_frame, shadow_frame, dest_reg, src_reg);
      }
    } else {
      DCHECK_LE(number_of_inputs, arraysize(arg));

      for (; arg_index < number_of_inputs; ++arg_index) {
        AssignRegister(new_shadow_frame, shadow_frame, first_dest_reg + arg_index, arg[arg_index]);
      }
    }
    self->EndAssertNoThreadSuspension(old_cause);
  }

  // Do the call now.
  if (LIKELY(Runtime::Current()->IsStarted())) {
    ArtMethod* target = new_shadow_frame->GetMethod();
    if (ClassLinker::ShouldUseInterpreterEntrypoint(
        target,
        target->GetEntryPointFromQuickCompiledCode())) {
      ArtInterpreterToInterpreterBridge(self, code_item, new_shadow_frame, result);
    } else {
      ArtInterpreterToCompiledCodeBridge(
          self, shadow_frame.GetMethod(), code_item, new_shadow_frame, result);
    }
  } else {
    UnstartedRuntime::Invoke(self, code_item, new_shadow_frame, result, first_dest_reg);
  }

  if (string_init && !self->IsExceptionPending()) {
    SetStringInitValueToAllAliases(&shadow_frame, string_init_vreg_this, *result);
  }

  return !self->IsExceptionPending();
}

template<bool is_range, bool do_assignability_check>
bool DoCall(ArtMethod* called_method, Thread* self, ShadowFrame& shadow_frame,
            const Instruction* inst, uint16_t inst_data, JValue* result) {
  // Argument word count.
  const uint16_t number_of_inputs =
      (is_range) ? inst->VRegA_3rc(inst_data) : inst->VRegA_35c(inst_data);

  // TODO: find a cleaner way to separate non-range and range information without duplicating
  //       code.
  uint32_t arg[Instruction::kMaxVarArgRegs] = {};  // only used in invoke-XXX.
  uint32_t vregC = 0;
  if (is_range) {
    vregC = inst->VRegC_3rc();
  } else {
    vregC = inst->VRegC_35c();
    inst->GetVarArgs(arg, inst_data);
  }

  return DoCallCommon<is_range, do_assignability_check>(
      called_method, self, shadow_frame,
      result, number_of_inputs, arg, vregC);
}

template <bool is_range, bool do_access_check, bool transaction_active>
bool DoFilledNewArray(const Instruction* inst, const ShadowFrame& shadow_frame,
                      Thread* self, JValue* result) {
  DCHECK(inst->Opcode() == Instruction::FILLED_NEW_ARRAY ||
         inst->Opcode() == Instruction::FILLED_NEW_ARRAY_RANGE);
  const int32_t length = is_range ? inst->VRegA_3rc() : inst->VRegA_35c();
  if (!is_range) {
    // Checks FILLED_NEW_ARRAY's length does not exceed 5 arguments.
    CHECK_LE(length, 5);
  }
  if (UNLIKELY(length < 0)) {
    ThrowNegativeArraySizeException(length);
    return false;
  }
  uint16_t type_idx = is_range ? inst->VRegB_3rc() : inst->VRegB_35c();
  Class* array_class = ResolveVerifyAndClinit(type_idx, shadow_frame.GetMethod(),
                                              self, false, do_access_check);
  if (UNLIKELY(array_class == nullptr)) {
    DCHECK(self->IsExceptionPending());
    return false;
  }
  CHECK(array_class->IsArrayClass());
  Class* component_class = array_class->GetComponentType();
  const bool is_primitive_int_component = component_class->IsPrimitiveInt();
  if (UNLIKELY(component_class->IsPrimitive() && !is_primitive_int_component)) {
    if (component_class->IsPrimitiveLong() || component_class->IsPrimitiveDouble()) {
      ThrowRuntimeException("Bad filled array request for type %s",
                            PrettyDescriptor(component_class).c_str());
    } else {
      self->ThrowNewExceptionF("Ljava/lang/InternalError;",
                               "Found type %s; filled-new-array not implemented for anything but 'int'",
                               PrettyDescriptor(component_class).c_str());
    }
    return false;
  }
  Object* new_array = Array::Alloc<true>(self, array_class, length,
                                         array_class->GetComponentSizeShift(),
                                         Runtime::Current()->GetHeap()->GetCurrentAllocator());
  if (UNLIKELY(new_array == nullptr)) {
    self->AssertPendingOOMException();
    return false;
  }
  uint32_t arg[Instruction::kMaxVarArgRegs];  // only used in filled-new-array.
  uint32_t vregC = 0;   // only used in filled-new-array-range.
  if (is_range) {
    vregC = inst->VRegC_3rc();
  } else {
    inst->GetVarArgs(arg);
  }
  for (int32_t i = 0; i < length; ++i) {
    size_t src_reg = is_range ? vregC + i : arg[i];
    if (is_primitive_int_component) {
      new_array->AsIntArray()->SetWithoutChecks<transaction_active>(
          i, shadow_frame.GetVReg(src_reg));
    } else {
      new_array->AsObjectArray<Object>()->SetWithoutChecks<transaction_active>(
          i, shadow_frame.GetVRegReference(src_reg));
    }
  }

  result->SetL(new_array);
  return true;
}

// TODO fix thread analysis: should be REQUIRES_SHARED(Locks::mutator_lock_).
template<typename T>
static void RecordArrayElementsInTransactionImpl(mirror::PrimitiveArray<T>* array, int32_t count)
    NO_THREAD_SAFETY_ANALYSIS {
  Runtime* runtime = Runtime::Current();
  for (int32_t i = 0; i < count; ++i) {
    runtime->RecordWriteArray(array, i, array->GetWithoutChecks(i));
  }
}

void RecordArrayElementsInTransaction(mirror::Array* array, int32_t count)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(Runtime::Current()->IsActiveTransaction());
  DCHECK(array != nullptr);
  DCHECK_LE(count, array->GetLength());
  Primitive::Type primitive_component_type = array->GetClass()->GetComponentType()->GetPrimitiveType();
  switch (primitive_component_type) {
    case Primitive::kPrimBoolean:
      RecordArrayElementsInTransactionImpl(array->AsBooleanArray(), count);
      break;
    case Primitive::kPrimByte:
      RecordArrayElementsInTransactionImpl(array->AsByteArray(), count);
      break;
    case Primitive::kPrimChar:
      RecordArrayElementsInTransactionImpl(array->AsCharArray(), count);
      break;
    case Primitive::kPrimShort:
      RecordArrayElementsInTransactionImpl(array->AsShortArray(), count);
      break;
    case Primitive::kPrimInt:
      RecordArrayElementsInTransactionImpl(array->AsIntArray(), count);
      break;
    case Primitive::kPrimFloat:
      RecordArrayElementsInTransactionImpl(array->AsFloatArray(), count);
      break;
    case Primitive::kPrimLong:
      RecordArrayElementsInTransactionImpl(array->AsLongArray(), count);
      break;
    case Primitive::kPrimDouble:
      RecordArrayElementsInTransactionImpl(array->AsDoubleArray(), count);
      break;
    default:
      LOG(FATAL) << "Unsupported primitive type " << primitive_component_type
                 << " in fill-array-data";
      break;
  }
}

// Explicit DoCall template function declarations.
#define EXPLICIT_DO_CALL_TEMPLATE_DECL(_is_range, _do_assignability_check)                      \
  template REQUIRES_SHARED(Locks::mutator_lock_)                                                \
  bool DoCall<_is_range, _do_assignability_check>(ArtMethod* method, Thread* self,              \
                                                  ShadowFrame& shadow_frame,                    \
                                                  const Instruction* inst, uint16_t inst_data,  \
                                                  JValue* result)
EXPLICIT_DO_CALL_TEMPLATE_DECL(false, false);
EXPLICIT_DO_CALL_TEMPLATE_DECL(false, true);
EXPLICIT_DO_CALL_TEMPLATE_DECL(true, false);
EXPLICIT_DO_CALL_TEMPLATE_DECL(true, true);
#undef EXPLICIT_DO_CALL_TEMPLATE_DECL

// Explicit DoInvokePolymorphic template function declarations.
#define EXPLICIT_DO_INVOKE_POLYMORPHIC_TEMPLATE_DECL(_is_range, _do_assignability_check)  \
  template REQUIRES_SHARED(Locks::mutator_lock_)                                          \
  bool DoInvokePolymorphic<_is_range, _do_assignability_check>(                           \
      Thread* self, ShadowFrame& shadow_frame, const Instruction* inst,                   \
      uint16_t inst_data, JValue* result)

EXPLICIT_DO_INVOKE_POLYMORPHIC_TEMPLATE_DECL(false, false);
EXPLICIT_DO_INVOKE_POLYMORPHIC_TEMPLATE_DECL(false, true);
EXPLICIT_DO_INVOKE_POLYMORPHIC_TEMPLATE_DECL(true, false);
EXPLICIT_DO_INVOKE_POLYMORPHIC_TEMPLATE_DECL(true, true);
#undef EXPLICIT_DO_INVOKE_POLYMORPHIC_TEMPLATE_DECL

// Explicit DoFilledNewArray template function declarations.
#define EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(_is_range_, _check, _transaction_active)       \
  template REQUIRES_SHARED(Locks::mutator_lock_)                                                  \
  bool DoFilledNewArray<_is_range_, _check, _transaction_active>(const Instruction* inst,         \
                                                                 const ShadowFrame& shadow_frame, \
                                                                 Thread* self, JValue* result)
#define EXPLICIT_DO_FILLED_NEW_ARRAY_ALL_TEMPLATE_DECL(_transaction_active)       \
  EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(false, false, _transaction_active);  \
  EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(false, true, _transaction_active);   \
  EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(true, false, _transaction_active);   \
  EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(true, true, _transaction_active)
EXPLICIT_DO_FILLED_NEW_ARRAY_ALL_TEMPLATE_DECL(false);
EXPLICIT_DO_FILLED_NEW_ARRAY_ALL_TEMPLATE_DECL(true);
#undef EXPLICIT_DO_FILLED_NEW_ARRAY_ALL_TEMPLATE_DECL
#undef EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL

}  // namespace interpreter
}  // namespace art
