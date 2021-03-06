//
// Copyright 2019 WebAssembly Community Group participants
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

#include "wasp/binary/lazy_expression.h"
#include "wasp/binary/read/context.h"

namespace wasp::binary {

LazyExpression ReadExpression(SpanU8 data, Context& context) {
  context.seen_final_end = false;
  return LazyExpression{data, context};
}

LazyExpression ReadExpression(Expression expr, Context& context) {
  return ReadExpression(expr.data, context);
}

}  // namespace wasp::binary
