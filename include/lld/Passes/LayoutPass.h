//===------ Passes/LayoutPass.h - Handles Layout of atoms ------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PASSES_LAYOUT_PASS_H
#define LLD_PASSES_LAYOUT_PASS_H

#include "lld/Core/Atom.h"
#include "lld/Core/File.h"
#include "lld/Core/Pass.h"
#include "lld/Core/range.h"
#include "lld/Core/Reference.h"

#include "llvm/ADT/DenseMap.h"

#include <map>
#include <vector>

namespace lld {
class DefinedAtom;
class MutableFile;

/// This linker pass does the layout of the atoms. The pass is done after the
/// order their .o files were found on the command line, then by order of the
/// atoms (address) in the .o file.  But some atoms have a prefered location
/// in their section (such as pinned to the start or end of the section), so
/// the sort must take that into account too.
class LayoutPass : public Pass {
public:

  // Compare and Sort Atoms by their ordinals
  class CompareAtoms {
  public:
    explicit CompareAtoms(const LayoutPass &pass) : _layout(pass) {}
    bool operator()(const DefinedAtom *left, const DefinedAtom *right) const;
  private:
    const LayoutPass &_layout;
  };

  LayoutPass() : Pass(), _compareAtoms(*this) {}

  /// Sorts atoms in mergedFile by content type then by command line order.
  virtual void perform(MutableFile &mergedFile);

  virtual ~LayoutPass() {}

private:
  // Build the followOn atoms chain as specified by the kindLayoutAfter
  // reference type
  void buildFollowOnTable(MutableFile::DefinedAtomRange &range);

  // Build the followOn atoms chain as specified by the kindInGroup
  // reference type
  void buildInGroupTable(MutableFile::DefinedAtomRange &range);

  // Build the PrecededBy Table as specified by the kindLayoutBefore
  // reference type
  void buildPrecededByTable(MutableFile::DefinedAtomRange &range);

  // Build a map of Atoms to ordinals for sorting the atoms
  void buildOrdinalOverrideMap(MutableFile::DefinedAtomRange &range);

  typedef llvm::DenseMap<const DefinedAtom *, const DefinedAtom *> AtomToAtomT;
  typedef llvm::DenseMap<const DefinedAtom *, uint64_t> AtomToOrdinalT;

  // A map to be used to sort atoms. It represents the order of atoms in the
  // result; if Atom X is mapped to atom Y in this map, X will be located
  // immediately before Y in the output file. Y might be mapped to another
  // atom, constructing a follow-on chain. An atom cannot be mapped to more
  // than one atom unless all but one atom are of size zero.
  AtomToAtomT _followOnNexts;

  // A map to be used to sort atoms. It's a map from an atom to its root of
  // follow-on chain. A root atom is mapped to itself. If an atom is not in
  // _followOnNexts, the atom is not in this map, and vice versa.
  AtomToAtomT _followOnRoots;

  AtomToOrdinalT _ordinalOverrideMap;
  CompareAtoms _compareAtoms;

  // Helper methods for buildFollowOnTable().
  const DefinedAtom *findAtomFollowedBy(const DefinedAtom *targetAtom);
  bool checkAllPrevAtomsZeroSize(const DefinedAtom *targetAtom);

  void setChainRoot(const DefinedAtom *targetAtom, const DefinedAtom *root);
};

} // namespace lld

#endif // LLD_PASSES_LAYOUT_PASS_H
