/* Machine independent support for QNX Neutrino /proc (process file system)
   for GDB.  Written by Colin Burgess at QNX Software Systems Limited.

   Copyright (C) 2003-2019 Free Software Foundation, Inc.

   Contributed by QNX Software Systems Ltd.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/debug.h>
#include <sys/procfs.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <dirent.h>
#include <sys/netmgr.h>
#include <sys/auxv.h>

#include "gdbcore.h"
#include "inferior.h"
#include "target.h"
#include "target-descriptions.h"
#include "objfiles.h"
#include "gdbthread.h"
#include "nto-tdep.h"
#include "command.h"
#include "regcache.h"
#include "solib.h"
#include "inf-child.h"
#include "common/filestuff.h"
#include "observable.h"

#define NULL_PID    0
#define _DEBUG_FLAG_TRACE  (_DEBUG_FLAG_TRACE_EXEC|_DEBUG_FLAG_TRACE_RD|\
    _DEBUG_FLAG_TRACE_WR|_DEBUG_FLAG_TRACE_MODIFY)

int ctl_fd;

static sighandler_t ofunc;

static procfs_run run;

static ptid_t do_attach (ptid_t ptid);

static const target_info nto_procfs_info = {
  "native",
  N_("QNX Neutrino local process"),
  N_("QNX Neutrino local process (started by the \"run\" command).")
};

/*
 * todo it may make sense to put the session data in here too..
 */
class nto_procfs final : public inf_child_target
{
public:
  /* we have no private data yet but that may follow..
  ~nto_procfs () override;
  */

  const target_info &info () const override
  { return nto_procfs_info; }

  static void nto_open (const char *, int);
  // void close () override;

  bool can_attach () override {
    return true;
  }
  void attach ( const char*, int ) override;
  void post_attach( int ) override;
  void detach (inferior *, int) override;
//  void disconnect (const char *, int) override;
  void resume (ptid_t, int TARGET_DEBUG_PRINTER (target_debug_print_step), enum gdb_signal) override;
  ptid_t wait (ptid_t, struct target_waitstatus *, int TARGET_DEBUG_PRINTER (target_debug_print_options)) override;
  void fetch_registers (struct regcache *, int) override;
  void store_registers (struct regcache *, int) override;
//  void prepare_to_store (struct regcache *) override;
  void files_info () override;
  int insert_breakpoint (struct gdbarch *, struct bp_target_info *) override;
  int remove_breakpoint (struct gdbarch *, struct bp_target_info *, enum remove_bp_reason) override;
  int can_use_hw_breakpoint (enum bptype, int, int) override;
  int insert_hw_breakpoint (struct gdbarch *, struct bp_target_info *) override;
  int remove_hw_breakpoint (struct gdbarch *, struct bp_target_info *) override;
  int remove_watchpoint (CORE_ADDR, int, enum target_hw_bp_type, struct expression *) override;
  int insert_watchpoint (CORE_ADDR, int, enum target_hw_bp_type, struct expression *) override;
  bool stopped_by_watchpoint () override {
    return nto_stopped_by_watchpoint();
  }
  bool have_continuable_watchpoint () override {
    return true;
  }
#if 0
  void terminal_init () override;
  void terminal_inferior () override;
  void terminal_ours_for_output () override;
  void terminal_info (const char *, int) override;
#endif
  void kill () override;
  void load (const char *args, int from_tty ) override {
    return generic_load (args, from_tty);
  }
  bool can_create_inferior () override {
    return true;
  }
/* todo */
  //int insert_fork_catchpoint (int) override;
  //int remove_fork_catchpoint (int) override;
  //int insert_vfork_catchpoint (int) override;
  //int remove_vfork_catchpoint (int) override;
  //int follow_fork (int, int) override;
  //int insert_exec_catchpoint (int) override;
  //int remove_exec_catchpoint (int) override;
  void create_inferior (const char *, const std::string &, char **, int) override;
  void mourn_inferior () override;
  bool can_run () override {
    return false;
  }
  bool thread_alive (ptid_t ptid) override;
  void update_thread_list () override;
  const char *pid_to_str (ptid_t ptid) override {
    return nto_pid_to_str (ptid);
  }
  char *pid_to_exec_file (int pid) override;
  const char *extra_thread_info (thread_info *ti ) override {
    return nto_extra_thread_info( ti );
  }
  const char *thread_name (thread_info *ti ) override {
    return nto_thread_name( NULL, ti );
  }

  bool can_async_p () override {
    /* Not yet. */
    return false;
  };
  bool supports_non_stop () override {
    /* Not yet. */
    return false;
  }
  const struct target_desc *read_description () override;
  void pass_signals (int, unsigned char * TARGET_DEBUG_PRINTER (target_debug_print_signals)) override;
  void stop (ptid_t) override;
  void interrupt () override;
  void pass_ctrlc () override;
  bool supports_multi_process () override {
    return true;
  }
//  int verify_memory (const gdb_byte *data, CORE_ADDR memaddr, ULONGEST size) override;
  enum target_xfer_status xfer_partial (enum target_object object,
                const char *annex,
                gdb_byte *readbuf,
                const gdb_byte *writebuf,
                ULONGEST offset, ULONGEST len,
                ULONGEST *xfered_len) override;
};

static nto_procfs nto_procfs_target;

/*
 * wrapper for default close()
 */
static void
close1 ( int fd ) {
  close( fd );
}

/* This is called when we call 'target native' or 'target procfs
   <arg>' from the (gdb) prompt. */
