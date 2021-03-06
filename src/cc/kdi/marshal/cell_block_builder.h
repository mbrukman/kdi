//---------------------------------------------------------- -*- Mode: C++ -*-
// Copyright (C) 2007 Josh Taylor (Kosmix Corporation)
// Created 2007-11-09
// 
// This file is part of KDI.
// 
// KDI is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2 of the License, or any later version.
// 
// KDI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
// 
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
//----------------------------------------------------------------------------

#ifndef KDI_MARSHAL_CELL_BLOCK_BUILDER_H
#define KDI_MARSHAL_CELL_BLOCK_BUILDER_H

#include <kdi/marshal/cell_block.h>
#include <kdi/strref.h>
#include <kdi/cell.h>
#include <warp/builder.h>
#include <warp/string_pool_builder.h>

namespace kdi {
namespace marshal {

    class CellBlockBuilder;

} // namespace marshal
} // namespace kdi

//----------------------------------------------------------------------------
// CellBlockBuilder
//----------------------------------------------------------------------------
class kdi::marshal::CellBlockBuilder
    : private boost::noncopyable
{
    warp::BuilderBlock * base;
    warp::BuilderBlock * arr;
    warp::StringPoolBuilder pool;
    size_t basePos;
    uint32_t nCells;

    void append(size_t rowOffset,
                size_t columnOffset,
                int64_t timestamp,
                size_t valueOffset)
    {
        warp::BuilderBlock * b = pool.getStringBlock();

        // Append CellData to array
        arr->appendOffset(b, rowOffset);         // key.row
        arr->appendOffset(b, columnOffset);      // key.column
        arr->append(timestamp);                  // key.timestamp
        if(valueOffset != size_t(-1))
            arr->appendOffset(b, valueOffset);   // value
        else
            arr->appendOffset(0);                // value
        arr->append<uint32_t>(0);                // __pad

        // Update cells.length in main block
        ++nCells;
        base->write(basePos + 4, nCells);  // cells.length
    }

public:
    /// Create a CellBlockBuilder over the given BuilderBlock.
    explicit CellBlockBuilder(warp::BuilderBlock * builder) :
        pool(builder)
    {
        reset(builder);
    }

    /// Reset the builder with a new BuilderBlock.
    void reset(warp::BuilderBlock * builder)
    {
        BOOST_STATIC_ASSERT(CellBlock::VERSION == 0);

        EX_CHECK_NULL(builder);

        base = builder;
        basePos = base->size();
        arr = base->subblock(8);
        pool.reset(builder);
        nCells = 0;

        *base << *arr           // cells.offset
              << nCells;        // cells.length
    }

    /// Reset the builder and reuse the same BuilderBlock.
    void reset() { reset(getBuilder()); }

    /// Get the backing BuilderBlock.
    warp::BuilderBlock * getBuilder() const { return base; }

    /// Append a Cell to the current CellBlock.
    void appendCell(strref_t row, strref_t column, int64_t timestamp,
                    strref_t value)
    {
        append(pool.getStringOffset(row),
               pool.getStringOffset(column),
               timestamp,
               pool.getStringOffset(value));
    }

    /// Append an erasure Cell to the current CellBlock.
    void appendErasure(strref_t row, strref_t column, int64_t timestamp)
    {
        append(pool.getStringOffset(row),
               pool.getStringOffset(column),
               timestamp,
               size_t(-1));
    }

    /// Append a erasure Cell to the current CellBlock.
    void append(Cell const & x)
    {
        append(pool.getStringOffset(x.getRow()),
               pool.getStringOffset(x.getColumn()),
               x.getTimestamp(),
               x.isErasure() ? size_t(-1)
               : pool.getStringOffset(x.getValue()));
    }

    /// Get approximate data size of current CellBlock.
    size_t getDataSize() const
    {
        return base->size() - basePos + arr->size() + pool.getDataSize();
    }

    /// Get number of Cells in current CellBlock.
    size_t getCellCount() const
    {
        return nCells;
    }
};

#endif // KDI_MARSHAL_CELL_BLOCK_BUILDER_H
