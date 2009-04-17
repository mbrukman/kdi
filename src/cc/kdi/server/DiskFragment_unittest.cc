//---------------------------------------------------------- -*- Mode: C++ -*-
// Copyright (C) 2007 Josh Taylor (Kosmix Corporation)
// Created 2007-10-09
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

#include <unittest/main.h>
#include <kdi/server/DiskFragment.h>
#include <kdi/server/CellBuilder.h>
#include <kdi/memory_table.h>

#include <warp/fs.h>
#include <string>
#include <boost/format.hpp>
#include <boost/test/test_tools.hpp>
#include <boost/test/output_test_stream.hpp>

#include <kdi/server/DiskOutput.h>

using namespace kdi;
using namespace kdi::local;
using namespace kdi::server;
using namespace warp;
using namespace std;
using boost::format;

BOOST_AUTO_TEST_CASE(output_test)
{
    DiskOutput out(128);
    out.open("memfs:output");
    BOOST_CHECK_EQUAL(0, out.getCellCount());
    size_t startSize = out.getDataSize();

    out.emitCell("row", "col", 0, "val");
    BOOST_CHECK_EQUAL(1, out.getCellCount());
    BOOST_CHECK(out.getDataSize() > startSize);

    out.emitErasure("erase", "col", 0);
    BOOST_CHECK_EQUAL(2, out.getCellCount());

    out.close();
}

namespace {

typedef std::auto_ptr<FragmentBlock> FragmentBlockPtr;
typedef std::auto_ptr<FragmentBlockReader> FragmentBlockReaderPtr;

void dumpCells(Fragment const & frag, CellOutput & out)
{
    ScanPredicate pred("");
    size_t blockAddr = frag.nextBlock(pred, 0);
    while(blockAddr != size_t(-1))
    {
        FragmentBlockPtr block = frag.loadBlock(blockAddr);
        FragmentBlockReaderPtr reader = block->makeReader(pred);

        CellKey nextCell;
        BOOST_CHECK(reader->advance(nextCell));
        reader->copyUntil(0, out);
        BOOST_CHECK(!reader->advance(nextCell));

        blockAddr = frag.nextBlock(pred, blockAddr+1);
    }
}

size_t countCells(Fragment const & frag)
{
    CellBuilder cellBuilder;
    dumpCells(frag, cellBuilder);   
    return cellBuilder.getCellCount();
}

typedef boost::test_tools::output_test_stream test_out_t;

class TestCellOutput : 
    public CellOutput,
    public test_out_t
{
    size_t cellCount;

public:
    TestCellOutput() : cellCount(0) {}
    ~TestCellOutput() {}
        
    void emitCell(strref_t row, strref_t column, int64_t timestamp,
                  strref_t value) 
    {
        *this << '(' << row << ',' << column << ',' << timestamp << ',' 
              << value << ')';
    }

    void emitErasure(strref_t row, strref_t column, int64_t timestamp)
    {
        *this << '(' << row << ',' << column << ',' << timestamp << ',' 
              << "ERASED)";
    }

    size_t getCellCount() const { return cellCount; }
    size_t getDataSize() const { return cellCount; }
};

class TestFragmentBuilder
{
    MemoryTablePtr memTable;
    DiskOutput out;

public:
    
    explicit TestFragmentBuilder(string const & file, size_t blockSz=128) :
        memTable(MemoryTable::create(false)),
        out(blockSz)
    {
        out.open(file);
    }

    ~TestFragmentBuilder()
    {
        out.close();
    }

    void set(strref_t row, strref_t column,
             int64_t timestamp, strref_t value)
    {
        memTable->set(row, column, timestamp, value);
    }

    void erase(strref_t row, strref_t column, int64_t timestamp)
    {
        memTable->erase(row, column, timestamp);
    }

