/*
 * Copyright Elasticsearch B.V. and/or licensed to Elasticsearch B.V. under one
 * or more contributor license agreements. Licensed under the Elastic License
 * 2.0 and the following additional limitation. Functionality enabled by the
 * files subject to the Elastic License 2.0 may only be used in production when
 * invoked by an Elasticsearch process with a license key installed that permits
 * use of machine learning features. You may not use this file except in
 * compliance with the Elastic License 2.0 and the foregoing additional
 * limitation.
 */

#ifndef INCLUDED_ml_model_CFlatPrefixTree_h
#define INCLUDED_ml_model_CFlatPrefixTree_h

#include <core/ImportExport.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ml {
namespace core {

//! \brief A flat prefix tree that allows efficient string lookups
//!
//! DESCRIPTION:\n
//! A trie tree that is packed in a vector. It allows for efficient
//! full or prefix string lookups. Incremental updates are not supported.
//! Updating the tree requires rebuilding it.
//!
//! IMPLEMENTATION DECISIONS:\n
//! Trie trees allow for efficient string partial or full lookups.
//! They usually are implemented using pointers. The downside of that
//! approach is the lack of spatial locality resulting to low performance
//! due to cache misses. The CFlatPrefixTree works around this issue
//! by packing the trie in a vector. Each vector item is a node containing
//! a char, the type (leaf (l), branch (b) or both (*)) and the index of the node at
//! the next tree level. The vector contains two types of nodes:
//!   1) padding node: uses the char '$' and the next field is the
//!   number of distinct characters that are contained at a tree level.
//!   2) char node: contains the distinct character that is present at
//!   a tree level, its type and the index of the next tree level.
//! A tree level is better understood as the sum of the padding node and
//! the associated char nodes. For the input of (ab, abd, bdf)
//! the vector would look like:
//! [($, $, 2) (a, b, 3) (b, b, 7) ($, $, 1) (b, *, 5) ($, $, 1) (d, l, -) ($, $, 1) (d, b, 9) ($, $, 1) (f, l, -) ]
//! where '-' means no child and is actually represented by max(std::uint32_t).
//! This representation allows for search by visiting the first node, applying
//! binary search on the first character, moving on to the node indicated by
//! the characters next index, applying binary search on the second character,
//! and so on.
class CORE_EXPORT CFlatPrefixTree {
public:
    using TStrVec = std::vector<std::string>;
    using TStrVecCItr = TStrVec::const_iterator;
    using TStrCItr = std::string::const_iterator;
    using TStrCRItr = std::string::const_reverse_iterator;

private:
    struct SNode {
        //! See CMemory.
        static constexpr bool dynamicSizeAlwaysZero() { return true; }

        SNode(char c, char type, std::uint32_t next);

        std::uint64_t checksum() const;

        bool operator<(char rhs) const;
        char s_Char;
        char s_Type;
        std::uint32_t s_Next;
    };

    struct SDistinctChar {
        SDistinctChar(char c, char type, std::size_t start, std::size_t end);

        char s_Char;
        char s_Type;
        std::size_t s_Start;
        std::size_t s_End;
    };

private:
    using TNodeVec = std::vector<SNode>;
    using TNodeVecCItr = TNodeVec::const_iterator;
    using TDistinctCharVec = std::vector<SDistinctChar>;

public:
    //! Default constructor.
    CFlatPrefixTree();

    //! Builds the tree from a list of \p prefixes. The \p prefixes
    //! vector is required to be lexicographically sorted.
    //! Returns true if the tree was build successfully.
    bool build(const TStrVec& prefixes);

    //! Returns true if the \p key starts with a prefix present in the tree.
    bool matchesStart(const std::string& key) const;

    //! Returns true if the \p key fully matches a prefix present in the tree.
    bool matchesFully(const std::string& key) const;

    //! Returns true if the string described by \p start, \p end
    //! starts with a prefix present in the tree.
    bool matchesStart(TStrCItr start, TStrCItr end) const;

    //! Returns true if the string described by \p start, \p end
    //! fully matches a prefix present in the tree.
    bool matchesFully(TStrCItr start, TStrCItr end) const;

    //! Returns true if the string described by \p start, \p end
    //! starts with a prefix present in the tree.
    bool matchesStart(TStrCRItr start, TStrCRItr end) const;

    //! Returns true if the string described by \p start, \p end
    //! fully matches a prefix present in the tree.
    bool matchesFully(TStrCRItr start, TStrCRItr end) const;

    //! Clears the tree.
    void clear();

    //! Pretty-prints the tree.
    std::string print() const;

    std::uint64_t checksum() const;

private:
    //! The recursive building helper.
    void buildRecursively(const TStrVec& prefixes,
                          std::size_t prefixesStart,
                          std::size_t prefixesEnd,
                          std::size_t charPos);

    //! Extracts the distinct characters and stores it in \p distinctChars
    //! along with the start and end index in the \p prefixes vector.
    void extractDistinctCharacters(const TStrVec& prefixes,
                                   std::size_t prefixesStart,
                                   std::size_t prefixesEnd,
                                   std::size_t charPos,
                                   TDistinctCharVec& distinctChars);

    //! Implementation of the search algorithm.
    template<typename ITR>
    bool matches(ITR start, ITR end, bool requireFullMatch) const;

private:
    //! The vector representing the trie tree.
    TNodeVec m_FlatTree;
};
}
}

#endif // INCLUDED_ml_model_CFlatPrefixTree_h
