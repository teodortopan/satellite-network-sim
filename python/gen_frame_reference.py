"""Print independent reference values for the satsim frames tests, as a C++ header.

Every number comes from pyerfa (the Python wrapper of ERFA, the BSD-licensed copy of the
IAU SOFA library) and numpy, never from satsim. The C++ tests compare satsim's own
implementation against them:

- IAU 1976 precession angles and matrix (erfa.prec76, erfa.pmat76), IAU 1980 nutation
  (erfa.nut80) and mean obliquity (erfa.obl80), and TEME -> J2000 (FK5) vectors built as
  P^T N^T R3(-eqe) with N = erfa.numat and eqe = dpsi * cos(eps_mean), at TT epochs from
  1965 to 2099.
- TEME -> ITRF: GMST (erfa.gmst82 at UT1) followed by polar motion (erfa.pom00 with
  s' = 0), at UTC epochs with given Earth orientation values.

Dates are passed to ERFA as the same two-part Julian date satsim uses (0h of the day plus
the fraction of the day), so both sides round the same inputs.

Regenerate (inside the CI image, which has pyerfa from python/requirements.txt):

    python3 python/gen_frame_reference.py > tests/frames/erfa_reference.hpp
    clang-format -i tests/frames/erfa_reference.hpp
"""

import erfa
import numpy as np

NANOSECONDS_PER_DAY = 86_400 * 10**9
ARCSECOND_RAD = np.pi / (180.0 * 3600.0)

# TT epochs for the precession / nutation / TEME -> J2000 references: (y, m, d, h, min, s).
# Seconds are exact in binary so both sides start from the same instant.
FK5_EPOCHS_TT = [
    (1965, 3, 1, 6, 0, 0.0),
    (1984, 7, 15, 18, 30, 15.0),
    (2000, 1, 1, 12, 0, 0.0),
    (2004, 4, 6, 7, 52, 32.5),
    (2026, 11, 18, 0, 0, 0.0),
    (2026, 11, 18, 12, 34, 56.25),
    (2050, 6, 15, 3, 0, 0.0),
    (2099, 12, 31, 23, 59, 59.0),
]

# TEME vectors rotated at every FK5 epoch: Vallado's 2004 example position (km) and
# velocity (km/s) from AIAA 2006-6753 Appendix C, and a point on a 550 km shell (km).
TEME_VECTORS = [
    ("kTemePositionKm", "j2000_position_km", (5094.18016210, 6127.64465950, 6380.34453270)),
    ("kTemeVelocityKmS", "j2000_velocity_km_s", (-4.746131487, 0.785818041, 5.531931288)),
    (
        "kTemeStarlinkKm",
        "j2000_starlink_km",
        (-2088.838995857464, 3617.975269656285, 5509.793033880422),
    ),
]

# UTC epochs with Earth orientation values (UT1 - UTC in s, x_p and y_p in arcseconds).
# The first is the AIAA 2006-6753 Appendix C example; the others use the finals2000A rows of
# 2026-11-17 and 2026-11-18 (tests/time/data/finals2000A_2026q4.txt), at 0h.
ECEF_CASES = [
    ((2004, 4, 6, 7, 51, 28.386), (-0.439961, -0.140682, 0.333309)),
    ((2026, 11, 17, 0, 0, 0.0), (-0.0714893, 0.118407, 0.326797)),
    ((2026, 11, 18, 0, 0, 0.0), (-0.0728860, 0.117290, 0.327387)),
]


def two_part_jd(year, month, day, hour, minute, second, offset_s=0.0):
    """(0h JD of the day, fraction of the day) of a calendar instant plus an offset, at
    nanosecond resolution, the way satsim's UtcTime holds it."""
    jd0, mjd = erfa.cal2jd(year, month, day)
    nanoseconds = (hour * 3600 + minute * 60) * 10**9 + round(second * 1e9)
    nanoseconds += round(offset_s * 1e9)
    days, nanoseconds = divmod(nanoseconds, NANOSECONDS_PER_DAY)
    return float(jd0 + mjd + days), nanoseconds / NANOSECONDS_PER_DAY


def rot3(angle):
    """Frame rotation about z (axes turned by +angle)."""
    c, s = np.cos(angle), np.sin(angle)
    return np.array([[c, s, 0.0], [-s, c, 0.0], [0.0, 0.0, 1.0]])


def num(value):
    """Shortest text that reads back as the same double."""
    return repr(float(value))


def vector(values):
    return "{" + ", ".join(num(v) for v in values) + "}"