void
nto_procfs::nto_open (const char *arg, int from_tty)
{
  char *endstr;
  char buffer[50];
  int fd, total_size;
  procfs_sysinfo *sysinfo;
  struct cleanup *cleanups;
  char nto_procfs_path[PATH_MAX]="/proc";

  /* Offer to kill previous inferiors before opening this target.  */
  target_preopen (from_tty);

  init_thread_list ();

  fd = open (nto_procfs_path, O_RDONLY);
  if (fd == -1)
    {
      printf_filtered ("Error opening %s : %d (%s)\n", nto_procfs_path, errno,
           safe_strerror (errno));
      error (_("Invalid procfs arg"));
    }
  cleanups = make_cleanup_close (fd);

  sysinfo = (procfs_sysinfo *) buffer;
  if (devctl (fd, DCMD_PROC_SYSINFO, sysinfo, sizeof buffer, 0) != EOK)
    {
      printf_filtered ("Error getting size: %d (%s)\n", errno,
           safe_strerror (errno));
      error (_("Devctl failed."));
    }
  else
    {
      total_size = sysinfo->total_size;
      sysinfo = (procfs_sysinfo *) alloca (total_size);
      if (sysinfo == NULL)
  {
    printf_filtered ("Memory error: %d (%s)\n", errno,
         safe_strerror (errno));
    error (_("alloca failed."));
  }
      else
  {
    if (devctl (fd, DCMD_PROC_SYSINFO, sysinfo, total_size, 0) != EOK)
      {
        printf_filtered ("Error getting sysinfo: %d (%s)\n", errno,
             safe_strerror (errno));
        error (_("Devctl failed."));
      }
    else
      {
        if (sysinfo->type !=
      nto_map_arch_to_cputype (gdbarch_bfd_arch_info
             (target_gdbarch ())->arch_name))
    error (_("Invalid target CPU."));
      }
  }
    }
  do_cleanups (cleanups);

  inf_child_open_target (arg, from_tty);
  printf_filtered ("Debugging using %s\n", nto_procfs_path);
}

static void
procfs_set_thread (ptid_t ptid)
{
  int tid;

  tid = ptid.lwp();
  devctl (ctl_fd, DCMD_PROC_CURTHREAD, &tid, sizeof (tid), 0);
}

/* wrapper to access native kill() function */
static int
kill_1( pid_t pid, int sig )
{
  return kill( pid, sig );
}

/*  Return nonzero if the thread TH is still alive.  */
bool
nto_procfs::thread_alive (ptid_t ptid)
{
  int tid;
  pid_t pid;
  procfs_status status;
  int err;

  tid = ptid.lwp ();
  pid = ptid.pid ();

  if ( kill_1 (pid, 0) == -1)
    return false;

  status.tid = tid;
  if ((err = devctl (ctl_fd, DCMD_PROC_TIDSTATUS,
         &status, sizeof (status), 0)) != EOK)
    return false;

  /* Thread is alive or dead but not yet joined,
     or dead and there is an alive (or dead unjoined) thread with
     higher tid.
     If the tid is not the same as requested, requested tid is dead.  */
  return (status.tid == tid) && (status.state != STATE_DEAD);
}

/* the NTO specific way of gathering thread information */
static void
update_thread_private_data (struct thread_info *new_thread,
          pthread_t tid, int state, int flags)
{
  struct nto_thread_info *pti;
  procfs_info pidinfo;
  struct _thread_name *tn;
  procfs_threadctl tctl;

  gdb_assert (new_thread != NULL);

  if (devctl (ctl_fd, DCMD_PROC_INFO, &pidinfo,
        sizeof(pidinfo), 0) != EOK)
    return;

  memset (&tctl, 0, sizeof (tctl));
  tctl.cmd = _NTO_TCTL_NAME;
  tn = (struct _thread_name *) (&tctl.data);

  /* Fetch name for the given thread.  */
  tctl.tid = tid;
  tn->name_buf_len = sizeof (tctl.data) - sizeof (*tn);
  tn->new_name_len = -1; /* Getting, not setting.  */
  if (devctl (ctl_fd, DCMD_PROC_THREADCTL, &tctl, sizeof (tctl), NULL) != EOK)
    tn->name_buf[0] = '\0';

  tn->name_buf[_NTO_THREAD_NAME_MAX] = '\0';

  if (!new_thread->priv)
    new_thread->priv.reset(new nto_thread_info());

  pti = (struct nto_thread_info *) new_thread->priv.get();
  pti->setName(tn->name_buf);
  pti->state = state;
  pti->flags = flags;
}

void
nto_procfs::update_thread_list ( )
{
  procfs_status status;
  int pid;
  ptid_t ptid;
  pthread_t tid;
  struct thread_info *new_thread;

  if (ctl_fd == -1)
    return;

  prune_threads ();
  pid = inferior_ptid.pid();
  status.tid = 1;

  /* fetch all the threads, starting with 1 as NTO threads start with 1
   * this is tricky as the devctl either returns EOK with the expected
   * thread ID, which means there is a thread with the number. Or devctl
   * returns with EOK but another threadID which means that the requested
   * thread is dead but the next thread has the returned id. Or devctl
   * returns with an error which means that there are no more threads */
  for (tid = 1;; ++tid)
    {
      /* if tid has caught up request thread info */
      if ( (status.tid == tid ) &&
          ( devctl (ctl_fd, DCMD_PROC_TIDSTATUS, &status, sizeof (status), 0 )
          != EOK ) )
        break;
      /* either devctl set a new threadID or tid has not yet caught up, try
       * with the new one */
      if (status.tid != tid) {
        continue;
      }
      /* there is a new thread, set up the internal representation */
      ptid = ptid_t (pid, tid, 0);
      new_thread = nto_find_thread (ptid);
      if (!new_thread)
        new_thread = add_thread (ptid);
      update_thread_private_data (new_thread, tid, status.state, 0);
      status.tid++;
    }
  return;
}

static void
do_closedir_cleanup (void *dir)
{
  closedir ((DIR*) dir);
}

