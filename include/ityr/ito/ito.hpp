#pragma once

#include <functional>

#include "ityr/common/util.hpp"
#include "ityr/common/options.hpp"
#include "ityr/common/topology.hpp"
#include "ityr/common/wallclock.hpp"
#include "ityr/common/profiler.hpp"
#include "ityr/common/prof_events.hpp"
#include "ityr/ito/util.hpp"
#include "ityr/ito/options.hpp"
#include "ityr/ito/thread.hpp"
#include "ityr/ito/worker.hpp"
#include "ityr/ito/prof_events.hpp"

namespace ityr::ito {

class ito {
public:
  ito(MPI_Comm comm)
    : mi_(comm),
      topo_(comm) {}

private:
  common::mpi_initializer                                    mi_;
  common::runtime_options                                    common_opts_;
  common::singleton_initializer<common::topology::instance>  topo_;
  common::singleton_initializer<common::wallclock::instance> clock_;
  common::singleton_initializer<common::profiler::instance>  prof_;
  common::prof_events                                        common_prof_events_;

  runtime_options                                            ito_opts_;
  aslr_checker                                               aslr_checker_;
  common::singleton_initializer<worker::instance>            worker_;
  prof_events                                                ito_prof_events_;
};

using instance = common::singleton<ito>;

inline void init(MPI_Comm comm = MPI_COMM_WORLD) {
  instance::init(comm);
}

inline void fini() {
  instance::fini();
}

template <typename Fn, typename... Args>
inline auto root_exec(Fn&& fn, Args&&... args) {
  return root_exec(with_callback, nullptr, std::forward<Fn>(fn), std::forward<Args>(args)...);
}

template <typename SchedLoopCallback, typename Fn, typename... Args>
inline auto root_exec(with_callback_t, SchedLoopCallback&& cb, Fn&& fn, Args&&... args) {
  auto& w = worker::instance::get();
  return w.root_exec(std::forward<SchedLoopCallback>(cb), std::forward<Fn>(fn), std::forward<Args>(args)...);
}

inline bool is_spmd() {
  auto& w = worker::instance::get();
  return w.is_spmd();
}

inline scheduler::task_group_data task_group_begin() {
  auto& w = worker::instance::get();
  return w.sched().task_group_begin();
}

inline void task_group_end(scheduler::task_group_data& tgdata) {
  auto& w = worker::instance::get();
  w.sched().task_group_end(tgdata);
}

ITYR_TEST_CASE("[ityr::ito] fib") {
  init();

  std::function<int(int)> fib = [&](int n) -> int {
    if (n <= 1) {
      return 1;
    } else {
      thread<int> th([=]{ return fib(n - 1); });
      int y = fib(n - 2);
      int x = th.join();
      return x + y;
    }
  };

  int r = root_exec(fib, 10);
  ITYR_CHECK(r == 89);

  fini();
}

ITYR_TEST_CASE("[ityr::ito] load balancing") {
  init();

  std::function<void(int)> lb = [&](int n) {
    if (n == 0) {
      return;
    } else if (n == 1) {
      common::mpi_barrier(common::topology::mpicomm());
    } else {
      thread<void> th([=]{ return lb(n / 2); });
      lb(n - n / 2);
      th.join();
    }
  };

  root_exec(lb, common::topology::n_ranks());

  fini();
}

}
