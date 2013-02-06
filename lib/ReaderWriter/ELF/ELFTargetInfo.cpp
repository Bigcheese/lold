//===- lib/ReaderWriter/ELF/ELFTargetInfo.cpp -----------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/ReaderWriter/ELFTargetInfo.h"

#include "TargetHandler.h"
#include "Targets.h"

#include "lld/Core/LinkerOptions.h"

#include "llvm/ADT/Triple.h"
#include "llvm/Support/ELF.h"

namespace lld {
ELFTargetInfo::ELFTargetInfo(const LinkerOptions &lo) : TargetInfo(lo) {}

uint16_t ELFTargetInfo::getOutputType() const {
  switch (_options._outputKind) {
  case OutputKind::Executable:
    return llvm::ELF::ET_EXEC;
  case OutputKind::Relocatable:
    return llvm::ELF::ET_REL;
  case OutputKind::Shared:
    return llvm::ELF::ET_DYN;
  case OutputKind::Core:
    return llvm::ELF::ET_CORE;
  case OutputKind::SharedStubs:
  case OutputKind::DebugSymbols:
  case OutputKind::Bundle:
  case OutputKind::Preload:
    break;
  }
  llvm_unreachable("Unhandled OutputKind");
}

uint16_t ELFTargetInfo::getOutputMachine() const {
  switch (getTriple().getArch()) {
  case llvm::Triple::x86:
    return llvm::ELF::EM_386;
  case llvm::Triple::x86_64:
    return llvm::ELF::EM_X86_64;
  case llvm::Triple::hexagon:
    return llvm::ELF::EM_HEXAGON;
  case llvm::Triple::ppc:
    return llvm::ELF::EM_PPC;
  default:
    llvm_unreachable("Unhandled arch");
  }
}

ErrorOr<Reader &> ELFTargetInfo::getReader(const LinkerInput &input) const {
  if (!_reader)
    _reader = createReaderELF(*this, std::bind(&ELFTargetInfo::getReader, this,
                                               std::placeholders::_1));
  return *_reader;
}

ErrorOr<Writer &> ELFTargetInfo::getWriter() const {
  if (!_writer) {
    if (_options._outputYAML)
      _writer = createWriterYAML(*this);
    else
      _writer = createWriterELF(*this);
  }
  return *_writer;
}

std::unique_ptr<ELFTargetInfo> ELFTargetInfo::create(const LinkerOptions &lo) {
  switch (llvm::Triple(llvm::Triple::normalize(lo._target)).getArch()) {
  case llvm::Triple::x86:
    return std::unique_ptr<ELFTargetInfo>(new lld::elf::X86TargetInfo(lo));
  case llvm::Triple::x86_64:
    return std::unique_ptr<
        ELFTargetInfo>(new lld::elf::X86_64TargetInfo(lo));
  case llvm::Triple::hexagon:
    return std::unique_ptr<
        ELFTargetInfo>(new lld::elf::HexagonTargetInfo(lo));
  case llvm::Triple::ppc:
    return std::unique_ptr<ELFTargetInfo>(new lld::elf::PPCTargetInfo(lo));
  default:
    return std::unique_ptr<ELFTargetInfo>();
  }
}

StringRef ELFTargetInfo::getEntry() const {
  if (!_options._entrySymbol.empty())
    return _options._entrySymbol;
  return "_start";
}

} // end namespace lld
