//===- lib/ReaderWriter/ReaderArchive.cpp - Archive Library Reader--------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//

#include "lld/ReaderWriter/ReaderArchive.h"

#include "lld/Core/ArchiveLibraryFile.h"
#include "lld/Core/LinkerOptions.h"

#include "llvm/ADT/Hashing.h"
#include "llvm/Object/ObjectFile.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace lld {
/// \brief The FileArchive class represents an Archive Library file
class FileArchive : public ArchiveLibraryFile {
public:

  virtual ~FileArchive() { }

  /// \brief Check if any member of the archive contains an Atom with the
  /// specified name and return the File object for that member, or nullptr.
  virtual const File *find(StringRef name, bool dataSymbolOnly) const {
    auto member = _symbolMemberMap.find(name);
    if (member == _symbolMemberMap.end())
      return nullptr;

    auto &child = member->second;
    std::unique_lock<std::mutex> lock(*child._mutex);
    if (dataSymbolOnly) {
      OwningPtr<MemoryBuffer> buff;
      if (child._childIterator->getMemoryBuffer(buff, true))
        return nullptr;
      if (isDataSymbol(buff.take(), name))
        return nullptr;
    }

    // Check if the file is already being read.
    if (child._file) {
      return child._file;
    }

    // Read it.
    std::vector<std::unique_ptr<File>> result;

    OwningPtr<MemoryBuffer> buff;
    if (child._childIterator->getMemoryBuffer(buff, true))
      return nullptr;
    LinkerInput li(std::unique_ptr<MemoryBuffer>(buff.take()));
    if (_getReader(li)->parseFile(li.takeBuffer(), result))
      return nullptr;

    assert(result.size() == 1);

    std::unique_lock<std::mutex> ordLock(_curChildOrdMutex);
    result[0]->setOrdinalAndIncrement(_curChildOrd);
    ordLock.unlock();

    // give up the pointer so that this object no longer manages it
    return child._file = result[0].release();
  }

  virtual void setOrdinalAndIncrement(uint64_t &ordinal) const {
    _ordinal = ordinal++;
    _curChildOrd = _ordinal;
    // Leave space in ordinal range for all children
    for (auto mf = _archive->begin_children(),
              me = _archive->end_children(); mf != me; ++mf) {
        ordinal++;
    }
  }

  virtual const atom_collection<DefinedAtom> &defined() const {
    return _definedAtoms;
  }

  virtual const atom_collection<UndefinedAtom> &undefined() const {
    return _undefinedAtoms;
  }

  virtual const atom_collection<SharedLibraryAtom> &sharedLibrary() const {
    return _sharedLibraryAtoms;
  }

  virtual const atom_collection<AbsoluteAtom> &absolute() const {
    return _absoluteAtoms;
  }

protected:
  error_code isDataSymbol(MemoryBuffer *mb, StringRef symbol) const {
    std::unique_ptr<llvm::object::ObjectFile>
                    obj(llvm::object::ObjectFile::createObjectFile(mb));
    error_code ec;
    llvm::object::SymbolRef::Type symtype;
    uint32_t symflags;
    llvm::object::symbol_iterator ibegin = obj->begin_symbols();
    llvm::object::symbol_iterator iend = obj->end_symbols();
    StringRef symbolname;

    for (llvm::object::symbol_iterator i = ibegin; i != iend; i.increment(ec)) {
      if (ec) return ec;

      // Get symbol name
      if ((ec = (i->getName(symbolname)))) return ec;

      if (symbolname != symbol)
          continue;

      // Get symbol flags
      if ((ec = (i->getFlags(symflags)))) return ec;

      if (symflags <= llvm::object::SymbolRef::SF_Undefined)
          continue;

      // Get Symbol Type
      if ((ec = (i->getType(symtype)))) return ec;

      if (symtype == llvm::object::SymbolRef::ST_Data) {
        return error_code::success();
      }
    }
    return llvm::object::object_error::parse_failed;
  }

private:
  std::function<ErrorOr<Reader&> (const LinkerInput &)> _getReader;
  std::unique_ptr<llvm::object::Archive> _archive;
  atom_collection_vector<DefinedAtom>       _definedAtoms;
  atom_collection_vector<UndefinedAtom>     _undefinedAtoms;
  atom_collection_vector<SharedLibraryAtom> _sharedLibraryAtoms;
  atom_collection_vector<AbsoluteAtom>      _absoluteAtoms;
  mutable uint64_t _curChildOrd;
  mutable std::mutex _curChildOrdMutex;
  
