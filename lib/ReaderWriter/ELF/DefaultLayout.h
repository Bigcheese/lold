//===- lib/ReaderWriter/ELF/DefaultLayout.h -------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_DEFAULT_LAYOUT_H
#define LLD_READER_WRITER_ELF_DEFAULT_LAYOUT_H

#include "Chunk.h"
#include "HeaderChunks.h"
#include "Layout.h"
#include "SectionChunks.h"
#include "SegmentChunks.h"

#include "lld/Core/LinkerOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"

#include <map>
#include <tuple>
#include <unordered_map>

namespace lld {
namespace elf {
/// \brief The DefaultLayout class is used by the Writer to arrange
///        sections and segments in the order determined by the target ELF
///        format. The writer creates a single instance of the DefaultLayout
///        class
template<class ELFT>
class DefaultLayout : public Layout {
public:

  // The order in which the sections appear in the output file
  // If its determined, that the layout needs to change
  // just changing the order of enumerations would essentially
  // change the layout in the output file
  // Change the enumerations so that Target can override and stick 
  // a section anywhere it wants to
  enum DefaultSectionOrder {
    ORDER_NOT_DEFINED = 0,
    ORDER_INTERP = 10,
    ORDER_NOTE = 20,
    ORDER_HASH = 30,
    ORDER_DYNAMIC_SYMBOLS = 40,
    ORDER_DYNAMIC_STRINGS = 50,
    ORDER_INIT = 60,
    ORDER_TEXT = 70,
    ORDER_PLT = 80,
    ORDER_FINI = 90,
    ORDER_REL = 95,
    ORDER_RODATA = 100,
    ORDER_EH_FRAME = 110,
    ORDER_EH_FRAMEHDR = 120,
    ORDER_TDATA = 124,
    ORDER_TBSS = 128,
    ORDER_CTORS = 130,
    ORDER_DTORS = 140,
    ORDER_INIT_ARRAY = 150,
    ORDER_FINI_ARRAY = 160,
    ORDER_DYNAMIC = 170,
    ORDER_GOT = 180,
    ORDER_GOT_PLT = 190,
    ORDER_DATA = 200,
    ORDER_BSS = 210,
    ORDER_OTHER = 220,
    ORDER_SECTION_STRINGS = 230,
    ORDER_SYMBOL_TABLE = 240,
    ORDER_STRING_TABLE = 250,
    ORDER_SECTION_HEADERS = 260
  };

public:

  // The Key used for creating Sections
  // The sections are created using
  // SectionName, contentPermissions
  struct SectionKey {
    SectionKey(StringRef name, DefinedAtom::ContentPermissions perm)
        : _name(name), _perm(perm) {
    }

    // Data members
    StringRef _name;
    DefinedAtom::ContentPermissions _perm;
  };

  struct SectionKeyHash {
    int64_t operator()(const SectionKey &k) const {
      return llvm::hash_combine(k._name, k._perm);
    }
  };

  struct SectionKeyEq {
    bool operator()(const SectionKey &lhs, const SectionKey &rhs) const {
      return ((lhs._name == rhs._name) && (lhs._perm == rhs._perm));
    }
  };

  typedef typename std::vector<Chunk<ELFT> *>::iterator ChunkIter;
  // The key used for Segments
  // The segments are created using
  // SegmentName, Segment flags
  typedef std::pair<StringRef, int64_t> SegmentKey;
  // Merged Sections contain the map of Sectionnames to a vector of sections,
  // that have been merged to form a single section
  typedef std::map<StringRef, MergedSections<ELFT> *> MergedSectionMapT;
  typedef typename std::vector<
      MergedSections<ELFT> *>::iterator MergedSectionIter;

  // HashKey for the Segment
  class SegmentHashKey {
  public:
    int64_t operator() (const SegmentKey &k) const {
      // k.first = SegmentName
      // k.second = SegmentFlags
      return llvm::hash_combine(k.first, k.second);
    }
  };

