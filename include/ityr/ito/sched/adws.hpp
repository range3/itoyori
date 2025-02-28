#pragma once

#include "ityr/common/util.hpp"
#include "ityr/common/mpi_util.hpp"
#include "ityr/common/mpi_rma.hpp"
#include "ityr/common/topology.hpp"
#include "ityr/common/logger.hpp"
#include "ityr/common/allocator.hpp"
#include "ityr/common/profiler.hpp"
#include "ityr/ito/util.hpp"
#include "ityr/ito/options.hpp"
#include "ityr/ito/context.hpp"
#include "ityr/ito/callstack.hpp"
#include "ityr/ito/wsqueue.hpp"
#include "ityr/ito/prof_events.hpp"
#include "ityr/ito/sched/util.hpp"

namespace ityr::ito {

class flipper {
public:
  using value_type = uint64_t;

  value_type value() const { return val_; }

  void flip(int at) {
    ITYR_CHECK(0 <= at);
    ITYR_CHECK(at < sizeof(value_type) * 8);

    val_ ^= (value_type(1) << at);
  }

  bool match(flipper f, int until) const {
    ITYR_CHECK(0 <= until);
    ITYR_CHECK(until < sizeof(value_type) * 8);

    value_type mask = (value_type(1) << (until + 1)) - 1;
    return (val_ & mask) == (f.value() & mask);
  }

private:
  value_type val_ = 0;
};

class dist_range {
public:
  using value_type = double;

  dist_range() {}
  dist_range(common::topology::rank_t n_ranks)
    : begin_(0), end_(static_cast<value_type>(n_ranks)) {}
  dist_range(value_type begin, value_type end)
    : begin_(begin), end_(end) {}

  value_type begin() const { return begin_; }
  value_type end() const { return end_; }

  common::topology::rank_t begin_rank() const {
    return static_cast<common::topology::rank_t>(begin_);
  }

  common::topology::rank_t end_rank() const {
    return static_cast<common::topology::rank_t>(end_);
  }

  bool is_at_end_boundary() const {
    return static_cast<value_type>(static_cast<common::topology::rank_t>(end_)) == end_;
  }

  void move_to_end_boundary() {
    end_ = static_cast<value_type>(static_cast<common::topology::rank_t>(end_));
  }

  template <typename T>
  std::pair<dist_range, dist_range> divide(T r1, T r2) const {
    value_type at = begin_ + (end_ - begin_) * r1 / (r1 + r2);

    // Boundary condition for tasks at the very bottom of the task hierarchy.
    // A task with range [P, P) such that P = #workers would be assigned to worker P,
    // but worker P does not exist; thus we need to assign the task to worker P-1.
    if (at == end_) {
      constexpr value_type eps = 0.00001;
      at -= eps;
      if (at < begin_) at = begin_;
    }

    return std::make_pair(dist_range{begin_, at}, dist_range{at, end_});
  }

  common::topology::rank_t owner() const {
    return static_cast<common::topology::rank_t>(begin_);
  }

  bool is_cross_worker() const {
    return static_cast<common::topology::rank_t>(begin_) != static_cast<common::topology::rank_t>(end_);
  }

  void make_non_cross_worker() {
    end_ = begin_;
  }

  bool is_sufficiently_small() const {
    return (end_ - begin_) < adws_min_drange_size_option::value();
  }

private:
  value_type begin_;
  value_type end_;
};

class dist_tree {
  using version_t = int;

public:
  struct node_ref {
    common::topology::rank_t owner_rank = -1;
    int                      depth      = -1;
  };

  struct node {
    node() {}

    int depth() const { return parent.depth + 1; }

    node_ref   parent;
    dist_range drange;
    flipper    tg_version;
    version_t  version = 0;
  };

  dist_tree(int max_depth)
    : max_depth_(max_depth),
      node_win_(common::topology::mpicomm(), max_depth_),
      dominant_flag_win_(common::topology::mpicomm(), max_depth_, 0),
      versions_(max_depth_, common::topology::my_rank() + 1) {}

  node_ref append(node_ref parent, dist_range drange, flipper tg_version) {
    int depth = parent.depth + 1;

    // handle version overflow
    auto n_ranks = common::topology::n_ranks();
    if (versions_[depth] >= std::numeric_limits<version_t>::max() - n_ranks) {
      versions_[depth] = common::topology::my_rank() + 1;
    }

    node& new_node = local_node(depth);
    new_node.parent     = parent;
    new_node.drange     = drange;
    new_node.tg_version = tg_version;
    new_node.version    = (versions_[depth] += n_ranks);

    return {common::topology::my_rank(), depth};
  }

  void set_dominant(node_ref nr, bool dominant) {
    // Store the version as the flag if dominant
    // To disable steals from this dist range, set -version as the special dominant flag value
    version_t value = (dominant ? 1 : -1) * local_node(nr.depth).version;

    local_dominant_flag(nr.depth).store(value, std::memory_order_relaxed);

    if (nr.owner_rank != common::topology::my_rank()) {
      std::size_t disp_dominant = nr.depth * sizeof(version_t);
      common::mpi_atomic_put_value(value, nr.owner_rank, disp_dominant, dominant_flag_win_.win());
    }
  }

  // The meaning of a dominant flag value:
  //          0 : undetermined
  //    version : the node with this "version" is dominant
  //   -version : the node with this "version" is removed and non-dominant
  std::optional<node> get_topmost_dominant(node_ref nr) {
    if (nr.depth < 0) return std::nullopt;

    ITYR_PROFILER_RECORD(prof_event_sched_adws_scan_tree);

    for (int d = 0; d <= nr.depth; d++) {
      auto owner_rank = (d == nr.depth) ? nr.owner_rank
                                        : local_node(d + 1).parent.owner_rank;

      node& n = local_node(d);
      auto& dominant_flag = local_dominant_flag(d);

      ITYR_CHECK(n.parent.depth == d - 1);
      ITYR_CHECK(n.version != 0);

      if (owner_rank != common::topology::my_rank() &&
          dominant_flag.load(std::memory_order_relaxed) != -n.version) {
        // To avoid network contention on the owner rank, we randomly choose a worker within the
        // distribution range to query the dominant flag (decentralized dominant node propagation)
        ITYR_CHECK(owner_rank == n.drange.begin_rank());
        auto target_rank = get_random_rank(owner_rank, n.drange.end_rank() - 1);

        if (target_rank != owner_rank &&
            dominant_flag.load(std::memory_order_relaxed) == n.version) {
          // If the remote value is 0, propagate the dominant flag to remote
          std::size_t disp_dominant = d * sizeof(version_t);
          version_t dominant_val = common::mpi_atomic_cas_value(n.version, 0,
              target_rank, disp_dominant, dominant_flag_win_.win());

          if (dominant_val == -n.version) {
            dominant_flag.store(dominant_val, std::memory_order_relaxed);
          }
        } else {
          // Read the remote dominant flag
          std::size_t disp_dominant = d * sizeof(version_t);
          version_t dominant_val = common::mpi_atomic_get_value<version_t>(
              target_rank, disp_dominant, dominant_flag_win_.win());

          if (dominant_val == n.version || dominant_val == -n.version) {
            dominant_flag.store(dominant_val, std::memory_order_relaxed);
          }
        }
      }

      if (dominant_flag.load(std::memory_order_relaxed) == n.version) {
        // return the topmost dominant node
        return n;
      }
    }

    return std::nullopt;
  }

