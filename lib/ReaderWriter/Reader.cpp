//===- lib/ReaderWriter/Reader.cpp ----------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/ReaderWriter/Reader.h"

#include "lld/Core/LinkingContext.h"
#include "lld/ReaderWriter/Writer.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/system_error.h"

#include <memory>

namespace lld {

Reader::~Reader() {
}


YamlIOTaggedDocumentHandler::~YamlIOTaggedDocumentHandler() { }


void Registry::add(std::unique_ptr<Reader> reader) {
  _readers.push_back(std::move(reader));
}

void Registry::add(std::unique_ptr<YamlIOTaggedDocumentHandler> handler) {
  _yamlHandlers.push_back(std::move(handler));
}

error_code Registry::parseFile(std::unique_ptr<MemoryBuffer> &mb,
                               std::vector<std::unique_ptr<File>> &result,
                               bool allowRoundTrip) const {
  // Get file type.
  StringRef content(mb->getBufferStart(), mb->getBufferSize());
  llvm::sys::fs::file_magic fileType = llvm::sys::fs::identify_magic(content);
  // Get file extension.
  StringRef extension = llvm::sys::path::extension(mb->getBufferIdentifier());

  // Ask each registered reader if it can handle this file type or extension.
  for (const std::unique_ptr<Reader> &reader : _readers) {
    if (!reader->canParse(fileType, extension, *mb))
      continue;
    std::vector<std::unique_ptr<File>> res;
    error_code ec = reader->parseFile(mb, *this, res);
#ifndef NDEBUG
    llvm::Optional<std::string> env =
        llvm::sys::Process::GetEnv("LLD_RUN_ROUNDTRIP_TEST");
    if (allowRoundTrip && env.hasValue() && !env.getValue().empty()) {
      for (auto &file : res) {
        std::unique_ptr<Writer> yamlWriter = createWriterYAML(_context);
        SmallString<128> tmpYAMLFile;
        // Separate the directory from the filename
        StringRef outFile = llvm::sys::path::filename(_context.outputPath());
        if (error_code ec = llvm::sys::fs::createTemporaryFile(outFile, "yaml",
                                                               tmpYAMLFile))
          return ec;
        DEBUG_WITH_TYPE("RoundTripYAMLPass", {
          llvm::dbgs() << "RoundTripYAMLPass: " << tmpYAMLFile << "\n";
        });

        // The file that is written would be kept around if there is a problem
        // writing to the file or when reading atoms back from the file.
        yamlWriter->writeFile(*file, tmpYAMLFile.str());
        std::unique_ptr<MemoryBuffer> mb;
        if (error_code ec = MemoryBuffer::getFile(tmpYAMLFile.str(), mb))
          return ec;

        std::vector<std::unique_ptr<File>> yamlFile;
        if (parseFile(mb, yamlFile, false))
          llvm_unreachable("yaml reader not registered or read error");

        assert(yamlFile.size() == 1 && "Expected one File result");

        std::swap(file, yamlFile[0]);

        llvm::sys::fs::remove(tmpYAMLFile.str());
      }
    }
#endif
    result.swap(res);
    return ec;
  }

  // No Reader could parse this file.
  return llvm::make_error_code(llvm::errc::executable_format_error);
}

static const Registry::KindStrings kindStrings[] = {
    {Reference::kindInGroup, "in-group"},
    {Reference::kindLayoutAfter, "layout-after"},
    {Reference::kindLayoutBefore, "layout-before"},
    {Reference::kindGroupChild, "group-child"},
    {Reference::kindGroupParent, "group-parent"},
    LLD_KIND_STRING_END};

Registry::Registry(LinkingContext &c) : _context(c) {
  addKindTable(Reference::KindNamespace::all, Reference::KindArch::all,
               kindStrings);
}

bool Registry::handleTaggedDoc(llvm::yaml::IO &io,
                               const lld::File *&file) const {
  for (const std::unique_ptr<YamlIOTaggedDocumentHandler> &h : _yamlHandlers) {
    if (h->handledDocTag(io, file))
      return true;
  }
  return false;
}


void Registry::addKindTable(Reference::KindNamespace ns,
                            Reference::KindArch arch,
                            const KindStrings array[]) {
  KindEntry entry = { ns, arch, array };
  _kindEntries.push_back(entry);
}

bool Registry::referenceKindFromString(StringRef inputStr,
                                       Reference::KindNamespace &ns,
                                       Reference::KindArch &arch,
                                       Reference::KindValue &value) const {
  for (const KindEntry &entry : _kindEntries) {
    for (const KindStrings *pair = entry.array; !pair->name.empty(); ++pair) {
      if (!inputStr.equals(pair->name))
        continue;
      ns = entry.ns;
      arch = entry.arch;
      value = pair->value;
      return true;
    }
  }
  return false;
}

bool Registry::referenceKindToString(Reference::KindNamespace ns,
                                     Reference::KindArch arch,
                                     Reference::KindValue value,
                                     StringRef &str) const {
  for (const KindEntry &entry : _kindEntries) {
    if (entry.ns != ns)
      continue;
    if (entry.arch != arch)
      continue;
    for (const KindStrings *pair = entry.array; !pair->name.empty(); ++pair) {
      if (pair->value != value)
        continue;
      str = pair->name;
      return true;
    }
  }
  return false;
}

} // end namespace lld
