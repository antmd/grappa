////////////////////////////////////////////////////////////////////////
// This file is part of Grappa, a system for scaling irregular
// applications on commodity clusters. 

// Copyright (C) 2010-2014 University of Washington and Battelle
// Memorial Institute. University of Washington authorizes use of this
// Grappa software.

// Grappa is free software: you can redistribute it and/or modify it
// under the terms of the Affero General Public License as published
// by Affero, Inc., either version 1 of the License, or (at your
// option) any later version.

// Grappa is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// Affero General Public License for more details.

// You should have received a copy of the Affero General Public
// License along with this program. If not, you may obtain one from
// http://www.affero.org/oagpl.html.
////////////////////////////////////////////////////////////////////////

#include <signal.h>

#ifdef HEAPCHECK_ENABLE
#include <gperftools/heap-checker.h>
#endif

#include "GlobalMemory.hpp"
#include "tasks/Task.hpp"
#include "Cache.hpp"
#include "PerformanceTools.hpp"

#include "Collective.hpp"
#include "MetricsTools.hpp"
#include "tasks/StealQueue.hpp"

// #include "tasks/GlobalQueue.hpp"

#include "FileIO.hpp"

#include "RDMAAggregator.hpp"
#include "LocaleSharedMemory.hpp"
#include "SharedMessagePool.hpp"
#include "Metrics.hpp"

#include <fstream>

#include <mpi.h>

#include "Grappa.hpp"

#ifdef VTRACE
#include <vt_user.h>
#endif

// command line arguments
DEFINE_uint64( num_starting_workers, 512, "Number of starting workers in task-executer pool" );
DEFINE_bool( set_affinity, false, "Set processor affinity based on local rank" );

DEFINE_int64( node_memsize, -1, "User-specified node memory size; overrides autodetection" );

DEFINE_uint64( io_blocks_per_node, 4, "Maximum number of asynchronous IO operations to issue concurrently per node.");
DEFINE_uint64( io_blocksize_mb, 4, "Size of each asynchronous IO operation's buffer." );

DECLARE_int64( locale_shared_size );
DECLARE_double( locale_shared_fraction );
DECLARE_double( locale_user_heap_fraction );
DECLARE_double( global_heap_fraction );
DECLARE_int64( shared_pool_max_size );
DECLARE_bool( global_memory_use_hugepages );
DECLARE_double(locale_shared_fraction);


using namespace Grappa::impl;
using namespace Grappa::Metrics;
using namespace Grappa;

/// Flag to tell this node it's okay to exit.
bool Grappa_done_flag;

static int jobid = 0;
static const char * nodelist_str = NULL;

Core * node_neighbors;

#ifdef HEAPCHECK_ENABLE
HeapLeakChecker * Grappa_heapchecker = 0;
#endif

namespace Grappa {
  
  double tick_rate = 0.0;
  
  Worker * master_thread;
  static Worker * user_main_thr;
  
  // defined here so FileIO.hpp doesn't need a .cpp
  IODescriptor * aio_completed_stack;
  
  namespace impl {

    int64_t global_memory_size_bytes = 0;
    int64_t global_bytes_per_core = 0;
    int64_t global_bytes_per_locale = 0;
    
    /// Tell all nodes that we are ready to exit.
    /// This will terminate the automatic portions of the communication layer
    void signal_done() {
      VLOG(5) << "mark done";
      Grappa_done_flag = true;
    }
  }
  
