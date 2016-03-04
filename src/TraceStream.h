/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#ifndef RR_TRACE_H_
#define RR_TRACE_H_

#include <unistd.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "CompressedReader.h"
#include "CompressedWriter.h"
#include "Event.h"
#include "remote_ptr.h"
#include "TraceFrame.h"
#include "TraceTaskEvent.h"

class KernelMapping;

/**
 * TraceStream stores all the data common to both recording and
 * replay.  TraceWriter deals with recording-specific logic, and
 * TraceReader handles replay-specific details.
 *
 * These classes are all in the same .h/.cc file to keep trace reading and
 * writing code together for easier coordination.
 */
class TraceStream {
protected:
  typedef std::string string;

public:
  /**
   * Update |substreams| and TRACE_VERSION when you update this list.
   */
  enum Substream {
    SUBSTREAM_FIRST,
    // Substream that stores events (trace frames).
    EVENTS = SUBSTREAM_FIRST,
    // Substreams that store raw data saved from tracees (|RAW_DATA|), and
    // metadata about the stored data (|RAW_DATA_HEADER|).
    RAW_DATA_HEADER,
    RAW_DATA,
    // Substream that stores metadata about files mmap'd during
    // recording.
    MMAPS,
    // Substream that stores task creation and exec events
    TASKS,
    SUBSTREAM_COUNT
  };

  /** Return the directory storing this trace's files. */
  const string& dir() const { return trace_dir; }

  const string& initial_exe() const { return argv[0]; }
  const std::vector<string>& initial_argv() const { return argv; }
  const std::vector<string>& initial_envp() const { return envp; }
  const string& initial_cwd() const { return cwd; }
  int bound_to_cpu() const { return bind_to_cpu; }

  /**
   * Return the current "global time" (event count) for this
   * trace.
   */
  TraceFrame::Time time() const { return global_time; }

protected:
  TraceStream(const string& trace_dir, TraceFrame::Time initial_time)
      : trace_dir(trace_dir), global_time(initial_time) {}

  /**
   * Return the path of the file for the given substream.
   */
  string path(Substream s);

  /**
   * Return the path of the "args_env" file, into which the
   * initial tracee argv and envp are recorded.
   */
  string args_env_path() const { return trace_dir + "/args_env"; }
  /**
   * Return the path of "version" file, into which the current
   * trace format version of rr is stored upon creation of the
   * trace.
   */
  string version_path() const { return trace_dir + "/version"; }

  /**
   * Increment the global time and return the incremented value.
   */
  void tick_time() { ++global_time; }

  // Directory into which we're saving the trace files.
  string trace_dir;
  // The initial argv and envp for a tracee.
  std::vector<string> argv;
  std::vector<string> envp;
  // Current working directory at start of record/replay.
  string cwd;
  // CPU core# that the tracees are bound to
  int bind_to_cpu;

  // Arbitrary notion of trace time, ticked on the recording of
  // each event (trace frame).
  TraceFrame::Time global_time;
};

class TraceWriter : public TraceStream {
public:
  /**
   * Write trace frame to the trace.
   *
   * Recording a trace frame has the side effect of ticking
   * the global time.
   */
  void write_frame(const TraceFrame& frame);

  enum RecordInTrace { DONT_RECORD_IN_TRACE, RECORD_IN_TRACE };
  enum MappingOrigin { SYSCALL_MAPPING, EXEC_MAPPING, PATCH_MAPPING };
  /**
   * Write mapped-region record to the trace.
   * If this returns RECORD_IN_TRACE, then the data for the map should be
   * recorded in the trace raw-data.
   */
  RecordInTrace write_mapped_region(const KernelMapping& map,
                                    const struct stat& stat,
                                    MappingOrigin origin = SYSCALL_MAPPING);

  /**
   * Write a raw-data record to the trace.
   * 'addr' is the address in the tracee where the data came from/will be
   * restored to.
   */
  void write_raw(const void* data, size_t len, remote_ptr<void> addr);

  /**
   * Write a task event (clone or exec record) to the trace.
   */
  void write_task_event(const TraceTaskEvent& event);

  /**
   * Return true iff all trace files are "good".
   */
  bool good() const;

  /** Call close() on all the relevant trace files.
   *  Normally this will be called by the destructor. It's helpful to
   *  call this before a crash that won't call the destructor, to ensure
   *  buffered data is flushed.
   */
  void close();