static void
procfs_pidlist (const char *args, int from_tty)
{
  DIR *dp = NULL;
  struct dirent *dirp = NULL;
  char buf[PATH_MAX];
  procfs_info *pidinfo = NULL;
  procfs_debuginfo *info = NULL;
  procfs_status *status = NULL;
  int num_threads = 0;
  int pid;
  char name[PATH_MAX];
  struct cleanup *cleanups;
  char procfs_dir[PATH_MAX]="/proc";

  dp = opendir (procfs_dir);
  if (dp == NULL)
    {
      fprintf_unfiltered (gdb_stderr, "failed to opendir \"%s\" - %d (%s)",
        procfs_dir, errno, safe_strerror (errno));
      return;
    }

  cleanups = make_cleanup (do_closedir_cleanup, dp);

  /* Start scan at first pid.  */
  rewinddir (dp);

  do
    {
      int fd;
      struct cleanup *inner_cleanup;

      /* Get the right pid and procfs path for the pid.  */
      do
        {
          dirp = readdir (dp);
          if (dirp == NULL)
            {
              do_cleanups (cleanups);
              return;
            }
          snprintf (buf, sizeof (buf), "/proc/%s/as", dirp->d_name);
          pid = atoi (dirp->d_name);
        }
      while (pid == 0);

      /* Open the procfs path.  */
      fd = open (buf, O_RDONLY);
      if (fd == -1)
        {
          fprintf_unfiltered (gdb_stderr, "failed to open %s - %d (%s)\n",
              buf, errno, safe_strerror (errno));
          continue;
        }
      inner_cleanup = make_cleanup_close (fd);

      pidinfo = (procfs_info *) buf;
      if (devctl (fd, DCMD_PROC_INFO, pidinfo, sizeof (buf), 0) != EOK)
        {
          fprintf_unfiltered (gdb_stderr,
              "devctl DCMD_PROC_INFO failed - %d (%s)\n",
              errno, safe_strerror (errno));
          break;
        }
      num_threads = pidinfo->num_threads;

      info = (procfs_debuginfo *) buf;
      if (devctl (fd, DCMD_PROC_MAPDEBUG_BASE, info, sizeof (buf), 0) != EOK)
         strcpy (name, "unavailable");
      else
         strcpy (name, info->path);

      /* Collect state info on all the threads.  */
      status = (procfs_status *) buf;
      for (status->tid = 1; status->tid <= num_threads; status->tid++)
        {
          const int err
              = devctl (fd, DCMD_PROC_TIDSTATUS, status, sizeof (buf), 0);
          printf_filtered ("%s - %d", name, pid);
          if (err == EOK && status->tid != 0)
             printf_filtered ("/%d\n", status->tid);
          else
            {
              printf_filtered ("\n");
              break;
            }
        }

      do_cleanups (inner_cleanup);
    }
  while (dirp != NULL);

  do_cleanups (cleanups);
  return;
}

static void
procfs_meminfo (const char *args, int from_tty)
{
  procfs_mapinfo *mapinfos = NULL;
  static int num_mapinfos = 0;
  procfs_mapinfo *mapinfo_p, *mapinfo_p2;
  int flags = ~0, err, num, i, j;

  struct
  {
    procfs_debuginfo info;
    char buff[_POSIX_PATH_MAX];
  } map;

  struct info
  {
    unsigned addr;
    unsigned size;
    unsigned flags;
    unsigned debug_vaddr;
    unsigned long long offset;
  };

  struct printinfo
  {
    unsigned long long ino;
    unsigned dev;
    struct info text;
    struct info data;
    char name[256];
  } printme;

  /* Get the number of map entrys.  */
  err = devctl (ctl_fd, DCMD_PROC_MAPINFO, NULL, 0, &num);
  if (err != EOK)
    {
      printf ("failed devctl num mapinfos - %d (%s)\n", err,
        safe_strerror (err));
      return;
    }

  mapinfos = XNEWVEC (procfs_mapinfo, num);

  num_mapinfos = num;
  mapinfo_p = mapinfos;

  /* Fill the map entrys.  */
  err = devctl (ctl_fd, DCMD_PROC_MAPINFO, mapinfo_p, num
    * sizeof (procfs_mapinfo), &num);
  if (err != EOK)
    {
      printf ("failed devctl mapinfos - %d (%s)\n", err, safe_strerror (err));
      xfree (mapinfos);
      return;
    }

  num = (num < num_mapinfos)?num:num_mapinfos;

  /* Run through the list of mapinfos, and store the data and text info
     so we can print it at the bottom of the loop.  */
  for (mapinfo_p = mapinfos, i = 0; i < num; i++, mapinfo_p++)
    {
      if (!(mapinfo_p->flags & flags))
          mapinfo_p->ino = 0;

      if (mapinfo_p->ino == 0)  /* Already visited.  */
          continue;

      map.info.vaddr = mapinfo_p->vaddr;

      err = devctl (ctl_fd, DCMD_PROC_MAPDEBUG, &map, sizeof (map), 0);
      if (err != EOK)
          continue;

      memset (&printme, 0, sizeof printme);
      printme.dev = mapinfo_p->dev;
      printme.ino = mapinfo_p->ino;
      printme.text.addr = mapinfo_p->vaddr;
      printme.text.size = mapinfo_p->size;
      printme.text.flags = mapinfo_p->flags;
      printme.text.offset = mapinfo_p->offset;
      printme.text.debug_vaddr = map.info.vaddr;
      strcpy (printme.name, map.info.path);

      /* Check for matching data.  */
      for (mapinfo_p2 = mapinfos, j = 0; j < num; j++, mapinfo_p2++)
        {
          if (mapinfo_p2->vaddr != mapinfo_p->vaddr
              && mapinfo_p2->ino == mapinfo_p->ino
              && mapinfo_p2->dev == mapinfo_p->dev)
            {
              map.info.vaddr = mapinfo_p2->vaddr;
              err =
                  devctl (ctl_fd, DCMD_PROC_MAPDEBUG, &map, sizeof (map), 0);
              if (err != EOK)
                  continue;

              if (strcmp (map.info.path, printme.name))
                  continue;

              /* Lower debug_vaddr is always text, if nessessary, swap.  */
              if ((int) map.info.vaddr < (int) printme.text.debug_vaddr)
                {
                  memcpy (&(printme.data), &(printme.text),
                  sizeof (printme.data));
                  printme.text.addr = mapinfo_p2->vaddr;
                  printme.text.size = mapinfo_p2->size;
                  printme.text.flags = mapinfo_p2->flags;
                  printme.text.offset = mapinfo_p2->offset;
                  printme.text.debug_vaddr = map.info.vaddr;
                }
              else
                {
                  printme.data.addr = mapinfo_p2->vaddr;
                  printme.data.size = mapinfo_p2->size;
                  printme.data.flags = mapinfo_p2->flags;
                  printme.data.offset = mapinfo_p2->offset;
                  printme.data.debug_vaddr = map.info.vaddr;
                }
              mapinfo_p2->ino = 0;
            }
        }
      mapinfo_p->ino = 0;

      printf_filtered ("%s\n", printme.name);
      printf_filtered ("\ttext=%08x bytes @ 0x%08x\n", printme.text.size,
           printme.text.addr);
      printf_filtered ("\t\tflags=%08x\n", printme.text.flags);
      printf_filtered ("\t\tdebug=%08x\n", printme.text.debug_vaddr);
      printf_filtered ("\t\toffset=%s\n", phex (printme.text.offset, 8));
      if (printme.data.size)
        {
          printf_filtered ("\tdata=%08x bytes @ 0x%08x\n", printme.data.size,
             printme.data.addr);
          printf_filtered ("\t\tflags=%08x\n", printme.data.flags);
          printf_filtered ("\t\tdebug=%08x\n", printme.data.debug_vaddr);
          printf_filtered ("\t\toffset=%s\n", phex (printme.data.offset, 8));
        }
      printf_filtered ("\tdev=0x%x\n", printme.dev);
      printf_filtered ("\tino=0x%x\n", (unsigned int) printme.ino);
    }
  xfree (mapinfos);
  return;
}