  void global_heap_init(size_t init_size) {
    // by default, will allocate as much shared memory as it is
    // possible to evenly split among the processors on a node
    if (init_size != -1) {
      impl::global_memory_size_bytes = init_size;
      impl::global_bytes_per_core = init_size / cores();
      impl::global_bytes_per_locale = init_size / locales();
      return;
    }
    
    // Decide how much memory we should allocate for global shared heap.
    // this uses the locale shared size calculated in LocaleSharedMemory.cpp
    auto sz = static_cast<int64_t>(FLAGS_locale_shared_size * FLAGS_global_heap_fraction);
    
    int64_t nnode = global_communicator.locales;
    int64_t ppn = global_communicator.locale_cores;
    
    int64_t bytes_per_core = sz / ppn;
    // round down to page size so we don't ask for too much
    bytes_per_core &= ~( (1L << 12) - 1 );
    
    // be aware of hugepages
    // Each core should ask for a multiple of 1GB hugepages
    // and the whole node should ask for no more than the total pages available
    if ( FLAGS_global_memory_use_hugepages ) {
      int64_t pages_per_core = bytes_per_core / (1L << 30);
      int64_t new_bpp = pages_per_core * (1L << 30);
      if (new_bpp == 0) {
        MASTER_ONLY VLOG(1) << "Allocating 1GB per core anyway.";
        new_bpp = 1L << 30;
      }
      MASTER_ONLY VLOG_IF(1, bytes_per_core != new_bpp) << "With ppn=" << ppn << ", can only allocate "
      << pages_per_core*ppn << " / " << FLAGS_node_memsize / (1L << 30) << " 1GB huge pages per node";
      bytes_per_core = new_bpp;
    }
    
    int64_t bytes = nnode * ppn * bytes_per_core;
    int64_t bytes_per_node = ppn * bytes_per_core;
    MASTER_ONLY DVLOG(2) << "bpp = " << bytes_per_core << ", bytes = " << bytes << ", bytes_per_node = " << bytes_per_node
    << ", node_memsize = " << FLAGS_node_memsize << ", heap_size = " << sz;
    MASTER_ONLY VLOG(2) << "nnode: " << nnode << ", ppn: " << ppn << ", iBs/node: " << log2((double)bytes_per_node) << ", total_iBs: " << log2((double)bytes);
    
    impl::global_memory_size_bytes = bytes;
    impl::global_bytes_per_core = bytes_per_core;
    impl::global_bytes_per_locale = bytes_per_node;
  }
  
}

/// Check whether we are ready to exit.
bool Grappa_done() {
  return Grappa_done_flag;
}

/// Body of the polling thread.
static void poller( Worker * me, void * args ) {
  StateTimer::setThreadState( StateTimer::COMMUNICATION );
  StateTimer::enterState_communication();
  while( !Grappa_done() ) {
    global_scheduler.stats.sample();

    Grappa::impl::poll();
    
    // check async. io completions
    if (aio_completed_stack) {
      // atomically grab the stack, replacing it with an empty stack again
      IODescriptor * desc = __sync_lock_test_and_set(&aio_completed_stack, NULL);

      while (desc != NULL) {
        desc->handle_completion();
        IODescriptor * temp = desc->nextCompleted;
        desc->nextCompleted = NULL;
        desc = temp;
      }
    }

    Grappa::yield_periodic();
  }
  // cleanup stragglers on readyQ since I should be last to run;
  // no one else matters.
  // Tasks on task queues would be a programmer error
  global_scheduler.shutdown_readyQ();
  VLOG(5) << "polling Worker exiting";

  // master will be scheduled upon exit of poller thread
}

// from google
namespace google {
typedef void (*override_handler_t)(int);
extern void OverrideDefaultSignalHandler( override_handler_t handler );
extern void DumpStackTrace();
}

/// handler for dumping stats on a signal
static int stats_dump_signal = SIGUSR2;
static void stats_dump_sighandler( int signum ) {
  google::DumpStackTrace();

  Grappa::Metrics::print( LOG(INFO), registered_stats(), "" );

  global_rdma_aggregator.dump_counts();

  // instantaneous state
  LOG(INFO) << global_scheduler;
  LOG(INFO) << global_task_manager;
}


bool freeze_flag = false;

namespace Grappa {
namespace impl {

void freeze_for_debugger() {
  auto pid = getpid();
  LOG(INFO) << global_communicator.hostname() << ":" << pid << " freezing for debugger. Set freeze_flag=false to continue.";
  google::FlushLogFiles(google::GLOG_INFO);
  fflush(stdout);
  fflush(stderr);

  while( freeze_flag ) {
    sleep(1);
  }
}

/// called on failures to backtrace and pause for debugger
void failure_function() {
  google::FlushLogFiles(google::GLOG_INFO);
  google::DumpStackTrace();
  if( freeze_flag ) {
    freeze_for_debugger();
  }
  LOG(INFO) << "Exiting via failure function";
  google::FlushLogFiles(google::GLOG_INFO);
  exit(1);
}

static void failure_sighandler( int signum ) {
  google::FlushLogFiles(google::GLOG_INFO);
  google::DumpStackTrace();
  if( freeze_flag ) {
    freeze_for_debugger();
  }
  LOG(INFO) << "Exiting due to signal " << signum;
  google::FlushLogFiles(google::GLOG_INFO);
  exit(1);
}

static void mpi_failure_function( MPI_Comm * comm, int * error_code, ... ) {
  char error_string[MPI_MAX_ERROR_STRING];
  int length;
  MPI_Error_string( *error_code, error_string, &length);
  LOG(FATAL) << "MPI call failed: " << error_string;
  failure_function();
}

}
}

