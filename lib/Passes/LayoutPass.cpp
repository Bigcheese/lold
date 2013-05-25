//===- Passes/LayoutPass.cpp - Layout atoms -------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "LayoutPass"

#include "lld/Passes/LayoutPass.h"
#include "lld/Core/Instrumentation.h"
#include "lld/Core/Parallel.h"
#include "llvm/Support/Debug.h"

using namespace lld;

/// The function compares atoms by sorting atoms in the following order
/// a) Sorts atoms with the same permissions
/// b) Sorts atoms with the same content Type
/// c) Sorts atoms by Section position preference
/// d) Sorts atoms by how they follow / precede each atom
/// e) Sorts atoms on how they appear using File Ordinality
/// f) Sorts atoms on how they appear within the File
bool LayoutPass::CompareAtoms::operator()(const DefinedAtom *left,
                                          const DefinedAtom *right) const {
  DEBUG(llvm::dbgs() << "Sorting " << left->name() << " " << right->name() << "\n");
  if (left == right)
    return false;

  // Sort by section position preference.
  DefinedAtom::SectionPosition leftPos = left->sectionPosition();
  DefinedAtom::SectionPosition rightPos = right->sectionPosition();

  DEBUG(llvm::dbgs() << "Sorting by sectionPos"
                     << "(" << leftPos << "," << rightPos << ")\n");

  bool leftSpecialPos = (leftPos != DefinedAtom::sectionPositionAny);
  bool rightSpecialPos = (rightPos != DefinedAtom::sectionPositionAny);
  if (leftSpecialPos || rightSpecialPos) {
    if (leftPos != rightPos)
      return leftPos < rightPos;
  }

  DEBUG(llvm::dbgs() << "Sorting by override\n");

  AtomToOrdinalT::const_iterator lPos = _layout._ordinalOverrideMap.find(left);
  AtomToOrdinalT::const_iterator rPos = _layout._ordinalOverrideMap.find(right);
  AtomToOrdinalT::const_iterator end = _layout._ordinalOverrideMap.end();
  if (lPos != end) {
    if (rPos != end) {
      // both left and right are overridden, so compare overridden ordinals
      if (lPos->second != rPos->second)
        return lPos->second < rPos->second;
    } else {
      // left is overridden and right is not, so left < right
      return true;
    }
  } else {
    if (rPos != end) {
      // right is overridden and left is not, so right < left
      return false;
    } else {
      // neither are overridden,
      // fall into default sorting below
    }
  }

  // Sort same permissions together.
  DefinedAtom::ContentPermissions leftPerms = left->permissions();
  DefinedAtom::ContentPermissions rightPerms = right->permissions();

  DEBUG(llvm::dbgs() << "Sorting by contentPerms"
                     << "(" << leftPerms << "," << rightPerms << ")\n");

  if (leftPerms != rightPerms)
    return leftPerms < rightPerms;

  // Sort same content types together.
  DefinedAtom::ContentType leftType = left->contentType();
  DefinedAtom::ContentType rightType = right->contentType();

  DEBUG(llvm::dbgs() << "Sorting by contentType"
                     << "(" << leftType << "," << rightType << ")\n");

  if (leftType != rightType)
    return leftType < rightType;

  // TO DO: Sort atoms in customs sections together.

  // Sort by .o order.
  const File *leftFile = &left->file();
  const File *rightFile = &right->file();

  DEBUG(llvm::dbgs()
        << "Sorting by .o order("
        << "(" << leftFile->ordinal() << "," << rightFile->ordinal() << ")"
        << "[" << leftFile->path() << "," << rightFile->path() << "]\n");

  if (leftFile != rightFile)
    return leftFile->ordinal() < rightFile->ordinal();

  // Sort by atom order with .o file.
  uint64_t leftOrdinal = left->ordinal();
  uint64_t rightOrdinal = right->ordinal();

  DEBUG(llvm::dbgs() << "Sorting by ordinal(" << left->ordinal() << ","
                     << right->ordinal() << ")\n");

  if (leftOrdinal != rightOrdinal)
    return leftOrdinal < rightOrdinal;

  DEBUG(llvm::dbgs() << "Unordered\n");

  return false;
}

