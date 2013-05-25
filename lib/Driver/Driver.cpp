//===- lib/Driver/Driver.cpp - Linker Driver Emulator ---------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/Driver/Driver.h"

#include "lld/Core/Allocators.h"
#include "lld/Core/ArchiveLibraryFile.h"
#include "lld/Core/ConcurrentUnorderedSet.h"
#include "lld/Core/LLVM.h"
#include "lld/Core/InputFiles.h"
#include "lld/Core/Instrumentation.h"
#include "lld/Core/PassManager.h"
#include "lld/Core/Parallel.h"
#include "lld/Core/Resolver.h"
#include "lld/ReaderWriter/Reader.h"
#include "lld/ReaderWriter/Writer.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Option/Arg.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace lld {

/// This is where the link is actually performed.
bool Driver::link(const TargetInfo &targetInfo, raw_ostream &diagnostics) {
  // Honor -mllvm
  if (!targetInfo.llvmOptions().empty()) {
    unsigned numArgs = targetInfo.llvmOptions().size();
    const char **args = new const char*[numArgs + 2];
    args[0] = "lld (LLVM option parsing)";
    for (unsigned i = 0; i != numArgs; ++i)
      args[i + 1] = targetInfo.llvmOptions()[i];
    args[numArgs + 1] = 0;
    llvm::cl::ParseCommandLineOptions(numArgs + 1, args);
  }

  if (targetInfo.maxConcurrency())
    setDefaultExecutorMaxConcurrency(targetInfo.maxConcurrency());

  // Read inputs
  ScopedTask readTask(getDefaultDomain(), "Read Args");
  std::vector<std::vector<std::unique_ptr<File>>> files(
      targetInfo.inputFiles().size());
  size_t index = 0;
  std::atomic<bool> fail(false);
  TaskGroup tg;
  for (auto &input : targetInfo.inputFiles()) {
    if (targetInfo.logInputFiles())
      llvm::outs() << input.getPath() << "\n";

    tg.spawn([&, index] {
      if (error_code ec = targetInfo.readFile(input, files[index])) {
        diagnostics << "Failed to read file: " << input.getPath()
                    << ": " << ec.message() << "\n";
        fail = true;
        return;
      }
    });
    ++index;
  }
  tg.sync();
  readTask.end();

  if (fail)
    return true;

  ScopedTask specReadTask(getDefaultDomain(), "Read Specutively");
  // Get list of archives.
  std::vector<const ArchiveLibraryFile *> archives;
  for (auto &f : files)
    for (auto &file : f)
      if (const ArchiveLibraryFile *a = dyn_cast<ArchiveLibraryFile>(file.get()))
        archives.push_back(a);

  {
    ConcurrentUnorderedSet<StringRef, std::hash<StringRef>, std::equal_to<StringRef>, ConcurrentRegionAllocator<StringRef>> symbols;

    // Fill in already defined symbols.
    for (auto &f : files)
      for (auto &file : f)
        for (const auto &atom : file->defined())
          symbols.insert(atom->name());

    std::function<void (const File *)> lookup;
    lookup = [&] (const File *f) {
      for (const auto &atom : f->undefined()) {
        tg.spawn([&, atom] {
        // Lookup atom in the table. Skip if it already exists.
        if (!symbols.insert(atom->name()).second)
          return;
        for (auto arch : archives) {
          // Search archives in a separate task. Ideally we would only spawn a
          // task when we know the given archive has the symbol.
          //tg.spawn([&, arch, atom] {
            auto fi = arch->find(atom->name(), false);
            if (fi) {
              // Fill in new defined.
              for (const auto &atom : fi->defined())
                symbols.insert(atom->name());
              // Lookup new undefined.
              lookup(fi);
              break;
            }
          //});
        }
      });
      }
    };

    // Specutively load files.
    for (auto &f : files)
      for (auto &file : f)
        tg.spawn(std::bind(lookup, file.get()));
    tg.sync();
    specReadTask.end();
  }

  InputFiles inputs;
  for (auto &f : files)
    inputs.appendFiles(f);

  // Give target a chance to add files.
  targetInfo.addImplicitFiles(inputs);

  // assign an ordinal to each file so sort() can preserve command line order
  inputs.assignFileOrdinals();

  // Do core linking.
  ScopedTask resolveTask(getDefaultDomain(), "Resolve");
  Resolver resolver(targetInfo, inputs);
  if (resolver.resolve()) {
    if (!targetInfo.allowRemainingUndefines())
      return true;
  }
  MutableFile &merged = resolver.resultFile();
  resolveTask.end();

  // Run passes on linked atoms.
  ScopedTask passTask(getDefaultDomain(), "Passes");
  PassManager pm;
  targetInfo.addPasses(pm);
  pm.runOnFile(merged);
  passTask.end();

  // Give linked atoms to Writer to generate output file.
  ScopedTask writeTask(getDefaultDomain(), "Write");
  if (error_code ec = targetInfo.writeFile(merged)) {
    diagnostics << "Failed to write file '" << targetInfo.outputPath() 
                << "': " << ec.message() << "\n";
    return true;
  }

  return false;
}


} // namespace