  void copy_parents(node_ref nr) {
    for (int d = 0; d <= nr.depth; d++) {
      // non-owners write 0 as a non-dominant flag
      local_dominant_flag(d).store(0, std::memory_order_relaxed);
    }
    common::mpi_get(&local_node(0), nr.depth + 1, nr.owner_rank, 0, node_win_.win());
  }

  node& get_local_node(node_ref nr) {
    ITYR_CHECK(nr.owner_rank == common::topology::my_rank());
    return local_node(nr.depth);
  }

private:
  node& local_node(int depth) {
    ITYR_CHECK(0 <= depth);
    ITYR_CHECK(depth < max_depth_);
    return node_win_.local_buf()[depth];
  }

  std::atomic<version_t>& local_dominant_flag(int depth) {
    ITYR_CHECK(0 <= depth);
    ITYR_CHECK(depth < max_depth_);
    return dominant_flag_win_.local_buf()[depth];
  }

  int                                             max_depth_;
  common::mpi_win_manager<node>                   node_win_;
  common::mpi_win_manager<std::atomic<version_t>> dominant_flag_win_;
  std::vector<version_t>                          versions_;
};

class scheduler_adws {
public:
  struct suspended_state {
    void*       evacuation_ptr;
    void*       frame_base;
    std::size_t frame_size;
  };

  template <typename T>
  struct thread_retval {
    T            value;
    dag_profiler dag_prof;
  };

  template <typename T>
  struct thread_state {
    thread_retval<T> retval;
    int              resume_flag = 0;
    suspended_state  suspended;
  };

  template <typename T>
  struct thread_handler {
    thread_state<T>* state      = nullptr;
    bool             serialized = false;
    thread_retval<T> retval_ser; // return the result by value if the thread is serialized
  };

  struct thread_local_storage {
    dist_range          drange;         // distribution range of this thread
    dist_tree::node_ref dtree_node_ref; // distribution tree node of the cross-worker task group that this thread belongs to
    flipper             tg_version;
    bool                undistributed;
    dag_profiler        dag_prof;
  };

  struct task_group_data {
    dist_range   drange;
    bool         owns_dtree_node;
    dag_profiler dag_prof; // to record the dag prof data of this thread prior to this task group
  };

  scheduler_adws()
    : max_depth_(adws_max_depth_option::value()),
      stack_(stack_size_option::value()),
      primary_wsq_(adws_wsqueue_capacity_option::value(), max_depth_),
      migration_wsq_(adws_wsqueue_capacity_option::value(), max_depth_),
      thread_state_allocator_(thread_state_allocator_size_option::value()),
      suspended_thread_allocator_(suspended_thread_allocator_size_option::value()),
      dtree_(max_depth_) {}

  template <typename T, typename SchedLoopCallback, typename Fn, typename... Args>
  T root_exec(SchedLoopCallback&& cb, Fn&& fn, Args&&... args) {
    common::profiler::switch_phase<prof_phase_spmd, prof_phase_sched_fork>();

    thread_state<T>* ts = new (thread_state_allocator_.allocate(sizeof(thread_state<T>))) thread_state<T>;

    suspend([&, ts](context_frame* cf) {
      sched_cf_ = cf;
      root_on_stack([&, ts, fn, args...]() {
        common::verbose("Starting root thread %p", ts);

        dist_range root_drange {common::topology::n_ranks()};
        tls_ = new (alloca(sizeof(thread_local_storage)))
               thread_local_storage{.drange         = root_drange,
                                    .dtree_node_ref = {},
                                    .tg_version     = {},
                                    .undistributed  = true,
                                    .dag_prof       = {}};

        tls_->dag_prof.start();
        tls_->dag_prof.increment_thread_count();
        tls_->dag_prof.increment_strand_count();

        common::profiler::switch_phase<prof_phase_sched_fork, prof_phase_thread>();

        T ret = invoke_fn<T>(fn, args...);

        common::profiler::switch_phase<prof_phase_thread, prof_phase_sched_die>();
        common::verbose("Root thread %p is completed", ts);

        tls_->dag_prof.stop();

        on_root_die(ts, ret);
      });
    });

    sched_loop(std::forward<SchedLoopCallback>(cb),
               [=]() { return ts->resume_flag >= 1; });

    common::profiler::switch_phase<prof_phase_sched_loop, prof_phase_sched_join>();

    thread_retval<T> retval = ts->retval;
    std::destroy_at(ts);
    thread_state_allocator_.deallocate(ts, sizeof(thread_state<T>));

    if (dag_prof_enabled_) {
      dag_prof_result_ = retval.dag_prof;
    }

    common::profiler::switch_phase<prof_phase_sched_join, prof_phase_spmd>();

    return retval.value;
  }

  task_group_data task_group_begin() {
    tls_->dag_prof.stop();

    task_group_data tgdata {.drange          = tls_->drange,
                            .owns_dtree_node = false,
                            .dag_prof        = tls_->dag_prof};

    if (tls_->drange.is_cross_worker()) {
      if (tls_->dtree_node_ref.depth + 1 < max_depth_) {
        tls_->dtree_node_ref = dtree_.append(tls_->dtree_node_ref, tls_->drange, tls_->tg_version);
        dtree_local_bottom_ref_ = tls_->dtree_node_ref;
        tgdata.owns_dtree_node = true;
      }

      tls_->undistributed = true;

      common::verbose("Begin a cross-worker task group of distribution range [%f, %f) at depth %d",
                      tls_->drange.begin(), tls_->drange.end(), tls_->dtree_node_ref.depth);
    }

    tls_->dag_prof.clear();
    tls_->dag_prof.start();
    tls_->dag_prof.increment_strand_count();

    return tgdata;
  }

  template <typename PreSuspendCallback, typename PostSuspendCallback>
  void task_group_end(task_group_data&      tgdata,
                      PreSuspendCallback&&  pre_suspend_cb,
                      PostSuspendCallback&& post_suspend_cb) {
    // Just in case no threads are spawned in this task group
    on_task_die();

    // restore the original distribution range of this thread at the beginning of the task group
    tls_->drange = tgdata.drange;

    if (tls_->drange.is_cross_worker()) {
      common::verbose("End a cross-worker task group of distribution range [%f, %f) at depth %d",
                      tls_->drange.begin(), tls_->drange.end(), tls_->dtree_node_ref.depth);

      // migrate the cross-worker-task to the owner
      auto target_rank = tls_->drange.owner();
      if (target_rank != common::topology::my_rank()) {
        auto cb_ret = call_cb<prof_phase_thread, prof_phase_sched_migrate,
                              prof_phase_cb_pre_suspend>(std::forward<PreSuspendCallback>(pre_suspend_cb));

        suspend([&](context_frame* cf) {
          suspended_state ss = evacuate(cf);

          common::verbose("Migrate continuation of cross-worker-task [%f, %f) to process %d",
                          tls_->drange.begin(), tls_->drange.end(), target_rank);

          cross_worker_mailbox_.put({ss.evacuation_ptr, ss.frame_base, ss.frame_size}, target_rank);

          evacuate_all();
          common::profiler::switch_phase<prof_phase_sched_migrate, prof_phase_sched_loop>();
          resume_sched();
        });

        call_cb<prof_phase_sched_resume_migrate, prof_phase_thread,
                prof_phase_cb_post_suspend>(std::forward<PostSuspendCallback>(post_suspend_cb), cb_ret);
      }

      if (tgdata.owns_dtree_node) {
        // Set the completed current task group as non-dominant to reduce steal requests
        dtree_.set_dominant(tls_->dtree_node_ref, false);

        // Set the parent dist_tree node to the current thread
        auto& dtree_node = dtree_.get_local_node(tls_->dtree_node_ref);
        tls_->dtree_node_ref = dtree_node.parent;
        dtree_local_bottom_ref_ = tls_->dtree_node_ref;

        // Flip the next version of the task group at this depth
        tls_->tg_version.flip(dtree_node.depth());
      }

      tls_->undistributed = false;
    }

    tls_->dag_prof.merge_serial(tgdata.dag_prof);
    tls_->dag_prof.start();
    tls_->dag_prof.increment_strand_count();
  }