  struct ChildEntry {
    ChildEntry() : _file(nullptr) {}

    ChildEntry(llvm::object::Archive::child_iterator ci)
        : _childIterator(ci), _file(nullptr), _mutex(new std::mutex) {}

    ChildEntry(ChildEntry &&other)
        : _childIterator(other._childIterator),
          _file(other._file),
          _mutex(std::move(other._mutex)) {}

    ChildEntry &operator =(ChildEntry &&other) {
      _childIterator = other._childIterator;
      _file = other._file;
      std::swap(_mutex, other._mutex);
      return *this;
    }
    
    llvm::object::Archive::child_iterator _childIterator;
    /// \brief If the child has already been read, this is set to the file that
    ///   was read.
    const File * _file;
    /// \brief Mutex for when the file is currently being read.
    std::unique_ptr<std::mutex> _mutex;

  private:
    ChildEntry(const ChildEntry &) {
      llvm_unreachable("MSVC fails at properly dealing with move semantics ;/");
    }

    ChildEntry &operator =(const ChildEntry &) {
      llvm_unreachable("MSVC fails at properly dealing with move semantics ;/");
    }
  };

  mutable std::unordered_map<StringRef, ChildEntry> _symbolMemberMap;

public:
  /// only subclasses of ArchiveLibraryFile can be instantiated
  FileArchive(const TargetInfo &ti,
              std::function<ErrorOr<Reader &>(const LinkerInput &)> getReader,
              std::unique_ptr<llvm::MemoryBuffer> mb, error_code &ec)
      : ArchiveLibraryFile(ti, mb->getBufferIdentifier()),
        _getReader(getReader) {
    std::unique_ptr<llvm::object::Archive> archive_obj(
        new llvm::object::Archive(mb.release(), ec));
    if (ec)
      return;
    _archive.swap(archive_obj);

    // Cache symbols.
    for (auto i = _archive->begin_symbols(), e = _archive->end_symbols();
              i != e; ++i) {
      StringRef name;
      llvm::object::Archive::child_iterator member;
      if ((ec = i->getName(name)))
        return;
      if ((ec = i->getMember(member)))
        return;
      _symbolMemberMap[name] = ChildEntry(member);
    }
  }
}; // class FileArchive

// Returns a vector of Files that are contained in the archive file
// pointed to by the MemoryBuffer
error_code ReaderArchive::parseFile(std::unique_ptr<llvm::MemoryBuffer> mb,
                                    std::vector<std::unique_ptr<File>> &result){
  error_code ec;

  if (_options._forceLoadArchives) {
    _archive.reset(new llvm::object::Archive(mb.release(), ec));
    if (ec)
      return ec;

    for (auto mf = _archive->begin_children(),
              me = _archive->end_children(); mf != me; ++mf) {
      OwningPtr<MemoryBuffer> buff;
      if ((ec = mf->getMemoryBuffer(buff, true)))
        return ec;
      LinkerInput li(std::unique_ptr<MemoryBuffer>(buff.take()));
      if ((ec = _getReader(li)->parseFile(li.takeBuffer(), result)))
        return ec;
    }
  } else {
    std::unique_ptr<File> f;
    f.reset(new FileArchive(_targetInfo, _getReader, std::move(mb), ec));
    if (ec)
      return ec;

    result.push_back(std::move(f));
  }
  return llvm::error_code::success();
}
} // end namespace lld