void adjust_footprints() {
  auto locale_cores = global_communicator.locale_cores;
  auto locale_total = FLAGS_locale_shared_size;
  
  // (FLAGS_locale_shared_size either set manually or computed from fraction of node_memsize)
  auto locale_heap_bytes = static_cast<size_t>(FLAGS_locale_user_heap_fraction * (double)locale_total);
  auto global_heap_bytes = Grappa::impl::global_memory_size_bytes;
  
  // memory left for Grappa components
  auto grappa_bytes = (locale_total - global_heap_bytes - locale_heap_bytes) / locale_cores;
  CHECK_GT(grappa_bytes, 0)
    << "\nMust leave some memory for Grappa system components!\n"
    << " - locale_heap_bytes: " << locale_heap_bytes << "\n"
    << " - global_heap_bytes: " << global_heap_bytes << "\n"
    << " - total:    " << locale_total;
  
  auto total_footprint = []{
    return global_communicator.estimate_footprint()
    + global_rdma_aggregator.estimate_footprint()
    + global_task_manager.estimate_footprint()
    + SharedMessagePool::estimate_footprint();
  };
  
  if (total_footprint() < grappa_bytes) return;
  
  // otherwise try to get all the grappa components to play along
  long remaining = grappa_bytes;
  
  try {
    
    remaining -= SharedMessagePool::adjust_footprint(remaining / 4);
    if (remaining < 0) throw "SharedMessagePool";
    if (total_footprint() < grappa_bytes) throw true;
    
    remaining -= global_communicator.adjust_footprint(remaining / 3);
    if (remaining < 0) throw "Communicator";
    if (total_footprint() < grappa_bytes) throw true;
    
    remaining -= global_rdma_aggregator.adjust_footprint(remaining / 2);
    if (remaining < 0) throw "RDMA Aggregator";
    if (total_footprint() < grappa_bytes) throw true;
    
    remaining -= global_task_manager.adjust_footprint(remaining);
    if (remaining < 0) throw "TaskManager";
    if (total_footprint() < grappa_bytes) throw true;
    
  } catch (bool success) {
    
    MASTER_ONLY LOG(INFO) << "\nFootprint estimates: "
    << "\n- locale_heap_bytes: " << locale_heap_bytes
    << "\n- global_heap_bytes: " << global_heap_bytes
    << "\n- total for Grappa:  " << grappa_bytes
    << "\n  - global_communicator:    " << global_communicator.estimate_footprint()
    << "\n  - global_rdma_aggregator: " << global_rdma_aggregator.estimate_footprint()
    << "\n  - global_task_manager:    " << global_task_manager.estimate_footprint();

  } catch (char const* component) {
    MASTER_ONLY LOG(ERROR)
      << "\nUnable to fit Grappa components in memory. Failed at " << component
      << "\n  locale_heap_bytes:      " << locale_heap_bytes
      << "\n  global_heap_bytes:      " << global_heap_bytes
      << "\n  total for Grappa:       " << grappa_bytes
      << "\n  global_communicator:    " << global_communicator.estimate_footprint()
      << "\n  global_rdma_aggregator: " << global_rdma_aggregator.estimate_footprint()
      << "\n  global_task_manager:    " << global_task_manager.estimate_footprint()
      << "\n  shared_message_pool:    " << SharedMessagePool::estimate_footprint();
    exit(1);
  }
}

