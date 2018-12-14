//
// Copyright 2018 WebAssembly Community Group participants
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <type_traits>

#include "src/base/formatters.h"
#include "src/binary/encoding.h"

#define WASP_TRY_READ(var, call) \
  auto opt_##var = call;         \
  if (!opt_##var) {              \
    return nullopt;              \
  }                              \
  auto var = *opt_##var /* No semicolon. */

#define WASP_TRY_READ_CONTEXT(var, call, desc) \
  errors.PushContext(*data, desc);             \
  WASP_TRY_READ(var, call);                    \
  errors.PopContext();

namespace wasp {
namespace binary {

template <typename T>
struct Tag {};

template <typename T, typename Errors>
optional<T> Read(SpanU8* data, Errors&, Tag<T>);

template <typename T, typename Errors>
optional<T> Read(SpanU8* data, Errors& errors) {
  return Read(data, errors, Tag<T>{});
}

template <typename Errors>
optional<u8> Read(SpanU8* data, Errors& errors, Tag<u8>) {
  if (data->size() < 1) {
    errors.OnError(*data, "Unable to read u8");
    return nullopt;
  }

  u8 result{(*data)[0]};
  *data = remove_prefix(*data, 1);
  return result;
}

template <typename Errors>
optional<SpanU8> ReadBytes(SpanU8* data, SpanU8::index_type N, Errors& errors) {
  if (data->size() < N) {
    errors.OnError(*data, format("Unable to read {} bytes", N));
    return nullopt;
  }

  SpanU8 result{data->begin(), N};
  *data = remove_prefix(*data, N);
  return result;
}

template <typename S>
S SignExtend(typename std::make_unsigned<S>::type x, int N) {
  constexpr size_t kNumBits = sizeof(S) * 8;
  return static_cast<S>(x << (kNumBits - N - 1)) >> (kNumBits - N - 1);
}

template <typename T, typename Errors>
optional<T> ReadVarInt(SpanU8* data, Errors& errors, string_view desc) {
  using U = typename std::make_unsigned<T>::type;
  constexpr bool is_signed = std::is_signed<T>::value;
  constexpr int kMaxBytes = (sizeof(T) * 8 + 6) / 7;
  constexpr int kUsedBitsInLastByte = sizeof(T) * 8 - 7 * (kMaxBytes - 1);
  constexpr int kMaskBits = kUsedBitsInLastByte - (is_signed ? 1 : 0);
  constexpr u8 kMask = ~((1 << kMaskBits) - 1);
  constexpr u8 kOnes = kMask & 0x7f;

  ErrorsContextGuard<Errors> guard{errors, *data, desc};

  U result{};
  for (int i = 0;;) {
    WASP_TRY_READ(byte, Read<u8>(data, errors));

    const int shift = i * 7;
    result |= U(byte & 0x7f) << shift;

    if (++i == kMaxBytes) {
      if ((byte & kMask) == 0 || (is_signed && (byte & kMask) == kOnes)) {
        return static_cast<T>(result);
      }
      const u8 zero_ext = byte & ~kMask & 0x7f;
      const u8 one_ext = (byte | kOnes) & 0x7f;
      if (is_signed) {
        errors.OnError(
            *data, format("Last byte of {} must be sign "
                          "extension: expected {:#2x} or {:#2x}, got {:#2x}",
                          desc, zero_ext, one_ext, byte));
      } else {
        errors.OnError(*data, format("Last byte of {} must be zero "
                                     "extension: expected {:#2x}, got {:#2x}",
                                     desc, zero_ext, byte));
      }
      return nullopt;
    } else if ((byte & 0x80) == 0) {
      return is_signed ? SignExtend<T>(result, 6 + shift) : result;
    }
  }
}

template <typename Errors>
optional<u32> Read(SpanU8* data, Errors& errors, Tag<u32>) {
  return ReadVarInt<u32>(data, errors, "vu32");
}

template <typename Errors>
optional<Index> ReadIndex(SpanU8* data, Errors& errors) {
  return ReadVarInt<Index>(data, errors, "index");
}

template <typename Errors>
optional<s32> Read(SpanU8* data, Errors& errors, Tag<s32>) {
  return ReadVarInt<s32>(data, errors, "vs32");
}

template <typename Errors>
optional<s64> Read(SpanU8* data, Errors& errors, Tag<s64>) {
  return ReadVarInt<s64>(data, errors, "vs64");
}

template <typename Errors>
optional<f32> Read(SpanU8* data, Errors& errors, Tag<f32>) {
  static_assert(sizeof(f32) == 4, "sizeof(f32) != 4");
  WASP_TRY_READ(bytes, ReadBytes(data, sizeof(f32), errors));
  f32 result;
  memcpy(&result, bytes.data(), sizeof(f32));
  return result;
}

template <typename Errors>
optional<f64> Read(SpanU8* data, Errors& errors, Tag<f64>) {
  static_assert(sizeof(f64) == 8, "sizeof(f64) != 8");
  WASP_TRY_READ(bytes, ReadBytes(data, sizeof(f64), errors));
  f64 result;
  memcpy(&result, bytes.data(), sizeof(f64));
  return result;
}

template <typename Errors>
optional<Index> ReadCount(SpanU8* data, Errors& errors) {
  WASP_TRY_READ(count, ReadIndex(data, errors));

  // There should be at least one byte per count, so if the data is smaller
  // than that, the module must be malformed.
  if (count > data->size()) {
    errors.OnError(
        *data, format("Count is longer than the data length: {} > {}", count,
                      data->size()));
    return nullopt;
  }

  return count;
}

template <typename Errors>
optional<string_view> ReadStr(SpanU8* data, Errors& errors, string_view desc) {
  ErrorsContextGuard<Errors> guard{errors, *data, desc};
  WASP_TRY_READ(len, ReadCount(data, errors));
  if (len > data->size()) {
    errors.OnError(*data, format("Unable to read string of length {}", len));
    return nullopt;
  }

  string_view result{reinterpret_cast<const char*>(data->data()), len};
  *data = remove_prefix(*data, len);
  return result;
}

template <typename T, typename Errors>
optional<std::vector<T>> ReadVec(SpanU8* data,
                                 Errors& errors,
                                 string_view desc) {
  ErrorsContextGuard<Errors> guard{errors, *data, desc};
  std::vector<T> result;
  WASP_TRY_READ(len, ReadCount(data, errors));
  result.reserve(len);
  for (u32 i = 0; i < len; ++i) {
    WASP_TRY_READ(elt, Read<T>(data, errors));
    result.emplace_back(std::move(elt));
  }
  return result;
}

// -----------------------------------------------------------------------------

template <typename Sequence>
void LazySequenceIteratorBase<Sequence>::Increment() {
  if (!empty()) {
    value_ = Read(&data_, sequence_->errors_, Tag<value_type>{});
    if (!value_) {
      clear();
    }
  } else {
    clear();
  }
}

template <typename Sequence>
LazySequenceIterator<Sequence>::LazySequenceIterator(Sequence* seq, SpanU8 data)
    : base{seq, data} {
  if (!this->empty()) {
    operator++();
  }
}

template <typename Sequence>
auto LazySequenceIterator<Sequence>::operator++() -> LazySequenceIterator& {
  this->Increment();
  return *this;
}

template <typename Sequence>
auto LazySequenceIterator<Sequence>::operator++(int) -> LazySequenceIterator {
  auto temp = *this;
  operator++();
  return temp;
}

// -----------------------------------------------------------------------------

template <typename Errors>
LazyModule<Errors>::LazyModule(SpanU8 data, Errors& errors)
    : magic{ReadBytes(&data, 4, errors)},
      version{ReadBytes(&data, 4, errors)},
      sections{data, errors} {
  const SpanU8 kMagic{encoding::Magic};
  const SpanU8 kVersion{encoding::Version};

  if (magic != kMagic) {
    errors.OnError(
        data, format("Magic mismatch: expected {}, got {}", kMagic, *magic));
  }

  if (version != kVersion) {
    errors.OnError(data, format("Version mismatch: expected {}, got {}",
                                kVersion, *version));
  }
}

// -----------------------------------------------------------------------------

template <typename T, typename Errors>
LazySection<T, Errors>::LazySection(SpanU8 data, Errors& errors)
    : count(ReadCount(&data, errors)), sequence(data, errors) {}

template <typename T, typename Errors>
LazySection<T, Errors>::LazySection(KnownSection<> section, Errors& errors)
    : LazySection(section.data, errors) {}

template <typename Errors>
StartSection<Errors>::StartSection(SpanU8 data, Errors& errors)
    : errors_(errors), start_(Read<Start>(&data, errors)) {}

template <typename Errors>
StartSection<Errors>::StartSection(KnownSection<> section, Errors& errors)
    : errors_(errors), start_(Read<Start>(&section.data, errors)) {}

template <typename Errors>
optional<Start> StartSection<Errors>::start() {
  return start_;
}

// -----------------------------------------------------------------------------

#define WASP_TRY_DECODE(out_var, in_var, Type, name)            \
  auto out_var = encoding::Type::Decode(in_var);                \
  if (!out_var) {                                               \
    errors.OnError(*data, format("Unknown " name " {}", in_var)); \
    return nullopt;                                             \
  }

template <typename Errors>
optional<ValType> Read(SpanU8* data, Errors& errors, Tag<ValType>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "valtype"};
  WASP_TRY_READ(val, Read<u8>(data, errors));
  WASP_TRY_DECODE(decoded, val, ValType, "valtype");
  return decoded;
}

template <typename Errors>
optional<BlockType> Read(SpanU8* data, Errors& errors, Tag<BlockType>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "blocktype"};
  WASP_TRY_READ(val, Read<u8>(data, errors));
  WASP_TRY_DECODE(decoded, val, BlockType, "blocktype");
  return decoded;
}