/* Print status information about what we're accessing.  */
void
nto_procfs::files_info ( )
{
  struct inferior *inf = current_inferior ();

  printf_unfiltered ("\tUsing the running image of %s %s.\n",
         inf->attach_flag ? "attached" : "child",
         target_pid_to_str (inferior_ptid) );
}

/* Target to_pid_to_exec_file implementation.  */

char *
nto_procfs::pid_to_exec_file (int pid)
{
  int proc_fd;
  static char proc_path[PATH_MAX];
  ssize_t rd;

  /* Read exe file name.  */
  snprintf (proc_path, sizeof (proc_path), "/proc/%d/exefile", pid);
  proc_fd = open (proc_path, O_RDONLY);
  if (proc_fd == -1)
      return NULL;

  rd = read (proc_fd, proc_path, sizeof (proc_path) - 1);
  close1 (proc_fd);
  if (rd <= 0)
    {
      proc_path[0] = '\0';
      return NULL;
    }
  proc_path[rd] = '\0';
  return proc_path;
}

/* Attach to process PID, then initialize for debugging it.  */
void
nto_procfs::attach (const char *args, int from_tty)
{
  char *exec_file;
  int pid;
  struct inferior *inf;

  pid = parse_pid_to_attach (args);

  if (pid == getpid ())
    error (_("Attaching GDB to itself is not a good idea..."));

  if (from_tty)
    {
      exec_file = (char *) get_exec_file (0);

      if (exec_file)
          printf_unfiltered ("Attaching to program `%s', %s\n", exec_file,
              nto_pid_to_str (ptid_t (pid,1,0)));
      else
          printf_unfiltered ("Attaching to %s\n",
              nto_pid_to_str (ptid_t (pid,1,0)));

      gdb_flush (gdb_stdout);
    }
  inferior_ptid = do_attach (ptid_t (pid,1,0));
  inf = current_inferior ();
  inferior_appeared (inf, pid);
  inf->attach_flag = 1;

  update_thread_list ( );
}

/*
 * todo: is this really enough?
 */
void
nto_procfs::post_attach (int pid)
{
  if (exec_bfd)
    solib_create_inferior_hook (0);
}

static ptid_t
do_attach (ptid_t ptid)
{
  procfs_status status;
  struct sigevent event;
  char path[PATH_MAX];

  snprintf (path, PATH_MAX - 1, "/proc/%d/as", ptid.pid () );
  ctl_fd = open (path, O_RDWR);
  if (ctl_fd == -1)
      error (_("Couldn't open proc file %s, error %d (%s)"), path, errno,
          safe_strerror (errno));
  if (devctl (ctl_fd, DCMD_PROC_STOP, &status, sizeof (status), 0) != EOK)
      error (_("Couldn't stop process"));

  /* Define a sigevent for process stopped notification.  */
  event.sigev_notify = SIGEV_SIGNAL_THREAD;
  event.sigev_signo = SIGUSR1;
  event.sigev_code = 0;
  event.sigev_value.sival_ptr = NULL;
  event.sigev_priority = -1;
  devctl (ctl_fd, DCMD_PROC_EVENT, &event, sizeof (event), 0);

  if (devctl (ctl_fd, DCMD_PROC_STATUS, &status, sizeof (status), 0) == EOK
      && ( status.flags & _DEBUG_FLAG_STOPPED) )
      SignalKill ( ND_LOCAL_NODE, ptid.pid (), 0, SIGCONT, 0, 0);
  return ptid_t (ptid.pid (), status.tid, 0);
}

/* Ask the user what to do when an interrupt is received.  */
static void
interrupt_query (void)
{
  if (query (_("Interrupted while waiting for the program.\n\
Give up (and stop debugging it)? ")))
    {
      target_mourn_inferior (inferior_ptid);
      quit ();
    }
}

/* The user typed ^C twice.  */
static void
nto_handle_sigint_twice (int signo)
{
  signal (signo, ofunc);
  interrupt_query ();
  signal (signo, nto_handle_sigint_twice);
}

static void
nto_handle_sigint (int signo)
{
  /* If this doesn't work, try more severe steps.  */
  signal (signo, nto_handle_sigint_twice);

  target_interrupt ( );
}