  template <typename T, typename OnDriftForkCallback, typename OnDriftDieCallback,
            typename WorkHint, typename Fn, typename... Args>
  void fork(thread_handler<T>& th,
            OnDriftForkCallback&& on_drift_fork_cb, OnDriftDieCallback&& on_drift_die_cb,
            WorkHint w_new, WorkHint w_rest, Fn&& fn, Args&&... args) {
    common::profiler::switch_phase<prof_phase_thread, prof_phase_sched_fork>();

    auto my_rank = common::topology::my_rank();

    thread_state<T>* ts = new (thread_state_allocator_.allocate(sizeof(thread_state<T>))) thread_state<T>;
    th.state = ts;
    th.serialized = false;

    dist_range new_drange;
    common::topology::rank_t target_rank;
    if (tls_->drange.is_cross_worker()) {
      // Avoid too fine-grained task migration
      if (tls_->drange.is_sufficiently_small()) {
        tls_->drange.move_to_end_boundary();
      }

      auto [dr_rest, dr_new] = tls_->drange.divide(w_rest, w_new);

      common::verbose("Distribution range [%f, %f) is divided into [%f, %f) and [%f, %f)",
                      tls_->drange.begin(), tls_->drange.end(),
                      dr_rest.begin(), dr_rest.end(), dr_new.begin(), dr_new.end());

      tls_->drange = dr_rest;
      new_drange = dr_new;
      target_rank = dr_new.owner();

    } else {
      // quick path for non-cross-worker tasks (without dividing the distribution range)
      new_drange = tls_->drange;
      // Since this task may have been stolen by workers outside of this task group,
      // the target rank should be itself.
      target_rank = my_rank;
    }

    if (target_rank == my_rank) {
      /* Put the continuation into the local queue and execute the new task (work-first) */

      suspend([&, ts, fn, args...](context_frame* cf) mutable {
        common::verbose<3>("push context frame [%p, %p) into task queue", cf, cf->parent_frame);

        tls_ = new (alloca(sizeof(thread_local_storage)))
               thread_local_storage{.drange         = new_drange,
                                    .dtree_node_ref = tls_->dtree_node_ref,
                                    .tg_version     = tls_->tg_version,
                                    .undistributed  = true,
                                    .dag_prof       = {}};

        std::size_t cf_size = reinterpret_cast<uintptr_t>(cf->parent_frame) - reinterpret_cast<uintptr_t>(cf);

        if (use_primary_wsq_) {
          primary_wsq_.push({nullptr, cf, cf_size, tls_->tg_version},
                            tls_->dtree_node_ref.depth);
        } else {
          migration_wsq_.push({true, nullptr, cf, cf_size, tls_->tg_version},
                              tls_->dtree_node_ref.depth);
        }

        tls_->dag_prof.start();
        tls_->dag_prof.increment_thread_count();
        tls_->dag_prof.increment_strand_count();

        common::verbose<3>("Starting new thread %p", ts);
        common::profiler::switch_phase<prof_phase_sched_fork, prof_phase_thread>();

        T ret = invoke_fn<T>(fn, args...);

        common::profiler::switch_phase<prof_phase_thread, prof_phase_sched_die>();
        common::verbose<3>("Thread %p is completed", ts);

        on_task_die();
        on_die_workfirst(ts, ret, std::forward<OnDriftDieCallback>(on_drift_die_cb));

        common::verbose<3>("Thread %p is serialized (fast path)", ts);

        // The following is executed only when the thread is serialized
        std::destroy_at(ts);
        thread_state_allocator_.deallocate(ts, sizeof(thread_state<T>));
        th.state      = nullptr;
        th.serialized = true;
        th.retval_ser = {ret, tls_->dag_prof};

        common::verbose<3>("Resume parent context frame [%p, %p) (fast path)", cf, cf->parent_frame);

        common::profiler::switch_phase<prof_phase_sched_die, prof_phase_sched_resume_popped>();
      });

      // reload my_rank because this thread might have been migrated
      if (target_rank == common::topology::my_rank()) {
        common::profiler::switch_phase<prof_phase_sched_resume_popped, prof_phase_thread>();
      } else {
        call_cb<prof_phase_sched_resume_stolen, prof_phase_thread,
                prof_phase_cb_drift_fork>(std::forward<OnDriftForkCallback>(on_drift_fork_cb));
      }

    } else {
      /* Pass the new task to another worker and execute the continuation */

      auto new_task_fn = [&, my_rank, ts, new_drange,
                          dtree_node_ref = tls_->dtree_node_ref,
                          tg_version = tls_->tg_version,
                          on_drift_fork_cb, on_drift_die_cb, fn, args...]() mutable {
        common::verbose("Starting a migrated thread %p [%f, %f)",
                        ts, new_drange.begin(), new_drange.end());

        tls_ = new (alloca(sizeof(thread_local_storage)))
               thread_local_storage{.drange         = new_drange,
                                    .dtree_node_ref = dtree_node_ref,
                                    .tg_version     = tg_version,
                                    .undistributed  = true,
                                    .dag_prof       = {}};

        if (new_drange.is_cross_worker()) {
          dtree_.copy_parents(dtree_node_ref);
          dtree_local_bottom_ref_ = dtree_node_ref;
        }

        tls_->dag_prof.start();
        tls_->dag_prof.increment_thread_count();
        tls_->dag_prof.increment_strand_count();

        // If the new task is executed on another process
        if (my_rank != common::topology::my_rank()) {
          call_cb<prof_phase_sched_start_new, prof_phase_thread,
                  prof_phase_cb_drift_fork>(std::forward<OnDriftForkCallback>(on_drift_fork_cb));
        } else {
          common::profiler::switch_phase<prof_phase_sched_start_new, prof_phase_thread>();
        }

        T ret = invoke_fn<T>(fn, args...);

        common::profiler::switch_phase<prof_phase_thread, prof_phase_sched_die>();
        common::verbose("A migrated thread %p [%f, %f) is completed",
                        ts, new_drange.begin(), new_drange.end());

        on_task_die();
        on_die_drifted(ts, ret, on_drift_die_cb);
      };

      size_t task_size = sizeof(callable_task<decltype(new_task_fn)>);
      void* task_ptr = suspended_thread_allocator_.allocate(task_size);

      auto t = new (task_ptr) callable_task(new_task_fn);

      if (new_drange.is_cross_worker()) {
        common::verbose("Migrate cross-worker-task %p [%f, %f) to process %d",
                        ts, new_drange.begin(), new_drange.end(), target_rank);

        cross_worker_mailbox_.put({nullptr, t, task_size}, target_rank);
      } else {
        common::verbose("Migrate non-cross-worker-task %p [%f, %f) to process %d",
                        ts, new_drange.begin(), new_drange.end(), target_rank);

        migration_wsq_.pass({false, nullptr, t, task_size, tls_->tg_version},
                            target_rank, tls_->dtree_node_ref.depth);
      }

      common::profiler::switch_phase<prof_phase_sched_fork, prof_phase_thread>();
    }

    // restart to count only the last task in the task group
    tls_->dag_prof.clear();
    tls_->dag_prof.start();
    tls_->dag_prof.increment_strand_count();
  }