template <typename Errors>
optional<ElemType> Read(SpanU8* data, Errors& errors, Tag<ElemType>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "elemtype"};
  WASP_TRY_READ(val, Read<u8>(data, errors));
  WASP_TRY_DECODE(decoded, val, ElemType, "elemtype");
  return decoded;
}

template <typename Errors>
optional<ExternalKind> Read(SpanU8* data, Errors& errors, Tag<ExternalKind>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "external kind"};
  WASP_TRY_READ(val, Read<u8>(data, errors));
  WASP_TRY_DECODE(decoded, val, ExternalKind, "external kind");
  return decoded;
}

template <typename Errors>
optional<Mutability> Read(SpanU8* data, Errors& errors, Tag<Mutability>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "mutability"};
  WASP_TRY_READ(val, Read<u8>(data, errors));
  WASP_TRY_DECODE(decoded, val, Mutability, "mutability");
  return decoded;
}

template <typename Errors>
optional<SectionId> Read(SpanU8* data, Errors& errors, Tag<SectionId>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "section"};
  WASP_TRY_READ(val, Read<u32>(data, errors));
  WASP_TRY_DECODE(decoded, val, Section, "section");
  return decoded;
}

template <typename Errors>
optional<Opcode> Read(SpanU8* data, Errors& errors, Tag<Opcode>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "opcode"};
  WASP_TRY_READ(val, Read<u8>(data, errors));
  WASP_TRY_DECODE(decoded, val, Opcode, "opcode");
  return decoded;
}

