//---------------------------------------------------------- -*- Mode: C++ -*-
// Copyright (C) 2007 Josh Taylor (Kosmix Corporation)
// Created 2007-10-08
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

#include <kdi/local/table_types.h>
#include <kdi/server/DiskWriter.h>

#include <warp/file.h>
#include <warp/log.h>
#include <ex/exception.h>
#include <oort/record.h>
#include <oort/recordstream.h>
#include <oort/recordbuilder.h>
#include <oort/recordbuffer.h>
#include <oort/fileio.h>
#include <warp/string_pool_builder.h>
#include <warp/adler.h>
#include <warp/bloom_filter.h>
#include <boost/static_assert.hpp>

#include <set>
#include <map>

using namespace kdi;
using namespace kdi::local;
using namespace kdi::server;
using namespace oort;
using namespace warp;
using namespace ex;
using namespace std;

//----------------------------------------------------------------------------
// PooledBuilder
//----------------------------------------------------------------------------
namespace
{
    struct PooledBuilder
    {
        RecordBuilder builder;
        StringPoolBuilder pool;
        BuilderBlock * arr;
        BuilderBlock * fams;
        uint32_t nItems;
        bool addFams;
        uint32_t nFams;

        PooledBuilder() :
            builder(),
            pool(&builder),
            arr(builder.subblock(8)),
            nItems(0),
            addFams(false)
        {
        }

        void reset()
        {
            builder.reset();
            pool.reset(&builder);
            arr = builder.subblock(8);
            fams = builder.subblock(8);
            nItems = 0;
        }

        void build(Record & r, Allocator * alloc) {
            // Finish array
            builder.appendOffset(arr);
            builder.append(nItems);

            if(addFams) {
                builder.appendOffset(fams);
                builder.append(nFams);
            }

            // Construct record
            builder.build(r, alloc);
        }

        void write(RecordStreamHandle const & output, Record const & r)
        {
            output->put(r);
            reset();
        }

        void write(RecordStreamHandle const & output, Allocator * alloc)
        {
            Record r;
            build(r, alloc);
            write(output, r);
        }

        size_t getDataSize() const
        {
            return pool.getDataSize() + arr->size() + 8;
        }
    };

}

//----------------------------------------------------------------------------
// DiskWriter::Impl
//----------------------------------------------------------------------------
class DiskWriter::Impl
{
    FilePtr fp;
    FileOutput::handle_t output;
    RecordBufferAllocator alloc;
    size_t blockSize;

    PooledBuilder block;
    PooledBuilder index;
    size_t cellCount;

    int64_t lowestTime;
    int64_t highestTime;

    // Maps string offsets in the index header to col family bitmasks
    map<size_t, uint32_t> colFamilyMasks;
    vector<size_t> allColFamilies;
    uint32_t nextColMask; // Mask to assign to the next column family
    uint32_t curColMask;  // Computed mask for the current cell block

    void addIndexEntry(Record const & cellBlock);
    void writeCellBlock();
    void writeBlockIndex();

    void addCell(strref_t row, strref_t column, int64_t timestamp,
                 strref_t value, bool isErasure);

public:
    explicit Impl(size_t blockSize);

    void open(FilePtr const & out);
    void close();

    void emit(strref_t row, strref_t column, int64_t timestamp, 
              strref_t value, bool isErasure);

    size_t getCellCount() const;
    size_t getDataSize() const;
};

//----------------------------------------------------------------------------
// DiskWriter::Impl
//----------------------------------------------------------------------------
void DiskWriter::Impl::addIndexEntry(Record const & cbRec)
{
    BOOST_STATIC_ASSERT(disk::BlockIndexV1::VERSION == 1);

    // Get the last cell record from the cell block 
    disk::CellBlock const * cellBlock = cbRec.cast<disk::CellBlock>();
    disk::CellKey const & lastCellKey = cellBlock->cells[cellBlock->cells.size()-1].key;
    strref_t lastRow = *lastCellKey.row;

    // Get string offset for cell key (just the last row)
    BuilderBlock * b = index.pool.getStringBlock();
    size_t         r = index.pool.getStringOffset(lastRow);

    // Calculate Adler-32 checksum for the cell block, written in index 
    uint32_t cbChecksum = adler32((uint8_t*)cbRec.getData(), cbRec.getLength());

    // Append IndexEntry to array
    index.arr->append(cbChecksum);   // checkSum
    index.arr->appendOffset(b, r);   // row
    index.arr->append(fp->tell());   // blockOffset
    index.arr->append(lowestTime);   // timeRange-min
    index.arr->append(highestTime);  // timeRange-max
    index.arr->append(curColMask);   // column family mask
    curColMask = 0;

    // Pad record out to full alignment
    index.arr->appendPadding(8);

    ++index.nItems;
}