    void write(string const & file)
    {
        out.open(file);

        Cell x;
        CellStreamPtr scan = memTable->scan();
        while(scan->get(x))
        {
            if(x.isErasure())
            {
                out.emitErasure(x.getRow(), x.getColumn(), x.getTimestamp());
            }
            else
            {
                out.emitCell(x.getRow(), x.getColumn(), x.getTimestamp(),
                             x.getValue());
            }
        }

        out.close();
    }
};

/// Fill a fragment with cells of the form:
///   ("row-i", "col-j", k, "val-i-j-k")
/// for i in [1, nRows], j in [1, nCols], and k in [1, nRevs]
void makeTestFragment(size_t blockSize, string const & filename, 
                      size_t nRows, size_t nCols, size_t nRevs,
                      string const & fmt = "%d")
{
    TestFragmentBuilder out(filename, blockSize);

    string rowFmt = (format("row-%s") % fmt).str();
    string colFmt = (format("col-%s") % fmt).str();
    string valFmt = (format("val-%s-%s-%s") % fmt % fmt % fmt).str();

    for(size_t i = 1; i <= nRows; ++i)
    {
        string row = (format(rowFmt)%i).str();
        for(size_t j = 1; j <= nCols; ++j)
        {
            string col = (format(colFmt)%j).str();
            for(size_t k = 1; k <= nRevs; ++k)
            {
                string val = (format(valFmt)%i%j%k).str();
                out.set(row, col, k, val);
            }
        }
    }
}
 
} // namespace

BOOST_AUTO_TEST_CASE(empty_test)
{
    // Make empty table
    {
        DiskOutput out(128);
        out.open("memfs:empty");
        out.close();
    }

    DiskFragment df("memfs:empty");
    BOOST_CHECK_EQUAL(0, countCells(df));
}

#define CHECK_FRAGMENT(name, expected) \
{ \
    DiskFragment df(name); \
    TestCellOutput test; \
    dumpCells(df, test); \
    BOOST_CHECK(test.is_equal(expected)); \
}

BOOST_AUTO_TEST_CASE(simple_test)
{
    // Write some cells
    {
        DiskOutput out(128);
        out.open("memfs:simple");
        out.emitCell("row1", "col1", 42, "val1");
        out.emitCell("row1", "col2", 42, "val2");
        out.emitCell("row1", "col2", 23, "val3");
        out.emitErasure("row1", "col3", 23);
        out.emitCell("row2", "col1", 42, "val4");
        out.emitCell("row2", "col3", 42, "val5");
        out.emitCell("row3", "col2", 23, "val6");
        out.close();
    }

    DiskFragment df("memfs:simple");
    BOOST_CHECK_EQUAL(7, countCells(df));

    CHECK_FRAGMENT("memfs:simple",
        "(row1,col1,42,val1)"
        "(row1,col2,42,val2)"
        "(row1,col2,23,val3)"
        "(row1,col3,23,ERASED)"
        "(row2,col1,42,val4)"
        "(row2,col3,42,val5)"
        "(row3,col2,23,val6)"
    );
}

BOOST_AUTO_TEST_CASE(pred_test)
{
    // Write some cells
    {
        DiskOutput out(128);
        out.open("memfs:pred");
        out.emitCell("row1", "col1", 42, "val1");
        out.emitCell("row1", "col2", 42, "val2");
        out.emitCell("row1", "col2", 23, "val3");
        out.emitErasure("row1", "col3", 23);
        out.emitCell("row2", "col1", 42, "val4");
        out.emitCell("row2", "col3", 42, "val5");
        out.emitCell("row3", "col2", 23, "val6");
        out.close();
    }

    CHECK_FRAGMENT("memfs:pred",
        "(row1,col1,42,val1)"
        "(row1,col2,42,val2)"
        "(row1,col2,23,val3)"
        "(row1,col3,23,ERASED)"
        "(row2,col1,42,val4)"
        "(row2,col3,42,val5)"
        "(row3,col2,23,val6)"
    );
}

BOOST_AUTO_TEST_CASE(rewrite_test)
{
    DiskOutput out(128);
    
    // First fragment
    out.open("memfs:one");
    out.emitCell("row1", "col1", 42, "one1");
    out.emitCell("row1", "col2", 42, "one2");
    out.close();

    // Second fragment
    out.open("memfs:two");
    out.emitCell("row1", "col1", 42, "two1");
    out.emitCell("row1", "col3", 42, "two2");
    out.close();

    // Check first fragment
    CHECK_FRAGMENT("memfs:one",
        "(row1,col1,42,one1)"
        "(row1,col2,42,one2)"
    );

    // Check second fragment
    CHECK_FRAGMENT("memfs:two",
        "(row1,col1,42,two1)"
        "(row1,col3,42,two2)"
    );
}