ptid_t
nto_procfs::wait (ptid_t ptid, struct target_waitstatus *ourstatus, int options)
{
  sigset_t set;
  siginfo_t info;
  procfs_status status;
  static enum gdb_signal exit_signo = GDB_SIGNAL_0;  /* To track signals that cause termination.  */

  ourstatus->kind = TARGET_WAITKIND_SPURIOUS;

  if ( inferior_ptid == null_ptid )
    {
      ourstatus->kind = TARGET_WAITKIND_STOPPED;
      ourstatus->value.sig = GDB_SIGNAL_0;
      exit_signo = GDB_SIGNAL_0;
      return null_ptid;
    }

  sigemptyset (&set);
  sigaddset (&set, SIGUSR1);

  devctl (ctl_fd, DCMD_PROC_STATUS, &status, sizeof (status), 0);
  while (!(status.flags & _DEBUG_FLAG_ISTOP))
    {
      ofunc = signal (SIGINT, nto_handle_sigint);
      sigwaitinfo (&set, &info);
      signal (SIGINT, ofunc);
      devctl (ctl_fd, DCMD_PROC_STATUS, &status, sizeof (status), 0);
    }

  nto_inferior_data (NULL)->stopped_flags = status.flags;
  nto_inferior_data (NULL)->stopped_pc = status.ip;

  if (status.flags & _DEBUG_FLAG_SSTEP)
    {
      ourstatus->kind = TARGET_WAITKIND_STOPPED;
      ourstatus->value.sig = GDB_SIGNAL_TRAP;
    }
  /* Was it a breakpoint?  */
  else if (status.flags & _DEBUG_FLAG_TRACE)
    {
      ourstatus->kind = TARGET_WAITKIND_STOPPED;
      ourstatus->value.sig = GDB_SIGNAL_TRAP;
    }
  else if (status.flags & _DEBUG_FLAG_ISTOP)
    {
      switch (status.why)
        {
        case _DEBUG_WHY_SIGNALLED:
          ourstatus->kind = TARGET_WAITKIND_STOPPED;
          ourstatus->value.sig =
              gdb_signal_from_host (status.info.si_signo);
          exit_signo = GDB_SIGNAL_0;
        break;
        case _DEBUG_WHY_FAULTED:
          ourstatus->kind = TARGET_WAITKIND_STOPPED;
          if (status.info.si_signo == SIGTRAP)
            {
              ourstatus->value.sig = GDB_SIGNAL_0;
              exit_signo = GDB_SIGNAL_0;
            }
          else
            {
              ourstatus->value.sig =
                  gdb_signal_from_host (status.info.si_signo);
              exit_signo = ourstatus->value.sig;
            }
        break;

        case _DEBUG_WHY_TERMINATED:
          {
            int waitval = 0;

            waitpid (inferior_ptid.pid(), &waitval, WNOHANG);
            if (exit_signo)
              {
                /* Abnormal death.  */
                ourstatus->kind = TARGET_WAITKIND_SIGNALLED;
                ourstatus->value.sig = exit_signo;
              }
            else
              {
                /* Normal death.  */
                ourstatus->kind = TARGET_WAITKIND_EXITED;
                ourstatus->value.integer = WEXITSTATUS (waitval);
              }
            exit_signo = GDB_SIGNAL_0;
          }
          break;

        case _DEBUG_WHY_REQUESTED:
          /* We are assuming a requested stop is due to a SIGINT.  */
          ourstatus->kind = TARGET_WAITKIND_STOPPED;
          ourstatus->value.sig = GDB_SIGNAL_INT;
          exit_signo = GDB_SIGNAL_0;
        break;
        }
    }

  return ptid_t (status.pid, status.tid, 0);
}

/* Read the current values of the inferior's registers, both the
   general register set and floating point registers (if supported)
   and update gdb's idea of their current values.  */
void
nto_procfs::fetch_registers (struct regcache *regcache, int regno)
{
  union
  {
    procfs_greg greg;
    procfs_fpreg fpreg;
    procfs_altreg altreg;
  }
  reg;
  int regsize;

  nto_trace(0)("nto_procfs::fetch_registers (#%i)\n", regno);
  procfs_set_thread (inferior_ptid);
  if (devctl (ctl_fd, DCMD_PROC_GETGREG, &reg, sizeof (reg), &regsize) == EOK)
    {
      nto_supply_gregset (regcache, (const gdb_byte *) &reg.greg, regsize);
    }
  else
    {
      warning("Could not fetch general registers!");
    }
  if (devctl (ctl_fd, DCMD_PROC_GETFPREG, &reg, sizeof (reg), &regsize)
      == EOK)
    {
      nto_supply_fpregset (regcache, (const gdb_byte *) &reg.fpreg, regsize);
    }
  if (devctl (ctl_fd, DCMD_PROC_GETALTREG, &reg, sizeof (reg), &regsize)
      == EOK)
    {
      nto_supply_altregset (regcache, (const gdb_byte*) &reg.altreg, regsize);
    }
}

/* Helper for procfs_xfer_partial that handles memory transfers.
   Arguments are like target_xfer_partial.  */
static enum target_xfer_status
procfs_xfer_memory (gdb_byte *readbuf, const gdb_byte *writebuf,
        ULONGEST memaddr, ULONGEST len, ULONGEST *xfered_len)
{
  int nbytes;

  if (lseek (ctl_fd, (off_t) memaddr, SEEK_SET) != (off_t) memaddr)
      return TARGET_XFER_E_IO;

  if (writebuf != NULL)
      nbytes = write (ctl_fd, writebuf, len);
  else
      nbytes = read (ctl_fd, readbuf, len);
  if (nbytes <= 0)
      return TARGET_XFER_E_IO;
  *xfered_len = nbytes;
  return TARGET_XFER_OK;
}