  typedef std::unordered_map<SectionKey, AtomSection<ELFT> *, SectionKeyHash,
                             SectionKeyEq> SectionMapT;
  typedef std::unordered_map<SegmentKey, Segment<ELFT> *,
                             SegmentHashKey> SegmentMapT;

  /// \brief find a absolute atom pair given a absolute atom name
  struct FindByName {
    const std::string _name;
    FindByName(StringRef name) : _name(name) {}
    bool operator()(const AtomLayout *j) { return j->_atom->name() == _name; }
  };

  typedef typename std::vector<AtomLayout *>::iterator AbsoluteAtomIterT;

  DefaultLayout(const ELFTargetInfo &ti)
      : _relocationTable(nullptr), _targetInfo(ti) {
  }

  /// \brief Return the section order for a input section
  virtual SectionOrder getSectionOrder(StringRef name, int32_t contentType,
                                       int32_t contentPermissions);

  /// \brief This maps the input sections to the output section names
  virtual StringRef getSectionName(StringRef name, const int32_t contentType,
                                   const int32_t contentPermissions);

  /// \brief Returns the section to be created
  virtual AtomSection<ELFT> *getSection(
      StringRef name, int32_t contentType,
      DefinedAtom::ContentPermissions contentPermissions);

  /// \brief Gets the segment for a output section
  virtual Layout::SegmentType getSegmentType(Section<ELFT> *section) const;

  /// \brief Returns true/false depending on whether the section has a Output
  //         segment or not
  static bool hasOutputSegment(Section<ELFT> *section);

  // Adds an atom to the section
  virtual ErrorOr<const AtomLayout &> addAtom(const Atom *atom);

  /// \brief Find an output Section given a section name.
  MergedSections<ELFT> *findOutputSection(StringRef name) {
    auto iter = _mergedSectionMap.find(name);
    if (iter == _mergedSectionMap.end()) 
      return nullptr;
    return iter->second;
  }

  /// \brief find a absolute atom given a name
  AbsoluteAtomIterT findAbsoluteAtom(StringRef name) {
    return std::find_if(_absoluteAtoms.begin(), _absoluteAtoms.end(),
                        FindByName(name));
  }

  // Merge sections with the same name into a MergedSections
  void mergeSimiliarSections();

  void assignSectionsToSegments();

  void assignVirtualAddress();

  void assignOffsetsForMiscSections();

  void assignFileOffsets();

  /// Inline functions
  inline range<AbsoluteAtomIterT> absoluteAtoms() { return _absoluteAtoms; }

  inline void addSection(Chunk<ELFT> *c) {
    _sections.push_back(c);
  }

  inline void finalize() {
    for (auto &si : _sections)
      si->finalize();
  }

  inline bool findAtomAddrByName(StringRef name, uint64_t &addr) {
    for (auto sec : _sections)
      if (auto section = dyn_cast<Section<ELFT> >(sec))
        if (section->findAtomAddrByName(name, addr))
         return true;
    return false;
  }

  inline void setHeader(Header<ELFT> *e) {
    _header = e;
  }

  inline void setProgramHeader(ProgramHeader<ELFT> *p) {
    _programHeader = p;
  }

  inline range<MergedSectionIter> mergedSections() { return _mergedSections; }

  inline range<ChunkIter> sections() { return _sections; }

  inline range<ChunkIter> segments() { return _segments; }

  inline Header<ELFT> *getHeader() {
    return _header;
  }

  inline ProgramHeader<ELFT> *getProgramHeader() {
    return _programHeader;
  }

  RelocationTable<ELFT> *getRelocationTable() {
    // Only create the relocation table if it is needed.
    if (!_relocationTable) {
      _relocationTable = new (_allocator)
          RelocationTable<ELFT>(_targetInfo, ".rela.plt", ORDER_REL);
      addSection(_relocationTable);
    }
    return _relocationTable;
  }