  template <typename T>
  T join(thread_handler<T>& th) {
    common::profiler::switch_phase<prof_phase_thread, prof_phase_sched_join>();

    // Note that this point is also considered the end of the last task of a task group
    // (the last task of a task group may not be spawned as a thread)
    on_task_die();

    thread_retval<T> retval;
    if (th.serialized) {
      common::verbose<3>("Skip join for serialized thread (fast path)");
      // We can skip deallocaton for its thread state because it has been already deallocated
      // when the thread is serialized (i.e., at a fork)
      retval = th.retval_ser;

    } else {
      ITYR_CHECK(th.state != nullptr);
      thread_state<T>* ts = th.state;

      if (remote_get_value(thread_state_allocator_, &ts->resume_flag) >= 1) {
        common::verbose("Thread %p is already joined", ts);
        if constexpr (!std::is_same_v<T, no_retval_t> || dag_profiler::enabled) {
          retval = remote_get_value(thread_state_allocator_, &ts->retval);
        }

      } else {
        bool migrated = true;
        suspend([&, ts](context_frame* cf) {
          suspended_state ss = evacuate(cf);

          remote_put_value(thread_state_allocator_, ss, &ts->suspended);

          // race
          if (remote_faa_value(thread_state_allocator_, 1, &ts->resume_flag) == 0) {
            common::verbose("Win the join race for thread %p (joining thread)", ts);
            evacuate_all();
            common::profiler::switch_phase<prof_phase_sched_join, prof_phase_sched_loop>();
            resume_sched();

          } else {
            common::verbose("Lose the join race for thread %p (joining thread)", ts);
            suspended_thread_allocator_.deallocate(ss.evacuation_ptr, ss.frame_size);
            migrated = false;
          }
        });

        common::verbose("Resume continuation of join for thread %p", ts);

        if (migrated) {
          common::profiler::switch_phase<prof_phase_sched_resume_join, prof_phase_sched_join>();
        }

        if constexpr (!std::is_same_v<T, no_retval_t> || dag_profiler::enabled) {
          retval = remote_get_value(thread_state_allocator_, &ts->retval);
        }
      }

      std::destroy_at(ts);
      thread_state_allocator_.deallocate(ts, sizeof(thread_state<T>));
      th.state = nullptr;
    }

    tls_->dag_prof.merge_parallel(retval.dag_prof);

    common::profiler::switch_phase<prof_phase_sched_join, prof_phase_thread>();
    return retval.value;
  }

  template <typename SchedLoopCallback, typename CondFn>
  void sched_loop(SchedLoopCallback&& cb, CondFn&& cond_fn) {
    common::verbose("Enter scheduling loop");

    while (!should_exit_sched_loop(std::forward<CondFn>(cond_fn))) {
      auto cwt = cross_worker_mailbox_.pop();
      if (cwt.has_value()) {
        execute_cross_worker_task(*cwt);
        continue;
      }

      auto pwe = pop_from_primary_queues(primary_wsq_.n_queues() - 1);
      if (pwe.has_value()) {
        common::profiler::switch_phase<prof_phase_sched_loop, prof_phase_sched_resume_popped>();

        // No on-stack thread can exist while the scheduler thread is running
        ITYR_CHECK(pwe->evacuation_ptr);
        suspend([&](context_frame* cf) {
          sched_cf_ = cf;
          resume(suspended_state{pwe->evacuation_ptr, pwe->frame_base, pwe->frame_size});
        });
        continue;
      }

      auto mwe = pop_from_migration_queues();
      if (mwe.has_value()) {
        use_primary_wsq_ = false;
        execute_migrated_task(*mwe);
        use_primary_wsq_ = true;
        continue;
      }

      if (adws_enable_steal_option::value()) {
        steal();
      }

      if constexpr (!std::is_null_pointer_v<std::remove_reference_t<SchedLoopCallback>>) {
        cb();
      }
    }

    dtree_local_bottom_ref_ = {};

    common::verbose("Exit scheduling loop");
  }

  template <typename PreSuspendCallback, typename PostSuspendCallback>
  void poll(PreSuspendCallback&&  pre_suspend_cb,
            PostSuspendCallback&& post_suspend_cb) {
    check_cross_worker_task_arrival<prof_phase_thread, prof_phase_thread>(
        std::forward<PreSuspendCallback>(pre_suspend_cb),
        std::forward<PostSuspendCallback>(post_suspend_cb));
  }

  template <typename Fn, typename... Args>
  auto coll_exec(Fn&& fn, Args&&... args) {
    using retval_t = std::invoke_result_t<Fn, Args...>;

    auto begin_rank = common::topology::my_rank();
    std::conditional_t<std::is_void_v<retval_t>, no_retval_t, retval_t> retv;

    auto coll_task_fn = [&, fn, args...] {
      if constexpr (std::is_void_v<retval_t>) {
        fn(args...);
      } else {
        auto&& ret = fn(args...);
        if (common::topology::my_rank() == begin_rank) {
          retv = std::forward<decltype(ret)>(ret);
        }
      }
    };

    size_t task_size = sizeof(callable_task<decltype(coll_task_fn)>);
    void* task_ptr = suspended_thread_allocator_.allocate(task_size);

    auto t = new (task_ptr) callable_task(coll_task_fn);

    coll_task ct {.task_ptr = task_ptr, .task_size = task_size, .begin_rank = begin_rank};
    execute_coll_task(t, ct);

    suspended_thread_allocator_.deallocate(t, task_size);

    if constexpr (!std::is_void_v<retval_t>) {
      return retv;
    }
  }

  bool is_executing_root() const {
    return cf_top_ && cf_top_ == stack_top();
  }

  template <typename T>
  static bool is_serialized(thread_handler<T> th) {
    return th.serialized;
  }

  void dag_prof_begin() { dag_prof_enabled_ = true; }
  void dag_prof_end() { dag_prof_enabled_ = false; }

  void dag_prof_print() const {
    if (common::topology::my_rank() == 0) {
      dag_prof_result_.print();
    }
  }

private:
  struct coll_task {
    void*                    task_ptr;
    std::size_t              task_size;
    common::topology::rank_t begin_rank;
  };

  struct cross_worker_task {
    void*       evacuation_ptr;
    void*       frame_base;
    std::size_t frame_size;
  };

  struct primary_wsq_entry {
    void*       evacuation_ptr;
    void*       frame_base;
    std::size_t frame_size;
    flipper     tg_version;
  };

  struct migration_wsq_entry {
    bool        is_continuation;
    void*       evacuation_ptr;
    void*       frame_base;
    std::size_t frame_size;
    flipper     tg_version;
  };

