/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

//#define DEBUGTAG "Trace"

#include "TraceStream.h"

#include <inttypes.h>
#include <limits.h>
#include <sysexits.h>

#include <fstream>
#include <string>
#include <sstream>

#include "log.h"
#include "util.h"

using namespace std;

//
// This represents the format and layout of recorded traces.  This
// version number doesn't track the rr version number, because changes
// to the trace format will be rare.
//
// NB: if you *do* change the trace format for whatever reason, you
// MUST increment this version number.  Otherwise users' old traces
// will become unreplayable and they won't know why.
//
#define TRACE_VERSION 41

struct SubstreamData {
  const char* name;
  size_t block_size;
  int threads;
};

static const SubstreamData substreams[TraceStream::SUBSTREAM_COUNT] = {
  { "events", 1024 * 1024, 1 },
  { "data_header", 1024 * 1024, 1 },
  { "data", 8 * 1024 * 1024, 3 },
  { "mmaps", 64 * 1024, 1 },
  { "tasks", 64 * 1024, 1 }
};

static const SubstreamData& substream(TraceStream::Substream s) {
  return substreams[s];
}

static TraceStream::Substream operator++(TraceStream::Substream& s) {
  s = (TraceStream::Substream)(s + 1);
  return s;
}

static bool dir_exists(const string& dir) {
  struct stat dummy;
  return !dir.empty() && stat(dir.c_str(), &dummy) == 0;
}

static string default_rr_trace_dir() {
  static string cached_dir;

  if (!cached_dir.empty()) {
    return cached_dir;
  }

  string dot_dir;
  const char* home = getenv("HOME");
  if (home) {
    dot_dir = string(home) + "/.rr";
  }
  string xdg_dir;
  const char* xdg_data_home = getenv("XDG_DATA_HOME");
  if (xdg_data_home) {
    xdg_dir = string(xdg_data_home) + "/rr";
  } else if (home) {
    xdg_dir = string(home) + "/.local/share/rr";
  }

  // If XDG dir does not exist but ~/.rr does, prefer ~/.rr for backwards
  // compatibility.
  if (dir_exists(xdg_dir)) {
    cached_dir = xdg_dir;
  } else if (dir_exists(dot_dir)) {
    cached_dir = dot_dir;
  } else if (!xdg_dir.empty()) {
    cached_dir = xdg_dir;
  } else {
    cached_dir = "/tmp/rr";
  }

  return cached_dir;
}

static string trace_save_dir() {
  const char* output_dir = getenv("_RR_TRACE_DIR");
  return output_dir ? output_dir : default_rr_trace_dir();
}

static string latest_trace_symlink() {
  return trace_save_dir() + "/latest-trace";
}

static void ensure_dir(const string& dir, mode_t mode) {
  string d = dir;
  while (!d.empty() && d[d.length() - 1] == '/') {
    d = d.substr(0, d.length() - 1);
  }

  struct stat st;
  if (0 > stat(d.c_str(), &st)) {
    if (errno != ENOENT) {
      FATAL() << "Error accessing trace directory `" << dir << "'";
    }

    size_t last_slash = d.find_last_of('/');
    if (last_slash == string::npos || last_slash == 0) {
      FATAL() << "Can't find trace directory `" << dir << "'";
    }
    ensure_dir(d.substr(0, last_slash), mode);

    // Allow for a race condition where someone else creates the directory
    if (0 > mkdir(d.c_str(), mode) && errno != EEXIST) {
      FATAL() << "Can't create trace directory `" << dir << "'";
    }
    if (0 > stat(d.c_str(), &st)) {
      FATAL() << "Can't stat trace directory `" << dir << "'";
    }
  }

  if (!(S_IFDIR & st.st_mode)) {
    FATAL() << "`" << dir << "' exists but isn't a directory.";
  }
  if (access(d.c_str(), W_OK)) {
    FATAL() << "Can't write to `" << dir << "'.";
  }
}

/**
 * Create the default ~/.rr directory if it doesn't already exist.
 */
static void ensure_default_rr_trace_dir() {
  ensure_dir(default_rr_trace_dir(), S_IRWXU);
}

string TraceStream::path(Substream s) {
  return trace_dir + "/" + substream(s).name;
}

bool TraceWriter::good() const {
  for (auto& w : writers) {
    if (!w->good()) {
      return false;
    }
  }
  return true;
}

