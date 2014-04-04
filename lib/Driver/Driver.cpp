//===- lib/Driver/Driver.cpp - Linker Driver Emulator ---------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/Driver/Driver.h"

#include "lld/Core/LLVM.h"
#include "lld/Core/Instrumentation.h"
#include "lld/Core/PassManager.h"
#include "lld/Core/Parallel.h"
#include "lld/Core/Resolver.h"
#include "lld/ReaderWriter/Reader.h"
#include "lld/ReaderWriter/Writer.h"
#include "lld/Passes/RoundTripNativePass.h"
#include "lld/Passes/RoundTripYAMLPass.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Option/Arg.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <mutex>

namespace lld {

/// This is where the link is actually performed.
bool Driver::link(LinkingContext &context, raw_ostream &diagnostics) {
  // Honor -mllvm
  if (!context.llvmOptions().empty()) {
    unsigned numArgs = context.llvmOptions().size();
    const char **args = new const char *[numArgs + 2];
    args[0] = "lld (LLVM option parsing)";
    for (unsigned i = 0; i != numArgs; ++i)
      args[i + 1] = context.llvmOptions()[i];
    args[numArgs + 1] = 0;
    llvm::cl::ParseCommandLineOptions(numArgs + 1, args);
  }
  InputGraph &inputGraph = context.getInputGraph();
  if (!inputGraph.size())
    return false;

  std::atomic<bool> fail(false);

  // Read inputs
  ScopedTask readTask(getDefaultDomain(), "Read Args");
  TaskGroup tg;
  std::mutex diagnosticsMutex;
  for (std::unique_ptr<InputElement> &ie : inputGraph.inputElements()) {
    tg.spawn([&] {
      // Writes to the same output stream is not guaranteed to be thread-safe.
      // We buffer the diagnostics output to a separate string-backed output
      // stream, acquire the lock, and then print it out.
      std::string buf;
      llvm::raw_string_ostream stream(buf);

      if (error_code ec = ie->parse(context, stream)) {
        FileNode *fileNode = dyn_cast<FileNode>(ie.get());
        stream << fileNode->errStr(ec) << "\n";
        fail = true;
      }

      stream.flush();
      if (!buf.empty()) {
        std::lock_guard<std::mutex> lock(diagnosticsMutex);
        diagnostics << buf;
      }
    });
  }
  tg.sync();
  readTask.end();

  if (fail)
    return false;

  std::unique_ptr<SimpleFileNode> fileNode(
      new SimpleFileNode("Internal Files"));

  InputGraph::FileVectorT internalFiles;
  context.createInternalFiles(internalFiles);

  if (internalFiles.size())
    fileNode->addFiles(std::move(internalFiles));

  // Give target a chance to add files.
  InputGraph::FileVectorT implicitFiles;
  context.createImplicitFiles(implicitFiles);
  if (implicitFiles.size())
    fileNode->addFiles(std::move(implicitFiles));
  context.getInputGraph().insertElementAt(std::move(fileNode),
                                          InputGraph::Position::BEGIN);

  // Do core linking.
  ScopedTask resolveTask(getDefaultDomain(), "Resolve");
  Resolver resolver(context);
  if (!resolver.resolve())
    return false;
  std::unique_ptr<MutableFile> merged = resolver.resultFile();
  resolveTask.end();

  // Run passes on linked atoms.
  ScopedTask passTask(getDefaultDomain(), "Passes");
  PassManager pm;
  context.addPasses(pm);

#ifndef NDEBUG
  llvm::Optional<std::string> env = llvm::sys::Process::GetEnv("LLD_RUN_ROUNDTRIP_TEST");
  if (env.hasValue() && !env.getValue().empty()) {
    pm.add(std::unique_ptr<Pass>(new RoundTripYAMLPass(context)));
    pm.add(std::unique_ptr<Pass>(new RoundTripNativePass(context)));
  }
#endif

  pm.runOnFile(merged);
  passTask.end();

  // Give linked atoms to Writer to generate output file.
  ScopedTask writeTask(getDefaultDomain(), "Write");
  if (error_code ec = context.writeFile(*merged)) {
    diagnostics << "Failed to write file '" << context.outputPath()
                << "': " << ec.message() << "\n";
    return false;
  }

  return true;
}

} // namespace
