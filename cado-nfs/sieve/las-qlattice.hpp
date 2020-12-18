#ifndef LAS_QLATTICE_HPP_
#define LAS_QLATTICE_HPP_

// IWYU pragma: no_include <bits/exception.h>

#include <exception>    // IWYU pragma: keep   // for exception
#include <cstdint>             // for int64_t, uint64_t, INT64_C, uint32_t
#include <iosfwd>              // for ostream
#include <vector>              // for vector

#include "fb-types.h"          // for fbprime_t, redc_invp_t, sublat_t
#include "las-todo-entry.hpp"  // for las_todo_entry

struct qlattice_basis {
    las_todo_entry doing;

    int64_t a0=0, b0=0, a1=0, b1=0;
    unsigned long q_ulong=0;
    // q (== doing.p) itself or 0 if q is too large to fit

    // handy to have here.
    sublat_t sublat;

    qlattice_basis() = default;
    inline double skewed_norm0(double s) const { return a0*a0/s+b0*b0*s; }
    inline double skewed_norm1(double s) const { return a1*a1/s+b1*b1*s; }

    // Assumes ell is prime.
    bool is_coprime_to(unsigned long ell) const {
        if (doing.is_prime()) {
            return (ell != q_ulong);
        } else {
            for (auto const & p : doing.prime_factors)
                if (p == ell)
                    return false;
            return true;
        }
    }

    inline bool fits_31bits() const {
        return !(
                 a0 <= INT64_C(-2147483648) ||
                 a0 >= INT64_C( 2147483648) ||
                 a1 <= INT64_C(-2147483648) ||
                 a1 >= INT64_C( 2147483648) ||
                 b0 <= INT64_C(-2147483648) ||
                 b0 >= INT64_C( 2147483648) ||
                 b1 <= INT64_C(-2147483648) ||
                 b1 >= INT64_C( 2147483648)
                 );
    }

    struct too_skewed : public std::exception { };

    qlattice_basis(las_todo_entry const & doing, double skew);
};

std::ostream& operator<<(std::ostream& os, qlattice_basis const & Q);

#endif	/* LAS_QLATTICE_HPP_ */