#undef WASP_TRY_DECODE

template <typename Errors>
optional<Limits> Read(SpanU8* data, Errors& errors, Tag<Limits>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "limits"};
  const u32 kFlags_HasMax = 1;
  WASP_TRY_READ_CONTEXT(flags, Read<u32>(data, errors), "flags");
  WASP_TRY_READ_CONTEXT(min, Read<u32>(data, errors), "min");

  if (flags & kFlags_HasMax) {
    WASP_TRY_READ_CONTEXT(max, Read<u32>(data, errors), "max");
    return Limits{min, max};
  } else {
    return Limits{min};
  }
}

template <typename Errors>
optional<LocalDecl> Read(SpanU8* data, Errors& errors, Tag<LocalDecl>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "local decl"};
  WASP_TRY_READ_CONTEXT(count, ReadIndex(data, errors), "count");
  WASP_TRY_READ_CONTEXT(type, Read<ValType>(data, errors), "type");
  return LocalDecl{count, type};
}

template <typename Errors>
optional<FuncType> Read(SpanU8* data, Errors& errors, Tag<FuncType>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "func type"};
  WASP_TRY_READ(param_types, ReadVec<ValType>(data, errors, "param types"));
  WASP_TRY_READ(result_types, ReadVec<ValType>(data, errors, "result types"));
  return FuncType{std::move(param_types), std::move(result_types)};
}