  uint64_t getTLSSize() const {
    for (const auto &phdr : *_programHeader)
      if (phdr->p_type == llvm::ELF::PT_TLS)
        return phdr->p_memsz;
    return 0;
  }

private:
  SectionMapT _sectionMap;
  MergedSectionMapT _mergedSectionMap;
  SegmentMapT _segmentMap;
  std::vector<Chunk<ELFT> *> _sections;
  std::vector<Segment<ELFT> *> _segments;
  std::vector<MergedSections<ELFT> *> _mergedSections;
  Header<ELFT> *_header;
  ProgramHeader<ELFT> *_programHeader;
  RelocationTable<ELFT> *_relocationTable;
  std::vector<AtomLayout *> _absoluteAtoms;
  llvm::BumpPtrAllocator _allocator;
  const ELFTargetInfo &_targetInfo;
};

template <class ELFT>
Layout::SectionOrder DefaultLayout<ELFT>::getSectionOrder(
    StringRef name, int32_t contentType, int32_t contentPermissions) {
  switch (contentType) {
  case DefinedAtom::typeResolver:
  case DefinedAtom::typeCode:
    return llvm::StringSwitch<Reference::Kind>(name)
      .StartsWith(".eh_frame_hdr", ORDER_EH_FRAMEHDR)
      .StartsWith(".eh_frame", ORDER_EH_FRAME)
      .StartsWith(".init", ORDER_INIT)
      .StartsWith(".fini", ORDER_FINI)
      .StartsWith(".hash", ORDER_HASH)
      .Default(ORDER_TEXT);
  
  case DefinedAtom::typeConstant:
    return ORDER_RODATA;
  
  case DefinedAtom::typeData:
    return llvm::StringSwitch<Reference::Kind>(name)
      .StartsWith(".init_array", ORDER_INIT_ARRAY)
      .Default(ORDER_DATA);
  
  case DefinedAtom::typeZeroFill:
    return ORDER_BSS;

  case DefinedAtom::typeGOT:
    return ORDER_GOT;
  case DefinedAtom::typeStub:
    return ORDER_PLT;

  case DefinedAtom::typeTLVInitialData:
    return ORDER_TDATA;
  case DefinedAtom::typeTLVInitialZeroFill:
    return ORDER_TBSS;
  default:
    // If we get passed in a section push it to OTHER
    if (contentPermissions == DefinedAtom::perm___)
      return ORDER_OTHER;
  
    return ORDER_NOT_DEFINED;
  }
}

/// \brief This maps the input sections to the output section names
template <class ELFT>
StringRef DefaultLayout<ELFT>::getSectionName(
    StringRef name, const int32_t contentType,
    const int32_t contentPermissions) {
  if (contentType == DefinedAtom::typeZeroFill)
    return ".bss";
  if (name.startswith(".text"))
    return ".text";
  if (name.startswith(".rodata"))
    return ".rodata";
  if (name.startswith(".tdata"))
    return ".tdata";
  if (name.startswith(".tbss"))
    return ".tbss";
  return name;
}

/// \brief Gets the segment for a output section
template <class ELFT>
Layout::SegmentType DefaultLayout<ELFT>::getSegmentType(
    Section<ELFT> *section) const {

  switch (section->order()) {
  case ORDER_INTERP:
    return llvm::ELF::PT_INTERP;

  case ORDER_TEXT:
  case ORDER_HASH:
  case ORDER_DYNAMIC_SYMBOLS:
  case ORDER_DYNAMIC_STRINGS:
  case ORDER_REL:
  case ORDER_INIT:
  case ORDER_PLT:
  case ORDER_FINI:
  case ORDER_RODATA:
  case ORDER_EH_FRAME:
  case ORDER_EH_FRAMEHDR:
    return llvm::ELF::PT_LOAD;

  case ORDER_NOTE:
    return llvm::ELF::PT_NOTE;

  case ORDER_DYNAMIC:
    return llvm::ELF::PT_DYNAMIC;

  case ORDER_CTORS:
  case ORDER_DTORS:
    return llvm::ELF::PT_GNU_RELRO;

  case ORDER_GOT:
  case ORDER_GOT_PLT:
  case ORDER_DATA:
  case ORDER_BSS:
  case ORDER_INIT_ARRAY:
  case ORDER_FINI_ARRAY:
    return llvm::ELF::PT_LOAD;

  case ORDER_TDATA:
  case ORDER_TBSS:
    return llvm::ELF::PT_TLS;

  default:
    return llvm::ELF::PT_NULL;
  }
}

template <class ELFT>
bool DefaultLayout<ELFT>::hasOutputSegment(Section<ELFT> *section) {
  switch (section->order()) {
  case ORDER_INTERP:
  case ORDER_HASH:
  case ORDER_DYNAMIC_SYMBOLS:
  case ORDER_DYNAMIC_STRINGS:
  case ORDER_REL:
  case ORDER_INIT:
  case ORDER_PLT:
  case ORDER_TEXT:
  case ORDER_FINI:
  case ORDER_RODATA:
  case ORDER_EH_FRAME:
  case ORDER_EH_FRAMEHDR:
  case ORDER_TDATA:
  case ORDER_TBSS:
  case ORDER_NOTE:
  case ORDER_DYNAMIC:
  case ORDER_CTORS:
  case ORDER_DTORS:
  case ORDER_GOT:
  case ORDER_GOT_PLT:
  case ORDER_DATA:
  case ORDER_INIT_ARRAY:
  case ORDER_FINI_ARRAY:
  case ORDER_BSS:
    return true;  
  default:
    return section->hasOutputSegment();
  }
}

template <class ELFT>
AtomSection<ELFT> *DefaultLayout<ELFT>::getSection(
    StringRef sectionName, int32_t contentType,
    DefinedAtom::ContentPermissions permissions) {
  const SectionKey sectionKey(sectionName, permissions);
  auto sec = _sectionMap.find(sectionKey);
  if (sec != _sectionMap.end())
    return sec->second;
  SectionOrder sectionOrder =
      getSectionOrder(sectionName, contentType, permissions);
  AtomSection<ELFT> *newSec = new (_allocator) AtomSection<ELFT>(
      _targetInfo, sectionName, contentType, permissions, sectionOrder);
  newSec->setOrder(sectionOrder);
  _sections.push_back(newSec);
  _sectionMap.insert(std::make_pair(sectionKey, newSec));
  return newSec;
}

template <class ELFT>
ErrorOr<const AtomLayout &> DefaultLayout<ELFT>::addAtom(const Atom *atom) {
  if (const DefinedAtom *definedAtom = dyn_cast<DefinedAtom>(atom)) {
    // HACK: Ignore undefined atoms. We need to adjust the interface so that
    // undefined atoms can still be included in the output symbol table for
    // -noinhibit-exec.
    if (definedAtom->contentType() == DefinedAtom::typeUnknown)
      return make_error_code(llvm::errc::invalid_argument);
    StringRef sectionName = definedAtom->customSectionName();
    const DefinedAtom::ContentPermissions permissions =
        definedAtom->permissions();
    const DefinedAtom::ContentType contentType = definedAtom->contentType();

    sectionName = getSectionName(sectionName, contentType, permissions);
    AtomSection<ELFT> *section =
        getSection(sectionName, contentType, permissions);
    // Add runtime relocations to the .rela section.
    for (const auto &reloc : *definedAtom)
      if (_targetInfo.isRuntimeRelocation(*definedAtom, *reloc))
        getRelocationTable()->addRelocation(*definedAtom, *reloc);
    return section->appendAtom(atom);
  } else if (const AbsoluteAtom *absoluteAtom = dyn_cast<AbsoluteAtom>(atom)) {
    // Absolute atoms are not part of any section, they are global for the whole
    // link
    _absoluteAtoms.push_back(
        new (_allocator) AtomLayout(absoluteAtom, 0, absoluteAtom->value()));
    return *_absoluteAtoms.back();
  } else {
    llvm_unreachable("Only absolute / defined atoms can be added here");
  }
}

/// Merge sections with the same name into a MergedSections
template<class ELFT>
void 
DefaultLayout<ELFT>::mergeSimiliarSections() {
  MergedSections<ELFT> *mergedSection;

  for (auto &si : _sections) {
    const std::pair<StringRef, MergedSections<ELFT> *>
      currentMergedSections(si->name(), nullptr);
    std::pair<typename MergedSectionMapT::iterator, bool>
                            mergedSectionInsert
                            (_mergedSectionMap.insert(currentMergedSections));
    if (!mergedSectionInsert.second) {
      mergedSection = mergedSectionInsert.first->second;
    } else {
      mergedSection = new (_allocator.Allocate<MergedSections<ELFT>>())
        MergedSections<ELFT>(si->name());
      _mergedSections.push_back(mergedSection);
      mergedSectionInsert.first->second = mergedSection;
    }
    mergedSection->appendSection(si);
  }
}

template <class ELFT> void DefaultLayout<ELFT>::assignSectionsToSegments() {
  // TODO: Do we want to give a chance for the targetHandlers
  // to sort segments in an arbitrary order ? 
  // sort the sections by their order as defined by the layout
  std::stable_sort(_sections.begin(), _sections.end(),
                   [](Chunk<ELFT> *A, Chunk<ELFT> *B) {
    return A->order() < B->order();
  });
  // Merge all sections
  mergeSimiliarSections();
  // Set the ordinal after sorting the sections
  int ordinal = 1;
  for (auto msi : _mergedSections) {
    msi->setOrdinal(ordinal);
    for (auto ai : msi->sections()) {
      ai->setOrdinal(ordinal);
    }
    ++ordinal;
  }
  for (auto msi : _mergedSections) {
    for (auto ai : msi->sections()) {
      if (auto section = dyn_cast<Section<ELFT>>(ai)) {
        if (!hasOutputSegment(section))
          continue;
        msi->setHasSegment();
        section->setSegment(getSegmentType(section));
        StringRef segmentName = section->segmentKindToStr();
        // Use the flags of the merged Section for the segment
        const SegmentKey key(segmentName, msi->flags());
        const std::pair<SegmentKey, Segment<ELFT> *> currentSegment(key,
                                                                    nullptr);
        std::pair<typename SegmentMapT::iterator, bool>
                            segmentInsert(_segmentMap.insert(currentSegment));
        Segment<ELFT> *segment;
        if (!segmentInsert.second) {
          segment = segmentInsert.first->second;
        } else {
          segment = new (_allocator)
              Segment<ELFT>(_targetInfo, segmentName, getSegmentType(section));
          segmentInsert.first->second = segment;
          _segments.push_back(segment);
        }
        segment->append(section);
      }
    }
  }
}

template <class ELFT> void DefaultLayout<ELFT>::assignFileOffsets() {
  // TODO: Do we want to give a chance for the targetHandlers
  // to sort segments in an arbitrary order ? 
  std::sort(_segments.begin(), _segments.end(), Segment<ELFT>::compareSegments);
  int ordinal = 0;
  // Compute the number of segments that might be needed, so that the
  // size of the program header can be computed
  uint64_t offset = 0;
  for (auto si : _segments) {
    si->setOrdinal(++ordinal);
    si->assignOffsets(offset);
    offset += si->fileSize();
  }
}


template<class ELFT>
void
DefaultLayout<ELFT>::assignVirtualAddress() {
  if (_segments.empty())
    return;
  
  uint64_t virtualAddress = _targetInfo.getBaseAddress();
  
  // HACK: This is a super dirty hack. The elf header and program header are
  // not part of a section, but we need them to be loaded at the base address
  // so that AT_PHDR is set correctly by the loader and so they are accessible
  // at runtime. To do this we simply prepend them to the first Segment and
  // let the layout logic take care of it.
  _segments[0]->prepend(_programHeader);
  _segments[0]->prepend(_header);
  
  bool newSegmentHeaderAdded = true;
  while (true) {
    for (auto si : _segments) {
      newSegmentHeaderAdded = _programHeader->addSegment(si);
    }
    if (!newSegmentHeaderAdded)
      break;
    uint64_t fileoffset = 0;
    uint64_t address = virtualAddress;
    // Fix the offsets after adding the program header
    for (auto &si : _segments) {
      // Align the segment to a page boundary
      fileoffset = llvm::RoundUpToAlignment(fileoffset,
                                            _targetInfo.getPageSize());
      si->assignOffsets(fileoffset);
      fileoffset = si->fileOffset() + si->fileSize();
    }
    // start assigning virtual addresses
    for (auto si = _segments.begin(); si != _segments.end(); ++si) {
      (*si)->setVAddr(virtualAddress);
      // The first segment has the virtualAddress set to the base address as
      // we have added the file header and the program header dont align the
      // first segment to the pagesize
      (*si)->assignVirtualAddress(address);
      (*si)->setMemSize(address - virtualAddress);
      virtualAddress = llvm::RoundUpToAlignment(address,
                                                _targetInfo.getPageSize());
    }
    _programHeader->resetProgramHeaders();
  }
  Section<ELFT> *section;
  // Fix the offsets of all the atoms within a section
  for (auto &si : _sections) {
    section = dyn_cast<Section<ELFT>>(si);
    if (section && DefaultLayout<ELFT>::hasOutputSegment(section))
      section->assignOffsets(section->fileOffset());
  }
  // Set the size of the merged Sections
  for (auto msi : _mergedSections) {
    uint64_t sectionfileoffset = 0;
    uint64_t startFileOffset = 0;
    uint64_t sectionsize = 0;
    bool isFirstSection = true;
    for (auto si : msi->sections()) {
      if (isFirstSection) {
        startFileOffset = si->fileOffset();
        isFirstSection = false;
      }
      sectionfileoffset = si->fileOffset();
      sectionsize = si->fileSize();
    }
    sectionsize = (sectionfileoffset - startFileOffset) + sectionsize;
    msi->setFileOffset(startFileOffset);
    msi->setSize(sectionsize);
  }
  // Set the virtual addr of the merged Sections
  for (auto msi : _mergedSections) {
    uint64_t sectionstartaddr = 0;
    uint64_t startaddr = 0;
    uint64_t sectionsize = 0;
    bool isFirstSection = true;
    for (auto si : msi->sections()) {
      if (isFirstSection) {
        startaddr = si->virtualAddr();
        isFirstSection = false;
      }
      sectionstartaddr = si->virtualAddr();
      sectionsize = si->memSize();
    }
    sectionsize = (sectionstartaddr - startaddr) + sectionsize;
    msi->setMemSize(sectionsize);
    msi->setAddr(startaddr);
  }
}

template<class ELFT>
void
DefaultLayout<ELFT>::assignOffsetsForMiscSections() {
  uint64_t fileoffset = 0;
  uint64_t size = 0;
  for (auto si : _segments) {
    fileoffset = si->fileOffset();
    size = si->fileSize();
  }
  fileoffset = fileoffset + size;
  Section<ELFT> *section;
  for (auto si : _sections) {
    section = dyn_cast<Section<ELFT>>(si);
    if (section && DefaultLayout<ELFT>::hasOutputSegment(section))
      continue;
    fileoffset = llvm::RoundUpToAlignment(fileoffset, si->align2());
    si->setFileOffset(fileoffset);
    si->setVAddr(0);
    fileoffset += si->fileSize();
  }
}
} // end namespace elf
} // end namespace lld

#endif