bool TraceReader::good() const {
  for (auto& r : readers) {
    if (!r->good()) {
      return false;
    }
  }
  return true;
}

struct BasicInfo {
  TraceFrame::Time global_time;
  pid_t tid_;
  EncodedEvent ev;
  Ticks ticks_;
  double monotonic_sec;
};

void TraceWriter::write_frame(const TraceFrame& frame) {
  auto& events = writer(EVENTS);

  BasicInfo basic_info = { frame.time(), frame.tid(), frame.event().encode(),
                           frame.ticks(), frame.monotonic_time() };
  events << basic_info;
  if (!events.good()) {
    FATAL() << "Tried to save " << sizeof(basic_info)
            << " bytes to the trace, but failed";
  }
  // TODO: only store exec info for non-async-sig events when
  // debugging assertions are enabled.
  if (frame.event().has_exec_info() == HAS_EXEC_INFO) {
    events << frame.regs() << frame.extra_perf_values();
    if (!events.good()) {
      FATAL() << "Tried to save registers to the trace, but failed";
    }

    int extra_reg_bytes = frame.extra_regs().data_size();
    char extra_reg_format = (char)frame.extra_regs().format();
    events << extra_reg_format << extra_reg_bytes;
    if (!events.good()) {
      FATAL() << "Tried to save "
              << sizeof(extra_reg_bytes) + sizeof(extra_reg_format)
              << " bytes to the trace, but failed";
    }
    if (extra_reg_bytes > 0) {
      events.write((const char*)frame.extra_regs().data_bytes(),
                   extra_reg_bytes);
      if (!events.good()) {
        FATAL() << "Tried to save " << extra_reg_bytes
                << " bytes to the trace, but failed";
      }
    }
  }
  if (frame.event().is_signal_event()) {
    events << frame.event().Signal().signal_data();
  }

  tick_time();
}

TraceFrame TraceReader::read_frame() {
  // Read the common event info first, to see if we also have
  // exec info to read.
  auto& events = reader(EVENTS);
  BasicInfo basic_info;
  events >> basic_info;
  TraceFrame frame(basic_info.global_time, basic_info.tid_,
                   Event(basic_info.ev), basic_info.ticks_,
                   basic_info.monotonic_sec);
  if (frame.event().has_exec_info() == HAS_EXEC_INFO) {
    events >> frame.recorded_regs >> frame.extra_perf;

    int extra_reg_bytes;
    char extra_reg_format;
    events >> extra_reg_format >> extra_reg_bytes;
    if (extra_reg_bytes > 0) {
      vector<uint8_t> data;
      data.resize(extra_reg_bytes);
      events.read((char*)data.data(), extra_reg_bytes);
      frame.recorded_extra_regs.set_arch(frame.event().arch());
      frame.recorded_extra_regs.set_to_raw_data(
          (ExtraRegisters::Format)extra_reg_format, data);
    } else {
      assert(extra_reg_format == ExtraRegisters::NONE);
      frame.recorded_extra_regs = ExtraRegisters(frame.event().arch());
    }
  }
  if (frame.event().is_signal_event()) {
    uint64_t signal_data;
    events >> signal_data;
    frame.ev.Signal().set_signal_data(signal_data);
  }

  tick_time();
  assert(time() == frame.time());
  return frame;
}

void TraceWriter::write_task_event(const TraceTaskEvent& event) {
  auto& tasks = writer(TASKS);
  tasks << event.type() << event.tid();
  switch (event.type()) {
    case TraceTaskEvent::CLONE:
      tasks << event.parent_tid() << event.clone_flags();
      break;
    case TraceTaskEvent::FORK:
      tasks << event.parent_tid();
      break;
    case TraceTaskEvent::EXEC:
      tasks << event.file_name() << event.cmd_line() << event.fds_to_close();
      break;
    case TraceTaskEvent::EXIT:
      break;
    case TraceTaskEvent::NONE:
      assert(0 && "Writing NONE TraceTaskEvent");
      break;
  }
}