template <typename Errors>
optional<TypeEntry> Read(SpanU8* data, Errors& errors, Tag<TypeEntry>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "type entry"};
  WASP_TRY_READ_CONTEXT(form, Read<u8>(data, errors), "form");

  if (form != encoding::Type::Func) {
    errors.OnError(*data, format("Unknown type form: {}", form));
    return nullopt;
  }

  WASP_TRY_READ(func_type, Read<FuncType>(data, errors));
  return TypeEntry{form, std::move(func_type)};
}

template <typename Errors>
optional<TableType> Read(SpanU8* data, Errors& errors, Tag<TableType>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "table type"};
  WASP_TRY_READ(elemtype, Read<ElemType>(data, errors));
  WASP_TRY_READ(limits, Read<Limits>(data, errors));
  return TableType{limits, elemtype};
}

template <typename Errors>
optional<MemoryType> Read(SpanU8* data, Errors& errors, Tag<MemoryType>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "memory type"};
  WASP_TRY_READ(limits, Read<Limits>(data, errors));
  return MemoryType{limits};
}

template <typename Errors>
optional<GlobalType> Read(SpanU8* data, Errors& errors, Tag<GlobalType>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "global type"};
  WASP_TRY_READ(type, Read<ValType>(data, errors));
  WASP_TRY_READ(mut, Read<Mutability>(data, errors));
  return GlobalType{type, mut};
}

template <typename Errors>
optional<Section<>> Read(SpanU8* data, Errors& errors, Tag<Section<>>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "section"};
  WASP_TRY_READ_CONTEXT(id, Read<SectionId>(data, errors), "id");
  WASP_TRY_READ_CONTEXT(len, Read<u32>(data, errors), "length");
  if (len > data->size()) {
    errors.OnError(*data, format("Section length is too long: {} > {}", len,
                                 data->size()));
    return nullopt;
  }

  SpanU8 section_span = data->subspan(0, len);
  *data = remove_prefix(*data, len);

  if (id == SectionId::Custom) {
    WASP_TRY_READ(name, ReadStr(&section_span, errors, "custom section name"));
    return Section<>{CustomSection<>{name, section_span}};
  } else {
    return Section<>{KnownSection<>{id, section_span}};
  }
}

template <typename Errors>
optional<Import<>> Read(SpanU8* data, Errors& errors, Tag<Import<>>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "import"};
  WASP_TRY_READ(module, ReadStr(data, errors, "module name"));
  WASP_TRY_READ(name, ReadStr(data, errors, "field name"));
  WASP_TRY_READ(kind, Read<ExternalKind>(data, errors));
  switch (kind) {
    case ExternalKind::Func: {
      WASP_TRY_READ(type_index, ReadIndex(data, errors));
      return Import<>{module, name, type_index};
    }
    case ExternalKind::Table: {
      WASP_TRY_READ(table_type, Read<TableType>(data, errors));
      return Import<>{module, name, table_type};
    }
    case ExternalKind::Memory: {
      WASP_TRY_READ(memory_type, Read<MemoryType>(data, errors));
      return Import<>{module, name, memory_type};
    }
    case ExternalKind::Global: {
      WASP_TRY_READ(global_type, Read<GlobalType>(data, errors));
      return Import<>{module, name, global_type};
    }
  }
}

template <typename Errors>
optional<ConstExpr<>> Read(SpanU8* data, Errors& errors, Tag<ConstExpr<>>) {
  LazyInstrs<Errors> instrs{*data, errors};
  auto iter = instrs.begin(), end = instrs.end();

  // Read instruction.
  if (iter == end) {
    errors.OnError(*data, "Unexpected end of const expr");
    return nullopt;
  }
  auto instr = *iter++;
  switch (instr.opcode) {
    case Opcode::I32Const:
    case Opcode::I64Const:
    case Opcode::F32Const:
    case Opcode::F64Const:
    case Opcode::GetGlobal:
      // OK.
      break;

    default:
      errors.OnError(*data,
                     format("Illegal instruction in const expr: {}", instr));
      return nullopt;
  }

  // Instruction must be followed by end.
  if (iter == end || iter->opcode != Opcode::End) {
    errors.OnError(*data, "Expected end instruction");
    return nullopt;
  }

  auto len = iter.data().begin() - data->begin();
  ConstExpr<> expr{data->subspan(0, len)};
  *data = remove_prefix(*data, len);
  return expr;
}

