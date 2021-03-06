//---------------------------------------------------------- -*- Mode: C++ -*-
// Copyright (C) 2008 Josh Taylor (Kosmix Corporation)
// Created 2008-03-19
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

#ifndef KDI_META_META_TABLE_H
#define KDI_META_META_TABLE_H

#include <kdi/table.h>
#include <kdi/meta/meta_cache.h>
#include <warp/interval.h>
#include <warp/functional.h>
#include <boost/noncopyable.hpp>
#include <map>
#include <string>

namespace kdi {
namespace meta {

    class MetaTable;

} // namespace meta
} // namespace kdi

//----------------------------------------------------------------------------
// MetaTable
//----------------------------------------------------------------------------
class kdi::meta::MetaTable
    : public kdi::Table,
      private boost::noncopyable
{
    typedef std::pair<TablePtr, bool> entry_t;
    typedef std::map<std::string, entry_t> locmap_t;

    MetaCache metaCache;
    std::string tableName;

    locmap_t tabletCache;

    warp::Interval<std::string> lastRows;
    locmap_t::iterator lastIt;

    TablePtr const & getTablet(strref_t row)
    {
        // Is the row in the range of the last thing we returned?
        if(!lastRows.contains(row, warp::less()))
        {
            // Nope, find the tablet info in the metaCache
            MetaEntry const & ent = metaCache.lookup(tableName, row);

            // Save the range and lookup the tablet for the location
            lastRows = ent.rows;
            lastIt = tabletCache.find(ent.location);

            // Check to see if we haven't seen this location before
            if(lastIt == tabletCache.end())
            {
                // New location, make a new entry in the location cache
                lastIt = tabletCache.insert(
                    make_pair(
                        ent.location,
                        entry_t(
                            Table::open(ent.location),
                            true
                            )
                        )
                    ).first;
            }
        }

        // Mark the entry as touched since last sync
        lastIt->second.second = true;

        // Return the table
        return lastIt->second.first;
    }

public:
    MetaTable(TablePtr const & metaTable, std::string const & tableName);

    virtual void set(strref_t row, strref_t column, int64_t timestamp,
                     strref_t value);
    virtual void erase(strref_t row, strref_t column, int64_t timestamp);
    virtual void sync();
    virtual CellStreamPtr scan(ScanPredicate const & pred) const;
};


#endif // KDI_META_META_TABLE_H
