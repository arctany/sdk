// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#if !defined(DART_PRECOMPILED_RUNTIME)

#include "vm/compiler/assembler/assembler.h"

#include "platform/utils.h"
#include "vm/cpu.h"
#include "vm/heap/heap.h"
#include "vm/memory_region.h"
#include "vm/os.h"
#include "vm/zone.h"

namespace dart {

DEFINE_FLAG(bool,
            check_code_pointer,
            false,
            "Verify instructions offset in code object."
            "NOTE: This breaks the profiler.");
DEFINE_FLAG(bool,
            code_comments,
            false,
            "Include comments into code and disassembly");
#if defined(TARGET_ARCH_ARM)
DEFINE_FLAG(bool, use_far_branches, false, "Enable far branches for ARM.");
#endif

static uword NewContents(intptr_t capacity) {
  Zone* zone = Thread::Current()->zone();
  uword result = zone->AllocUnsafe(capacity);
#if defined(DEBUG)
  // Initialize the buffer with kBreakPointInstruction to force a break
  // point if we ever execute an uninitialized part of the code buffer.
  Assembler::InitializeMemoryWithBreakpoints(result, capacity);
#endif
  return result;
}

#if defined(DEBUG)
AssemblerBuffer::EnsureCapacity::EnsureCapacity(AssemblerBuffer* buffer) {
  if (buffer->cursor() >= buffer->limit()) buffer->ExtendCapacity();
  // In debug mode, we save the assembler buffer along with the gap
  // size before we start emitting to the buffer. This allows us to
  // check that any single generated instruction doesn't overflow the
  // limit implied by the minimum gap size.
  buffer_ = buffer;
  gap_ = ComputeGap();
  // Make sure that extending the capacity leaves a big enough gap
  // for any kind of instruction.
  ASSERT(gap_ >= kMinimumGap);
  // Mark the buffer as having ensured the capacity.
  ASSERT(!buffer->HasEnsuredCapacity());  // Cannot nest.
  buffer->has_ensured_capacity_ = true;
}

AssemblerBuffer::EnsureCapacity::~EnsureCapacity() {
  // Unmark the buffer, so we cannot emit after this.
  buffer_->has_ensured_capacity_ = false;
  // Make sure the generated instruction doesn't take up more
  // space than the minimum gap.
  intptr_t delta = gap_ - ComputeGap();
  ASSERT(delta <= kMinimumGap);
}
#endif

AssemblerBuffer::AssemblerBuffer()
    : pointer_offsets_(new ZoneGrowableArray<intptr_t>(16)) {
  static const intptr_t kInitialBufferCapacity = 4 * KB;
  contents_ = NewContents(kInitialBufferCapacity);
  cursor_ = contents_;
  limit_ = ComputeLimit(contents_, kInitialBufferCapacity);
  fixup_ = NULL;
#if defined(DEBUG)
  has_ensured_capacity_ = false;
  fixups_processed_ = false;
#endif

  // Verify internal state.
  ASSERT(Capacity() == kInitialBufferCapacity);
  ASSERT(Size() == 0);
}

AssemblerBuffer::~AssemblerBuffer() {}

void AssemblerBuffer::ProcessFixups(const MemoryRegion& region) {
  AssemblerFixup* fixup = fixup_;
  while (fixup != NULL) {
    fixup->Process(region, fixup->position());
    fixup = fixup->previous();
  }
}

void AssemblerBuffer::FinalizeInstructions(const MemoryRegion& instructions) {
  // Copy the instructions from the buffer.
  MemoryRegion from(reinterpret_cast<void*>(contents()), Size());
  instructions.CopyFrom(0, from);

  // Process fixups in the instructions.
  ProcessFixups(instructions);
#if defined(DEBUG)
  fixups_processed_ = true;
#endif
}

void AssemblerBuffer::ExtendCapacity() {
  intptr_t old_size = Size();
  intptr_t old_capacity = Capacity();
  intptr_t new_capacity =
      Utils::Minimum(old_capacity * 2, old_capacity + 1 * MB);
  if (new_capacity < old_capacity) {
    FATAL("Unexpected overflow in AssemblerBuffer::ExtendCapacity");
  }

  // Allocate the new data area and copy contents of the old one to it.
  uword new_contents = NewContents(new_capacity);
  memmove(reinterpret_cast<void*>(new_contents),
          reinterpret_cast<void*>(contents_), old_size);

  // Compute the relocation delta and switch to the new contents area.
  intptr_t delta = new_contents - contents_;
  contents_ = new_contents;

  // Update the cursor and recompute the limit.
  cursor_ += delta;
  limit_ = ComputeLimit(new_contents, new_capacity);

  // Verify internal state.
  ASSERT(Capacity() == new_capacity);
  ASSERT(Size() == old_size);
}

class PatchCodeWithHandle : public AssemblerFixup {
 public:
  PatchCodeWithHandle(ZoneGrowableArray<intptr_t>* pointer_offsets,
                      const Object& object)
      : pointer_offsets_(pointer_offsets), object_(object) {}

