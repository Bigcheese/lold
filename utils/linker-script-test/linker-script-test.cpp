//===- utils/linker-script-test/linker-script-test.cpp --------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Tool for testing linker script parsing.
///
//===----------------------------------------------------------------------===//

#include "llvm/ADT/OwningPtr.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/system_error.h"

class Token {
public:
  enum Kind {
    unknown,
    eof,
    identifier,
    l_paren,
    r_paren,
    kw_group,
    kw_output_format,
    kw_as_needed
  };

  Token() : _kind(unknown) {}
  Token(StringRef range, Kind kind) : _range(range), _kind(kind) {}

  StringRef _range;
  Kind _kind;
};

using namespace llvm;

StringRef skipWhitespace(StringRef buffer) {
  if (buffer.empty())
    return buffer;

  while (true) {
    switch (buffer[0]) {
    case ' ':
    case '\r':
    case '\n':
    case '\t':
      buffer = buffer.substr(1);
      break;
    // Potential comment.
    case '\\':
      
      break;
    default:
      return buffer;
    }
  }
}

StringRef lex(StringRef buffer, Token &tok) {
  assert(buffer.size() >= 1 && "lex got empty buffer!");
  if (buffer[0] == 0) {
    tok = Token(buffer.substr(0, 1), Token::eof);
    return buffer.substr(1, 0);
  }
}

int main(int argc, const char **argv) {
  SourceMgr sm;
  llvm::OwningPtr<MemoryBuffer> mb;
  if (error_code ec = MemoryBuffer::getFileOrSTDIN(argv[1], mb)) {
    llvm::errs() << ec.message() << "\n";
    return 1;
  }
  StringRef file = mb->getBuffer();
  sm.AddNewSourceBuffer(mb.take(), SMLoc());
}