  void on_task_die() {
    if (!tls_->dag_prof.is_stopped()) {
      tls_->dag_prof.stop();
    }

    // TODO: handle corner cases where cross-worker tasks finish without distributing
    // child cross-worker tasks to their owners
    if (tls_->drange.is_cross_worker()) {
      // Set the parent cross-worker task group as "dominant" task group, which allows for
      // work stealing within the range of workers within the task group.
      common::verbose("Distribution tree node (owner=%d, depth=%d) becomes dominant",
                      tls_->dtree_node_ref.owner_rank, tls_->dtree_node_ref.depth);

      dtree_.set_dominant(tls_->dtree_node_ref, true);

      if (tls_->undistributed &&
          tls_->drange.begin_rank() + 1 < tls_->drange.end_rank()) {
        std::vector<std::pair<cross_worker_task, common::topology::rank_t>> tasks;

        // If a cross-worker task with range [i.xxx, j.xxx) is completed without distributing
        // child cross-worker tasks to workers i+1, i+2, ..., j-1, it should pass the dist node
        // tree reference to them, so that they can perform work stealing.
        for (common::topology::rank_t target_rank = tls_->drange.begin_rank() + 1;
             target_rank < tls_->drange.end_rank();
             target_rank++) {
          // Create a dummy task to set the parent dtree nodes
          // TODO: we can reduce communication as only dtree_node_ref needs to be passed
          auto new_task_fn = [&, dtree_node_ref = tls_->dtree_node_ref]() {
            dtree_.copy_parents(dtree_node_ref);
            dtree_local_bottom_ref_ = dtree_node_ref;

            common::profiler::switch_phase<prof_phase_sched_start_new, prof_phase_sched_loop>();
            resume_sched();
          };

          size_t task_size = sizeof(callable_task<decltype(new_task_fn)>);
          void* task_ptr = suspended_thread_allocator_.allocate(task_size);

          auto t = new (task_ptr) callable_task(new_task_fn);
          tasks.push_back({{nullptr, t, task_size}, target_rank});
        }

        // allocate memory then put
        for (auto [t, target_rank] : tasks) {
          cross_worker_mailbox_.put(t, target_rank);
        }

        // Wait until all tasks are completed on remote workers
        // TODO: barrier is a better solution to avoid network contention when many workers are involved
        for (auto [t, target_rank] : tasks) {
          while (!suspended_thread_allocator_.is_remotely_freed(t.frame_base));
        }
      }

      // Temporarily make this thread a non-cross-worker task, so that the thread does not enter
      // this scope multiple times. When a task group has multiple child tasks, the entering thread
      // makes multiple join calls, which causes this function to be called multiple times.
      // Even if we discard the current dist range, the task group's dist range is anyway restored
      // when the task group is completed after those join calls.
      tls_->drange.make_non_cross_worker();
    }
  }

  template <typename T, typename OnDriftDieCallback>
  void on_die_workfirst(thread_state<T>* ts, const T& ret, OnDriftDieCallback&& on_drift_die_cb) {
    if (use_primary_wsq_) {
      auto qe = primary_wsq_.pop(tls_->dtree_node_ref.depth);
      if (qe.has_value()) {
        if (!qe->evacuation_ptr) {
          // parent is popped
          ITYR_CHECK(qe->frame_base == cf_top_);
          return;
        } else {
          // If it might not be its parent, return it to the queue.
          // This is a conservative approach because the popped task can be its evacuated parent
          // (if qe->frame_base == cf_top_), but it is not guaranteed because multiple threads
          // can have the same base frame address due to the uni-address scheme.
          primary_wsq_.push(*qe, tls_->dtree_node_ref.depth);
        }
      }
    } else {
      auto qe = migration_wsq_.pop(tls_->dtree_node_ref.depth);
      if (qe.has_value()) {
        if (qe->is_continuation && !qe->evacuation_ptr) {
          ITYR_CHECK(qe->frame_base == cf_top_);
          return;
        } else {
          migration_wsq_.push(*qe, tls_->dtree_node_ref.depth);
        }
      }
    }

    on_die_drifted(ts, ret, std::forward<OnDriftDieCallback>(on_drift_die_cb));
  }

  template <typename T, typename OnDriftDieCallback>
  void on_die_drifted(thread_state<T>* ts, const T& ret, OnDriftDieCallback&& on_drift_die_cb) {
    if constexpr (!std::is_null_pointer_v<std::remove_reference_t<OnDriftDieCallback>>) {
      call_cb<prof_phase_sched_die, prof_phase_sched_die,
              prof_phase_cb_drift_die>(std::forward<OnDriftDieCallback>(on_drift_die_cb));
    }

    if constexpr (!std::is_same_v<T, no_retval_t> || dag_profiler::enabled) {
      thread_retval<T> retval = {ret, tls_->dag_prof};
      remote_put_value(thread_state_allocator_, retval, &ts->retval);
    }

    // race
    if (remote_faa_value(thread_state_allocator_, 1, &ts->resume_flag) == 0) {
      common::verbose("Win the join race for thread %p (joined thread)", ts);
      // Ancestor threads can remain on the stack here because ADWS no longer follows the work-first policy.
      // Threads that are in the middle of the call stack can be stolen because of the task depth management.
      // Therefore, we conservatively evacuate them before switching to the scheduler here.
      // Note that a fast path exists when the immediate parent thread is popped from the queue.
      evacuate_all();
      common::profiler::switch_phase<prof_phase_sched_die, prof_phase_sched_loop>();
      resume_sched();

    } else {
      common::verbose("Lose the join race for thread %p (joined thread)", ts);
      common::profiler::switch_phase<prof_phase_sched_die, prof_phase_sched_resume_join>();
      suspended_state ss = remote_get_value(thread_state_allocator_, &ts->suspended);
      resume(ss);
    }
  }

  template <typename T>
  void on_root_die(thread_state<T>* ts, const T& ret) {
    if constexpr (!std::is_same_v<T, no_retval_t> || dag_profiler::enabled) {
      thread_retval<T> retval = {ret, tls_->dag_prof};
      remote_put_value(thread_state_allocator_, retval, &ts->retval);
    }
    remote_put_value(thread_state_allocator_, 1, &ts->resume_flag);

    common::profiler::switch_phase<prof_phase_sched_die, prof_phase_sched_loop>();
    resume_sched();
  }

  void steal() {
    auto ne = dtree_.get_topmost_dominant(dtree_local_bottom_ref_);
    if (!ne.has_value()) {
      common::verbose<2>("Dominant dist_tree node not found");
      return;
    }
    dist_range steal_range = ne->drange;
    flipper    tg_version  = ne->tg_version;
    int        depth       = ne->depth();

    common::verbose<2>("Dominant dist_tree node found: drange=[%f, %f), depth=%d",
                       steal_range.begin(), steal_range.end(), depth);

    auto my_rank = common::topology::my_rank();

    auto begin_rank = steal_range.begin_rank();
    auto end_rank   = steal_range.end_rank();

    if (steal_range.is_at_end_boundary()) {
      end_rank--;
    }

    if (begin_rank == end_rank) {
      return;
    }

    ITYR_CHECK((begin_rank <= my_rank || my_rank <= end_rank));

    common::verbose<2>("Start work stealing for dominant task group [%f, %f)",
                       steal_range.begin(), steal_range.end());

    // reuse the dist tree information multiple times
    int max_reuse = std::max(1, adws_max_dtree_reuse_option::value());
    for (int i = 0; i < max_reuse; i++) {
      auto target_rank = get_random_rank(begin_rank, end_rank);

      common::verbose<2>("Target rank: %d", target_rank);

      if (target_rank != begin_rank) {
        bool success = steal_from_migration_queues(target_rank, depth, migration_wsq_.n_queues(),
            [=](migration_wsq_entry& mwe) { return mwe.tg_version.match(tg_version, depth); });
        if (success) {
          return;
        }
      }

      if (target_rank != end_rank || (target_rank == end_rank && steal_range.is_at_end_boundary())) {
        bool success = steal_from_primary_queues(target_rank, depth, primary_wsq_.n_queues(),
            [=](primary_wsq_entry& pwe) { return pwe.tg_version.match(tg_version, depth); });
        if (success) {
          return;
        }
      }

      // Periodic check for cross-worker task arrival
      auto cwt = cross_worker_mailbox_.pop();
      if (cwt.has_value()) {
        execute_cross_worker_task(*cwt);
        return;
      }
    }
  }