  void Process(const MemoryRegion& region, intptr_t position) {
    // Patch the handle into the code. Once the instructions are installed into
    // a raw code object and the pointer offsets are setup, the handle is
    // resolved.
    region.Store<const Object*>(position, &object_);
    pointer_offsets_->Add(position);
  }

  virtual bool IsPointerOffset() const { return true; }

 private:
  ZoneGrowableArray<intptr_t>* pointer_offsets_;
  const Object& object_;
};

intptr_t AssemblerBuffer::CountPointerOffsets() const {
  intptr_t count = 0;
  AssemblerFixup* current = fixup_;
  while (current != NULL) {
    if (current->IsPointerOffset()) ++count;
    current = current->previous_;
  }
  return count;
}

void AssemblerBuffer::EmitObject(const Object& object) {
  // Since we are going to store the handle as part of the fixup information
  // the handle needs to be a zone handle.
  ASSERT(object.IsNotTemporaryScopedHandle());
  ASSERT(object.IsOld());
  EmitFixup(new PatchCodeWithHandle(pointer_offsets_, object));
  cursor_ += kWordSize;  // Reserve space for pointer.
}

// Shared macros are implemented here.
void AssemblerBase::Unimplemented(const char* message) {
  const char* format = "Unimplemented: %s";
  const intptr_t len = Utils::SNPrint(NULL, 0, format, message);
  char* buffer = reinterpret_cast<char*>(malloc(len + 1));
  Utils::SNPrint(buffer, len + 1, format, message);
  Stop(buffer);
}

void AssemblerBase::Untested(const char* message) {
  const char* format = "Untested: %s";
  const intptr_t len = Utils::SNPrint(NULL, 0, format, message);
  char* buffer = reinterpret_cast<char*>(malloc(len + 1));
  Utils::SNPrint(buffer, len + 1, format, message);
  Stop(buffer);
}

void AssemblerBase::Unreachable(const char* message) {
  const char* format = "Unreachable: %s";
  const intptr_t len = Utils::SNPrint(NULL, 0, format, message);
  char* buffer = reinterpret_cast<char*>(malloc(len + 1));
  Utils::SNPrint(buffer, len + 1, format, message);
  Stop(buffer);
}

void AssemblerBase::Comment(const char* format, ...) {
  if (EmittingComments()) {
    char buffer[1024];

    va_list args;
    va_start(args, format);
    Utils::VSNPrint(buffer, sizeof(buffer), format, args);
    va_end(args);

    comments_.Add(
        new CodeComment(buffer_.GetPosition(),
                        String::ZoneHandle(String::New(buffer, Heap::kOld))));
  }
}

bool AssemblerBase::EmittingComments() {
  return FLAG_code_comments || FLAG_disassemble || FLAG_disassemble_optimized;
}

const Code::Comments& AssemblerBase::GetCodeComments() const {
  Code::Comments& comments = Code::Comments::New(comments_.length());

  for (intptr_t i = 0; i < comments_.length(); i++) {
    comments.SetPCOffsetAt(i, comments_[i]->pc_offset());
    comments.SetCommentAt(i, comments_[i]->comment());
  }

  return comments;
}

intptr_t ObjIndexPair::Hashcode(Key key) {
  if (key.type() != ObjectPool::kTaggedObject) {
    return key.raw_value_;
  }
  if (key.obj_->IsNull()) {
    return 2011;
  }
  if (key.obj_->IsString() || key.obj_->IsNumber()) {
    return Instance::Cast(*key.obj_).CanonicalizeHash();
  }
  if (key.obj_->IsCode()) {
    // Instructions don't move during compaction.
    return Code::Cast(*key.obj_).PayloadStart();
  }
  if (key.obj_->IsFunction()) {
    return Function::Cast(*key.obj_).Hash();
  }
  if (key.obj_->IsField()) {
    return String::HashRawSymbol(Field::Cast(*key.obj_).name());
  }
  // Unlikely.
  return key.obj_->GetClassId();
}
void ObjectPoolWrapper::Reset() {
  // Null out the handles we've accumulated.
  for (intptr_t i = 0; i < object_pool_.length(); ++i) {
    if (object_pool_[i].type() == ObjectPool::kTaggedObject) {
      *const_cast<Object*>(object_pool_[i].obj_) = Object::null();
      *const_cast<Object*>(object_pool_[i].equivalence_) = Object::null();
    }
  }

  object_pool_.Clear();
  object_pool_index_table_.Clear();
}

void ObjectPoolWrapper::InitializeFrom(const ObjectPool& other) {
  ASSERT(object_pool_.length() == 0);

  for (intptr_t i = 0; i < other.Length(); i++) {
    auto type = other.TypeAt(i);
    auto patchable = other.PatchableAt(i);
    switch (type) {
      case ObjectPool::kTaggedObject: {
        ObjectPoolWrapperEntry entry(&Object::ZoneHandle(other.ObjectAt(i)),
                                     patchable);
        AddObject(entry);
        break;
      }
      case ObjectPool::kImmediate:
      case ObjectPool::kNativeFunction:
      case ObjectPool::kNativeFunctionWrapper: {
        ObjectPoolWrapperEntry entry(other.RawValueAt(i), type, patchable);
        AddObject(entry);
        break;
      }
      default:
        UNREACHABLE();
    }
  }

  ASSERT(CurrentLength() == other.Length());
}

intptr_t ObjectPoolWrapper::AddObject(const Object& obj,
                                      ObjectPool::Patchability patchable) {
  ASSERT(obj.IsNotTemporaryScopedHandle());
  return AddObject(ObjectPoolWrapperEntry(&obj, patchable));
}

intptr_t ObjectPoolWrapper::AddImmediate(uword imm) {
  return AddObject(ObjectPoolWrapperEntry(imm, ObjectPool::kImmediate,
                                          ObjectPool::kNotPatchable));
}

intptr_t ObjectPoolWrapper::AddObject(ObjectPoolWrapperEntry entry) {
  ASSERT((entry.type() != ObjectPool::kTaggedObject) ||
         (entry.obj_->IsNotTemporaryScopedHandle() &&
          (entry.equivalence_ == NULL ||
           entry.equivalence_->IsNotTemporaryScopedHandle())));

  if (entry.type() == ObjectPool::kTaggedObject) {
    // If the owner of the object pool wrapper specified a specific zone we
    // shoulld use we'll do so.
    if (zone_ != NULL) {
      entry.obj_ = &Object::ZoneHandle(zone_, entry.obj_->raw());
      if (entry.equivalence_ != NULL) {
        entry.equivalence_ =
            &Object::ZoneHandle(zone_, entry.equivalence_->raw());
      }
    }
  }

  object_pool_.Add(entry);
  if (entry.patchable() == ObjectPool::kNotPatchable) {
    // The object isn't patchable. Record the index for fast lookup.
    object_pool_index_table_.Insert(
        ObjIndexPair(entry, object_pool_.length() - 1));
  }
  return object_pool_.length() - 1;
}

intptr_t ObjectPoolWrapper::FindObject(ObjectPoolWrapperEntry entry) {
  // If the object is not patchable, check if we've already got it in the
  // object pool.
  if (entry.patchable() == ObjectPool::kNotPatchable) {
    intptr_t idx = object_pool_index_table_.LookupValue(entry);
    if (idx != ObjIndexPair::kNoIndex) {
      return idx;
    }
  }
  return AddObject(entry);
}

intptr_t ObjectPoolWrapper::FindObject(const Object& obj,
                                       ObjectPool::Patchability patchable) {
  return FindObject(ObjectPoolWrapperEntry(&obj, patchable));
}

intptr_t ObjectPoolWrapper::FindObject(const Object& obj,
                                       const Object& equivalence) {
  return FindObject(
      ObjectPoolWrapperEntry(&obj, &equivalence, ObjectPool::kNotPatchable));
}

intptr_t ObjectPoolWrapper::FindImmediate(uword imm) {
  return FindObject(ObjectPoolWrapperEntry(imm, ObjectPool::kImmediate,
                                           ObjectPool::kNotPatchable));
}

intptr_t ObjectPoolWrapper::FindNativeFunction(
    const ExternalLabel* label,
    ObjectPool::Patchability patchable) {
  return FindObject(ObjectPoolWrapperEntry(
      label->address(), ObjectPool::kNativeFunction, patchable));
}

intptr_t ObjectPoolWrapper::FindNativeFunctionWrapper(
    const ExternalLabel* label,
    ObjectPool::Patchability patchable) {
  return FindObject(ObjectPoolWrapperEntry(
      label->address(), ObjectPool::kNativeFunctionWrapper, patchable));
}

RawObjectPool* ObjectPoolWrapper::MakeObjectPool() {
  intptr_t len = object_pool_.length();
  if (len == 0) {
    return Object::empty_object_pool().raw();
  }
  const ObjectPool& result = ObjectPool::Handle(ObjectPool::New(len));
  for (intptr_t i = 0; i < len; ++i) {
    auto type = object_pool_[i].type();
    auto patchable = object_pool_[i].patchable();
    result.SetTypeAt(i, type, patchable);
    if (type == ObjectPool::kTaggedObject) {
      result.SetObjectAt(i, *object_pool_[i].obj_);
    } else {
      result.SetRawValueAt(i, object_pool_[i].raw_value_);
    }
  }
  return result.raw();
}

}  // namespace dart

#endif  // !defined(DART_PRECOMPILED_RUNTIME)
