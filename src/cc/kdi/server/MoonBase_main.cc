//---------------------------------------------------------- -*- Mode: C++ -*-
// Copyright (C) 2009 Josh Taylor (Kosmix Corporation)
// Created 2009-04-07
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

#include <kdi/server/TabletServer.h>
#include <kdi/server/TabletServerI.h>
#include <warp/options.h>
#include <warp/filestream.h>
#include <warp/log.h>
#include <ex/exception.h>
#include <Ice/Ice.h>
#include <boost/scoped_ptr.hpp>
#include <string>
#include <memory>

#include <warp/WorkerPool.h>
#include <kdi/server/TestConfigReader.h>
#include <kdi/server/DirectBlockCache.h>
#include <kdi/server/NullLogWriter.h>
#include <kdi/server/NullConfigWriter.h>

using namespace kdi::server;
using namespace kdi;
using namespace warp;
using namespace ex;
using namespace std;

namespace {

    template <class T>
    void release(T * & p)
    {
        delete p;
        p = 0;
    }

    class MainServerAssembly
        : private boost::noncopyable
    {
        boost::scoped_ptr<TestConfigReader> configReader;
        boost::scoped_ptr<NullConfigWriter> configWriter;
        boost::scoped_ptr<warp::WorkerPool> workerPool;
        boost::scoped_ptr<TabletServer> server;
        boost::scoped_ptr<DirectBlockCache> cache;

    public:
        ~MainServerAssembly() { cleanup(); }

        TabletServer * getServer() const { return server.get(); }
        BlockCache * getBlockCache() const { return cache.get(); }

        void init()
        {
            workerPool.reset(new WorkerPool(4, "Pool", true));
            configReader.reset(new TestConfigReader);
            configWriter.reset(new NullConfigWriter);

            TabletServer::Bits bits;
            bits.createNewLog = &NullLogWriter::make;
            bits.configReader = configReader.get();
            bits.configWriter = configWriter.get();
            bits.workerPool = workerPool.get();
            
            server.reset(new TabletServer(bits));
            cache.reset(new DirectBlockCache);
        }

        void cleanup()
        {
            cache.reset();
            server.reset();
            configReader.reset();
            configWriter.reset();
            workerPool.reset();
        }
    };
    

    class ServerApp
        : public virtual Ice::Application
    {
    public:
        void appMain(int ac, char ** av)
        {
            // Set options
            OptionParser op("%prog [ICE-parameters] [options]");
            {
                using namespace boost::program_options;
                op.addOption("root,r", value<string>(),
                             "Root directory for tablet data");
                op.addOption("pidfile,p", value<string>(),
                             "Write PID to file");
                op.addOption("nodaemon", "Don't fork and run as daemon");
            }

            // Parse options
            OptionMap opt;
            ArgumentList args;
            op.parse(ac, av, opt, args);

            // Get table root directory
            string root;
            if(!opt.get("root", root))
                op.error("need --root");

            // Write PID file
            string pidfile;
            if(opt.get("pidfile", pidfile))
            {
                FileStream pid(File::output(pidfile));
                pid << getpid() << endl;
                pid.close();
            }

            // Create adapter
            Ice::CommunicatorPtr ic = communicator();
            Ice::ObjectAdapterPtr readWriteAdapter
                = ic->createObjectAdapter("ReadWriteAdapter");

            // Make our TabletServer
            MainServerAssembly serverAssembly;
            TabletServer * server = serverAssembly.getServer();

            // Create TableManagerI object
            Ice::ObjectPtr obj = new TabletServerI;
            readWriteAdapter->add(obj, ic->stringToIdentity("TabletServer"));

            // Run server
            readWriteAdapter->activate();
            ic->waitForShutdown();

            // Shutdown
            log("Shutting down");
            serverAssembly.cleanup();

            // Destructors after this point
            log("Cleaning up");
        }

        virtual int run(int ac, char ** av)
        {
            try {
                appMain(ac, av);
            }
            catch(OptionError const & err) {
                cerr << err << endl;
                return 2;
            }
            catch(Exception const & err) {
                cerr << err << endl
                     << err.getBacktrace();
                return 1;
            }
            return 0;
        }
    };

    void outOfMemory()
    {
        std::cerr << "Operator new failed.  Out of memory." << std::endl;
        _exit(1);
    }

}

int main(int ac, char ** av)
{
    std::set_new_handler(&outOfMemory);

    bool daemonize = true;
    for(int i = 1; i < ac; ++i)
    {
        if(!strcmp(av[i], "--"))
            break;

        if(!strcmp(av[i], "--help") ||
           !strcmp(av[i], "-h") ||
           !strcmp(av[i], "--nodaemon"))
        {
            daemonize = false;
            break;
        }
    }

    if(daemonize)
    {
        if(0 != daemon(1,1))
            raise<RuntimeError>("daemon failed: %s", getStdError());
    }

    ServerApp app;
    return app.main(ac, av);
}