/* Target to_xfer_partial implementation.  */
enum target_xfer_status
nto_procfs::xfer_partial ( enum target_object object,
         const char *annex, gdb_byte *readbuf,
         const gdb_byte *writebuf, ULONGEST offset, ULONGEST len,
         ULONGEST *xfered_len)
{
  switch (object)
    {
    case TARGET_OBJECT_MEMORY:
      return procfs_xfer_memory (readbuf, writebuf, offset, len, xfered_len);
    case TARGET_OBJECT_SIGNAL_INFO:
      if (readbuf != NULL)
        {
          union nto_procfs_status
          {
            debug_thread32_t _32;
            debug_thread64_t _64;
          } status;
          union nto_siginfo_t
          {
            __siginfo32_t _32;
            __siginfo64_t _64;
          } siginfo;
          const size_t sizeof_status = IS_64BIT() ? sizeof (status._64)
              : sizeof (status._32);
          const size_t sizeof_siginfo = IS_64BIT() ? sizeof (siginfo._64)
              : sizeof (siginfo._32);
          int err;
          LONGEST mylen = len;

          if ((err = devctl (ctl_fd, DCMD_PROC_STATUS, &status, sizeof_status,
              0)) != EOK)
              return TARGET_XFER_E_IO;
          if ((offset + mylen) > sizeof (siginfo))
            {
              if (offset < sizeof_siginfo)
                  mylen = sizeof (siginfo) - offset;
              else
                  return TARGET_XFER_EOF;
            }
          nto_get_siginfo_from_procfs_status (&status, &siginfo);
          memcpy (readbuf, (gdb_byte *)&siginfo + offset, mylen);
          *xfered_len = len;
          return len? TARGET_XFER_OK : TARGET_XFER_EOF;
        }
      /* Fallthru */
    case TARGET_OBJECT_AUXV:
      if (readbuf != NULL)
        {
          int err;
          CORE_ADDR initial_stack;
          debug_process_t procinfo;
          /* For 32-bit architecture, size of auxv_t is 8 bytes.  */
          const unsigned int sizeof_auxv_t = sizeof (auxv_t);
          const unsigned int sizeof_tempbuf = 20 * sizeof_auxv_t;
          int tempread;
          gdb_byte *const tempbuf = (gdb_byte *) alloca (sizeof_tempbuf);

          if (tempbuf == NULL)
              return TARGET_XFER_E_IO;

          err = devctl (ctl_fd, DCMD_PROC_INFO, &procinfo,
              sizeof procinfo, 0);
          if (err != EOK)
              return TARGET_XFER_E_IO;

          initial_stack = procinfo.initial_stack;

          /* procfs is always 'self-hosted', no byte-order manipulation.  */
          tempread = nto_read_auxv_from_initial_stack (initial_stack, tempbuf,
                   sizeof_tempbuf,
                   sizeof (auxv_t));
          tempread = ((tempread<len)?tempread:len) - offset;
          memcpy (readbuf, tempbuf + offset, tempread);
          *xfered_len = tempread;
          return tempread ? TARGET_XFER_OK : TARGET_XFER_EOF;
        }
      /* Fallthru */
    default:
      return this->beneath()->xfer_partial (object, annex, readbuf,
                  writebuf, offset, len, xfered_len);
    }
}

/* Take a program previously attached to and detaches it.
   The program resumes execution and will no longer stop
   on signals, etc.  We'd better not have left any breakpoints
   in the program or it'll die when it hits one.  */
void
nto_procfs::detach (inferior *inf, int from_tty)
{
  target_announce_detach (from_tty);

  close1 (ctl_fd);
  ctl_fd = -1;

  inferior_ptid = null_ptid;
  detach_inferior (inf);
  init_thread_list ();
  maybe_unpush_target ();
}

static int
procfs_breakpoint (CORE_ADDR addr, int type, int size)
{
  procfs_break brk;

  brk.type = type;
  brk.addr = addr;
  brk.size = size;
  errno = devctl (ctl_fd, DCMD_PROC_BREAK, &brk, sizeof (brk), 0);
  if (errno != EOK)
      return 1;
  return 0;
}

int
nto_procfs::insert_breakpoint (struct gdbarch *gdbarch,
        struct bp_target_info *bp_tgt)
{
  bp_tgt->placed_address = bp_tgt->reqstd_address;
  return procfs_breakpoint (bp_tgt->placed_address, _DEBUG_BREAK_EXEC,
          nto_breakpoint_size (bp_tgt->placed_address));
}

int
nto_procfs::remove_breakpoint (struct gdbarch *gdbarch,
        struct bp_target_info *bp_tgt,
        enum remove_bp_reason reason)
{
  return procfs_breakpoint (bp_tgt->placed_address, _DEBUG_BREAK_EXEC, -1);
}

int
nto_procfs::insert_hw_breakpoint (struct gdbarch *gdbarch,
           struct bp_target_info *bp_tgt)
{
  bp_tgt->placed_address = bp_tgt->reqstd_address;
  return procfs_breakpoint (bp_tgt->placed_address,
          _DEBUG_BREAK_EXEC | _DEBUG_BREAK_HW, 0);
}

int
nto_procfs::remove_hw_breakpoint (struct gdbarch *gdbarch,
           struct bp_target_info *bp_tgt)
{
  return procfs_breakpoint (bp_tgt->placed_address,
          _DEBUG_BREAK_EXEC | _DEBUG_BREAK_HW, -1);
}

void
nto_procfs::resume ( ptid_t ptid, int step, enum gdb_signal signo)
{
  int signal_to_pass;
  procfs_status status;
  sigset_t *run_fault = (sigset_t *) (void *) &run.fault;

  if ( inferior_ptid == null_ptid )
      return;

  procfs_set_thread ((ptid == minus_one_ptid) ? inferior_ptid :
         ptid);

  run.flags = _DEBUG_RUN_FAULT | _DEBUG_RUN_TRACE;
  if (step)
      run.flags |= _DEBUG_RUN_STEP;

  sigemptyset (run_fault);
  sigaddset (run_fault, FLTBPT);
  sigaddset (run_fault, FLTTRACE);
  sigaddset (run_fault, FLTILL);
  sigaddset (run_fault, FLTPRIV);
  sigaddset (run_fault, FLTBOUNDS);
  sigaddset (run_fault, FLTIOVF);
  sigaddset (run_fault, FLTIZDIV);
  sigaddset (run_fault, FLTFPE);
  /* Peter V will be changing this at some point.  */
  /* todo has this been done yet? */
  sigaddset (run_fault, FLTPAGE);

  run.flags |= _DEBUG_RUN_ARM;

  signal_to_pass = gdb_signal_to_host (signo);

  if (signal_to_pass)
    {
      devctl (ctl_fd, DCMD_PROC_STATUS, &status, sizeof (status), 0);
      signal_to_pass = gdb_signal_to_host (signo);
      if (status.why & (_DEBUG_WHY_SIGNALLED | _DEBUG_WHY_FAULTED))
        {
          if (signal_to_pass != status.info.si_signo)
            {
              SignalKill ( ND_LOCAL_NODE, inferior_ptid.pid(), 0,
                  signal_to_pass, 0, 0);
              run.flags |= _DEBUG_RUN_CLRFLT | _DEBUG_RUN_CLRSIG;
            }
          else    /* Let it kill the program without telling us.  */
              sigdelset (&run.trace, signal_to_pass);
        }
    }
  else
      run.flags |= _DEBUG_RUN_CLRSIG | _DEBUG_RUN_CLRFLT;

  errno = devctl (ctl_fd, DCMD_PROC_RUN, &run, sizeof (run), 0);
  if (errno != EOK)
    {
      perror (_("run error!\n"));
      return;
    }
}