  template <typename StealCondFn>
  bool steal_from_primary_queues(common::topology::rank_t target_rank,
                                 int min_depth, int max_depth, StealCondFn&& steal_cond_fn) {
    bool steal_success = false;

    primary_wsq_.for_each_nonempty_queue(target_rank, min_depth, max_depth, false, [&](int d) {
      auto ibd = common::profiler::interval_begin<prof_event_sched_steal>(target_rank);

      if (!primary_wsq_.lock().trylock(target_rank, d)) {
        common::profiler::interval_end<prof_event_sched_steal>(ibd, false);
        return false;
      }

      auto pwe = primary_wsq_.steal_nolock(target_rank, d);
      if (!pwe.has_value()) {
        primary_wsq_.lock().unlock(target_rank, d);
        common::profiler::interval_end<prof_event_sched_steal>(ibd, false);
        return false;
      }

      if (!steal_cond_fn(*pwe)) {
        primary_wsq_.abort_steal(target_rank, d);
        primary_wsq_.lock().unlock(target_rank, d);
        common::profiler::interval_end<prof_event_sched_steal>(ibd, false);
        return false;
      }

      // TODO: commonize implementation for primary and migration queues
      if (pwe->evacuation_ptr) {
        // This task is an evacuated continuation
        common::verbose("Steal an evacuated context frame [%p, %p) from primary wsqueue (depth=%d) on rank %d",
                        pwe->frame_base, reinterpret_cast<std::byte*>(pwe->frame_base) + pwe->frame_size,
                        d, target_rank);

        primary_wsq_.lock().unlock(target_rank, d);

        common::profiler::interval_end<prof_event_sched_steal>(ibd, true);

        common::profiler::switch_phase<prof_phase_sched_loop, prof_phase_sched_resume_stolen>();

        suspend([&](context_frame* cf) {
          sched_cf_ = cf;
          resume(suspended_state{pwe->evacuation_ptr, pwe->frame_base, pwe->frame_size});
        });

      } else {
        // This task is a context frame on the stack
        common::verbose("Steal context frame [%p, %p) from primary wsqueue (depth=%d) on rank %d",
                        pwe->frame_base, reinterpret_cast<std::byte*>(pwe->frame_base) + pwe->frame_size,
                        d, target_rank);

        stack_.direct_copy_from(pwe->frame_base, pwe->frame_size, target_rank);

        primary_wsq_.lock().unlock(target_rank, d);

        common::profiler::interval_end<prof_event_sched_steal>(ibd, true);

        common::profiler::switch_phase<prof_phase_sched_loop, prof_phase_sched_resume_stolen>();

        context_frame* next_cf = reinterpret_cast<context_frame*>(pwe->frame_base);
        suspend([&](context_frame* cf) {
          sched_cf_ = cf;
          context::clear_parent_frame(next_cf);
          resume(next_cf);
        });
      }

      steal_success = true;
      return true;
    });

    if (!steal_success) {
      common::verbose<2>("Steal failed for primary queues on rank %d", target_rank);
    }
    return steal_success;
  }

  template <typename StealCondFn>
  bool steal_from_migration_queues(common::topology::rank_t target_rank,
                                   int min_depth, int max_depth, StealCondFn&& steal_cond_fn) {
    bool steal_success = false;

    migration_wsq_.for_each_nonempty_queue(target_rank, min_depth, max_depth, true, [&](int d) {
      auto ibd = common::profiler::interval_begin<prof_event_sched_steal>(target_rank);

      if (!migration_wsq_.lock().trylock(target_rank, d)) {
        common::profiler::interval_end<prof_event_sched_steal>(ibd, false);
        return false;
      }

      auto mwe = migration_wsq_.steal_nolock(target_rank, d);
      if (!mwe.has_value()) {
        migration_wsq_.lock().unlock(target_rank, d);
        common::profiler::interval_end<prof_event_sched_steal>(ibd, false);
        return false;
      }

      if (!steal_cond_fn(*mwe)) {
        migration_wsq_.abort_steal(target_rank, d);
        migration_wsq_.lock().unlock(target_rank, d);
        common::profiler::interval_end<prof_event_sched_steal>(ibd, false);
        return false;
      }

      if (!mwe->is_continuation) {
        // This task is a new task
        common::verbose("Steal a new task from migration wsqueue (depth=%d) on rank %d",
                        d, target_rank);

        migration_wsq_.lock().unlock(target_rank, d);

        common::profiler::interval_end<prof_event_sched_steal>(ibd, true);

        common::profiler::switch_phase<prof_phase_sched_loop, prof_phase_sched_start_new>();

        suspend([&](context_frame* cf) {
          sched_cf_ = cf;
          start_new_task(mwe->frame_base, mwe->frame_size);
        });

      } else if (mwe->evacuation_ptr) {
        // This task is an evacuated continuation
        common::verbose("Steal an evacuated context frame [%p, %p) from migration wsqueue (depth=%d) on rank %d",
                        mwe->frame_base, reinterpret_cast<std::byte*>(mwe->frame_base) + mwe->frame_size,
                        d, target_rank);

        migration_wsq_.lock().unlock(target_rank, d);

        common::profiler::interval_end<prof_event_sched_steal>(ibd, true);

        common::profiler::switch_phase<prof_phase_sched_loop, prof_phase_sched_resume_stolen>();

        suspend([&](context_frame* cf) {
          sched_cf_ = cf;
          resume(suspended_state{mwe->evacuation_ptr, mwe->frame_base, mwe->frame_size});
        });

      } else {
        // This task is a continuation on the stack
        common::verbose("Steal a context frame [%p, %p) from migration wsqueue (depth=%d) on rank %d",
                        mwe->frame_base, reinterpret_cast<std::byte*>(mwe->frame_base) + mwe->frame_size,
                        d, target_rank);

        stack_.direct_copy_from(mwe->frame_base, mwe->frame_size, target_rank);

        migration_wsq_.lock().unlock(target_rank, d);

        common::profiler::interval_end<prof_event_sched_steal>(ibd, true);

        common::profiler::switch_phase<prof_phase_sched_loop, prof_phase_sched_resume_stolen>();

        suspend([&](context_frame* cf) {
          sched_cf_ = cf;
          context_frame* next_cf = reinterpret_cast<context_frame*>(mwe->frame_base);
          resume(next_cf);
        });
      }

      steal_success = true;
      return true;
    });

    if (!steal_success) {
      common::verbose<2>("Steal failed for migration queues on rank %d", target_rank);
    }
    return steal_success;
  }

