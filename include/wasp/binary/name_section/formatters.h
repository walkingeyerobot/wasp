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

#ifndef WASP_BINARY_NAME_SECTION_FORMATTERS_H_
#define WASP_BINARY_NAME_SECTION_FORMATTERS_H_

#include "wasp/base/format.h"
#include "wasp/binary/name_section/types.h"

namespace fmt {

#define WASP_DEFINE_FORMATTER(Name)                                   \
  template <>                                                         \
  struct formatter<::wasp::binary::Name> : formatter<string_view> {   \
    template <typename Ctx>                                           \
    typename Ctx::iterator format(const ::wasp::binary::Name&, Ctx&); \
  } /* No semicolon. */

WASP_DEFINE_FORMATTER(NameSubsectionId);
WASP_DEFINE_FORMATTER(NameAssoc);
WASP_DEFINE_FORMATTER(IndirectNameAssoc);
WASP_DEFINE_FORMATTER(NameSubsection);

#undef WASP_DEFINE_FORMATTER

}  // namespace fmt

#include "wasp/binary/name_section/formatters-inl.h"

#endif  // WASP_BINARY_NAME_SECTION_FORMATTERS_H_