template <typename Errors>
optional<Instr> Read(SpanU8* data, Errors& errors, Tag<Instr>) {
  WASP_TRY_READ_CONTEXT(opcode, Read<Opcode>(data, errors), "opcode");
  switch (opcode) {
    // No immediates:
    case Opcode::End:
    case Opcode::Unreachable:
    case Opcode::Nop:
    case Opcode::Else:
    case Opcode::Return:
    case Opcode::Drop:
    case Opcode::Select:
    case Opcode::I32Eqz:
    case Opcode::I32Eq:
    case Opcode::I32Ne:
    case Opcode::I32LtS:
    case Opcode::I32LeS:
    case Opcode::I32LtU:
    case Opcode::I32LeU:
    case Opcode::I32GtS:
    case Opcode::I32GeS:
    case Opcode::I32GtU:
    case Opcode::I32GeU:
    case Opcode::I64Eqz:
    case Opcode::I64Eq:
    case Opcode::I64Ne:
    case Opcode::I64LtS:
    case Opcode::I64LeS:
    case Opcode::I64LtU:
    case Opcode::I64LeU:
    case Opcode::I64GtS:
    case Opcode::I64GeS:
    case Opcode::I64GtU:
    case Opcode::I64GeU:
    case Opcode::F32Eq:
    case Opcode::F32Ne:
    case Opcode::F32Lt:
    case Opcode::F32Le:
    case Opcode::F32Gt:
    case Opcode::F32Ge:
    case Opcode::F64Eq:
    case Opcode::F64Ne:
    case Opcode::F64Lt:
    case Opcode::F64Le:
    case Opcode::F64Gt:
    case Opcode::F64Ge:
    case Opcode::I32Clz:
    case Opcode::I32Ctz:
    case Opcode::I32Popcnt:
    case Opcode::I32Add:
    case Opcode::I32Sub:
    case Opcode::I32Mul:
    case Opcode::I32DivS:
    case Opcode::I32DivU:
    case Opcode::I32RemS:
    case Opcode::I32RemU:
    case Opcode::I32And:
    case Opcode::I32Or:
    case Opcode::I32Xor:
    case Opcode::I32Shl:
    case Opcode::I32ShrS:
    case Opcode::I32ShrU:
    case Opcode::I32Rotl:
    case Opcode::I32Rotr:
    case Opcode::I64Clz:
    case Opcode::I64Ctz:
    case Opcode::I64Popcnt:
    case Opcode::I64Add:
    case Opcode::I64Sub:
    case Opcode::I64Mul:
    case Opcode::I64DivS:
    case Opcode::I64DivU:
    case Opcode::I64RemS:
    case Opcode::I64RemU:
    case Opcode::I64And:
    case Opcode::I64Or:
    case Opcode::I64Xor:
    case Opcode::I64Shl:
    case Opcode::I64ShrS:
    case Opcode::I64ShrU:
    case Opcode::I64Rotl:
    case Opcode::I64Rotr:
    case Opcode::F32Abs:
    case Opcode::F32Neg:
    case Opcode::F32Ceil:
    case Opcode::F32Floor:
    case Opcode::F32Trunc:
    case Opcode::F32Nearest:
    case Opcode::F32Sqrt:
    case Opcode::F32Add:
    case Opcode::F32Sub:
    case Opcode::F32Mul:
    case Opcode::F32Div:
    case Opcode::F32Min:
    case Opcode::F32Max:
    case Opcode::F32Copysign:
    case Opcode::F64Abs:
    case Opcode::F64Neg:
    case Opcode::F64Ceil:
    case Opcode::F64Floor:
    case Opcode::F64Trunc:
    case Opcode::F64Nearest:
    case Opcode::F64Sqrt:
    case Opcode::F64Add:
    case Opcode::F64Sub:
    case Opcode::F64Mul:
    case Opcode::F64Div:
    case Opcode::F64Min:
    case Opcode::F64Max:
    case Opcode::F64Copysign:
    case Opcode::I32WrapI64:
    case Opcode::I32TruncSF32:
    case Opcode::I32TruncUF32:
    case Opcode::I32TruncSF64:
    case Opcode::I32TruncUF64:
    case Opcode::I64ExtendSI32:
    case Opcode::I64ExtendUI32:
    case Opcode::I64TruncSF32:
    case Opcode::I64TruncUF32:
    case Opcode::I64TruncSF64:
    case Opcode::I64TruncUF64:
    case Opcode::F32ConvertSI32:
    case Opcode::F32ConvertUI32:
    case Opcode::F32ConvertSI64:
    case Opcode::F32ConvertUI64:
    case Opcode::F32DemoteF64:
    case Opcode::F64ConvertSI32:
    case Opcode::F64ConvertUI32:
    case Opcode::F64ConvertSI64:
    case Opcode::F64ConvertUI64:
    case Opcode::F64PromoteF32:
    case Opcode::I32ReinterpretF32:
    case Opcode::I64ReinterpretF64:
    case Opcode::F32ReinterpretI32:
    case Opcode::F64ReinterpretI64:
      return Instr{Opcode{opcode}};

    // Type immediate.
    case Opcode::Block:
    case Opcode::Loop:
    case Opcode::If: {
      WASP_TRY_READ(type, Read<BlockType>(data, errors));
      return Instr{Opcode{opcode}, type};
    }

    // Index immediate.
    case Opcode::Br:
    case Opcode::BrIf:
    case Opcode::Call:
    case Opcode::GetLocal:
    case Opcode::SetLocal:
    case Opcode::TeeLocal:
    case Opcode::GetGlobal:
    case Opcode::SetGlobal: {
      WASP_TRY_READ(index, ReadIndex(data, errors));
      return Instr{Opcode{opcode}, index};
    }

    // Index* immediates.
    case Opcode::BrTable: {
      WASP_TRY_READ(targets, ReadVec<Index>(data, errors, "br_table targets"));
      WASP_TRY_READ_CONTEXT(default_target, ReadIndex(data, errors),
                            "br_table default target");
      return Instr{Opcode{opcode},
                   BrTableImmediate{std::move(targets), default_target}};
    }

    // Index, reserved immediates.
    case Opcode::CallIndirect: {
      WASP_TRY_READ(index, ReadIndex(data, errors));
      WASP_TRY_READ_CONTEXT(reserved, Read<u8>(data, errors), "reserved");
      return Instr{Opcode{opcode}, CallIndirectImmediate{index, reserved}};
    }

    // Memarg (alignment, offset) immediates.
    case Opcode::I32Load:
    case Opcode::I64Load:
    case Opcode::F32Load:
    case Opcode::F64Load:
    case Opcode::I32Load8S:
    case Opcode::I32Load8U:
    case Opcode::I32Load16S:
    case Opcode::I32Load16U:
    case Opcode::I64Load8S:
    case Opcode::I64Load8U:
    case Opcode::I64Load16S:
    case Opcode::I64Load16U:
    case Opcode::I64Load32S:
    case Opcode::I64Load32U:
    case Opcode::I32Store:
    case Opcode::I64Store:
    case Opcode::F32Store:
    case Opcode::F64Store:
    case Opcode::I32Store8:
    case Opcode::I32Store16:
    case Opcode::I64Store8:
    case Opcode::I64Store16:
    case Opcode::I64Store32: {
      WASP_TRY_READ(memarg, Read<MemArg>(data, errors));
      return Instr{Opcode{opcode}, memarg};
    }

    // Reserved immediates.
    case Opcode::MemorySize:
    case Opcode::MemoryGrow: {
      WASP_TRY_READ_CONTEXT(reserved, Read<u8>(data, errors), "reserved");
      return Instr{Opcode{opcode}, reserved};
    }

    // Const immediates.
    case Opcode::I32Const: {
      WASP_TRY_READ_CONTEXT(value, Read<s32>(data, errors), "i32 constant");
      return Instr{Opcode{opcode}, value};
    }

    case Opcode::I64Const: {
      WASP_TRY_READ_CONTEXT(value, Read<s64>(data, errors), "i64 constant");
      return Instr{Opcode{opcode}, value};
    }

    case Opcode::F32Const: {
      WASP_TRY_READ_CONTEXT(value, Read<f32>(data, errors), "f32 constant");
      return Instr{Opcode{opcode}, value};
    }

    case Opcode::F64Const: {
      WASP_TRY_READ_CONTEXT(value, Read<f64>(data, errors), "f64 constant");
      return Instr{Opcode{opcode}, value};
    }

    default:
      errors.OnError(*data, format("Unknown opcode {:#02x}", opcode));
      return nullopt;
  }
}