  template <typename Fn>
  void suspend(Fn&& fn) {
    context_frame*        prev_cf_top = cf_top_;
    thread_local_storage* prev_tls    = tls_;

    context::save_context_with_call(prev_cf_top,
        [](context_frame* cf, void* cf_top_p, void* fn_p) {
      context_frame*& cf_top = *reinterpret_cast<context_frame**>(cf_top_p);
      Fn              fn     = *reinterpret_cast<Fn*>(fn_p); // copy closure to the new stack frame
      cf_top = cf;
      fn(cf);
    }, &cf_top_, &fn, prev_tls);

    cf_top_ = prev_cf_top;
    tls_    = prev_tls;
  }

  void resume(context_frame* cf) {
    common::verbose("Resume context frame [%p, %p) in the stack", cf, cf->parent_frame);
    context::resume(cf);
  }

  void resume(suspended_state ss) {
    common::verbose("Resume context frame [%p, %p) evacuated at %p",
                    ss.frame_base, reinterpret_cast<std::byte*>(ss.frame_base) + ss.frame_size, ss.evacuation_ptr);

    // We pass the suspended thread states *by value* because the current local variables can be overwritten by the
    // new stack we will bring from remote nodes.
    context::jump_to_stack(ss.frame_base, [](void* this_, void* evacuation_ptr, void* frame_base, void* frame_size_) {
      scheduler_adws& this_sched = *reinterpret_cast<scheduler_adws*>(this_);
      std::size_t     frame_size = reinterpret_cast<std::size_t>(frame_size_);

      common::remote_get(this_sched.suspended_thread_allocator_,
                         reinterpret_cast<std::byte*>(frame_base),
                         reinterpret_cast<std::byte*>(evacuation_ptr),
                         frame_size);
      this_sched.suspended_thread_allocator_.deallocate(evacuation_ptr, frame_size);

      context_frame* cf = reinterpret_cast<context_frame*>(frame_base);
      /* context::clear_parent_frame(cf); */
      context::resume(cf);
    }, this, ss.evacuation_ptr, ss.frame_base, reinterpret_cast<void*>(ss.frame_size));
  }

  void resume_sched() {
    cf_top_ = nullptr;
    tls_ = nullptr;
    common::verbose("Resume scheduler context");
    context::resume(sched_cf_);
  }

  void start_new_task(void* task_ptr, std::size_t task_size) {
    root_on_stack([&]() {
      task_general* t = reinterpret_cast<task_general*>(alloca(task_size));

      common::remote_get(suspended_thread_allocator_,
                         reinterpret_cast<std::byte*>(t),
                         reinterpret_cast<std::byte*>(task_ptr),
                         task_size);
      suspended_thread_allocator_.deallocate(task_ptr, task_size);

      t->execute();
    });
  }

  void execute_cross_worker_task(const cross_worker_task& cwt) {
    if (cwt.evacuation_ptr == nullptr) {
      // This task is a new task
      common::verbose("Received a new cross-worker task");
      common::profiler::switch_phase<prof_phase_sched_loop, prof_phase_sched_start_new>();

      suspend([&](context_frame* cf) {
        sched_cf_ = cf;
        start_new_task(cwt.frame_base, cwt.frame_size);
      });

    } else {
      // This task is an evacuated continuation
      common::verbose("Received a continuation of a cross-worker task");
      common::profiler::switch_phase<prof_phase_sched_loop, prof_phase_sched_resume_migrate>();

      suspend([&](context_frame* cf) {
        sched_cf_ = cf;
        resume(suspended_state{cwt.evacuation_ptr, cwt.frame_base, cwt.frame_size});
      });
    }
  }

  void execute_migrated_task(const migration_wsq_entry& mwe) {
    if (!mwe.is_continuation) {
      // This task is a new task
      common::verbose("Popped a new task from local migration queues");
      common::profiler::switch_phase<prof_phase_sched_loop, prof_phase_sched_start_new>();

      suspend([&](context_frame* cf) {
        sched_cf_ = cf;
        start_new_task(mwe.frame_base, mwe.frame_size);
      });

    } else if (mwe.evacuation_ptr) {
      // This task is an evacuated continuation
      common::verbose("Popped an evacuated continuation from local migration queues");
      common::profiler::switch_phase<prof_phase_sched_loop, prof_phase_sched_resume_popped>();

      suspend([&](context_frame* cf) {
        sched_cf_ = cf;
        resume(suspended_state{mwe.evacuation_ptr, mwe.frame_base, mwe.frame_size});
      });

    } else {
      // This task is a continuation on the stack
      common::die("On-stack threads cannot remain after switching to the scheduler. Something went wrong.");
    }
  }

  std::optional<primary_wsq_entry> pop_from_primary_queues(int depth_from) {
    // TODO: upper bound for depth can be tracked
    for (int d = depth_from; d >= 0; d--) {
      auto pwe = primary_wsq_.pop<false>(d);
      if (pwe.has_value()) {
        return pwe;
      }
    }
    return std::nullopt;
  }

  std::optional<migration_wsq_entry> pop_from_migration_queues() {
    for (int d = 0; d < migration_wsq_.n_queues(); d++) {
      auto mwe = migration_wsq_.pop<false>(d);
      if (mwe.has_value()) {
        return mwe;
      }
    }
    return std::nullopt;
  }

  suspended_state evacuate(context_frame* cf) {
    std::size_t cf_size = reinterpret_cast<uintptr_t>(cf->parent_frame) - reinterpret_cast<uintptr_t>(cf);
    void* evacuation_ptr = suspended_thread_allocator_.allocate(cf_size);
    std::memcpy(evacuation_ptr, cf, cf_size);

    common::verbose("Evacuate suspended thread context [%p, %p) to %p",
                    cf, cf->parent_frame, evacuation_ptr);

    return {evacuation_ptr, cf, cf_size};
  }

  void evacuate_all() {
    if (use_primary_wsq_) {
      for (int d = tls_->dtree_node_ref.depth; d >= 0; d--) {
        primary_wsq_.for_each_entry([&](primary_wsq_entry& pwe) {
          if (!pwe.evacuation_ptr) {
            context_frame* cf = reinterpret_cast<context_frame*>(pwe.frame_base);
            suspended_state ss = evacuate(cf);
            pwe = {ss.evacuation_ptr, ss.frame_base, ss.frame_size, pwe.tg_version};
          }
        }, d);
      }
    } else {
      migration_wsq_.for_each_entry([&](migration_wsq_entry& mwe) {
        if (mwe.is_continuation && !mwe.evacuation_ptr) {
          context_frame* cf = reinterpret_cast<context_frame*>(mwe.frame_base);
          suspended_state ss = evacuate(cf);
          mwe = {true, ss.evacuation_ptr, ss.frame_base, ss.frame_size, mwe.tg_version};
        }
      }, tls_->dtree_node_ref.depth);
    }
  }