/// Initialize Grappa components. We are not ready to run until the
/// user calls Grappa_activate().
void Grappa_init( int * argc_p, char ** argv_p[], int64_t global_memory_size_bytes)
{
  // std::cerr << "Argc is " << *argc_p << std::endl;
  // for( int i = 0; i < *argc_p; ++i ) {
  //   std::cerr << "Arg " << i << ": " << (*argv_p)[i] << std::endl;
  // }
  
  if (global_memory_size_bytes == -1) {
    
  }
  
  // help generate unique profile filename
  Grappa::impl::set_exe_name( (*argv_p)[0] );

  // parse command line flags
  google::ParseCommandLineFlags(argc_p, argv_p, true);

  // activate logging
  google::InitGoogleLogging( *argv_p[0] );
  google::InstallFailureFunction( &Grappa::impl::failure_function );
  
  DVLOG(2) << "Initializing Grappa library....";

#ifdef HEAPCHECK_ENABLE
  VLOG(1) << "heap check enabled";
  Grappa_heapchecker = new HeapLeakChecker("Grappa");
#endif
  
  char * mem_reg_disabled = getenv("MV2_USE_LAZY_MEM_UNREGISTER");
  if (mem_reg_disabled && strncmp(mem_reg_disabled,"0",1) == 0) {
    VLOG(2) << "memory registration disabled";
  }

  // how fast do we tick?
  Grappa::force_tick();
  Grappa::force_tick();
  Grappa::Timestamp start_ts = Grappa::timestamp();
  double start = Grappa::walltime();
  // now go do other stuff for a while
  
  // initializes system_wide global_communicator
  global_communicator.init( argc_p, argv_p );
  
  MPI_Errhandler mpi_error_handler;
  MPI_Comm_create_errhandler( &Grappa::impl::mpi_failure_function, &mpi_error_handler );
  MPI_Comm_set_errhandler( global_communicator.grappa_comm, mpi_error_handler );


  google::InstallFailureFunction( &Grappa::impl::failure_function );

  // check to see if we should freeze for the debugger on error
  char * freeze_on_error = getenv("GRAPPA_FREEZE_ON_ERROR");
  if( freeze_on_error && ( (strncmp(freeze_on_error,"1",1) == 0) ||
                           (strncmp(freeze_on_error,"true",4) == 0) ||
                           (strncmp(freeze_on_error,"True",4) == 0) ||
                           (strncmp(freeze_on_error,"TRUE",4) == 0) ||
                           (strncmp(freeze_on_error,"yes",3) == 0) ||
                           (strncmp(freeze_on_error,"Yes",3) == 0) ||
                           (strncmp(freeze_on_error,"YES",3) == 0) ) ) {
    freeze_flag = true;
  }

  // check to see if we should freeze for the debugger now
  char * freeze_now = getenv("GRAPPA_FREEZE");
  if( freeze_now && ( (strncmp(freeze_now,"1",1) == 0) ||
                           (strncmp(freeze_now,"true",4) == 0) ||
                           (strncmp(freeze_now,"True",4) == 0) ||
                           (strncmp(freeze_now,"TRUE",4) == 0) ||
                           (strncmp(freeze_now,"yes",3) == 0) ||
                           (strncmp(freeze_now,"Yes",3) == 0) ||
                           (strncmp(freeze_now,"YES",3) == 0) ) ) {
    freeze_flag = true;
    freeze_for_debugger();
  }


  // set up stats dump signal handler
  struct sigaction stats_dump_sa;
  sigemptyset( &stats_dump_sa.sa_mask );
  stats_dump_sa.sa_flags = 0;
  stats_dump_sa.sa_handler = &stats_dump_sighandler;
  CHECK_EQ( 0, sigaction( stats_dump_signal, &stats_dump_sa, 0 ) ) << "Stats dump signal handler installation failed.";

  // struct sigaction sigabrt_sa;
  // sigemptyset( &sigabrt_sa.sa_mask );
  // sigabrt_sa.sa_flags = 0;
  // sigabrt_sa.sa_handler = &gasnet_pause_sighandler;
  // CHECK_EQ( 0, sigaction( SIGABRT, &sigabrt_sa, 0 ) ) << "SIGABRT signal handler installation failed.";

  struct sigaction sigsegv_sa;
  sigemptyset( &sigsegv_sa.sa_mask );
  sigsegv_sa.sa_flags = 0;
  sigsegv_sa.sa_handler = &Grappa::impl::failure_sighandler;
  CHECK_EQ( 0, sigaction( SIGSEGV, &sigsegv_sa, 0 ) ) << "SIGSEGV signal handler installation failed.";

  // Asynchronous IO
  // initialize completed stack
  aio_completed_stack = NULL;

  // handler
#ifdef AIO_SIGNAL
  struct sigaction aio_sa;
  aio_sa.sa_flags = SA_RESTART | SA_SIGINFO;
  aio_sa.sa_sigaction = Grappa::impl::handle_async_io;
  if (sigaction(AIO_SIGNAL, &aio_sa, NULL) == -1) {
    fprintf(stderr, "Error setting up async io signal handler.\n");
    exit(1);
  }
#endif

  
  VLOG(2) << "Communicator initialized.";
  
  CHECK( global_communicator.locale_cores <= MAX_CORES_PER_LOCALE );
  
  //  initializes system_wide global_aggregator
  global_aggregator.init();

  VLOG(2) << "Aggregator initialized.";
  
  // set CPU affinity if requested
#ifdef CPU_SET
  if( FLAGS_set_affinity ) {
    char * localid_str = getenv("SLURM_LOCALID");
    if( NULL != localid_str ) {
      int localid = atoi( localid_str );
      cpu_set_t mask;
      CPU_ZERO( &mask );
      CPU_SET( localid, &mask );
      sched_setaffinity( 0, sizeof(mask), &mask );
    }
  }
#endif

  // initialize node shared memory
  if( FLAGS_node_memsize == -1 ) { 
    // if user doesn't specify how much memory each node has, try to estimate.
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    FLAGS_node_memsize = pages * page_size;
    VLOG(2) << "Estimated node memory size = " << FLAGS_node_memsize;
  }
  locale_shared_memory.init();
  
  // initialize shared message pool
  SharedMessagePool::init();
  
  global_heap_init(global_memory_size_bytes);
  
  adjust_footprints();
  
  Grappa_done_flag = false;

  // process command line args for Tau
  //TAU_INIT( argc_p, argv_p );
#ifdef GRAPPA_TRACE
  TAU_PROFILE_SET_NODE(Grappa::mycore());
#endif

  //TODO: options for local stealing
  node_neighbors = new Core[Grappa::cores()];
  for ( Core nod=0; nod < Grappa::cores(); nod++ ) {
    node_neighbors[nod] = nod;
  }
  
  // start threading layer
  master_thread = convert_to_master();
  VLOG(2) << "Initializing tasking layer."
           << " num_starting_workers=" << FLAGS_num_starting_workers;
  global_task_manager.init( Grappa::mycore(), node_neighbors, Grappa::cores() ); //TODO: options for local stealing
  global_scheduler.init( master_thread, &global_task_manager );
  
  VLOG(2) << "Scheduler initialized.";
  
  // start RDMA Aggregator *after* threading layer
  global_rdma_aggregator.init();
  
  VLOG(2) << "RDMA aggregator initialized.";
  
  // collect some stats on this job
  Grappa::force_tick();
  Grappa::force_tick();
  Grappa::Timestamp end_ts = Grappa::timestamp();
  double end = Grappa::walltime();
  Grappa::tick_rate = (double) (end_ts - start_ts) / (end-start);

  char * jobid_str = getenv("SLURM_JOB_ID");
  jobid = jobid_str ? atoi(jobid_str) : 0;
  nodelist_str = getenv("SLURM_NODELIST");
  if( NULL == nodelist_str ) nodelist_str = "undefined";
}