TraceTaskEvent TraceReader::read_task_event() {
  auto& tasks = reader(TASKS);
  TraceTaskEvent r;
  tasks >> r.type_ >> r.tid_;
  switch (r.type()) {
    case TraceTaskEvent::CLONE:
      tasks >> r.parent_tid_ >> r.clone_flags_;
      break;
    case TraceTaskEvent::FORK:
      tasks >> r.parent_tid_;
      break;
    case TraceTaskEvent::EXEC:
      tasks >> r.file_name_ >> r.cmd_line_ >> r.fds_to_close_;
      break;
    case TraceTaskEvent::EXIT:
      break;
    case TraceTaskEvent::NONE:
      // Should be EOF only
      assert(!tasks.good());
      break;
  }
  return r;
}

string TraceWriter::try_hardlink_file(const string& file_name) {
  char count_str[20];
  sprintf(count_str, "%d", mmap_count);

  size_t last_slash = file_name.rfind('/');
  string basename = (last_slash != file_name.npos)
                        ? file_name.substr(last_slash + 1)
                        : file_name;
  string link_path = dir() + "/mmap_" + count_str + "_hardlink_" + basename;
  int ret = link(file_name.c_str(), link_path.c_str());
  if (ret < 0) {
    // maybe tried to link across filesystems?
    return file_name;
  }
  return link_path;
}

TraceWriter::RecordInTrace TraceWriter::write_mapped_region(
    const KernelMapping& km, const struct stat& stat, MappingOrigin origin) {
  auto& mmaps = writer(MMAPS);
  TraceReader::MappedDataSource source;
  string backing_file_name;
  if (km.fsname().find("/SYSV") == 0) {
    source = TraceReader::SOURCE_TRACE;
  } else if (origin == SYSCALL_MAPPING &&
             (km.inode() == 0 || km.fsname() == "/dev/zero (deleted)")) {
    source = TraceReader::SOURCE_ZERO;
  } else if (should_copy_mmap_region(km, stat) &&
             files_assumed_immutable.find(make_pair(
                 stat.st_dev, stat.st_ino)) == files_assumed_immutable.end()) {
    source = TraceReader::SOURCE_TRACE;
  } else {
    source = TraceReader::SOURCE_FILE;
    // Try hardlinking file into the trace directory. This will avoid
    // replay failures if the original file is deleted or replaced (but not
    // if it is overwritten in-place). If try_hardlink_file fails it
    // just returns the original file name.
    // A relative backing_file_name is relative to the trace directory.
    backing_file_name = try_hardlink_file(km.fsname());
    files_assumed_immutable.insert(make_pair(stat.st_dev, stat.st_ino));
  }
  mmaps << global_time << source << km.start() << km.end() << km.fsname()
        << km.device() << km.inode() << km.prot() << km.flags()
        << km.file_offset_bytes() << backing_file_name << (uint32_t)stat.st_mode
        << (uint32_t)stat.st_uid << (uint32_t)stat.st_gid
        << (int64_t)stat.st_size << (int64_t)stat.st_mtime;
  ++mmap_count;
  return source == TraceReader::SOURCE_TRACE ? RECORD_IN_TRACE
                                             : DONT_RECORD_IN_TRACE;
}

KernelMapping TraceReader::read_mapped_region(MappedData* data, bool* found) {
  if (found) {
    *found = false;
  }

  auto& mmaps = reader(MMAPS);
  if (mmaps.at_end()) {
    return KernelMapping();
  }

  mmaps.save_state();
  TraceFrame::Time time;
  mmaps >> time;
  mmaps.restore_state();
  if (time != global_time) {
    return KernelMapping();
  }

  string original_file_name;
  string backing_file_name;
  remote_ptr<void> start, end;
  dev_t device;
  ino_t inode;
  int prot, flags;
  uint32_t uid, gid, mode;
  uint64_t file_offset_bytes;
  int64_t mtime, file_size;
  mmaps >> time >> data->source >> start >> end >> original_file_name >>
      device >> inode >> prot >> flags >> file_offset_bytes >>
      backing_file_name >> mode >> uid >> gid >> file_size >> mtime;
  assert(time == global_time);
  if (data->source == SOURCE_FILE) {
    if (backing_file_name[0] != '/') {
      backing_file_name = dir() + "/" + backing_file_name;
    }
    struct stat backing_stat;
    if (stat(backing_file_name.c_str(), &backing_stat)) {
      FATAL() << "Failed to stat " << backing_file_name
              << ": replay is impossible";
    }
    if (backing_stat.st_ino != inode || backing_stat.st_mode != mode ||
        backing_stat.st_uid != uid || backing_stat.st_gid != gid ||
        backing_stat.st_size != file_size || backing_stat.st_mtime != mtime) {
      LOG(error)
          << "Metadata of " << original_file_name
          << " changed: replay divergence likely, but continuing anyway ...";
    }
  }
  data->file_name = backing_file_name;
  data->file_data_offset_bytes = file_offset_bytes;
  data->file_size_bytes = file_size;
  if (found) {
    *found = true;
  }
  return KernelMapping(start, end, original_file_name, device, inode, prot,
                       flags, file_offset_bytes);
}

