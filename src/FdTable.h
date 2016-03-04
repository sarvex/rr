/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#ifndef RR_FD_TABLE_H_
#define RR_FD_TABLE_H_

#include <unordered_map>
#include <memory>

#include "AddressSpace.h"
#include "FileMonitor.h"

class TraceTaskEvent;

class FdTable : public HasTaskSet {
public:
  typedef std::shared_ptr<FdTable> shr_ptr;

  void add_monitor(int fd, FileMonitor* monitor);
  bool allow_close(int fd);
  Switchable will_write(Task* t, int fd);
  void did_write(Task* t, int fd,
                 const std::vector<FileMonitor::Range>& ranges);

  void did_dup(int from, int to);
  void did_close(int fd);

  shr_ptr clone(Task* t) {
    shr_ptr fds(new FdTable(*this));
    fds->insert_task(t);
    return fds;
  }
  static shr_ptr create(Task* t) {
    shr_ptr fds(new FdTable());
    fds->insert_task(t);
    return fds;
  }

  bool is_monitoring(int fd) { return fds.count(fd) > 0; }

  /**
   * Regenerate syscallbuf_fds_disabled in task |t|.
   * Called during initialization of the preload library.
   */
  void init_syscallbuf_fds_disabled(Task* t);

  /**
   * Called after task |t| for this FdTable has execed. Update for any fds
   * that were closed via CLOEXEC.
   * Rather than tracking CLOEXEC flags (which would be complicated), we just
   * scan /proc/<pid>/fd during recording and note any monitored fds that have
   * been closed, and record these in the TraceTaskEvent.
   */
  void update_for_cloexec(Task* t, TraceTaskEvent& event);

private:
  FdTable() {}
  FdTable(const FdTable& other) : fds(other.fds) {}

  void update_syscallbuf_fds_disabled(int fd);

  std::unordered_map<int, FileMonitor::shr_ptr> fds;
};

#endif /* RR_FD_TABLE_H_ */
