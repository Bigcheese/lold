//===- Symbols.cpp --------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Error.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm::object;
using llvm::sys::fs::identify_magic;
using llvm::sys::fs::file_magic;

using namespace lld;
using namespace lld::elfv2;

// Returns 1, 0 or -1 if this symbol should take precedence
// over the Other, tie or lose, respectively.
template <class ELFT>
int DefinedRegular<ELFT>::compare(SymbolBody *Other) {
  if (Other->kind() < kind())
    return -Other->compare(this);
  auto *R = dyn_cast<DefinedRegular>(Other);
  if (!R)
    return 1;

  // Common symbols are weaker than other types of defined symbols.
  if (isCommon() && R->isCommon())
    return (getCommonSize() < R->getCommonSize()) ? -1 : 1;
  // TODO: we are not sure if regular defined symbol and common
  // symbols are allowed to have the same name.
  if (isCommon())
    return -1;
  if (R->isCommon())
    return 1;
  return 0;
}

int DefinedBitcode::compare(SymbolBody *Other) {
  assert(Other->kind() >= kind());
  if (!isa<Defined>(Other))
    return 1;

  if (auto *B = dyn_cast<DefinedBitcode>(Other)) {
    if (!Replaceable && !B->Replaceable)
      return 0;
    // Non-replaceable symbols win.
    return Replaceable ? -1 : 1;
  }

  // As an approximation, regular symbols win over bitcode symbols,
  // but we definitely have a conflict if the regular symbol is not
  // replaceable and neither is the bitcode symbol. We do not
  // replicate the rest of the symbol resolution logic here; symbol
  // resolution will be done accurately after lowering bitcode symbols
  // to regular symbols in addCombinedLTOObject().
  if (auto *R = dyn_cast<DefinedRegular<llvm::object::ELF64LE>>(Other)) {
    if (!R->isCommon() && !Replaceable)
      return 0;
    return -1;
  }
  return 0;
}

int Defined::compare(SymbolBody *Other) {
  if (Other->kind() < kind())
    return -Other->compare(this);
  if (isa<Defined>(Other))
    return 0;
  return 1;
}

int Lazy::compare(SymbolBody *Other) {
  if (Other->kind() < kind())
    return -Other->compare(this);

  // Undefined symbols with weak aliases will turn into defined
  // symbols if they remain undefined, so we don't need to resolve
  // such symbols.
  if (auto *U = dyn_cast<Undefined>(Other))
    if (U->getWeakAlias())
      return -1;
  return 1;
}

int Undefined::compare(SymbolBody *Other) {
  if (Other->kind() < kind())
    return -Other->compare(this);
  if (cast<Undefined>(Other)->getWeakAlias())
    return -1;
  return 1;
}

template <class ELFT>
StringRef DefinedRegular<ELFT>::getName() {
  // DefinedSymbol's name is read lazily for a performance reason.
  // Non-external symbol names are never used by the linker
  // except for logging or debugging.
  // Their internal references are resolved not by name but by symbol index.
  // And because they are not external, no one can refer them by name.
  // Object files contain lots of non-external symbols, and creating
  // StringRefs for them (which involves lots of strlen() on the string table)
  // is a waste of time.
  if (Name.empty())
    Name = *File->getStaticSymbolName(Sym);
  return Name;
}

ErrorOr<std::unique_ptr<InputFile>> Lazy::getMember() {
  auto MBRefOrErr = File->getMember(&Sym);
  if (auto EC = MBRefOrErr.getError())
    return EC;
  MemoryBufferRef MBRef = MBRefOrErr.get();

  // getMember returns an empty buffer if the member was already
  // read from the library.
  if (MBRef.getBuffer().empty())
    return std::unique_ptr<InputFile>(nullptr);

  file_magic Magic = identify_magic(MBRef.getBuffer());
  if (Magic == file_magic::bitcode)
    return std::unique_ptr<InputFile>(new BitcodeFile(MBRef));
  if (Magic != file_magic::elf_relocatable) {
    llvm::errs() << File->getName() << ": unknown file type\n";
    return make_error_code(LLDError::InvalidFile);
  }

  std::unique_ptr<InputFile> Obj(new ObjectFile<llvm::object::ELF64LE>(MBRef));
  Obj->setParentName(File->getName());
  return std::move(Obj);
}

template class DefinedRegular<llvm::object::ELF32LE>;
template class DefinedRegular<llvm::object::ELF32BE>;
template class DefinedRegular<llvm::object::ELF64LE>;
template class DefinedRegular<llvm::object::ELF64BE>;
