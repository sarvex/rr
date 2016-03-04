/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#ifndef RR_REC_SCHED_H_
#define RR_REC_SCHED_H_

#include <deque>
#include <set>

#include "Ticks.h"
#include "TraceFrame.h"
#include "util.h"

class RecordSession;
class Task;

/**
 * Overview of rr scheduling:
 *
 * rr honours priorities set by setpriority(2) --- even in situations where the
 * kernel doesn't, e.g. when a non-privileged task tries to increase its
 * priority. Normally rr honors priorities strictly by scheduling the highest
 * priority runnable task; tasks with equal priorities are scheduled in
 * round-robin fashion. Strict priority scheduling helps find bugs due to
 * starvation.
 *
 * When a task calls sched_yield we temporarily switch to a completely
 * fair scheduler that ignores priorities. All tasks are placed on a queue
 * and while the queue is non-empty we take the next task from the queue and
 * run it for a quantum if it's runnable. We do this because tasks calling
 * sched_yield are often expecting some kind of fair scheduling and may deadlock
 * (e.g. trying to acquire a spinlock) if some other tasks don't get a chance
 * to run.
 *
 * The scheduler only runs during recording. During replay we're just replaying
 * the recorded scheduling decisions.
 *
 * The main interface to the scheduler is |get_next_thread|. This gets called
 * after every rr event to decide which task to run next.
 *
 * The scheduler gives the current task a 'timeslice', a ticks deadline after
 * which we will try to switch to another task. So |get_next_thread| first
 * checks whether the currently running task has exceeded that deadline. If
 * not, and the current task is runnable, we schedule it again. If it's blocked
 * or has exceeded its deadline, we search for another task to run:
 * taking tasks from the round-robin queue until we find one that's runnable,
 * and then if the round-robin queue is empty, choosing the highest-priority
 * task that's runnable. If the highest-priority runnable task has the same
 * priority as the current task, choose the next runnable task after the
 * current task (so equal priority tasks run in round-robin order).
 *
 * The main parameter to the scheduler is |max_ticks|, which controls the
 * length of each timeslice.
 */
class Scheduler {
public:
  /**
   * Like most task schedulers, there are conflicting goals to balance. Lower
   * max-ticks generally makes the application more "interactive", generally
   * speaking lower latency. (And wrt catching bugs, this setting generally
   * creates more opportunity for bugs to arise in multi-threaded/process
   * applications.) This comes at the cost of more overhead from scheduling and
   * context switching. Context switches during recording are expensive because
   * we must context switch to the rr process and then to the next tracee task.
   * Increasing max-ticks generally gives the application higher throughput.
   *
   * Using ticks (retired conditional branches) to compute timeslices is quite
   * crude, since they don't correspond to any unit of time in general.
   * Hopefully that can be improved, but empirical data from Firefox
   * demonstrate, surprisingly consistently, a distribution of insns/rcb massed
   * around 10. Somewhat arbitrarily guessing ~4cycles/insn on average
   * (fair amount of pointer chasing), that implies for a nominal 2GHz CPU
   * 50,000 ticks per millisecond. We choose the default max ticks to give us
   * 10ms timeslices, i.e. 500,000 ticks.
   */
  enum { DEFAULT_MAX_TICKS = 500000 };

  Scheduler(RecordSession& session);

  void set_max_ticks(Ticks max_ticks) { max_ticks_ = max_ticks; }
  void set_always_switch(bool always_switch) {
    this->always_switch = always_switch;
  }
  void set_enable_chaos(bool enable_chaos);

  /**
   * Schedule a new runnable task (which may be the same as current()).
   *
   * The new current() task is guaranteed to either have already been
   * runnable, or have been made runnable by a waitpid status change (in
   * which case, by_waitpid be true.
   *
   * Returns false if rescheduling is interrupted by a signal.
   */
  bool reschedule(Switchable switchable, bool* by_waitpid);

  /**
   * Set the priority of |t| to |value| and update related
   * state.
   */
  void update_task_priority(Task* t, int value);

  /**
   * Do one round of round-robin scheduling if we're not already doing one.
   * If we start round-robin scheduling now, make last_task the last
   * task to be scheduled.
   * If the task_round_robin_queue is empty this moves all tasks into it,
   * putting last_task last.
   */
  void schedule_one_round_robin(Task* last_task);

  void on_create(Task* t);
  /**
   * De-register a thread. This function should be called when a thread exits.
   */
  void on_destroy(Task* t);

  Task* current() const { return current_; }

  Ticks current_timeslice_end() const { return current_timeslice_end_; }

  void expire_timeslice() { current_timeslice_end_ = 0; }

  double interrupt_after_elapsed_time() const;

  /**
   * Return the number of cores we should report to applications.
   */
  int pretend_num_cores() const { return pretend_num_cores_; }

private:
  // Tasks sorted by priority.
  typedef std::set<std::pair<int, Task*> > TaskPrioritySet;
  typedef std::deque<Task*> TaskQueue;

  /**
   * Pull a task from the round-robin queue if available. Otherwise,
   * find the highest-priority task that is runnable. If the highest-priority
   * runnable task has the same priority as 't', return 't' or
   * the next runnable task after 't' in round-robin order.
   * Sets 'by_waitpid' to true if we determined the task was runnable by
   * calling waitpid on it and observing a state change. This task *must*
   * be returned by get_next_thread, and is_runnable_task must not be called
   * on it again until it has run.
   * Considers only tasks with priority <= priority_threshold.
   */
  Task* find_next_runnable_task(Task* t, bool* by_waitpid,
                                int priority_threshold);
  /**
   * Returns the first task in the round-robin queue or null if it's empty,
   * removing it from the round-robin queue.
   */
  Task* get_round_robin_task();
  void maybe_pop_round_robin_task(Task* t);
  Task* get_next_task_with_same_priority(Task* t);
  void setup_new_timeslice();
  void maybe_reset_priorities(double now);
  int choose_random_priority(Task* t);
  void update_task_priority_internal(Task* t, int value);
  void maybe_reset_high_priority_only_intervals(double now);
  bool in_high_priority_only_interval(double now);
  bool treat_as_high_priority(Task* t);
  bool is_task_runnable(Task* t, bool* by_waitpid);

  RecordSession& session;

  /**
   * Every task of this session is either in task_priority_set
   * (when in_round_robin_queue is false), or in task_round_robin_queue
   * (when in_round_robin_queue is true).
   *
   * task_priority_set is a set of pairs of (task->priority, task). This
   * lets us efficiently iterate over the tasks with a given priority, or
   * all tasks in priority order.
   */
  TaskPrioritySet task_priority_set;
  TaskQueue task_round_robin_queue;

  /**
   * The currently scheduled task. This may be nullptr if the last scheduled
   * task
   * has been destroyed.
   */
  Task* current_;
  Ticks current_timeslice_end_;

  /**
   * At this time (or later) we should refresh these values.
   */
  double high_priority_only_intervals_refresh_time;
  double high_priority_only_intervals_start;
  double high_priority_only_intervals_duration;
  double high_priority_only_intervals_period;
  /**
   * At this time (or later) we should rerandomize Task priorities.
   */
  double priorities_refresh_time;

  int pretend_num_cores_;

  Ticks max_ticks_;

  /**
   * When true, context switch at every possible point.
   */
  bool always_switch;
  /**
   * When true, make random scheduling decisions to try to increase the
   * probability of finding buggy schedules.
   */
  bool enable_chaos;

  bool last_reschedule_in_high_priority_only_interval;

  Task* must_run_task;
};

#endif /* RR_REC_SCHED_H_ */
