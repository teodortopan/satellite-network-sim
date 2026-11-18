#pragma once

// The IAU 1980 nutation series: 106 terms. Internal to satsim::frames.
//
// Copied by script, unchanged in value and order, from the table in ERFA src/nut80.c
// (the liberfa/erfa copy bundled in the pyerfa 2.0.1.5 source distribution; ERFA is
// derived from the IAU SOFA library). The series is the one of the Explanatory Supplement
// to the Astronomical Almanac (P. K. Seidelmann, ed., 1992), section 3.222, p. 111.
// ERFA's copyright notice and licence, which its terms require to be retained, follow.
//
//  Copyright (C) 2013-2023, NumFOCUS Foundation.
//  All rights reserved.
//
//  This library is derived, with permission, from the International
//  Astronomical Union's "Standards of Fundamental Astronomy" library,
//  available from http://www.iausofa.org.
//
//  The ERFA version is intended to retain identical functionality to
//  the SOFA library, but made distinct through different function and
//  file names, as set out in the SOFA license conditions.  The SOFA
//  original has a role as a reference standard for the IAU and IERS,
//  and consequently redistribution is permitted only in its unaltered
//  state.  The ERFA version is not subject to this restriction and
//  therefore can be included in distributions which do not support the
//  concept of "read only" software.
//
//  Although the intent is to replicate the SOFA API (other than
//  replacement of prefix names) and results (with the exception of
//  bugs;  any that are discovered will be fixed), SOFA is not
//  responsible for any errors found in this version of the library.
//
//  If you wish to acknowledge the SOFA heritage, please acknowledge
//  that you are using a library derived from SOFA, rather than SOFA
//  itself.
//
//
//  TERMS AND CONDITIONS
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions
//  are met:
//
//  1 Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
//  2 Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in
//    the documentation and/or other materials provided with the
//    distribution.
//
//  3 Neither the name of the Standards Of Fundamental Astronomy Board,
//    the International Astronomical Union nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
//  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
//  FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
//  COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
//  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
//  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
//  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
//  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
//  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//  POSSIBILITY OF SUCH DAMAGE.

#include <array>