// Returns the atom immediately followed by the given atom in the followon
// chain.
const DefinedAtom *LayoutPass::findAtomFollowedBy(
    const DefinedAtom *targetAtom) {
  // Start from the beginning of the chain and follow the chain until
  // we find the targetChain.
  const DefinedAtom *atom = _followOnRoots[targetAtom];
  while (true) {
    const DefinedAtom *prevAtom = atom;
    AtomToAtomT::iterator targetFollowOnAtomsIter = _followOnNexts.find(atom);
    // The target atom must be in the chain of its root.
    assert(targetFollowOnAtomsIter != _followOnNexts.end());
    atom = targetFollowOnAtomsIter->second;
    if (atom == targetAtom)
      return prevAtom;
  }
}

// Check if all the atoms followed by the given target atom are of size zero.
// When this method is called, an atom being added is not of size zero and
// will be added to the head of the followon chain. All the atoms between the
// atom and the targetAtom (specified by layout-after) need to be of size zero
// in this case. Otherwise the desired layout is impossible.
bool LayoutPass::checkAllPrevAtomsZeroSize(const DefinedAtom *targetAtom) {
  const DefinedAtom *atom = _followOnRoots[targetAtom];
  while (true) {
    if (atom == targetAtom)
      return true;
    if (atom->size() != 0)
      // TODO: print warning that an impossible layout is being desired by the
      // user.
      return false;
    AtomToAtomT::iterator targetFollowOnAtomsIter = _followOnNexts.find(atom);
    // The target atom must be in the chain of its root.
    assert(targetFollowOnAtomsIter != _followOnNexts.end());
    atom = targetFollowOnAtomsIter->second;
  }
}

// Set the root of all atoms in targetAtom's chain to the given root.
void LayoutPass::setChainRoot(const DefinedAtom *targetAtom,
                              const DefinedAtom *root) {
  // Walk through the followon chain and override each node's root.
  while (true) {
    _followOnRoots[targetAtom] = root;
    AtomToAtomT::iterator targetFollowOnAtomsIter =
        _followOnNexts.find(targetAtom);
    if (targetFollowOnAtomsIter == _followOnNexts.end())
      return;
    targetAtom = targetFollowOnAtomsIter->second;
  }
}

/// This pass builds the followon tables described by two DenseMaps
/// followOnRoots and followonNexts.
/// The followOnRoots map contains a mapping of a DefinedAtom to its root
/// The followOnNexts map contains a mapping of what DefinedAtom follows the
/// current Atom
/// The algorithm follows a very simple approach
/// a) If the atom is first seen, then make that as the root atom
/// b) The targetAtom which this Atom contains, has the root thats set to the
///    root of the current atom
/// c) If the targetAtom is part of a different tree and the root of the
///    targetAtom is itself, Chain all the atoms that are contained in the tree
///    to the current Tree
/// d) If the targetAtom is part of a different chain and the root of the
///    targetAtom until the targetAtom has all atoms of size 0, then chain the
///    targetAtoms and its tree to the current chain
void LayoutPass::buildFollowOnTable(MutableFile::DefinedAtomRange &range) {
  ScopedTask task(getDefaultDomain(), "LayoutPass::buildFollowOnTable");
  // Set the initial size of the followon and the followonNext hash to the
  // number of atoms that we have.
  _followOnRoots.resize(range.size());
  _followOnNexts.resize(range.size());
  for (const DefinedAtom *ai : range) {
    for (const Reference *r : *ai) {
      if (r->kind() != lld::Reference::kindLayoutAfter)
        continue;
      const DefinedAtom *targetAtom = llvm::dyn_cast<DefinedAtom>(r->target());
      _followOnNexts[ai] = targetAtom;

      // If we find a followon for the first time, lets make that atom as the
      // root atom.
      if (_followOnRoots.count(ai) == 0)
        _followOnRoots[ai] = ai;

      auto iter = _followOnRoots.find(targetAtom);
      if (iter == _followOnRoots.end()) {
        // If the targetAtom is not a root of any chain, lets make the root of
        // the targetAtom to the root of the current chain.
        _followOnRoots[targetAtom] = _followOnRoots[ai];
      } else if (iter->second == targetAtom) {
        // If the targetAtom is the root of a chain, the chain becomes part of
        // the current chain. Rewrite the subchain's root to the current
        // chain's root.
        setChainRoot(targetAtom, _followOnRoots[ai]);
      } else {
        // The targetAtom is already a part of a chain. If the current atom is
        // of size zero, we can insert it in the middle of the chain just
        // before the target atom, while not breaking other atom's followon
        // relationships. If it's not, we can only insert the current atom at
        // the beginning of the chain. All the atoms followed by the target
        // atom must be of size zero in that case to satisfy the followon
        // relationships.
        size_t currentAtomSize = ai->size();
        if (currentAtomSize == 0) {
          const DefinedAtom *targetPrevAtom = findAtomFollowedBy(targetAtom);
          _followOnNexts[targetPrevAtom] = ai;
          _followOnRoots[ai] = _followOnRoots[targetPrevAtom];
        } else {
          if (!checkAllPrevAtomsZeroSize(targetAtom))
            break;
          _followOnNexts[ai] = _followOnRoots[targetAtom];
          setChainRoot(_followOnRoots[targetAtom], _followOnRoots[ai]);
        }
      }
    }
  }
}