template <typename Errors>
optional<Func> Read(SpanU8* data, Errors& errors, Tag<Func>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "func"};
  WASP_TRY_READ(type_index, ReadIndex(data, errors));
  return Func{type_index};
}

template <typename Errors>
optional<Table> Read(SpanU8* data, Errors& errors, Tag<Table>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "table"};
  WASP_TRY_READ(table_type, Read<TableType>(data, errors));
  return Table{table_type};
}

template <typename Errors>
optional<Memory> Read(SpanU8* data, Errors& errors, Tag<Memory>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "memory"};
  WASP_TRY_READ(memory_type, Read<MemoryType>(data, errors));
  return Memory{memory_type};
}

template <typename Errors>
optional<Global<>> Read(SpanU8* data, Errors& errors, Tag<Global<>>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "global"};
  WASP_TRY_READ(global_type, Read<GlobalType>(data, errors));
  WASP_TRY_READ(init_expr, Read<ConstExpr<>>(data, errors));
  return Global<>{global_type, std::move(init_expr)};
}

template <typename Errors>
optional<Export<>> Read(SpanU8* data, Errors& errors, Tag<Export<>>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "export"};
  WASP_TRY_READ(name, ReadStr(data, errors, "name"));
  WASP_TRY_READ(kind, Read<ExternalKind>(data, errors));
  WASP_TRY_READ(index, ReadIndex(data, errors));
  return Export<>{kind, name, index};
}

