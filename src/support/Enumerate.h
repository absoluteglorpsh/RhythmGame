//
// Index+element iteration for toolchains whose C++ library predates C++23
// std::ranges::views::enumerate (e.g. AppleClang command line tools libc++).
//

#ifndef ENUMERATE_H
#define ENUMERATE_H

#include <ranges>
#include <type_traits>

namespace support {

/**
 * @brief Yields (index, element) pairs like C++23 views::enumerate.
 * @details Implemented over C++20 views::zip, which is available everywhere
 * this project builds. The index is the range difference type, matching the
 * standard adaptor. Prefer std::ranges::views::enumerate once the minimum
 * toolchain provides it.
 */
template<std::ranges::viewable_range Range>
auto
enumerate(Range&& range)
{
    using Difference =
      std::ranges::range_difference_t<std::remove_cvref_t<Range>>;
    return std::ranges::views::zip(std::ranges::views::iota(Difference{ 0 }),
                                   std::forward<Range>(range));
}

} // namespace support

#endif // ENUMERATE_H
