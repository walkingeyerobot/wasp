//
// Copyright 2020 WebAssembly Community Group participants
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

#include "wasp/valid/validate_visitor.h"

#include <cassert>

namespace wasp::valid {

ValidateVisitor::ValidateVisitor(Features features, Errors& errors)
    : context{features, errors}, features{features}, errors{errors} {}

auto ValidateVisitor::BeginTypeSection(binary::LazyTypeSection sec) -> Result {
  return FailUnless(valid::BeginTypeSection(context, sec.count.value_or(0)));
}

auto ValidateVisitor::OnType(const At<binary::DefinedType>& defined_type)
    -> Result {
  return FailUnless(Validate(context, defined_type));
}

auto ValidateVisitor::OnImport(const At<binary::Import>& import) -> Result {
  return FailUnless(Validate(context, import));
}

auto ValidateVisitor::OnFunction(const At<binary::Function>& function)
    -> Result {
  return FailUnless(Validate(context, function));
}

auto ValidateVisitor::OnTable(const At<binary::Table>& table) -> Result {
  return FailUnless(Validate(context, table));
}

auto ValidateVisitor::OnMemory(const At<binary::Memory>& memory) -> Result {
  return FailUnless(Validate(context, memory));
}

auto ValidateVisitor::OnGlobal(const At<binary::Global>& global) -> Result {
  return FailUnless(Validate(context, global));
}

auto ValidateVisitor::OnExport(const At<binary::Export>& export_) -> Result {
  return FailUnless(Validate(context, export_));
}

auto ValidateVisitor::OnStart(const At<binary::Start>& start) -> Result {
  return FailUnless(Validate(context, start));
}

auto ValidateVisitor::OnElement(const At<binary::ElementSegment>& segment)
    -> Result {
  return FailUnless(Validate(context, segment));
}

auto ValidateVisitor::OnDataCount(const At<binary::DataCount>& data_count)
    -> Result {
  return FailUnless(Validate(context, data_count));
}

auto ValidateVisitor::BeginCode(const At<binary::Code>& code) -> Result {
  return FailUnless(valid::BeginCode(context, code.loc()) &&
                    Validate(context, code->locals, RequireDefaultable::Yes));
}

auto ValidateVisitor::OnInstruction(const At<binary::Instruction>& instruction)
    -> Result {
  return FailUnless(Validate(context, instruction));
}

auto ValidateVisitor::OnData(const At<binary::DataSegment>& segment) -> Result {
  return FailUnless(Validate(context, segment));
}

auto ValidateVisitor::FailUnless(bool b) -> Result {
  return b ? Result::Ok : Result::Fail;
}

}  // namespace wasp::valid