template <typename Errors>
optional<MemArg> Read(SpanU8* data, Errors& errors, Tag<MemArg>) {
  WASP_TRY_READ_CONTEXT(align_log2, Read<u32>(data, errors), "align log2");
  WASP_TRY_READ_CONTEXT(offset, Read<u32>(data, errors), "offset");
  return MemArg{align_log2, offset};
}

template <typename Errors>
optional<Start> Read(SpanU8* data, Errors& errors, Tag<Start>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "start"};
  WASP_TRY_READ(index, ReadIndex(data, errors));
  return Start{index};
}

template <typename Errors>
optional<ElementSegment<>> Read(SpanU8* data,
                                Errors& errors,
                                Tag<ElementSegment<>>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "element segment"};
  WASP_TRY_READ_CONTEXT(table_index, ReadIndex(data, errors), "table index");
  WASP_TRY_READ_CONTEXT(offset, Read<ConstExpr<>>(data, errors), "offset");
  WASP_TRY_READ(init, ReadVec<Index>(data, errors, "initializers"));
  return ElementSegment<>{table_index, std::move(offset), std::move(init)};
}

template <typename Errors>
optional<Code<>> Read(SpanU8* data, Errors& errors, Tag<Code<>>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "code"};
  WASP_TRY_READ(body_size, ReadCount(data, errors));
  WASP_TRY_READ(body, ReadBytes(data, body_size, errors));
  WASP_TRY_READ(local_decls, ReadVec<LocalDecl>(&body, errors, "local decls"));
  return Code<>{std::move(local_decls), Expr<>{std::move(body)}};
}

template <typename Errors>
optional<DataSegment<>> Read(SpanU8* data, Errors& errors, Tag<DataSegment<>>) {
  ErrorsContextGuard<Errors> guard{errors, *data, "data segment"};
  WASP_TRY_READ_CONTEXT(memory_index, ReadIndex(data, errors), "memory index");
  WASP_TRY_READ_CONTEXT(offset, Read<ConstExpr<>>(data, errors), "offset");
  WASP_TRY_READ(len, ReadCount(data, errors));
  WASP_TRY_READ(init, ReadBytes(data, len, errors));
  return DataSegment<>{memory_index, std::move(offset), init};
}

#undef WASP_TRY_READ
#undef WASP_TRY_READ_CONTEXT

}  // namespace binary
}  // namespace wasp