/// This pass builds the followon tables using InGroup relationships
/// The algorithm follows a very simple approach
/// a) If the rootAtom is not part of any root, create a new root with the
///    as the head
/// b) If the current Atom root is not found, then make the current atoms root
///    point to the rootAtom
/// c) If the root of the current Atom is itself a root of some other tree
///    make all the atoms in the chain point to the ingroup reference
/// d) Check to see if the current atom is part of the chain from the rootAtom
///    if not add the atom to the chain, so that the current atom is part of the
///    the chain where the rootAtom is in
void LayoutPass::buildInGroupTable(MutableFile::DefinedAtomRange &range) {
  ScopedTask task(getDefaultDomain(), "LayoutPass::buildInGroupTable");
  // This table would convert precededby references to follow on
  // references so that we have only one table
  for (const DefinedAtom *ai : range) {
    for (const Reference *r : *ai) {
      if (r->kind() == lld::Reference::kindInGroup) {
        const DefinedAtom *rootAtom = llvm::dyn_cast<DefinedAtom>(r->target());
        // If the root atom is not part of any root
        // create a new root
        if (_followOnRoots.count(rootAtom) == 0) {
          _followOnRoots[rootAtom] = rootAtom;
        }
        // If the current Atom has not been seen yet and there is no root
        // that has been set, set the root of the atom to the targetAtom
        // as the targetAtom points to the ingroup root
        auto iter = _followOnRoots.find(ai);
        if (iter == _followOnRoots.end()) {
          _followOnRoots[ai] = rootAtom;
        } else if (iter->second == ai) {
          if (iter->second != rootAtom)
            setChainRoot(iter->second, rootAtom);
        } else {
          // TODO : Flag an error that the root of the tree
          // is different, Here is an example
          // Say there are atoms
          // chain 1 : a->b->c
          // chain 2 : d->e->f
          // and e,f have their ingroup reference as a
          // this could happen only if the root of e,f that is d
          // has root as 'a'
          continue;
        }

        // Check if the current atom is part of the chain
        bool isAtomInChain = false;
        const DefinedAtom *lastAtom = rootAtom;
        while (true) {
          AtomToAtomT::iterator followOnAtomsIter =
                  _followOnNexts.find(lastAtom);
          if (followOnAtomsIter != _followOnNexts.end()) {
            lastAtom = followOnAtomsIter->second;
            if (lastAtom == ai) {
              isAtomInChain = true;
              break;
            }
          }
          else
            break;
        } // findAtomInChain

        if (!isAtomInChain)
          _followOnNexts[lastAtom] = ai;
      }
    }
  }
}