static ostream& operator<<(ostream& out, const vector<string>& vs) {
  out << vs.size() << endl;
  for (auto& v : vs) {
    out << v << '\0';
  }
  return out;
}

static istream& operator>>(istream& in, vector<string>& vs) {
  size_t len;
  in >> len;
  in.ignore(1);
  for (size_t i = 0; i < len; ++i) {
    char buf[PATH_MAX];
    in.getline(buf, sizeof(buf), '\0');
    vs.push_back(buf);
  }
  return in;
}

void TraceWriter::write_raw(const void* d, size_t len, remote_ptr<void> addr) {
  auto& data = writer(RAW_DATA);
  auto& data_header = writer(RAW_DATA_HEADER);
  data_header << global_time << addr.as_int() << len;
  data.write(d, len);
}

TraceReader::RawData TraceReader::read_raw_data() {
  auto& data = reader(RAW_DATA);
  auto& data_header = reader(RAW_DATA_HEADER);
  TraceFrame::Time time;
  RawData d;
  size_t num_bytes;
  data_header >> time >> d.addr >> num_bytes;
  assert(time == global_time);
  d.data.resize(num_bytes);
  data.read((char*)d.data.data(), num_bytes);
  return d;
}

bool TraceReader::read_raw_data_for_frame(const TraceFrame& frame, RawData& d) {
  auto& data_header = reader(RAW_DATA_HEADER);
  if (data_header.at_end()) {
    return false;
  }
  TraceFrame::Time time;
  data_header.save_state();
  data_header >> time;
  data_header.restore_state();
  assert(time >= frame.time());
  if (time > frame.time()) {
    return false;
  }
  d = read_raw_data();
  return true;
}

void TraceWriter::close() {
  for (auto& w : writers) {
    w->close();
  }
}

static string make_trace_dir(const string& exe_path) {
  ensure_default_rr_trace_dir();

  // Find a unique trace directory name.
  int nonce = 0;
  int ret;
  string dir;
  do {
    stringstream ss;
    ss << trace_save_dir() << "/" << basename(exe_path.c_str()) << "-"
       << nonce++;
    dir = ss.str();
    ret = mkdir(dir.c_str(), S_IRWXU | S_IRWXG);
  } while (ret && EEXIST == errno);

  if (ret) {
    FATAL() << "Unable to create trace directory `" << dir << "'";
  }

  return dir;
}

TraceWriter::TraceWriter(const vector<string>& argv, const vector<string>& envp,
                         const string& cwd, int bind_to_cpu)
    : TraceStream(make_trace_dir(argv[0]),
                  // Somewhat arbitrarily start the
                  // global time from 1.
                  1),
      mmap_count(0) {
  this->argv = argv;
  this->envp = envp;
  this->cwd = cwd;
  this->bind_to_cpu = bind_to_cpu;

  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    writers[s] = unique_ptr<CompressedWriter>(new CompressedWriter(
        path(s), substream(s).block_size, substream(s).threads));
  }

  string ver_path = version_path();
  fstream version(ver_path.c_str(), fstream::out);
  if (!version.good()) {
    FATAL() << "Unable to create " << ver_path;
  }
  version << TRACE_VERSION << endl;

  if (!probably_not_interactive(STDOUT_FILENO)) {
    printf("rr: Saving the execution of `%s' to trace directory `%s'.\n",
           argv[0].c_str(), trace_dir.c_str());
  }

  ofstream out(args_env_path());
  out << cwd << '\0';
  out << argv;
  out << envp;
  out << bind_to_cpu;
  assert(out.good());
}