/// Activate Grappa network layer and enter barrier. After this,
/// arbitrary communication is allowed.
void Grappa_activate() 
{
  DVLOG(2) << "Activating Grappa library....";
  
  locale_shared_memory.activate(); // do this before communicator
  auto base_locale_shared_memory_allocated = locale_shared_memory.get_allocated();

  global_communicator.activate();
  auto communicator_locale_shared_memory_allocated = locale_shared_memory.get_allocated();

  global_task_manager.activate();
  auto tasks_locale_shared_memory_allocated = locale_shared_memory.get_allocated();

  global_communicator.barrier();

  // initializes system_wide global_memory pointer
  global_memory = new GlobalMemory( Grappa::impl::global_memory_size_bytes );
  auto heap_locale_shared_memory_allocated = locale_shared_memory.get_allocated();

  // fire up polling thread
  global_scheduler.periodic( impl::worker_spawn( master_thread, &global_scheduler, &poller, NULL ) );
  auto polling_locale_shared_memory_allocated = locale_shared_memory.get_allocated();

  global_rdma_aggregator.activate();
  auto aggregator_locale_shared_memory_allocated = locale_shared_memory.get_allocated();
  
  SharedMessagePool::activate();
  auto shared_pool_locale_shared_memory_allocated = locale_shared_memory.get_allocated();
  
  if (Grappa::mycore() == 0) {
    double node_sz_gb = static_cast<double>(FLAGS_node_memsize) / (1L<<30);
    double locale_sz_gb = static_cast<double>(FLAGS_locale_shared_size) / (1L<<30);
    double locale_core_sz_gb = static_cast<double>(FLAGS_locale_shared_size) / Grappa::locale_cores() / (1L<<30);
    double communicator_sz_gb = static_cast<double>( communicator_locale_shared_memory_allocated - base_locale_shared_memory_allocated ) / (1L<<30);
    double tasks_sz_gb = static_cast<double>( tasks_locale_shared_memory_allocated - communicator_locale_shared_memory_allocated ) / (1L<<30);
    double heap_sz_gb = static_cast<double>( heap_locale_shared_memory_allocated - tasks_locale_shared_memory_allocated ) / (1L<<30);
    tasks_sz_gb += static_cast<double>( polling_locale_shared_memory_allocated - heap_locale_shared_memory_allocated ) / (1L<<30);
    double aggregator_sz_gb = static_cast<double>( aggregator_locale_shared_memory_allocated - polling_locale_shared_memory_allocated ) / (1L<<30);
    double shared_pool_sz_gb = static_cast<double>( shared_pool_locale_shared_memory_allocated - aggregator_locale_shared_memory_allocated ) / (1L<<30);
    double shared_pool_max_sz_gb = static_cast<double>( FLAGS_shared_pool_max_size ) / (1L<<30);
    
    size_t free_sz = static_cast<double>(Grappa::impl::locale_shared_memory.get_free_memory());
    double free_sz_gb = static_cast<double>(free_sz) / (1L<<30);
    double free_core_sz_gb = static_cast<double>(free_sz) / Grappa::locale_cores() / (1L<<30);
    VLOG(1) << "\n-------------------------\nShared memory breakdown:\n"
            << "  node total:                   " << node_sz_gb << " GB\n"
            << "  locale shared heap total:     " << locale_sz_gb << " GB\n"
            << "  locale shared heap per core:  " << locale_core_sz_gb << " GB\n"
            << "  communicator per core:        " << communicator_sz_gb << " GB\n"
            << "  tasks per core:               " << tasks_sz_gb << " GB\n"
            << "  global heap per core:         " << heap_sz_gb << " GB\n"
            << "  aggregator per core:          " << aggregator_sz_gb << " GB\n"
            << "  shared_pool current per core: " << shared_pool_sz_gb << " GB\n"
            << "  shared_pool max per core:     " << shared_pool_max_sz_gb << " GB\n"
            << "  free per locale:              " << free_sz_gb << " GB\n"
            << "  free per core:                " << free_core_sz_gb << " GB\n"
            << "-------------------------";

    CHECK_GT( free_core_sz_gb, shared_pool_max_sz_gb ) 
      << "Not enough free locale shared heap for fully-allocated shared message pool";
  }
  
  global_communicator.barrier();
}