void DiskWriter::Impl::addCell(strref_t row, strref_t column, int64_t timestamp, 
                               strref_t value, bool isErasure)
{
    BOOST_STATIC_ASSERT(disk::CellBlock::VERSION == 0);

    // Get string offsets for cell data (null value for erasures)
    BuilderBlock * b = block.pool.getStringBlock();
    size_t         r = block.pool.getStringOffset(row);
    size_t         c = block.pool.getStringOffset(column);
    int64_t        t = timestamp;

    // Append CellData to array
    block.arr->appendOffset(b, r);         // key.row
    block.arr->appendOffset(b, c);         // key.column
    block.arr->append(t);                  // key.timestamp
    if(!isErasure)
    {
        size_t v = block.pool.getStringOffset(value);
        block.arr->appendOffset(b, v);     // value
    }
    else
    {
        block.arr->appendOffset(0);        // value
    }
    block.arr->append<uint32_t>(0);        // __pad
    ++block.nItems;

    // Remember range of timestamps added
    if(block.nItems == 1) {
        lowestTime = t;
        highestTime = t;
    } else {
        if(t < lowestTime) lowestTime = t;
        if(t > highestTime) highestTime = t;
    }

/*
    // Update the column family lookup and mask
    size_t colFamily = index.pool.getStringOffset(x.getColumnFamily());
    if(colFamilyMasks.count(colFamily) == 1) {
        curColMask |= colFamilyMasks[colFamily];
    } else {
        // The mask is allowed to wrap around!
        // If there are more than 32 column families (rare use case),
        // it simply results in false positives
        if(nextColMask == 0) nextColMask = 1;

        colFamilyMasks[colFamily] = nextColMask;
        curColMask |= nextColMask;
        nextColMask = nextColMask << 1;
    }
*/
}

void DiskWriter::Impl::writeCellBlock()
{
    Record r;
    block.build(r, &alloc);

    // Create the index entry 
    addIndexEntry(r);

    // Write out the block 
    block.write(output, r);
}

void DiskWriter::Impl::writeBlockIndex()
{
    BuilderBlock * b = index.pool.getStringBlock();
    uint32_t nFams = 0;
    
    map<size_t, uint32_t>::const_iterator mi;
    for(mi = colFamilyMasks.begin(); mi != colFamilyMasks.end(); ++mi) {
        pair<size_t, uint32_t> const & p = *mi;
        index.fams->appendOffset(b, p.first);
        ++nFams;
    }

    index.addFams = true;
    index.nFams = nFams;
    index.write(output, &alloc);
}

DiskWriter::Impl::Impl(size_t blockSize) :
    alloc(),
    blockSize(blockSize),
    cellCount(0)
{
    block.builder.setHeader<disk::CellBlock>();
    index.builder.setHeader<disk::BlockIndexV1>();
}

void DiskWriter::Impl::open(FilePtr const & out)
{
    fp = out;
    output = FileOutput::make(fp);

    block.reset();
    index.reset();

    lowestTime = 0;
    highestTime = 0;

    colFamilyMasks.clear();
    nextColMask = 1;
    curColMask = 0;
}

void DiskWriter::Impl::close()
{
    // Flush last cell block if there's something pending
    if(block.nItems) {
        writeCellBlock();
    }

    // Remember index position
    uint64_t indexOffset = fp->tell();

    // Write BlockIndex record
    writeBlockIndex();

    // Write TableInfo record
    Record r;
    alloc.construct<disk::TableInfo>(r, indexOffset);
    output->put(r);

    // Shut down
    output->flush();
    fp->close();
    output.reset();
    fp.reset();
}

void DiskWriter::Impl::emit(strref_t row, strref_t column, int64_t timestamp,
                            strref_t value, bool isErasure)
{
    ++cellCount;

    // Add cell to the current block
    addCell(row, column, timestamp, value, isErasure);

    // Flush block if it is big enough
    if(block.getDataSize() >= blockSize) {
        writeCellBlock();
    }
}

size_t DiskWriter::Impl::getDataSize() const
{
    return fp->tell() + block.getDataSize() + index.getDataSize();
}

size_t DiskWriter::Impl::getCellCount() const
{
    return cellCount;
}

//----------------------------------------------------------------------------
// DiskTableWriter
//----------------------------------------------------------------------------
DiskWriter::DiskWriter(warp::FilePtr const & out, std::string const & fn,
                       size_t blockSize) :
    impl(new DiskWriter::Impl(blockSize)),
    fn(fn),
    blockSize_deprecated(blockSize)
{
    impl->open(out);
}

DiskWriter::DiskWriter(size_t blockSize) :
    blockSize_deprecated(blockSize)
{
}

DiskWriter::~DiskWriter()
{
    impl.reset();
}

void DiskWriter::open(std::string const & fn)
{
    if(impl)
        raise<RuntimeError>("DiskWriter already open");

    impl.reset(new Impl(blockSize_deprecated));
    impl->open(File::output(fn));
    this->fn = fn;
}

void DiskWriter::close()
{
    finish();
}

void DiskWriter::emitCell(strref_t row, strref_t column, int64_t timestamp,
                          strref_t value)
{
    if(!impl)
        raise<RuntimeError>("emitCell() to closed DiskWriter");

    impl->emit(row, column, timestamp, value, false);
}

void DiskWriter::emitErasure(strref_t row, strref_t column, int64_t timestamp)
{
    if(!impl)
        raise<RuntimeError>("emitErasure() to closed DiskWriter");

    impl->emit(row, column, timestamp, StringRange(), true);
}

size_t DiskWriter::getCellCount() const
{
    if(!impl)
        raise<RuntimeError>("getCellCount() on closed DiskWriter");

    return impl->getCellCount();
}

size_t DiskWriter::getDataSize() const
{
    if(!impl)
        raise<RuntimeError>("getDataSize() on closed DiskWriter");

    return impl->getDataSize();
}

std::string DiskWriter::finish()
{
    if(!impl)
        raise<RuntimeError>("DiskWriter already closed");

    impl->close();
    impl.reset();
    return fn;
}