void TraceWriter::make_latest_trace() {
  string link_name = latest_trace_symlink();
  // Try to update the symlink to |this|.  We only try attempt
  // to set the symlink once.  If the link is re-created after
  // we |unlink()| it, then another rr process is racing with us
  // and it "won".  The link is then valid and points at some
  // very-recent trace, so that's good enough.
  unlink(link_name.c_str());
  int ret = symlink(trace_dir.c_str(), link_name.c_str());
  if (ret < 0 && errno != EEXIST) {
    FATAL() << "Failed to update symlink `" << link_name << "' to `"
            << trace_dir << "'.";
  }
}

TraceFrame TraceReader::peek_frame() {
  auto& events = reader(EVENTS);
  events.save_state();
  auto saved_time = global_time;
  TraceFrame frame;
  if (!at_end()) {
    frame = read_frame();
  }
  events.restore_state();
  global_time = saved_time;
  return frame;
}

TraceFrame TraceReader::peek_to(pid_t pid, EventType type, SyscallState state) {
  auto& events = reader(EVENTS);
  TraceFrame frame;
  events.save_state();
  auto saved_time = global_time;
  while (good() && !at_end()) {
    frame = read_frame();
    if (frame.tid() == pid && frame.event().type() == type &&
        (!frame.event().is_syscall_event() ||
         frame.event().Syscall().state == state)) {
      events.restore_state();
      global_time = saved_time;
      return frame;
    }
  }
  FATAL() << "Unable to find requested frame in stream";
  // Unreachable
  return frame;
}

void TraceReader::rewind() {
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    reader(s).rewind();
  }
  global_time = 0;
  assert(good());
}

TraceReader::TraceReader(const string& dir)
    : TraceStream(dir.empty() ? latest_trace_symlink() : dir,
                  // Initialize the global time at 0, so
                  // that when we tick it when reading
                  // the first trace, it matches the
                  // initial global time at recording, 1.
                  0) {
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    readers[s] = unique_ptr<CompressedReader>(new CompressedReader(path(s)));
  }

  string path = version_path();
  fstream vfile(path.c_str(), fstream::in);
  if (!vfile.good()) {
    fprintf(
        stderr,
        "\n"
        "rr: error: Version file for recorded trace `%s' not found.  Did you "
        "record\n"
        "           `%s' with an older version of rr?  If so, you'll need to "
        "replay\n"
        "           `%s' with that older version.  Otherwise, your trace is\n"
        "           likely corrupted.\n"
        "\n",
        path.c_str(), path.c_str(), path.c_str());
    exit(EX_DATAERR);
  }
  int version = 0;
  vfile >> version;
  if (vfile.fail() || TRACE_VERSION != version) {
    fprintf(stderr, "\n"
                    "rr: error: Recorded trace `%s' has an incompatible "
                    "version %d; expected\n"
                    "           %d.  Did you record `%s' with an older version "
                    "of rr?  If so,\n"
                    "           you'll need to replay `%s' with that older "
                    "version.  Otherwise,\n"
                    "           your trace is likely corrupted.\n"
                    "\n",
            path.c_str(), version, TRACE_VERSION, path.c_str(), path.c_str());
    exit(EX_DATAERR);
  }

  ifstream in(args_env_path());
  assert(in.good());
  char buf[PATH_MAX];
  in.getline(buf, sizeof(buf), '\0');
  cwd = buf;
  in >> argv;
  in >> envp;
  in >> bind_to_cpu;
}

/**
 * Create a copy of this stream that has exactly the same
 * state as 'other', but for which mutations of this
 * clone won't affect the state of 'other' (and vice versa).
 */
TraceReader::TraceReader(const TraceReader& other)
    : TraceStream(other.dir(), other.time()) {
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    readers[s] =
        unique_ptr<CompressedReader>(new CompressedReader(other.reader(s)));
  }

  argv = other.argv;
  envp = other.envp;
  cwd = other.cwd;
  bind_to_cpu = other.bind_to_cpu;
}

uint64_t TraceReader::uncompressed_bytes() const {
  uint64_t total = 0;
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    total += reader(s).uncompressed_bytes();
  }
  return total;
}

uint64_t TraceReader::compressed_bytes() const {
  uint64_t total = 0;
  for (Substream s = SUBSTREAM_FIRST; s < SUBSTREAM_COUNT; ++s) {
    total += reader(s).compressed_bytes();
  }
  return total;
}
