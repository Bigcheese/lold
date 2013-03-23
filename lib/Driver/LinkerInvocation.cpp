//===- lib/Driver/LinkerInvocation.cpp - Linker Invocation ----------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/Driver/LinkerInvocation.h"

#include "lld/Core/InputFiles.h"
#include "lld/Core/PassManager.h"
#include "lld/Core/Resolver.h"
#include "lld/Core/ThreadPool.h"
#include "lld/ReaderWriter/ELFTargetInfo.h"
#include "lld/ReaderWriter/Reader.h"
#include "lld/ReaderWriter/Writer.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <unordered_map>

using namespace lld;

namespace {
std::unique_ptr<TargetInfo> createTargetInfo(const LinkerOptions &lo) {
  return ELFTargetInfo::create(lo);
}
}

void LinkerInvocation::operator()() {
  // Honor -mllvm
  if (!_options._llvmArgs.empty()) {
    unsigned NumArgs = _options._llvmArgs.size();
    const char **Args = new const char*[NumArgs + 2];
    Args[0] = "lld (LLVM option parsing)";
    for (unsigned i = 0; i != NumArgs; ++i)
      Args[i + 1] = _options._llvmArgs[i].c_str();
    Args[NumArgs + 1] = 0;
    llvm::cl::ParseCommandLineOptions(NumArgs + 1, Args);
  }

  // Create target.
  std::unique_ptr<TargetInfo> targetInfo(createTargetInfo(_options));

  if (!targetInfo) {
    llvm::errs() << "Failed to create target for " << _options._target
                  << "\n";
    return;
  }

  // Read inputs
  std::vector<std::vector<std::unique_ptr<File>>> files(_options._input.size());
  {
    std::mutex inputsMutex;
    std::mutex undefMutex;
    /// \brief If an entry exists in here, 
    // std::unordered_set<StringRef> undefSearchState;
    ThreadPool readPool(std::min<size_t>(files.size(), std::thread::hardware_concurrency()));
    std::size_t index = 0;
    for (const auto &input : _options._input) {
      readPool.enqueue([&, index]() mutable {
        auto reader = targetInfo->getReader(input);
        if (error_code ec = reader) {
          llvm::errs() << "Failed to get reader for: " << input.getPath() << ": "
                        << ec.message() << "\n";
          return;
        }

        auto buffer = input.getBuffer();
        if (error_code ec = buffer) {
          llvm::errs() << "Failed to read file: " << input.getPath() << ": "
                        << ec.message() << "\n";
          return;
        }

        if (llvm::error_code ec = reader->parseFile(std::unique_ptr<MemoryBuffer>(MemoryBuffer::getMemBuffer(buffer->getBuffer(), buffer->getBufferIdentifier())), files[index])) {
          std::lock_guard<std::mutex> lock(inputsMutex);
          llvm::errs() << "Failed to read file: " << input.getPath() << ": "
                        << ec.message() << "\n";
          return;
        }
      });
      ++index;
    }
  } // sync with thread pool.
  InputFiles inputs;
  for (auto &f : files)
    inputs.appendFiles(f);
  inputs.assignFileOrdinals();

  auto writer = targetInfo->getWriter();

  // Give writer a chance to add files
  writer->addFiles(inputs);

  Resolver resolver(*targetInfo, inputs);
  resolver.resolve();
  MutableFile &merged = resolver.resultFile();

  PassManager pm;
  targetInfo->addPasses(pm);
  pm.runOnFile(merged);

  if (error_code ec = writer) {
    llvm::errs() << "Failed to get writer: " << ec.message() << ".\n";
    return;
  }

  if (error_code ec = writer->writeFile(merged, _options._outputPath))
    llvm::errs() << "Failed to write file: " << ec.message() << "\n";
}