/// This pass builds the followon tables using Preceded By relationships
/// The algorithm follows a very simple approach
/// a) If the targetAtom is not part of any root and the current atom is not
///    part of any root, create a chain with the current atom as root and
///    the targetAtom as following the current atom
/// b) Chain the targetAtom to the current Atom if the targetAtom is not part
///    of any chain and the currentAtom has no followOn's
/// c) If the targetAtom is part of a different tree and the root of the
///    targetAtom is itself, and if the current atom is not part of any root
///    chain all the atoms together
/// d) If the current atom has no followon and the root of the targetAtom is
///    not equal to the root of the current atom(the targetAtom is not in the
///    same chain), chain all the atoms that are lead by the targetAtom into
///    the current chain
void LayoutPass::buildPrecededByTable(MutableFile::DefinedAtomRange &range) {
  ScopedTask task(getDefaultDomain(), "LayoutPass::buildPrecededByTable");
  // This table would convert precededby references to follow on
  // references so that we have only one table
  for (const DefinedAtom *ai : range) {
    for (const Reference *r : *ai) {
      if (r->kind() == lld::Reference::kindLayoutBefore) {
        const DefinedAtom *targetAtom = llvm::dyn_cast<DefinedAtom>(r->target());
        // Is the targetAtom not chained
        if (_followOnRoots.count(targetAtom) == 0) {
          // Is the current atom not part of any root ?
          if (_followOnRoots.count(ai) == 0) {
            _followOnRoots[ai] = ai;
            _followOnNexts[ai] = targetAtom;
            _followOnRoots[targetAtom] = _followOnRoots[ai];
          } else if (_followOnNexts.count(ai) == 0) {
            // Chain the targetAtom to the current Atom
            // if the currentAtom has no followon references
            _followOnNexts[ai] = targetAtom;
            _followOnRoots[targetAtom] = _followOnRoots[ai];
          }
        } else if (_followOnRoots.find(targetAtom)->second == targetAtom) {
          // Is the targetAtom in chain with the targetAtom as the root ?
          bool changeRoots = false;
          if (_followOnRoots.count(ai) == 0) {
            _followOnRoots[ai] = ai;
            _followOnNexts[ai] = targetAtom;
            _followOnRoots[targetAtom] = _followOnRoots[ai];
            changeRoots = true;
          } else if (_followOnNexts.count(ai) == 0) {
            // Chain the targetAtom to the current Atom
            // if the currentAtom has no followon references
            if (_followOnRoots[ai] != _followOnRoots[targetAtom]) {
              _followOnNexts[ai] = targetAtom;
              _followOnRoots[targetAtom] = _followOnRoots[ai];
              changeRoots = true;
            }
          }
          // Change the roots of the targetAtom and its chain to
          // the current atoms root
          if (changeRoots) {
            setChainRoot(_followOnRoots[targetAtom], _followOnRoots[ai]);
          }
        }   // Is targetAtom root
      }     // kindLayoutBefore
    }       // Reference
  }         // atom iteration
}           // end function


/// Build an ordinal override map by traversing the followon chain, and
/// assigning ordinals to each atom, if the atoms have their ordinals
/// already assigned skip the atom and move to the next. This is the
/// main map thats used to sort the atoms while comparing two atoms together
void LayoutPass::buildOrdinalOverrideMap(MutableFile::DefinedAtomRange &range) {
  ScopedTask task(getDefaultDomain(), "LayoutPass::buildOrdinalOverrideMap");
  uint64_t index = 0;
  for (const DefinedAtom *ai : range) {
    const DefinedAtom *atom = ai;
    if (_ordinalOverrideMap.find(atom) != _ordinalOverrideMap.end())
      continue;
    AtomToAtomT::iterator start = _followOnRoots.find(atom);
    if (start != _followOnRoots.end()) {
      for (const DefinedAtom *nextAtom = start->second; nextAtom != NULL;
           nextAtom = _followOnNexts[nextAtom]) {
        AtomToOrdinalT::iterator pos = _ordinalOverrideMap.find(nextAtom);
        if (pos == _ordinalOverrideMap.end()) {
          _ordinalOverrideMap[nextAtom] = index++;
        }
      }
    } else {
      _ordinalOverrideMap[atom] = index++;
    }
  }
}

/// Perform the actual pass
void LayoutPass::perform(MutableFile &mergedFile) {
  ScopedTask task(getDefaultDomain(), "LayoutPass");
  MutableFile::DefinedAtomRange atomRange = mergedFile.definedAtoms();

  // Build follow on tables
  buildFollowOnTable(atomRange);

  // Build Ingroup reference table
  buildInGroupTable(atomRange);

  // Build preceded by tables
  buildPrecededByTable(atomRange);

  // Build override maps
  buildOrdinalOverrideMap(atomRange);

  DEBUG({
    llvm::dbgs() << "unsorted atoms:\n";
    for (const DefinedAtom *atom : atomRange) {
      llvm::dbgs() << "  file=" << atom->file().path()
                   << ", name=" << atom->name()
                   << ", size=" << atom->size()
                   << ", type=" << atom->contentType()
                   << ", ordinal=" << atom->ordinal()
                   << "\n";
    }
  });

  // sort the atoms
  parallel_sort(atomRange.begin(), atomRange.end(), _compareAtoms);

  DEBUG({
    llvm::dbgs() << "sorted atoms:\n";
    for (const DefinedAtom *atom : atomRange) {
      llvm::dbgs() << "  file=" << atom->file().path()
                   << ", name=" << atom->name()
                   << ", size=" << atom->size()
                   << ", type=" << atom->contentType()
                   << ", ordinal=" << atom->ordinal()
                   << "\n";
    }
  });

}
