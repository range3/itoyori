#pragma once

#include "ityr/common/util.hpp"
#include "ityr/ori/ori.hpp"
#include "ityr/pattern/count_iterator.hpp"
#include "ityr/pattern/global_iterator.hpp"
#include "ityr/container/checkout_span.hpp"

namespace ityr {

namespace execution {

/**
 * @brief Serial execution policy for iterator-based loop functions.
 * @see [std::execution -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/execution_policy_tag_t)
 * @see `ityr::execution::seq`
 * @see `ityr::execution::sequenced_policy`
 * @see `ityr::for_each()`
 */
struct sequenced_policy {
  /**
   * @brief The maximum number of elements to check out at the same time if automatic checkout is enabled.
   */
  std::size_t checkout_count = 1;
};

/**
 * @brief Parallel execution policy for iterator-based loop functions.
 * @see [std::execution -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/execution_policy_tag_t)
 * @see `ityr::execution::par`
 * @see `ityr::execution::parallel_policy`
 * @see `ityr::for_each()`
 */
struct parallel_policy {
  /**
   * @brief The maximum number of elements to check out at the same time if automatic checkout is enabled.
   */
  std::size_t cutoff_count = 1;

  /**
   * @brief The number of elements for leaf tasks to stop parallel recursion.
   */
  std::size_t checkout_count = 1;
};

/**
 * @brief Default serial execution policy for iterator-based loop functions.
 * @see `ityr::execution::sequenced_policy`
 */
inline constexpr sequenced_policy seq;

/**
 * @brief Default parallel execution policy for iterator-based loop functions.
 * @see `ityr::execution::sequenced_policy`
 */
inline constexpr parallel_policy par;

namespace internal {

inline sequenced_policy to_sequenced_policy(const sequenced_policy& opts) {
  return opts;
}

inline sequenced_policy to_sequenced_policy(const parallel_policy& opts) {
  return {.checkout_count = opts.checkout_count};
}

inline void assert_policy(const sequenced_policy& opts) {
  ITYR_CHECK(0 < opts.checkout_count);
}

inline void assert_policy(const parallel_policy& opts) {
  ITYR_CHECK(0 < opts.checkout_count);
  ITYR_CHECK(opts.checkout_count <= opts.cutoff_count);
}

}
}

namespace internal {

template <typename T, typename Mode>
inline auto make_checkout_iter_nb(global_iterator<T, Mode> it,
                                  std::size_t              count) {
  checkout_span<T, Mode> cs;
  cs.checkout_nb(&*it, count, Mode{});
  return std::make_tuple(std::move(cs), cs.data());
}

template <typename T>
inline auto make_checkout_iter_nb(global_move_iterator<T> it,
                                  std::size_t             count) {
  checkout_span<T, checkout_mode::read_write_t> cs;
  cs.checkout_nb(&*it, count, checkout_mode::read_write);
  return std::make_tuple(std::move(cs), std::make_move_iterator(cs.data()));
}

template <typename T>
inline auto make_checkout_iter_nb(global_construct_iterator<T> it,
                                  std::size_t                  count) {
  checkout_span<T, checkout_mode::write_t> cs;
  cs.checkout_nb(&*it, count, checkout_mode::write);
  return std::make_tuple(std::move(cs), make_count_iterator(cs.data()));
}

template <typename T>
inline auto make_checkout_iter_nb(global_destruct_iterator<T> it,
                                  std::size_t                 count) {
  checkout_span<T, checkout_mode::read_write_t> cs;
  cs.checkout_nb(&*it, count, checkout_mode::read_write);
  return std::make_tuple(std::move(cs), make_count_iterator(cs.data()));
}

inline auto checkout_global_iterators_aux(std::size_t) {
  return std::make_tuple(std::make_tuple(), std::make_tuple());
}

template <typename ForwardIterator, typename... ForwardIterators>
inline auto checkout_global_iterators_aux(std::size_t n, ForwardIterator it, ForwardIterators... rest) {
  if constexpr (is_global_iterator_v<ForwardIterator>) {
    if constexpr (ForwardIterator::auto_checkout) {
      auto [cs, it_] = make_checkout_iter_nb(it, n);
      auto [css, its] = checkout_global_iterators_aux(n, rest...);
      return std::make_tuple(std::tuple_cat(std::make_tuple(std::move(cs)), std::move(css)),
                             std::tuple_cat(std::make_tuple(it_), its));
    } else {
      auto [css, its] = checkout_global_iterators_aux(n, rest...);
      // &*: convert global_iterator -> global_ref -> global_ptr
      return std::make_tuple(std::move(css),
                             std::tuple_cat(std::make_tuple(&*it), its));
    }
  } else {
    auto [css, its] = checkout_global_iterators_aux(n, rest...);
    return std::make_tuple(std::move(css),
                           std::tuple_cat(std::make_tuple(it), its));
  }
}

template <typename... ForwardIterators>
inline auto checkout_global_iterators(std::size_t n, ForwardIterators... its) {
  auto ret = checkout_global_iterators_aux(n, its...);
  ori::checkout_complete();
  return ret;
}

template <typename Op, typename... ForwardIterators>
inline void apply_iterators(Op                  op,
                            std::size_t         n,
                            ForwardIterators... its) {
  for (std::size_t i = 0; i < n; (++i, ..., ++its)) {
    op(*its...);
  }
}

template <typename Op, typename ForwardIterator, typename... ForwardIterators>
inline void for_each_aux(const execution::sequenced_policy& policy,
                         Op                                 op,
                         ForwardIterator                    first,
                         ForwardIterator                    last,
                         ForwardIterators...                firsts) {
  if constexpr ((is_global_iterator_v<ForwardIterator> || ... ||
                 is_global_iterator_v<ForwardIterators>)) {
    // perform automatic checkout for global iterators
    std::size_t n = std::distance(first, last);
    std::size_t c = policy.checkout_count;

    for (std::size_t d = 0; d < n; d += c) {
      auto n_ = std::min(n - d, c);

      auto [css, its] = checkout_global_iterators(n_, first, firsts...);
      std::apply([&](auto&&... args) {
        apply_iterators(op, n_, std::forward<decltype(args)>(args)...);
      }, its);

      ((first = std::next(first, n_)), ..., (firsts = std::next(firsts, n_)));
    }

  } else {
    for (; first != last; (++first, ..., ++firsts)) {
      op(*first, *firsts...);
    }
  }
}

}

}
