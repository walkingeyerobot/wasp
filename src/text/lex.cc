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

#include "wasp/text/read/lex.h"

#include <cassert>

namespace wasp {
namespace text {

namespace {

struct MatchGuard {
  explicit MatchGuard(SpanU8* data) : data_{data}, orig{*data} {}

  SpanU8* Reset() { *data_ = orig; return data_; }
  void ResetUnless(bool b) { if (!b) { Reset(); } }

  Location loc() const { return Location(orig.begin(), data_->begin()); }

  SpanU8* data_;
  SpanU8 orig;
};

enum class CharClass { Reserved = 1, Keyword = 2, HexDigit = 4, Digit = 8 };

bool IsCharClass(int c, CharClass bit) {
  // Generated by the following python script:
  //
  //   def Range(c, lo, hi): return lo <= c <= hi
  //   def IsDigit(c): return Range(c, '0', '9')
  //   def IsHexDigit(c): return IsDigit(c) or Range(c.lower(), 'a', 'f')
  //   def IsKeyword(c): return Range(c, 'a', 'z')
  //   def IsReserved(c): return Range(c, '!', '~') and c not in '"(),;[]{}'
  //
  //   print ([0] + [
  //       (8 if IsDigit(c) else 0) |
  //       (4 if IsHexDigit(c) else 0) |
  //       (2 if IsKeyword(c) else 0) |
  //       (1 if IsReserved(c) else 0)
  //       for c in map(chr, range(0, 127))
  //   ])
  static const char kCharClasses[257] = {
      0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  1,  0,  1,  1,
      1,  1,  1, 0, 0, 1, 1, 0, 1, 1, 1, 13, 13, 13, 13, 13, 13, 13, 13,
      13, 13, 1, 0, 1, 1, 1, 1, 1, 5, 5, 5,  5,  5,  5,  1,  1,  1,  1,
      1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,  0,  1,  0,
      1,  1,  1, 7, 7, 7, 7, 7, 7, 3, 3, 3,  3,  3,  3,  3,  3,  3,  3,
      3,  3,  3, 3, 3, 3, 3, 3, 3, 3, 0, 1,  0,  1,
  };

  assert(c >= -1 && c < 256);
  return (kCharClasses[c + 1] & static_cast<int>(bit)) != 0;
}

bool IsDigit(int c) { return IsCharClass(c, CharClass::Digit); }
bool IsHexDigit(int c) { return IsCharClass(c, CharClass::HexDigit); }
bool IsReserved(int c) { return IsCharClass(c, CharClass::Reserved); }

auto PeekChar(SpanU8* data, int offset = 0) -> int {
  if (offset >= data->size()) {
    return -1;
  }
  return (*data)[offset];
}

void SkipChar(SpanU8* data) {
  assert(!data->empty());
  remove_prefix(data, 1);
}

int ReadReservedChars(SpanU8* data) {
  int count = 0;
  while (IsReserved(PeekChar(data))) {
    SkipChar(data);
    ++count;
  }
  return count;
}

bool NoTrailingReservedChars(SpanU8* data) {
  return ReadReservedChars(data) == 0;
}

auto ReadChar(SpanU8* data) -> int {
  auto result = PeekChar(data);
  if (result != -1) {
    SkipChar(data);
  }
  return result;
}

bool MatchChar(SpanU8* data, char c) {
  if (PeekChar(data) == c) {
    SkipChar(data);
    return true;
  }
  return false;
}

Sign MatchSign(SpanU8* data) {
  if (MatchChar(data, '+')) {
    return Sign::Plus;
  } else if (MatchChar(data, '-')) {
    return Sign::Minus;
  } else {
    return Sign::None;
  }
}

bool MatchString(SpanU8* data, string_view sv) {
  MatchGuard guard{data};
  for (char c : sv) {
    if (!MatchChar(data, c)) {
      guard.Reset();
      return false;
    }
  }
  return true;
}

bool MatchNum(SpanU8* data, HasUnderscores& has_underscores) {
  MatchGuard guard{data};
  bool ok = false;
  while (IsDigit(PeekChar(data))) {
    SkipChar(data);
    if (MatchChar(data, '_')) {
      ok = false;
      has_underscores = HasUnderscores::Yes;
    } else {
      ok = true;
    }
  }
  guard.ResetUnless(ok);
  return ok;
}

bool MatchHexNum(SpanU8* data, HasUnderscores& has_underscores) {
  MatchGuard guard{data};
  bool ok = false;
  while (IsHexDigit(PeekChar(data))) {
    SkipChar(data);
    if (MatchChar(data, '_')) {
      ok = false;
      has_underscores = HasUnderscores::Yes;
    } else {
      ok = true;
    }
  }
  guard.ResetUnless(ok);
  return ok;
}

auto LexReserved(SpanU8* data) -> Token {
  MatchGuard guard{data};
  while (IsReserved(PeekChar(data))) {
    SkipChar(data);
  }
  return Token(guard.loc(), TokenType::Reserved);
}

auto LexAnnotation(SpanU8* data) -> Token {
  MatchGuard guard{data};
  ReadReservedChars(data);
  return Token(guard.loc(), TokenType::LparAnn);
}

auto LexBlockComment(SpanU8* data) -> Token {
  MatchGuard guard{data};
  int nesting = 0;
  while (true) {
    switch (ReadChar(data)) {
      case -1:
        return Token(guard.loc(), TokenType::InvalidBlockComment);

      case ';':
        if (MatchChar(data, ')') && --nesting == 0) {
          return Token(guard.loc(), TokenType::BlockComment);
        }
        break;

      case '(':
        if (MatchChar(data, ';')) {
          nesting++;
        }
        break;

      default: break;
    }
  }
}

auto LexId(SpanU8* data) -> Token {
  MatchGuard guard{data};
  SkipChar(data);  // $
  if (NoTrailingReservedChars(data)) {
    return Token(guard.loc(), TokenType::Reserved);  // Just $
  }
  return Token(guard.loc(), TokenType::Id);
}

auto LexLineComment(SpanU8* data) -> Token {
  MatchGuard guard{data};
  while (true) {
    switch (ReadChar(data)) {
      case -1:
        return Token(guard.loc(), TokenType::InvalidLineComment);

      case '\n':
        return Token(guard.loc(), TokenType::LineComment);
      default: break;
    }
  }
}

auto LexNameEqNum(SpanU8* data, string_view sv, TokenType tt) -> Token {
  MatchGuard guard{data};
  if (MatchString(data, sv)) {
    HasUnderscores underscores{};
    if (MatchString(data, "0x")) {
      if (MatchHexNum(data, underscores) && NoTrailingReservedChars(data)) {
        return Token(guard.loc(), tt, LiteralInfo::HexNat(underscores));
      }
    } else {
      if (MatchNum(data, underscores) && NoTrailingReservedChars(data)) {
        return Token(guard.loc(), tt, LiteralInfo::Nat(underscores));
      }
    }
  }
  return LexReserved(guard.Reset());
}

auto LexInf(SpanU8* data) -> Token {
  MatchGuard guard{data};
  auto sign = MatchSign(data);
  if (MatchString(data, "inf") && NoTrailingReservedChars(data)) {
    return Token(guard.loc(), TokenType::Float, LiteralInfo::Infinity(sign));
  }
  return LexReserved(data);
}

auto LexNan(SpanU8* data) -> Token {
  MatchGuard guard{data};
  auto sign = MatchSign(data);
  if (MatchString(data, "nan")) {
    if (MatchChar(data, ':')) {
      HasUnderscores underscores{};
      if (MatchString(data, "0x") && MatchHexNum(data, underscores) &&
          NoTrailingReservedChars(data)) {
        return Token(guard.loc(), TokenType::Float,
                     LiteralInfo::NanPayload(sign, underscores));
      }
    } else if (NoTrailingReservedChars(data)) {
      return Token(guard.loc(), TokenType::Float, LiteralInfo::Nan(sign));
    }
  }
  return LexReserved(data);
}

auto LexNumber(SpanU8* data, TokenType tt) -> Token {
  MatchGuard guard{data};
  auto sign = MatchSign(data);
  HasUnderscores underscores{};
  if (MatchNum(data, underscores)) {
    if (MatchChar(data, '.')) {
      tt = TokenType::Float;
      if (IsDigit(PeekChar(data)) && !MatchNum(data, underscores)) {
        return LexReserved(guard.Reset());
      }
    }
    if (MatchChar(data, 'e') || MatchChar(data, 'E')) {
      tt = TokenType::Float;
      MatchSign(data);
      if (!MatchNum(data, underscores)) {
        return LexReserved(guard.Reset());
      }
    }
    if (NoTrailingReservedChars(data)) {
      return Token(guard.loc(), tt, LiteralInfo::Number(sign, underscores));
    }
  }
  return LexReserved(guard.Reset());
}

auto LexHexNumber(SpanU8* data, TokenType tt) -> Token {
  MatchGuard guard{data};
  auto sign = MatchSign(data);
  HasUnderscores underscores{};
  MatchString(data, "0x");
  if (MatchHexNum(data, underscores)) {
    if (MatchChar(data, '.')) {
      tt = TokenType::Float;
      if (IsHexDigit(PeekChar(data)) && !MatchHexNum(data, underscores)) {
        return LexReserved(guard.Reset());
      }
    }
    if (MatchChar(data, 'p') || MatchChar(data, 'P')) {
      tt = TokenType::Float;
      MatchSign(data);
      if (!MatchNum(data, underscores)) {
        return LexReserved(guard.Reset());
      }
    }
    if (NoTrailingReservedChars(data)) {
      return Token(guard.loc(), tt, LiteralInfo::HexNumber(sign, underscores));
    }
  }
  return LexReserved(guard.Reset());
}

auto LexText(SpanU8* data) -> Token {
  MatchGuard guard{data};
  MatchChar(data, '"');
  bool has_error = false;
  bool in_string = true;
  u32 byte_size = 0;
  while (in_string) {
    switch (ReadChar(data)) {
      case -1:
        has_error = true;
        in_string = false;
        break;

      case '\n':
        has_error = true;
        break;

      case '"':
        in_string = false;
        break;

      case '\\':
        switch (ReadChar(data)) {
          case 't':
          case 'n':
          case 'r':
          case '"':
          case '\'':
          case '\\':
            // Valid escape.
            byte_size++;
            break;

          case '0':
          case '1':
          case '2':
          case '3':
          case '4':
          case '5':
          case '6':
          case '7':
          case '8':
          case '9':
          case 'a':
          case 'b':
          case 'c':
          case 'd':
          case 'e':
          case 'f':
          case 'A':
          case 'B':
          case 'C':
          case 'D':
          case 'E':
          case 'F':  // Hex byte escape.
            if (IsHexDigit(PeekChar(data))) {
              SkipChar(data);
              byte_size++;
              break;
            }
            // Fallthrough.

          default:
            has_error = true;
            break;
        }
        break;

      default:
        byte_size++;
        break;
    }
  }

  Location loc = guard.loc();
  if (has_error) {
    return Token(loc, TokenType::InvalidText);
  }

  return Token(loc, TokenType::Text, Text{ToStringView(loc), byte_size});
}

auto LexWhitespace(SpanU8* data) -> Token {
  MatchGuard guard{data};
  while (true) {
    switch (PeekChar(data)) {
      case ' ':
      case '\t':
      case '\r':
        SkipChar(data);
        break;

      case '\n':
        SkipChar(data);
        break;

      default:
        return Token(guard.loc(), TokenType::Whitespace);
    }
  }
}

auto LexKeyword(SpanU8* data, string_view sv, TokenType tt) -> Token {
  MatchGuard guard{data};
  if (MatchString(data, sv) && NoTrailingReservedChars(data)) {
    return Token(guard.loc(), tt);
  }
  return LexReserved(guard.Reset());
}

auto LexKeyword(SpanU8* data, string_view sv, Opcode o, Features::Bits f = 0)
    -> Token {
  MatchGuard guard{data};
  if (MatchString(data, sv) && NoTrailingReservedChars(data)) {
    return Token(guard.loc(), TokenType::BareInstr, OpcodeInfo{o, Features{f}});
  }
  return LexReserved(guard.Reset());
}

auto LexKeyword(SpanU8* data,
                string_view sv,
                TokenType tt,
                Opcode o,
                Features::Bits f = 0) -> Token {
  MatchGuard guard{data};
  if (MatchString(data, sv) && NoTrailingReservedChars(data)) {
    return Token(guard.loc(), tt, OpcodeInfo{o, Features{f}});
  }
  return LexReserved(guard.Reset());
}

auto LexKeyword(SpanU8* data, string_view sv, ValueType vt) -> Token {
  MatchGuard guard{data};
  if (MatchString(data, sv) && NoTrailingReservedChars(data)) {
    return Token(guard.loc(), TokenType::ValueType, vt);
  }
  return LexReserved(guard.Reset());
}

auto LexKeyword(SpanU8* data, string_view sv, TokenType tt, ReferenceType rt)
    -> Token {
  MatchGuard guard{data};
  if (MatchString(data, sv) && NoTrailingReservedChars(data)) {
    return Token(guard.loc(), tt, rt);
  }
  return LexReserved(guard.Reset());
}

auto LexKeyword(SpanU8* data, string_view sv, TokenType tt, LiteralKind lk)
    -> Token {
  MatchGuard guard{data};
  if (MatchString(data, sv) && NoTrailingReservedChars(data)) {
    return Token(guard.loc(), tt, LiteralInfo{lk});
  }
  return LexReserved(guard.Reset());
}

}  // namespace

auto Lex(SpanU8* data) -> Token {
  MatchGuard guard{data};
  switch (PeekChar(data)) {
    case -1:
      return Token(*data, TokenType::Eof);

    case '(':
      if (MatchString(data, "(;")) {
        return LexBlockComment(guard.Reset());
      } else if (MatchString(data, "(@")) {
        return LexAnnotation(guard.Reset());
      } else {
        SkipChar(data);
        return Token(guard.loc(), TokenType::Lpar);
      }
      break;

    case ')':
      SkipChar(data);
      return Token(guard.loc(), TokenType::Rpar);

    case ';':
      if (MatchString(data, ";;")) {
        return LexLineComment(guard.Reset());
      } else {
        SkipChar(data);
        return Token(guard.loc(), TokenType::InvalidChar);
      }
      break;

    case ' ':
    case '\t':
    case '\r':
    case '\n':
      return LexWhitespace(data);

    case '"':
      return LexText(guard.Reset());

    case '+':
    case '-':
      switch (PeekChar(data, 1)) {
        case 'i':
          return LexInf(guard.Reset());

        case 'n':
          return LexNan(guard.Reset());

        case '0':
          SkipChar(data);
          return MatchString(data, "0x")
                     ? LexHexNumber(guard.Reset(), TokenType::Int)
                     : LexNumber(guard.Reset(), TokenType::Int);
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
          return LexNumber(guard.Reset(), TokenType::Int);

        default:
          return LexReserved(guard.Reset());
      }
      break;

    case '0':
      return MatchString(data, "0x")
                 ? LexHexNumber(guard.Reset(), TokenType::Nat)
                 : LexNumber(data, TokenType::Nat);

    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      return LexNumber(data, TokenType::Nat);

    case '$':
      return LexId(data);

    default:
#include "src/text/keywords-inl.cc"
  }
  if (IsReserved(PeekChar(data))) {
    return LexReserved(data);
  }
  SkipChar(data);
  return Token(guard.loc(), TokenType::InvalidChar);
}

auto LexNoWhitespace(SpanU8* data) -> Token {
  while (true) {
    auto token = Lex(data);
    switch (token.type) {
      case TokenType::BlockComment:
      case TokenType::LineComment:
      case TokenType::Whitespace:
        break;
      default:
        return token;
    }
  }
}

}  // namespace text
}  // namespace wasp