void
nto_procfs::mourn_inferior ( )
{
  if (inferior_ptid != null_ptid )
    {
      SignalKill (ND_LOCAL_NODE, inferior_ptid.pid(), 0, SIGKILL, 0, 0);
      close1 (ctl_fd);
    }
  inferior_ptid = null_ptid;
  init_thread_list ();

  maybe_unpush_target ();
}

/* This function breaks up an argument string into an argument
   vector suitable for passing to execvp().
   E.g., on "run a b c d" this routine would get as input
   the string "a b c d", and as output it would fill in argv with
   the four arguments "a", "b", "c", "d".  The only additional
   functionality is simple quoting.  The gdb command:
    run a "b c d" f
   will fill in argv with the three args "a", "b c d", "e".  */
static void
breakup_args (char *scratch, char **argv)
{
  char *pp, *cp = scratch;
  char quoting = 0;

  for (;;)
    {
      /* Scan past leading separators.  */
      quoting = 0;
      while (*cp == ' ' || *cp == '\t' || *cp == '\n')
          cp++;

      /* Break if at end of string.  */
      if (*cp == '\0')
          break;

      /* Take an arg.  */
      if (*cp == '"')
        {
           cp++;
           quoting = strchr (cp, '"') ? 1 : 0;
        }

      *argv++ = cp;

      /* Scan for next arg separator.  */
      pp = cp;
      if (quoting)
          cp = strchr (pp, '"');
      if ((cp == NULL) || (!quoting))
          cp = strchr (pp, ' ');
      if (cp == NULL)
          cp = strchr (pp, '\t');
      if (cp == NULL)
          cp = strchr (pp, '\n');

      /* No separators => end of string => break.  */
      if (cp == NULL)
        {
          pp = cp;
          break;
        }

      /* Replace the separator with a terminator.  */
      *cp++ = '\0';
    }

  /* Execv requires a null-terminated arg vector.  */
  *argv = NULL;
}

static unsigned nto_get_cpuflags (void)
{
  return SYSPAGE_ENTRY (cpuinfo)->flags;
}

/*
 * todo: by default GDB fork()'s the new process internally, while we are
 *       spawn()ing it on our own.
 *       Probably it makes sense to take the default approach instead of
 *       re-inventing the wheel. Otherwise this means that using GDB through
 *       QNet will no longer work as expected.
 */
void
nto_procfs::create_inferior (const char *exec_file, const std::string &allargs,
    char **env, int from_tty)
{
  struct inheritance inherit;
  int pid;
  int flags, errn;
  char **argv;
  char *args;
  const char *in = "", *out = "", *err = "";
  int fd, fds[3];
  sigset_t set;
//  const char *inferior_io_terminal = get_inferior_io_terminal ();
  struct inferior *inf;

  /* todo: the whole argument vector handling is QUESTIONABLE! */
  args = xstrdup (allargs.c_str());

  argv = (char **) xmalloc (((strlen (args) + 1U) / 2U + 2U) *
      sizeof (*argv));
  argv[0] = get_exec_file (1);
  if (!argv[0])
    {
      if (exec_file)
          argv[0] = xstrdup(exec_file);
      else
          return;
    }

  breakup_args (args, (exec_file != NULL) ? &argv[1] : &argv[0]);

  argv = nto_parse_redirection (argv, &in, &out, &err);

  fds[0] = STDIN_FILENO;
  fds[1] = STDOUT_FILENO;
  fds[2] = STDERR_FILENO;

  /* If the user specified I/O via gdb's --tty= arg, use it, but only
     if the i/o is not also being specified via redirection.  */
/*  if (inferior_io_terminal)
    {
      if (!in[0])
  in = inferior_io_terminal;
      if (!out[0])
  out = inferior_io_terminal;
      if (!err[0])
  err = inferior_io_terminal;
    }
*/
  if (in[0])
    {
      fd = open (in, O_RDONLY);
      if (fd == -1)
          perror (in);
      else
          fds[0] = fd;
    }
  if (out[0])
    {
      fd = open (out, O_WRONLY);
      if (fd == -1)
          perror (out);
      else
          fds[1] = fd;
    }
  if (err[0])
    {
      fd = open (err, O_WRONLY);
      if (fd == -1)
          perror (err);
      else
          fds[2] = fd;
    }

  /* Clear any pending SIGUSR1's but keep the behavior the same.  */
  signal (SIGUSR1, signal (SIGUSR1, SIG_IGN));

  sigemptyset (&set);
  sigaddset (&set, SIGUSR1);
  sigprocmask (SIG_UNBLOCK, &set, NULL);

  memset (&inherit, 0, sizeof (inherit));

  inherit.flags |= SPAWN_SETGROUP | SPAWN_HOLD;
  inherit.pgroup = SPAWN_NEWPGROUP;
  pid = spawnp (argv[0], 3, fds, &inherit, argv, env );
  xfree (args);

  sigprocmask (SIG_BLOCK, &set, NULL);

  if (pid == -1)
      error (_("Error spawning %s: %d (%s)"), argv[0], errno,
          safe_strerror (errno));

  if (fds[0] != STDIN_FILENO)
      close1 (fds[0]);
  if (fds[1] != STDOUT_FILENO)
      close1 (fds[1]);
  if (fds[2] != STDERR_FILENO)
      close1 (fds[2]);

  /* todo: a lot of this should probably be done with
   * add_thread_silent (ptid_t (pid));
   * instead */

  inferior_ptid = do_attach (ptid_t (pid));

  inf = current_inferior ();
  inferior_appeared (inf, pid);
  inf->attach_flag = 0;

  update_thread_list ( );

  flags = _DEBUG_FLAG_KLC;  /* Kill-on-Last-Close flag.  */
  errn = devctl (ctl_fd, DCMD_PROC_SET_FLAG, &flags, sizeof (flags), 0);
  if (errn != EOK)
    {
      /* FIXME: expected warning?  */
      /* warning( "Failed to set Kill-on-Last-Close flag: errno = %d(%s)\n",
         errn, strerror(errn) ); */
    }

  terminal_init ();

  if (exec_bfd != NULL
      || (symfile_objfile != NULL && symfile_objfile->obfd != NULL))
      solib_create_inferior_hook (0);

  if (!target_is_pushed (this))
      push_target (this);
}