def fk5_case(epoch):
    jd1, jd2 = two_part_jd(*epoch)
    zeta, z, theta = erfa.prec76(erfa.DJ00, 0.0, jd1, jd2)
    dpsi, deps = erfa.nut80(jd1, jd2)
    eps = erfa.obl80(jd1, jd2)
    p = erfa.pmat76(jd1, jd2)
    n = erfa.numat(eps, dpsi, deps)
    eqe = dpsi * np.cos(eps)
    m = p.T @ n.T @ rot3(-eqe)
    lines = [
        "    Fk5Case{",
        f"        .tt = {{{epoch[0]}, {epoch[1]}, {epoch[2]}, {epoch[3]}, {epoch[4]}, "
        f"{num(epoch[5])}}},",
        f"        .zeta_rad = {num(zeta)},",
        f"        .theta_rad = {num(theta)},",
        f"        .z_rad = {num(z)},",
        f"        .nutation_longitude_rad = {num(dpsi)},",
        f"        .nutation_obliquity_rad = {num(deps)},",
        f"        .mean_obliquity_rad = {num(eps)},",
        "        .precession_matrix = {{",
    ]
    lines += [f"            {vector(row)}," for row in p]
    lines.append("        }},")
    for _, field, values in TEME_VECTORS:
        lines.append(f"        .{field} = {vector(m @ np.array(values))},")
    lines.append("    },")
    return lines


def ecef_case(epoch, eop):
    ut1_minus_utc, xp_arcsec, yp_arcsec = eop
    ut1_jd1, ut1_jd2 = two_part_jd(*epoch, offset_s=ut1_minus_utc)
    gmst = erfa.gmst82(ut1_jd1, ut1_jd2)
    pom = erfa.pom00(xp_arcsec * ARCSECOND_RAD, yp_arcsec * ARCSECOND_RAD, 0.0)
    teme = np.array(TEME_VECTORS[0][2])
    pef = rot3(gmst) @ teme
    itrf = pom @ pef
    return [
        "    EcefCase{",
        f"        .utc = {{{epoch[0]}, {epoch[1]}, {epoch[2]}, {epoch[3]}, {epoch[4]}, "
        f"{num(epoch[5])}}},",
        f"        .ut1_minus_utc_s = {num(ut1_minus_utc)},",
        f"        .polar_x_arcsec = {num(xp_arcsec)},",
        f"        .polar_y_arcsec = {num(yp_arcsec)},",
        f"        .gmst_rad = {num(gmst)},",
        f"        .pef_km = {vector(pef)},",
        f"        .itrf_km = {vector(itrf)},",
        "    },",
    ]


def main():
    out = [
        "#pragma once",
        "",
        "// Generated by python/gen_frame_reference.py with pyerfa "
        f"{erfa.__version__} and numpy {np.__version__}.",
        "// Do not edit; regenerate with the commands in that script's docstring.",
        "",
        "#include <array>",
        "",
        '#include "satsim/time/civil_time.hpp"',
        "",
        "namespace satsim_test::erfa_reference {",
        "",
        "using Vector = std::array<double, 3>;",
        "",
    ]
    for name, _, values in TEME_VECTORS:
        out.append(f"inline constexpr Vector {name}{vector(values)};")
    out += [
        "",
        "// TT epochs; rows of the precession matrix (J2000 to mean of date); TEME vectors above",
        "// rotated to J2000 (FK5).",
        "struct Fk5Case {",
        "  satsim::time::CivilTime tt{};",
        "  double zeta_rad{};",
        "  double theta_rad{};",
        "  double z_rad{};",
        "  double nutation_longitude_rad{};",
        "  double nutation_obliquity_rad{};",
        "  double mean_obliquity_rad{};",
        "  std::array<Vector, 3> precession_matrix{};",
        "  Vector j2000_position_km{};",
        "  Vector j2000_velocity_km_s{};",
        "  Vector j2000_starlink_km{};",
        "};",
        "",
        f"inline const std::array<Fk5Case, {len(FK5_EPOCHS_TT)}> kFk5Cases{{",
    ]
    for epoch in FK5_EPOCHS_TT:
        out += fk5_case(epoch)
    out += [
        "};",
        "",
        "// UTC epochs with Earth orientation values; kTemePositionKm rotated to PEF by GMST",
        "// (IAU 1982) at UT1, then to ITRF by polar motion.",
        "struct EcefCase {",
        "  satsim::time::CivilTime utc{};",
        "  double ut1_minus_utc_s{};",
        "  double polar_x_arcsec{};",
        "  double polar_y_arcsec{};",
        "  double gmst_rad{};",
        "  Vector pef_km{};",
        "  Vector itrf_km{};",
        "};",
        "",
        f"inline const std::array<EcefCase, {len(ECEF_CASES)}> kEcefCases{{",
    ]
    for epoch, eop in ECEF_CASES:
        out += ecef_case(epoch, eop)
    out += ["};", "", "}  // namespace satsim_test::erfa_reference"]
    print("\n".join(out))


if __name__ == "__main__":
    main()