namespace satsim::frames::detail {

// One term: the argument is the integer combination of the Delaunay arguments
// l, l', F, D, Omega. Coefficients are in units of 0.1 milliarcsecond; the "per_century"
// ones multiply T, Julian centuries of TT since J2000.0.
struct NutationTerm {
  int l;
  int l_prime;
  int f;
  int d;
  int omega;
  double longitude_sin;              // d_psi coefficient of sin(argument)
  double longitude_sin_per_century;  // its rate
  double obliquity_cos;              // d_eps coefficient of cos(argument)
  double obliquity_cos_per_century;  // its rate
};

inline constexpr std::array<NutationTerm, 106> kNutationIau1980Series{{
    // 1-10
    {0, 0, 0, 0, 1, -171996.0, -174.2, 92025.0, 8.9},
    {0, 0, 0, 0, 2, 2062.0, 0.2, -895.0, 0.5},
    {-2, 0, 2, 0, 1, 46.0, 0.0, -24.0, 0.0},
    {2, 0, -2, 0, 0, 11.0, 0.0, 0.0, 0.0},
    {-2, 0, 2, 0, 2, -3.0, 0.0, 1.0, 0.0},
    {1, -1, 0, -1, 0, -3.0, 0.0, 0.0, 0.0},
    {0, -2, 2, -2, 1, -2.0, 0.0, 1.0, 0.0},
    {2, 0, -2, 0, 1, 1.0, 0.0, 0.0, 0.0},
    {0, 0, 2, -2, 2, -13187.0, -1.6, 5736.0, -3.1},
    {0, 1, 0, 0, 0, 1426.0, -3.4, 54.0, -0.1},
    // 11-20
    {0, 1, 2, -2, 2, -517.0, 1.2, 224.0, -0.6},
    {0, -1, 2, -2, 2, 217.0, -0.5, -95.0, 0.3},
    {0, 0, 2, -2, 1, 129.0, 0.1, -70.0, 0.0},
    {2, 0, 0, -2, 0, 48.0, 0.0, 1.0, 0.0},
    {0, 0, 2, -2, 0, -22.0, 0.0, 0.0, 0.0},
    {0, 2, 0, 0, 0, 17.0, -0.1, 0.0, 0.0},
    {0, 1, 0, 0, 1, -15.0, 0.0, 9.0, 0.0},
    {0, 2, 2, -2, 2, -16.0, 0.1, 7.0, 0.0},
    {0, -1, 0, 0, 1, -12.0, 0.0, 6.0, 0.0},
    {-2, 0, 0, 2, 1, -6.0, 0.0, 3.0, 0.0},
    // 21-30
    {0, -1, 2, -2, 1, -5.0, 0.0, 3.0, 0.0},
    {2, 0, 0, -2, 1, 4.0, 0.0, -2.0, 0.0},
    {0, 1, 2, -2, 1, 4.0, 0.0, -2.0, 0.0},
    {1, 0, 0, -1, 0, -4.0, 0.0, 0.0, 0.0},
    {2, 1, 0, -2, 0, 1.0, 0.0, 0.0, 0.0},
    {0, 0, -2, 2, 1, 1.0, 0.0, 0.0, 0.0},
    {0, 1, -2, 2, 0, -1.0, 0.0, 0.0, 0.0},
    {0, 1, 0, 0, 2, 1.0, 0.0, 0.0, 0.0},
    {-1, 0, 0, 1, 1, 1.0, 0.0, 0.0, 0.0},
    {0, 1, 2, -2, 0, -1.0, 0.0, 0.0, 0.0},
    // 31-40
    {0, 0, 2, 0, 2, -2274.0, -0.2, 977.0, -0.5},
    {1, 0, 0, 0, 0, 712.0, 0.1, -7.0, 0.0},
    {0, 0, 2, 0, 1, -386.0, -0.4, 200.0, 0.0},
    {1, 0, 2, 0, 2, -301.0, 0.0, 129.0, -0.1},
    {1, 0, 0, -2, 0, -158.0, 0.0, -1.0, 0.0},
    {-1, 0, 2, 0, 2, 123.0, 0.0, -53.0, 0.0},
    {0, 0, 0, 2, 0, 63.0, 0.0, -2.0, 0.0},
    {1, 0, 0, 0, 1, 63.0, 0.1, -33.0, 0.0},
    {-1, 0, 0, 0, 1, -58.0, -0.1, 32.0, 0.0},
    {-1, 0, 2, 2, 2, -59.0, 0.0, 26.0, 0.0},
    // 41-50
    {1, 0, 2, 0, 1, -51.0, 0.0, 27.0, 0.0},
    {0, 0, 2, 2, 2, -38.0, 0.0, 16.0, 0.0},
    {2, 0, 0, 0, 0, 29.0, 0.0, -1.0, 0.0},
    {1, 0, 2, -2, 2, 29.0, 0.0, -12.0, 0.0},
    {2, 0, 2, 0, 2, -31.0, 0.0, 13.0, 0.0},
    {0, 0, 2, 0, 0, 26.0, 0.0, -1.0, 0.0},
    {-1, 0, 2, 0, 1, 21.0, 0.0, -10.0, 0.0},
    {-1, 0, 0, 2, 1, 16.0, 0.0, -8.0, 0.0},
    {1, 0, 0, -2, 1, -13.0, 0.0, 7.0, 0.0},
    {-1, 0, 2, 2, 1, -10.0, 0.0, 5.0, 0.0},
    // 51-60
    {1, 1, 0, -2, 0, -7.0, 0.0, 0.0, 0.0},
    {0, 1, 2, 0, 2, 7.0, 0.0, -3.0, 0.0},
    {0, -1, 2, 0, 2, -7.0, 0.0, 3.0, 0.0},
    {1, 0, 2, 2, 2, -8.0, 0.0, 3.0, 0.0},
    {1, 0, 0, 2, 0, 6.0, 0.0, 0.0, 0.0},
    {2, 0, 2, -2, 2, 6.0, 0.0, -3.0, 0.0},
    {0, 0, 0, 2, 1, -6.0, 0.0, 3.0, 0.0},
    {0, 0, 2, 2, 1, -7.0, 0.0, 3.0, 0.0},
    {1, 0, 2, -2, 1, 6.0, 0.0, -3.0, 0.0},
    {0, 0, 0, -2, 1, -5.0, 0.0, 3.0, 0.0},
    // 61-70
    {1, -1, 0, 0, 0, 5.0, 0.0, 0.0, 0.0},
    {2, 0, 2, 0, 1, -5.0, 0.0, 3.0, 0.0},
    {0, 1, 0, -2, 0, -4.0, 0.0, 0.0, 0.0},
    {1, 0, -2, 0, 0, 4.0, 0.0, 0.0, 0.0},
    {0, 0, 0, 1, 0, -4.0, 0.0, 0.0, 0.0},
    {1, 1, 0, 0, 0, -3.0, 0.0, 0.0, 0.0},
    {1, 0, 2, 0, 0, 3.0, 0.0, 0.0, 0.0},
    {1, -1, 2, 0, 2, -3.0, 0.0, 1.0, 0.0},
    {-1, -1, 2, 2, 2, -3.0, 0.0, 1.0, 0.0},
    {-2, 0, 0, 0, 1, -2.0, 0.0, 1.0, 0.0},
    // 71-80
    {3, 0, 2, 0, 2, -3.0, 0.0, 1.0, 0.0},
    {0, -1, 2, 2, 2, -3.0, 0.0, 1.0, 0.0},
    {1, 1, 2, 0, 2, 2.0, 0.0, -1.0, 0.0},
    {-1, 0, 2, -2, 1, -2.0, 0.0, 1.0, 0.0},
    {2, 0, 0, 0, 1, 2.0, 0.0, -1.0, 0.0},
    {1, 0, 0, 0, 2, -2.0, 0.0, 1.0, 0.0},
    {3, 0, 0, 0, 0, 2.0, 0.0, 0.0, 0.0},
    {0, 0, 2, 1, 2, 2.0, 0.0, -1.0, 0.0},
    {-1, 0, 0, 0, 2, 1.0, 0.0, -1.0, 0.0},
    {1, 0, 0, -4, 0, -1.0, 0.0, 0.0, 0.0},
    // 81-90
    {-2, 0, 2, 2, 2, 1.0, 0.0, -1.0, 0.0},
    {-1, 0, 2, 4, 2, -2.0, 0.0, 1.0, 0.0},
    {2, 0, 0, -4, 0, -1.0, 0.0, 0.0, 0.0},
    {1, 1, 2, -2, 2, 1.0, 0.0, -1.0, 0.0},
    {1, 0, 2, 2, 1, -1.0, 0.0, 1.0, 0.0},
    {-2, 0, 2, 4, 2, -1.0, 0.0, 1.0, 0.0},
    {-1, 0, 4, 0, 2, 1.0, 0.0, 0.0, 0.0},
    {1, -1, 0, -2, 0, 1.0, 0.0, 0.0, 0.0},
    {2, 0, 2, -2, 1, 1.0, 0.0, -1.0, 0.0},
    {2, 0, 2, 2, 2, -1.0, 0.0, 0.0, 0.0},
    // 91-100
    {1, 0, 0, 2, 1, -1.0, 0.0, 0.0, 0.0},
    {0, 0, 4, -2, 2, 1.0, 0.0, 0.0, 0.0},
    {3, 0, 2, -2, 2, 1.0, 0.0, 0.0, 0.0},
    {1, 0, 2, -2, 0, -1.0, 0.0, 0.0, 0.0},
    {0, 1, 2, 0, 1, 1.0, 0.0, 0.0, 0.0},
    {-1, -1, 0, 2, 1, 1.0, 0.0, 0.0, 0.0},
    {0, 0, -2, 0, 1, -1.0, 0.0, 0.0, 0.0},
    {0, 0, 2, -1, 2, -1.0, 0.0, 0.0, 0.0},
    {0, 1, 0, 2, 0, -1.0, 0.0, 0.0, 0.0},
    {1, 0, -2, -2, 0, -1.0, 0.0, 0.0, 0.0},
    // 101-106
    {0, -1, 2, 0, 1, -1.0, 0.0, 0.0, 0.0},
    {1, 1, 0, -2, 1, -1.0, 0.0, 0.0, 0.0},
    {1, 0, -2, 2, 0, -1.0, 0.0, 0.0, 0.0},
    {2, 0, 0, 2, 0, 1.0, 0.0, 0.0, 0.0},
    {0, 0, 2, 4, 2, -1.0, 0.0, 0.0, 0.0},
    {0, 1, 0, 1, 0, 1.0, 0.0, 0.0, 0.0},
}};

}  // namespace satsim::frames::detail