  /**
   * Create a trace that will record the initial exe
   * image |argv[0]| with initial args |argv|, initial environment |envp|,
   * current working directory |cwd| and bound to cpu |bind_to_cpu|. This
   * data is recored in the trace.
   * The trace name is determined by the global rr args and environment.
   */
  TraceWriter(const std::vector<std::string>& argv,
              const std::vector<std::string>& envp, const string& cwd,
              int bind_to_cpu);

  /**
   * We got far enough into recording that we should set this as the latest
   * trace.
   */
  void make_latest_trace();

private:
  std::string try_hardlink_file(const std::string& file_name);

  CompressedWriter& writer(Substream s) { return *writers[s]; }
  const CompressedWriter& writer(Substream s) const { return *writers[s]; }

  std::unique_ptr<CompressedWriter> writers[SUBSTREAM_COUNT];
  /**
   * Files that have already been mapped without being copied to the trace,
   * i.e. that we have already assumed to be immutable.
   */
  std::set<std::pair<dev_t, ino_t> > files_assumed_immutable;
  uint32_t mmap_count;
};

class TraceReader : public TraceStream {
public:
  /**
   * A parcel of recorded tracee data.  |data| contains the data read
   * from |addr| in the tracee.
   */
  struct RawData {
    std::vector<uint8_t> data;
    remote_ptr<void> addr;
  };

  /**
   * Read relevant data from the trace.
   *
   * NB: reading a trace frame has the side effect of ticking
   * the global time to match the time recorded in the trace
   * frame.
   */
  TraceFrame read_frame();

  enum MappedDataSource { SOURCE_TRACE, SOURCE_FILE, SOURCE_ZERO };
  /**
   * Where to obtain data for the mapped region.
   */
  struct MappedData {
    MappedDataSource source;
    /** Name of file to map the data from. */
    string file_name;
    /** Data offset within the file. */
    uint64_t file_data_offset_bytes;
    /** Original size of mapped file. */
    uint64_t file_size_bytes;
  };
  /**
   * Read the next mapped region descriptor and return it.
   * Also returns where to get the mapped data in 'data'.
   * If |found| is non-null, set *found to indicate whether a descriptor
   * was found for the current event.
   */
  KernelMapping read_mapped_region(MappedData* data, bool* found = nullptr);

  /**
   * Peek at the next mapping. Returns an empty region if there isn't one for
   * the current event.
   */
  KernelMapping peek_mapped_region();

  /**
   * Read a task event (clone or exec record) from the trace.
   * Returns a record of type NONE at the end of the trace.
   */
  TraceTaskEvent read_task_event();

  /**
   * Read the next raw data record and return it.
   */
  RawData read_raw_data();

  /**
   * Reads the next raw data record for 'frame' from the current point in
   * the trace. If there are no more raw data records for 'frame', returns
   * false.
   */
  bool read_raw_data_for_frame(const TraceFrame& frame, RawData& d);

  /**
   * Return true iff all trace files are "good".
   * for more details.
   */
  bool good() const;

  /**
   * Return true if we're at the end of the trace file.
   */
  bool at_end() const { return reader(EVENTS).at_end(); }

  /**
   * Return the next trace frame, without mutating any stream
   * state.
   */
  TraceFrame peek_frame();

  /**
   * Peek ahead in the stream to find the next trace frame that
   * matches the requested parameters. Returns the frame if one
   * was found, and issues a fatal error if not.
   */
  TraceFrame peek_to(pid_t pid, EventType type, SyscallState state);

  /**
   * Restore the state of this to what it was just after
   * |open()|.
   */
  void rewind();

  uint64_t uncompressed_bytes() const;
  uint64_t compressed_bytes() const;

  /**
   * Open the trace in 'dir'. When 'dir' is the empty string, open the
   * latest trace.
   */
  TraceReader(const string& dir);

  /**
   * Create a copy of this stream that has exactly the same
   * state as 'other', but for which mutations of this
   * clone won't affect the state of 'other' (and vice versa).
   */
  TraceReader(const TraceReader& other);

private:
  CompressedReader& reader(Substream s) { return *readers[s]; }
  const CompressedReader& reader(Substream s) const { return *readers[s]; }

  std::unique_ptr<CompressedReader> readers[SUBSTREAM_COUNT];
};

#endif /* RR_TRACE_H_ */