  template <typename PhaseFrom, typename PhaseTo,
            typename PreSuspendCallback, typename PostSuspendCallback>
  bool check_cross_worker_task_arrival(PreSuspendCallback&&  pre_suspend_cb,
                                       PostSuspendCallback&& post_suspend_cb) {
    if (cross_worker_mailbox_.arrived()) {
      tls_->dag_prof.stop();

      auto cb_ret = call_cb<PhaseFrom, prof_phase_sched_evacuate,
                            prof_phase_cb_pre_suspend>(std::forward<PreSuspendCallback>(pre_suspend_cb));

      auto my_rank = common::topology::my_rank();

      evacuate_all();

      suspend([&](context_frame* cf) {
        suspended_state ss = evacuate(cf);

        if (use_primary_wsq_) {
          primary_wsq_.push({ss.evacuation_ptr, ss.frame_base, ss.frame_size, tls_->tg_version},
                            tls_->dtree_node_ref.depth);
        } else {
          migration_wsq_.push({true, ss.evacuation_ptr, ss.frame_base, ss.frame_size, tls_->tg_version},
                              tls_->dtree_node_ref.depth);
        }

        common::profiler::switch_phase<prof_phase_sched_evacuate, prof_phase_sched_loop>();
        resume_sched();
      });

      if (my_rank == common::topology::my_rank()) {
        call_cb<prof_phase_sched_resume_popped, PhaseTo,
                prof_phase_cb_post_suspend>(std::forward<PostSuspendCallback>(post_suspend_cb), cb_ret);
      } else {
        call_cb<prof_phase_sched_resume_stolen, PhaseTo,
                prof_phase_cb_post_suspend>(std::forward<PostSuspendCallback>(post_suspend_cb), cb_ret);
      }

      tls_->dag_prof.start();

      return true;

    } else if constexpr (!std::is_same_v<PhaseTo, PhaseFrom>) {
      common::profiler::switch_phase<PhaseFrom, PhaseTo>();
    }

    return false;
  }

  template <typename Callback, typename... CallbackArgs>
  struct callback_retval {
    using type = std::invoke_result_t<Callback, CallbackArgs...>;
  };

  template <typename... CallbackArgs>
  struct callback_retval<std::nullptr_t, CallbackArgs...> {
    using type = void;
  };

  template <typename Callback, typename... CallbackArgs>
  using callback_retval_t = typename callback_retval<Callback, CallbackArgs...>::type;

  template <typename PhaseFrom, typename PhaseTo, typename PhaseCB,
            typename Callback, typename... CallbackArgs>
  auto call_cb(Callback&& cb, CallbackArgs&&... cb_args) {
    using retval_t = callback_retval_t<Callback, CallbackArgs...>;

    if constexpr (!std::is_null_pointer_v<std::remove_reference_t<Callback>>) {
      common::profiler::switch_phase<PhaseFrom, PhaseCB>();

      if constexpr (!std::is_void_v<retval_t>) {
        auto ret = std::forward<Callback>(cb)(std::forward<CallbackArgs>(cb_args)...);
        common::profiler::switch_phase<PhaseCB, PhaseTo>();
        return ret;

      } else {
        std::forward<Callback>(cb)(std::forward<CallbackArgs>(cb_args)...);
        common::profiler::switch_phase<PhaseCB, PhaseTo>();
      }

    } else if constexpr (!std::is_same_v<PhaseFrom, PhaseTo>) {
      common::profiler::switch_phase<PhaseFrom, PhaseTo>();
    }

    if constexpr (!std::is_void_v<retval_t>) {
      return retval_t{};
    } else {
      return no_retval_t{};
    }
  }

  template <typename PhaseFrom, typename PhaseTo, typename PhaseCB,
            typename Callback, typename... CallbackArgs>
  auto call_cb(Callback&& cb, no_retval_t, CallbackArgs&&... cb_args) {
    // skip no_retval_t args
    return call_cb<PhaseFrom, PhaseTo, PhaseCB>(
        std::forward<Callback>(cb), std::forward<CallbackArgs>(cb_args)...);
  }

  context_frame* stack_top() const {
    // Add a margin of sizeof(context_frame) to the bottom of the stack, because
    // this region can be accessed by the clear_parent_frame() function later
    return reinterpret_cast<context_frame*>(stack_.bottom()) - 1;
  }

  template <typename Fn>
  void root_on_stack(Fn&& fn) {
    cf_top_ = stack_top();
    context::call_on_stack(stack_.top(), stack_.size() - sizeof(context_frame),
                           [](void* fn_, void*, void*, void*) {
      Fn fn = *reinterpret_cast<Fn*>(fn_); // copy closure to the new stack frame
      fn();
    }, &fn, nullptr, nullptr, nullptr);
  }

  void execute_coll_task(task_general* t, coll_task ct) {
    coll_task ct_ {.task_ptr = t, .task_size = ct.task_size, .begin_rank = ct.begin_rank};

    // pass coll task to other processes in a binary tree form
    auto n_ranks = common::topology::n_ranks();
    auto my_rank_shifted = (common::topology::my_rank() + n_ranks - ct.begin_rank) % n_ranks;
    for (common::topology::rank_t i = common::next_pow2(n_ranks); i > 1; i /= 2) {
      if (my_rank_shifted % i == 0) {
        auto target_rank_shifted = my_rank_shifted + i / 2;
        if (target_rank_shifted < n_ranks) {
          auto target_rank = (target_rank_shifted + ct.begin_rank) % n_ranks;
          coll_task_mailbox_.put(ct_, target_rank);
        }
      }
    }

    // Ensure all processes have finished coll task execution before deallocation
    common::mpi_barrier(common::topology::mpicomm());

    t->execute();

    // Ensure all processes have finished coll task execution before deallocation
    common::mpi_barrier(common::topology::mpicomm());
  }

  void execute_coll_task_if_arrived() {
    auto ct = coll_task_mailbox_.pop();
    if (ct.has_value()) {
      task_general* t = reinterpret_cast<task_general*>(
          suspended_thread_allocator_.allocate(ct->task_size));

      common::remote_get(suspended_thread_allocator_,
                         reinterpret_cast<std::byte*>(t),
                         reinterpret_cast<std::byte*>(ct->task_ptr),
                         ct->task_size);

      execute_coll_task(t, *ct);

      suspended_thread_allocator_.deallocate(t, ct->task_size);
    }
  }

  template <typename CondFn>
  bool should_exit_sched_loop(CondFn&& cond_fn) {
    if (sched_loop_make_mpi_progress_option::value()) {
      common::mpi_make_progress();
    }

    execute_coll_task_if_arrived();

    if (sched_loop_exit_req_ == MPI_REQUEST_NULL &&
        std::forward<CondFn>(cond_fn)()) {
      // If a given condition is met, enters a barrier
      sched_loop_exit_req_ = common::mpi_ibarrier(common::topology::mpicomm());
    }
    if (sched_loop_exit_req_ != MPI_REQUEST_NULL) {
      // If the barrier is resolved, the scheduler loop should terminate
      return common::mpi_test(sched_loop_exit_req_);
    }
    return false;
  }

  int                                max_depth_;
  callstack                          stack_;
  oneslot_mailbox<coll_task>         coll_task_mailbox_;
  oneslot_mailbox<cross_worker_task> cross_worker_mailbox_;
  wsqueue<primary_wsq_entry, false>  primary_wsq_;
  wsqueue<migration_wsq_entry, true> migration_wsq_;
  common::remotable_resource         thread_state_allocator_;
  common::remotable_resource         suspended_thread_allocator_;
  context_frame*                     cf_top_              = nullptr;
  context_frame*                     sched_cf_            = nullptr;
  thread_local_storage*              tls_                 = nullptr;
  MPI_Request                        sched_loop_exit_req_ = MPI_REQUEST_NULL;
  bool                               use_primary_wsq_     = true;
  dist_tree                          dtree_;
  dist_tree::node_ref                dtree_local_bottom_ref_;
  bool                               dag_prof_enabled_ = false;
  dag_profiler                       dag_prof_result_;
};

}
