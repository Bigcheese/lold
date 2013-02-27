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
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/system_error.h"

using namespace llvm;

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

  void dump(llvm::raw_ostream &os) {
    switch (_kind) {
    case Token::eof:
      os << "eof: ";
      break;
    case Token::identifier:
      os << "identifier: ";
      break;
    case Token::kw_as_needed:
      os << "kw_as_needed: ";
      break;
    case Token::kw_group:
      os << "kw_group: ";
      break;
    case Token::kw_output_format:
      os << "kw_output_format: ";
      break;
    case Token::l_paren:
      os << "l_paren: ";
      break;
    case Token::r_paren:
      os << "r_paren: ";
      break;
    case Token::unknown:
      os << "unknown: ";
      break;
    }
    os << _range << "\n";
  }

  StringRef _range;
  Kind _kind;
};

class Lexer {
public:
  Lexer(std::unique_ptr<llvm::MemoryBuffer> mb) : _buffer(mb->getBuffer()) {
    _sourceManager.AddNewSourceBuffer(mb.release(), SMLoc());
  }

  void lex(Token &tok) {
    skipWhitespace();
    if (_buffer.empty()) {
      tok = Token(_buffer, Token::eof);
      return;
    }
    switch (_buffer[0]) {
    case 0:
      tok = Token(_buffer.substr(0, 1), Token::eof);
      _buffer = _buffer.drop_front();
      return;
    case '(':
      tok = Token(_buffer.substr(0, 1), Token::l_paren);
      _buffer = _buffer.drop_front();
      return;
    case ')':
      tok = Token(_buffer.substr(0, 1), Token::r_paren);
      _buffer = _buffer.drop_front();
      return;
    default:
      /// keyword or identifer.
      StringRef::size_type end = _buffer.find_first_of(" !@#$%^&*?><,~`\t\r\n()+={}[]\"':;\0");
      if (end == StringRef::npos || end == 0) {
        tok = Token(_buffer.substr(0, 1), Token::unknown);
        _buffer = _buffer.drop_front();
        return;
      }
      StringRef word = _buffer.substr(0, end);
      Token::Kind kind = llvm::StringSwitch<Token::Kind>(word)
        .Case("OUTPUT_FORMAT", Token::kw_output_format)
        .Case("GROUP", Token::kw_group)
        .Case("AS_NEEDED", Token::kw_as_needed)
        .Default(Token::identifier);
      tok = Token(word, kind);
      _buffer = _buffer.drop_front(end);
      return;
    }
  }

private:
  void skipWhitespace() {
    while (true) {
      if (_buffer.empty())
        return;
      switch (_buffer[0]) {
      case ' ':
      case '\r':
      case '\n':
      case '\t':
        _buffer = _buffer.drop_front();
        break;
      // Potential comment.
      case '/':
        if (_buffer.size() >= 2 && _buffer[1] == '*') {
          // Skip starting /*
          _buffer = _buffer.drop_front(2);
          // If the next char is also a /, it's not the end.
          if (!_buffer.empty() && _buffer[0] == '/')
            _buffer = _buffer.drop_front();
        
          // Scan for /'s. We're done if it is preceeded by a *.
          while (true) {
            if (_buffer.empty())
              break;
            _buffer = _buffer.drop_front();
            if (_buffer.data()[-1] == '/' && _buffer.data()[-2] == '*')
              break;
          }
        } else
          return;
        break;
      default:
        return;
      }
    }
  }

  /// \brief The current buffer state.
  StringRef _buffer;
  // Lexer owns the input files.
  SourceMgr _sourceManager;
};

class ASTNode {
public:
};

int main(int argc, const char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal();
  llvm::PrettyStackTraceProgram X(argc, argv);

  llvm::OwningPtr<MemoryBuffer> mb;
  if (error_code ec = MemoryBuffer::getFileOrSTDIN(argv[1], mb)) {
    llvm::errs() << ec.message() << "\n";
    return 1;
  }
  Lexer l(std::unique_ptr<MemoryBuffer>(mb.take()));
  Token tok;
  while (true) {
    l.lex(tok);
    tok.dump(llvm::outs());
    if (tok._kind == Token::eof || tok._kind == Token::unknown)
      break;
  }
}