/**
 * added trace to check if each of these is really necessary
 */
void
nto_procfs::stop (ptid_t ptid)
{
  nto_trace(0)("stop()\n");
  devctl (ctl_fd, DCMD_PROC_STOP, NULL, 0, 0);
}

void
nto_procfs::interrupt ()
{
  nto_trace(0)("interrupt()\n");
  devctl (ctl_fd, DCMD_PROC_STOP, NULL, 0, 0);
}

void
nto_procfs::pass_ctrlc ()
{
  nto_trace(0)("pass_ctrlc()\n");
  devctl (ctl_fd, DCMD_PROC_STOP, NULL, 0, 0);
}

/**
 * terminates the current inferior and cleans up the data structures
 */
void
nto_procfs::kill ( )
{
  /* Use catch_errors so the user can quit from gdb even when we aren't on
     speaking terms with the remote system.  */
  mourn_inferior ( );
}

/* Fill buf with regset and return devctl cmd to do the setting.  Return
   -1 if we fail to get the regset.  Store size of regset in bufsize.  */
static int
get_regset (int regset, char *buf, int *bufsize)
{
  int dev_get, dev_set;
  switch (regset)
    {
    case NTO_REG_GENERAL:
      dev_get = DCMD_PROC_GETGREG;
      dev_set = DCMD_PROC_SETGREG;
      break;

    case NTO_REG_FLOAT:
      dev_get = DCMD_PROC_GETFPREG;
      dev_set = DCMD_PROC_SETFPREG;
      break;

    case NTO_REG_ALT:
      dev_get = DCMD_PROC_GETALTREG;
      dev_set = DCMD_PROC_SETALTREG;
      break;

    case NTO_REG_SYSTEM:
    default:
      return -1;
    }
  if (devctl (ctl_fd, dev_get, buf, *bufsize, bufsize) != EOK)
      return -1;

  return dev_set;
}

void
nto_procfs::store_registers (struct regcache *regcache, int regno)
{
  union
  {
    procfs_greg greg;
    procfs_fpreg fpreg;
    procfs_altreg altreg;
  } reg;
  unsigned off;
  int len, regsize, err, dev_set, regset;
  char *data;

  if ( inferior_ptid == null_ptid )
      return;
  procfs_set_thread (inferior_ptid);

  for (regset = NTO_REG_GENERAL; regset < NTO_REG_END; regset++)
    {
      regsize = sizeof (reg);
      dev_set = get_regset (regset, (char *) &reg, &regsize);
      if (dev_set == -1)
          continue;

      if (nto_regset_fill (regcache, regset, (gdb_byte *) &reg, regsize) == -1)
          continue;

      err = devctl (ctl_fd, dev_set, &reg, sizeof (reg), 0);
      if (err != EOK)
          fprintf_unfiltered (gdb_stderr,
              "Warning unable to write regset %d: %s\n",
              regno, safe_strerror (err));
    }
}

/* Set list of signals to be handled in the target.  */

void
nto_procfs::pass_signals (int numsigs, unsigned char *pass_signals)
{
  int signo;

  sigfillset (&run.trace);

  for (signo = 1; signo < NSIG; signo++)
    {
      int target_signo = gdb_signal_from_host (signo);
      if (target_signo < numsigs && pass_signals[target_signo])
        sigdelset (&run.trace, signo);
    }
}

const struct target_desc *
nto_procfs::read_description ( )
{
  if (ntoops_read_description)
      return ntoops_read_description (nto_get_cpuflags ());
  else
    {
      warning("Target description unavailable!");
      return NULL;
    }
}

/* Create the "native" and "procfs" targets.  */

#define OSTYPE_NTO 1

extern initialize_file_ftype _initialize_procfs;

void
_initialize_procfs (void)
{
  sigset_t set;

  /* We use SIGUSR1 to gain control after we block waiting for a process.
     We use sigwaitevent to wait.  */
  sigemptyset (&set);
  sigaddset (&set, SIGUSR1);
  sigprocmask (SIG_BLOCK, &set, NULL);

  /* Initially, make sure all signals are reported.  */
  sigfillset (&run.trace);

  add_info ("pidlist", procfs_pidlist, _("pidlist"));
  add_info ("meminfo", procfs_meminfo, _("memory information"));

  add_inf_child_target (&nto_procfs_target);
}

static int
procfs_hw_watchpoint (int addr, int len, enum target_hw_bp_type type)
{
  procfs_break brk;

  switch (type)
    {
    case hw_read:
      brk.type = _DEBUG_BREAK_RD;
      break;
    case hw_access:
      brk.type = _DEBUG_BREAK_RW;
      break;
    default:      /* Modify.  */
/* FIXME: brk.type = _DEBUG_BREAK_RWM gives EINVAL for some reason.  */
      brk.type = _DEBUG_BREAK_RW;
    }
  brk.type |= _DEBUG_BREAK_HW;  /* Always ask for HW.  */
  brk.addr = addr;
  brk.size = len;

  errno = devctl (ctl_fd, DCMD_PROC_BREAK, &brk, sizeof (brk), 0);
  if (errno != EOK)
    {
      perror (_("Failed to set hardware watchpoint"));
      return -1;
    }
  return 0;
}

int
nto_procfs::can_use_hw_breakpoint (enum bptype type,
            int cnt, int othertype)
{
  return 1;
}

int
nto_procfs::remove_watchpoint (CORE_ADDR addr, int len,
           enum target_hw_bp_type type,
           struct expression *cond)
{
  return procfs_hw_watchpoint (addr, -1, type);
}

int
nto_procfs::insert_watchpoint (CORE_ADDR addr, int len,
           enum target_hw_bp_type type,
           struct expression *cond)
{
  return procfs_hw_watchpoint (addr, len, type);
}