static bool global_queue_initialized = false;

/// Initialize global queue for load balancing.
/// Must be called in user_main
void Grappa_global_queue_initialize() {
  // if ( global_task_manager.global_queue_on() ) {
  //   on_all_cores([]{
  //     GlobalQueue<Task>::global_queue.init();
  //     global_queue_initialized = true;
  //   });
  // }
}

bool Grappa_global_queue_isInit() {
  return global_queue_initialized;
}


///
/// Job exit routines
///

/// User main done
void Grappa_end_tasks() {
  // send task termination signal
  CHECK( Grappa::mycore() == 0 );
  // TODO: we should really flush the aggregator here.
  for ( Core n = 0; n < Grappa::cores(); n++ ) {
    global_communicator.send_immediate( n, [] {
        global_task_manager.signal_termination();
      } );
  }
}


///// Active message to tell this node it's okay to exit.
//static void Grappa_mark_done_am( void * args, size_t args_size, void * payload, size_t payload_size ) {
//  VLOG(5) << "mark done";
//  Grappa_done_flag = true;
//}

/// Finish the job. 
/// 
/// If we've already been notified that we can exit, enter global
/// barrier and then clean up. If we have not been notified, then
/// notify everyone else, enter the barrier, and then clean up.
int Grappa_finish( int retval )
{
  Grappa::impl::signal_done(); // this may be overkill (just set done bit?)

  //TAU_PROFILE_EXIT("Tau_profile_exit called");
  global_communicator.barrier();

  DVLOG(1) << "Cleaning up Grappa library....";

  StateTimer::finish();

  global_task_manager.finish();
  global_aggregator.finish();

  if (global_memory) delete global_memory;
  locale_shared_memory.finish();

  global_communicator.finish( retval );
 
//  Grappa_dump_stats();

  // probably never get here (depending on communication layer)

  destroy_thread( master_thread );

#ifdef HEAPCHECK_ENABLE
  assert( Grappa_heapchecker->NoLeaks() );
#endif

  return retval;
}

namespace Grappa {

  void init( int * argc_p, char ** argv_p[], int64_t size ) {
    Grappa_init( argc_p, argv_p, size );
    Grappa_activate();
  }

  int finalize() {
    return Grappa_finish(0);
  }

} // namespace Grappa
