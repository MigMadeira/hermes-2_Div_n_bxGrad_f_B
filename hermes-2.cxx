/*

    Copyright B.Dudson, J.Leddy, University of York, 2016-2019
              email: benjamin.dudson@york.ac.uk

    This file is part of Hermes-2 (Hot ion version)

    Hermes is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Hermes is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Hermes.  If not, see <http://www.gnu.org/licenses/>.

*/
#include "hermes-2.hxx"

#include <derivs.hxx>
#include <field_factory.hxx>
#include <initialprofiles.hxx>

#include <invert_parderiv.hxx>
#include "parallel_boundary_region.hxx"
#include "boundary_region.hxx"

#include "div_ops.hxx"
#include "loadmetric.hxx"

#include <bout/constants.hxx>
#include <bout/assert.hxx>
#include <bout/fv_ops.hxx>


// OpenADAS interface Atomicpp by T.Body
#include "atomicpp/ImpuritySpecies.hxx"
#include "atomicpp/Prad.hxx"

#define SETNAME(expr) setName(expr, #expr)

std::string parbc{"parallel_neumann_o1"};

namespace FV {
  template<typename CellEdges = MC>
  const Field3D Div_par_fvv(const Field3D &f_in, const Field3D &v_in,
                            const Field3D &wave_speed_in, bool fixflux=true) {

    ASSERT1(areFieldsCompatible(f_in, v_in));
    ASSERT1(areFieldsCompatible(f_in, wave_speed_in));
    bool use_parallel_slices = (f_in.hasParallelSlices() && v_in.hasParallelSlices()
                                  && wave_speed_in.hasParallelSlices());

    Mesh* mesh = f_in.getMesh();

    CellEdges cellboundary;

    /// Ensure that f, v and wave_speed are field aligned
    Field3D f = use_parallel_slices ? f_in : toFieldAligned(f_in, "RGN_NOX");
    Field3D v = use_parallel_slices ? v_in : toFieldAligned(v_in, "RGN_NOX");
    Field3D wave_speed = use_parallel_slices ?
      wave_speed_in : toFieldAligned(wave_speed_in, "RGN_NOX");

    Coordinates *coord = f_in.getCoordinates();

    Field3D result{zeroFrom(f)};

    // Only need one guard cell, so no need to communicate fluxes
    // Instead calculate in guard cells to preserve fluxes
    int ys = mesh->ystart-1;
    int ye = mesh->yend+1;

    for (int i = mesh->xstart; i <= mesh->xend; i++) {

      if (!mesh->firstY(i) || mesh->periodicY(i)) {
        // Calculate in guard cell to get fluxes consistent between processors
        ys = mesh->ystart - 1;
      } else {
        // Don't include the boundary cell. Note that this implies special
        // handling of boundaries later
        ys = mesh->ystart;
      }

      if (!mesh->lastY(i) || mesh->periodicY(i)) {
        // Calculate in guard cells
        ye = mesh->yend + 1;
      } else {
        // Not in boundary cells
        ye = mesh->yend;
      }

      for (int j = ys; j <= ye; j++) {
        for (int k = 0; k < mesh->LocalNz; k++) {

          // For right cell boundaries
          BoutReal common_factor = (coord->J(i, j, k) + coord->J(i, j + 1, k)) /
            (sqrt(coord->g_22(i, j, k)) + sqrt(coord->g_22(i, j + 1, k)));

          BoutReal flux_factor_rc = common_factor / (coord->dy(i, j, k) * coord->J(i, j, k));
          BoutReal flux_factor_rp = common_factor / (coord->dy(i, j + 1, k) * coord->J(i, j + 1, k));

          // For left cell boundaries
          common_factor = (coord->J(i, j, k) + coord->J(i, j - 1, k)) /
            (sqrt(coord->g_22(i, j, k)) + sqrt(coord->g_22(i, j - 1, k)));

          BoutReal flux_factor_lc = common_factor / (coord->dy(i, j, k) * coord->J(i, j, k));
          BoutReal flux_factor_lm = common_factor / (coord->dy(i, j - 1, k) * coord->J(i, j - 1, k));

          ////////////////////////////////////////////
          // Reconstruct f at the cell faces
          // This calculates s.R and s.L for the Right and Left
          // face values on this cell

          // Reconstruct f at the cell faces
          Stencil1D s;
          s.c = f(i, j, k);
          s.m = f(i, j - 1, k);
          s.p = f(i, j + 1, k);

          cellboundary(s); // Calculate s.R and s.L

          // Reconstruct v at the cell faces
          Stencil1D sv;
          sv.c = v(i, j, k);
          sv.m = v(i, j - 1, k);
          sv.p = v(i, j + 1, k);

          cellboundary(sv);

          ////////////////////////////////////////////
          // Right boundary

          // Calculate velocity at right boundary (y+1/2)
          BoutReal vpar = 0.5 * (v(i, j, k) + v(i, j + 1, k));
          BoutReal flux;

          if (mesh->lastY(i) && (j == mesh->yend) && !mesh->periodicY(i)) {
            // Last point in domain

            BoutReal bndryval = 0.5 * (s.c + s.p);
            if (fixflux) {
              // Use mid-point to be consistent with boundary conditions
              flux = bndryval * vpar * vpar;
            } else {
              // Add flux due to difference in boundary values
              flux = s.R * vpar * sv.R + wave_speed(i, j, k) * (s.R * sv.R - bndryval * vpar);
            }
          } else {

            // Maximum wave speed in the two cells
            BoutReal amax = BOUTMAX(wave_speed(i, j, k), wave_speed(i, j + 1, k));

            if (vpar > amax) {
              // Supersonic flow out of this cell
              flux = s.R * vpar * sv.R;
            } else if (vpar < -amax) {
              // Supersonic flow into this cell
              flux = 0.0;
            } else {
              // Subsonic flow, so a mix of right and left fluxes
              flux = s.R * 0.5 * (vpar + amax) * sv.R;
            }
          }

          result(i, j, k) += flux * flux_factor_rc;
          result(i, j + 1, k) -= flux * flux_factor_rp;

          ////////////////////////////////////////////
          // Calculate at left boundary

          vpar = 0.5 * (v(i, j, k) + v(i, j - 1, k));

          if (mesh->firstY(i) && (j == mesh->ystart) && !mesh->periodicY(i)) {
            // First point in domain
            BoutReal bndryval = 0.5 * (s.c + s.m);
            if (fixflux) {
              // Use mid-point to be consistent with boundary conditions
              flux = bndryval * vpar * vpar;
            } else {
              // Add flux due to difference in boundary values
              flux = s.L * vpar * sv.L - wave_speed(i, j, k) * (s.L * sv.L - bndryval * vpar);
            }
          } else {

            // Maximum wave speed in the two cells
            BoutReal amax = BOUTMAX(wave_speed(i, j, k), wave_speed(i, j - 1, k));

            if (vpar < -amax) {
              // Supersonic out of this cell
              flux = s.L * vpar * sv.L;
            } else if (vpar > amax) {
              // Supersonic into this cell
              flux = 0.0;
            } else {
              flux = s.L * 0.5 * (vpar - amax) * sv.L;
            }
          }

          result(i, j, k) -= flux * flux_factor_lc;
          result(i, j - 1, k) += flux * flux_factor_lm;

        }
      }
    }
    return fromFieldAligned(result, "RGN_NOBNDRY");
  }
}

BoutReal floor(BoutReal var, BoutReal f) {
  if (var < f)
    return f;
  return var;
}

/// Returns a copy of input \p var with all values greater than \p f replaced by
/// \p f.
const Field3D ceil(const Field3D &var, BoutReal f, REGION rgn = RGN_ALL) {
  checkData(var);
  Field3D result = copy(var);

  BOUT_FOR(d, var.getRegion(rgn)) {
    if (result[d] > f) {
      result[d] = f;
    }
  }

  return result;
}

// Square function for vectors
Field3D SQ(const Vector3D &v) { return v * v; }

void setRegions(Field3D &f) {
  f.yup().setRegion("RGN_YPAR_+1");
  f.ydown().setRegion("RGN_YPAR_-1");
}

const Field3D &yup(const Field3D &f) { return f.yup(); }
BoutReal yup(BoutReal f) { return f; };
const Field3D &ydown(const Field3D &f) { return f.ydown(); }
BoutReal ydown(BoutReal f) { return f; };
const BoutReal yup(BoutReal f, Ind3D i) { return f; };
const BoutReal ydown(BoutReal f, Ind3D i) { return f; };
// const BoutReal& yup(const Field3D &f, Ind3D i) { return f.yup()[i.yp()]; }
// const BoutReal& ydown(const Field3D &f, Ind3D i) { return f.ydown()[i.ym()];
// } BoutReal& yup(Field3D &f, Ind3D i) { return f.yup()[i.yp()]; } BoutReal&
// ydown(Field3D &f, Ind3D i) { return f.ydown()[i.ym()]; }
const BoutReal &yup(const Field3D &f, Ind3D i) { return f.yup()[i]; }
const BoutReal &ydown(const Field3D &f, Ind3D i) { return f.ydown()[i]; }
BoutReal &yup(Field3D &f, Ind3D i) { return f.yup()[i]; }
BoutReal &ydown(Field3D &f, Ind3D i) { return f.ydown()[i]; }
const BoutReal &_get(const Field3D &f, Ind3D i) { return f[i]; }
BoutReal &_get(Field3D &f, Ind3D i) { return f[i]; }
BoutReal _get(BoutReal f, Ind3D i) { return f; };
BoutReal copy(BoutReal f) { return f; };

void alloc_all(Field3D &f) {
  f.allocate();
  f.splitParallelSlices();
  f.yup().allocate();
  f.ydown().allocate();
  setRegions(f);
}

#define GET_ALL(name)                                                          \
  auto *name##a = &name[Ind3D(0)];                                             \
  auto *name##b = &name.yup()[Ind3D(0)];                                       \
  auto *name##c = &name.ydown()[Ind3D(0)];

#define DO_ALL(op, name)                                                       \
  template <class A, class B> Field3D name##_all(const A &a, const B &b) {     \
    Field3D result;                                                            \
    alloc_all(result);                                                         \
    BOUT_FOR(i, result.getRegion("RGN_ALL")) { name##_all(result, a, b, i); }  \
    setRegions(result);                                                        \
    return result;                                                             \
  }                                                                            \
  template <class A, class B>                                                  \
  void name##_all(Field3D &result, const A &a, const B &b, Ind3D i) {          \
    result[i] = op(_get(a, i), _get(b, i));                                    \
    yup(result, i) = op(yup(a, i), yup(b, i));                                 \
    ydown(result, i) = op(ydown(a, i), ydown(b, i));                           \
  }                                                                            \
  template <class B> void name##_all(Field3D &result, const B &b, Ind3D i) {   \
    result[i] = op(result[i], _get(b, i));                                     \
    yup(result, i) = op(yup(result, i), yup(b, i));                            \
    ydown(result, i) = op(ydown(result, i), ydown(b, i));                      \
  }

DO_ALL(floor, floor)
DO_ALL(pow, pow)

#undef DO_ALL
#define DO_ALL(op, name)                                                       \
  template <class A, class B> Field3D name##_all(const A &a, const B &b) {     \
    Field3D result;                                                            \
    alloc_all(result);                                                         \
    BOUT_FOR(i, result.getRegion("RGN_ALL")) { name##_all(result, a, b, i); }  \
    checkData(result, "RGN_ALL");                                              \
    setRegions(result);                                                        \
    return result;                                                             \
  }                                                                            \
  Field3D name##_all(const Field3D &a, const Field3D &b) {                     \
    Field3D result;                                                            \
    alloc_all(result);                                                         \
    const int n = result.getNx() * result.getNy() * result.getNz();            \
    GET_ALL(result);                                                           \
    GET_ALL(a);                                                                \
    GET_ALL(b);                                                                \
    BOUT_OMP(omp parallel for simd)                                            \
    for (int i = 0; i < n; ++i) {                                              \
      resulta[i] = aa[i] op ba[i];                                             \
      resultb[i] = ab[i] op bb[i];                                             \
      resultc[i] = ac[i] op bc[i];                                             \
    }                                                                          \
    setRegions(result);                                                        \
    return result;                                                             \
  }                                                                            \
  Field3D name##_all(const Field3D &a, BoutReal b) {                           \
    Field3D result;                                                            \
    alloc_all(result);                                                         \
    const int n = result.getNx() * result.getNy() * result.getNz();            \
    GET_ALL(result);                                                           \
    GET_ALL(a);                                                                \
    BOUT_OMP(omp parallel for simd)                                            \
    for (int i = 0; i < n; ++i) {                                              \
      resulta[i] = aa[i] op b;                                                 \
      resultb[i] = ab[i] op b;                                                 \
      resultc[i] = ac[i] op b;                                                 \
    }                                                                          \
    setRegions(result);                                                        \
    return result;                                                             \
  }                                                                            \
  template <class A, class B>                                                  \
  void name##_all(Field3D &result, const A &a, const B &b, Ind3D i) {          \
    result[i] = _get(a, i) op _get(b, i);                                      \
    yup(result, i) = yup(a, i) op yup(b, i);                                   \
    ydown(result, i) = ydown(a, i) op ydown(b, i);                             \
  }

// void div_all(Field3D & result, const Field3D & a, const Field3D & b, Ind3D i)
// {
//   result[i] = a[i] / b[i];
//   result.yup()[i.yp()] = a.yup()[i.yp()] / b.yup()[i.yp()];
//   result.ydown()[i.ym()] = a.ydown()[i.ym()] / b.ydown()[i.ym()];
// }

//#include "mul_all.cxx"
DO_ALL(*, mul)
DO_ALL(/, div)
DO_ALL(+, add)
DO_ALL(-, sub)
#undef DO_ALL

#define DO_ALL(op, name)                                                       \
  template <class A, class B> Field3D &name##_all_inp(A &a, const B &b) {      \
    BOUT_FOR(i, a.getRegion("RGN_ALL")) { name##_all(a, b, i); }               \
    checkData(a, "RGN_ALL");                                                   \
    return a;                                                                  \
  }                                                                            \
  Field3D &name##_all_inp(Field3D &a, const Field3D &b) {                      \
    const int n = a.getNx() * a.getNy() * a.getNz();                           \
    GET_ALL(a);                                                                \
    GET_ALL(b);                                                                \
    BOUT_OMP(omp parallel for simd)                                            \
    for (int i = 0; i < n; ++i) {                                              \
      aa[i] op ba[i];                                                          \
      ab[i] op bb[i];                                                          \
      ac[i] op bc[i];                                                          \
    }                                                                          \
    return a;                                                                  \
  }                                                                            \
  Field3D &name##_all_inp(Field3D &a, BoutReal b) {                            \
    const int n = a.getNx() * a.getNy() * a.getNz();                           \
    GET_ALL(a);                                                                \
    BOUT_OMP(omp parallel for simd)                                            \
    for (int i = 0; i < n; ++i) {                                              \
      aa[i] op b;                                                              \
      ab[i] op b;                                                              \
      ac[i] op b;                                                              \
    }                                                                          \
    return a;                                                                  \
  }                                                                            \
  template <class A, class B> void name##_all_inp(A &a, const B &b, Ind3D i) { \
    a[i] op _get(b, i);                                                        \
    yup(a, i) op yup(b, i);                                                    \
    ydown(a, i) op ydown(b, i);                                                \
  }

// #include "mul_all.cxx"
DO_ALL(*=, mul)
DO_ALL(/=, div)
DO_ALL(+=, add)
DO_ALL(-=, sub)

#undef DO_ALL
#define DO_ALL(op)                                                             \
  inline void op##_all(Field3D &result, const Field3D &a, Ind3D i) {           \
    result[i] = op(a[i]);                                                      \
    yup(result, i) = op(yup(a, i));                                            \
    ydown(result, i) = op(ydown(a, i));                                        \
  }                                                                            \
  inline Field3D op##_all(const Field3D &a) {                                  \
    Field3D result;                                                            \
    alloc_all(result);                                                         \
    BOUT_FOR(i, result.getRegion("RGN_ALL")) { op##_all(result, a, i); }       \
    checkData(result, "RGN_ALL");                                              \
    setRegions(result);                                                        \
    return result;                                                             \
  }

DO_ALL(sqrt)
DO_ALL(SQ)
DO_ALL(copy)
DO_ALL(exp)
DO_ALL(log)

#undef DO_ALL

void set_all(Field3D &f, BoutReal val) {
  alloc_all(f);
  BOUT_FOR(i, f.getRegion("RGN_ALL")) {
    f[i] = val;
    f.yup()[i] = val;
    f.ydown()[i] = val;
  }
}
void zero_all(Field3D &f) { set_all(f, 0); }

void check_all(Field3D &f) {
  checkData(f);
  checkData(f.yup());
  checkData(f.ydown());
}

void ASSERT_CLOSE_ALL(const Field3D &a, const Field3D &b) {
  BOUT_FOR(i, a.getRegion("RGN_NOY")) {
    ASSERT0(std::abs(a[i] - b[i]) < 1e-10);
    ASSERT0(std::abs(yup(a, i) - yup(b, i)) < 1e-10);
    ASSERT0(std::abs(ydown(a, i) - ydown(b, i)) < 1e-10);
  }
}

/// Modifies and returns the first argument, taking the boundary from second argument
/// This is used because unfortunately Field3D::setBoundary returns void
Field3D withBoundary(Field3D &&f, const Field3D &bndry) {
  f.setBoundaryTo(bndry);
  return f;
}

int Hermes::init(bool restarting) {

  auto& opt = Options::root();

  // Switches in model section
  auto& optsc = opt["Hermes"];

  OPTION(optsc, evolve_plasma, true);
  OPTION(optsc, show_timesteps, false);
  if (BoutComm::rank() != 0) {
    show_timesteps = false;
  }

  electromagnetic = optsc["electromagnetic"]
                        .doc("Include vector potential psi in Ohm's law?")
                        .withDefault<bool>(true);

  FiniteElMass = optsc["FiniteElMass"]
                     .doc("Include electron inertia in Ohm's law?")
                     .withDefault<bool>(true);

  j_diamag = optsc["j_diamag"]
                 .doc("Diamagnetic current: Vort <-> Pe")
                 .withDefault<bool>(true);

  j_par = optsc["j_par"]
              .doc("Parallel current:    Vort <-> Psi")
              .withDefault<bool>(true);

  j_pol_pi = optsc["j_pol_pi"]
              .doc("Polarisation current with explicit Pi dependence")
              .withDefault<bool>(true);

  j_pol_simplified = optsc["j_pol_simplified"]
              .doc("Polarisation current without explicit Pi dependence")
              .withDefault<bool>(false);

  relaxation  = optsc["relaxation"]
              .doc("Relaxation method for potential solvers")
              .withDefault<bool>(false);

  OPTION(optsc, lambda_0, 1e3);
  OPTION(optsc, lambda_2, 1e5);
  OPTION(optsc, parallel_flow, true);
  OPTION(optsc, parallel_flow_p_term, parallel_flow);
  OPTION(optsc, pe_par, true);
  OPTION(optsc, pe_par_p_term, pe_par);
  OPTION(optsc, resistivity, true);
  OPTION(optsc, thermal_flux, true);

  thermal_force = optsc["thermal_force"]
                    .doc("Force on electrons due to temperature gradients")
                    .withDefault<bool>(true);

  OPTION(optsc, electron_viscosity, true);
  ion_viscosity = optsc["ion_viscosity"].doc("Include ion viscosity?").withDefault<bool>(true);
  ion_viscosity_par = optsc["ion_viscosity_par"].doc("Include parallel diffusion of ion momentum?").withDefault<bool>(ion_viscosity);

  electron_neutral = optsc["electron_neutral"]
                       .doc("Include electron-neutral collisions in resistivity?")
                       .withDefault<bool>(true);

  ion_neutral = optsc["ion_neutral"]
                  .doc("Include ion-neutral collisions in tau_i?")
                  .withDefault<bool>(false);

  poloidal_flows = optsc["poloidal_flows"]
                       .doc("Include ExB flows in X-Y plane")
                       .withDefault(true);

  OPTION(optsc, ion_velocity, true);

  OPTION(optsc, thermal_conduction, true);
  OPTION(optsc, electron_ion_transfer, true);

  OPTION(optsc, neutral_friction, false);
  OPTION(optsc, frecycle, 0.9);

  OPTION(optsc, phi3d, false);

  OPTION(optsc, ne_bndry_flux, true);
  OPTION(optsc, pe_bndry_flux, true);
  OPTION(optsc, vort_bndry_flux, false);

  OPTION(optsc, ramp_mesh, true);
  OPTION(optsc, ramp_timescale, 1e4);

  OPTION(optsc, energy_source, false);

  ion_neutral_rate = optsc["ion_neutral_rate"]
                     .doc("A fixed ion-neutral collision rate, normalised to ion cyclotron frequency.")
                     .withDefault(0.0);

  OPTION(optsc, staggered, false);

  OPTION(optsc, boussinesq, false);

  OPTION(optsc, sinks, false);
  OPTION(optsc, sheath_closure, true);
  OPTION(optsc, drift_wave, false);

  // Cross-field transport
  classical_diffusion = optsc["classical_diffusion"]
                          .doc("Collisional cross-field diffusion, including viscosity")
                          .withDefault<bool>(false);
  OPTION(optsc, anomalous_D, -1);
  OPTION(optsc, anomalous_chi, -1);
  OPTION(optsc, anomalous_nu, -1);
  OPTION(optsc, anomalous_D_nvi, true);
  OPTION(optsc, anomalous_D_pepi, true);

  // Flux limiters
  OPTION(optsc, flux_limit_alpha, -1);
  OPTION(optsc, kappa_limit_alpha, -1);
  OPTION(optsc, eta_limit_alpha, -1);

  // Numerical dissipation terms
  OPTION(optsc, numdiff, -1.0);
  OPTION(optsc, hyper, -1);
  OPTION(optsc, hyperpar, -1);
  OPTION(optsc, low_pass_z, -1);
  OPTION(optsc, x_hyper_viscos, -1.0);
  OPTION(optsc, y_hyper_viscos, -1.0);
  OPTION(optsc, z_hyper_viscos, -1.0);
  OPTION(optsc, scale_num_cs, 1.0);
  OPTION(optsc, floor_num_cs, -1.0);
  OPTION(optsc, vepsi_dissipation, false);
  OPTION(optsc, vort_dissipation, false);

  phi_dissipation = optsc["phi_dissipation"]
    .doc("Add a dissipation term to vorticity, depending on reconstruction of potential?")
    .withDefault<bool>(false);

  ne_num_diff = optsc["ne_num_diff"]
                    .doc("Numerical Ne diffusion in X-Z plane. < 0 => off.")
                    .withDefault(-1.0);

  ne_num_hyper = optsc["ne_num_hyper"]
                     .doc("Numerical Ne hyper-diffusion in X-Z plane. < 0 => off.")
                     .withDefault(-1.0);

  vi_num_diff = optsc["vi_num_diff"]
                    .doc("Numerical Vi diffusion in X-Z plane. < 0 => off.")
                    .withDefault(-1.0);

  ve_num_diff = optsc["ve_num_diff"]
                    .doc("Numerical Ve diffusion in X-Z plane. < 0 => off.")
                    .withDefault(-1.0);

  ve_num_hyper = optsc["ve_num_hyper"]
                     .doc("Numerical Ve hyper-diffusion in X-Z plane. < 0 => off.")
                     .withDefault(-1.0);

  OPTION(optsc, ne_hyper_z, -1.0);
  OPTION(optsc, pe_hyper_z, -1.0);

  OPTION(optsc, low_n_diffuse, false);
  OPTION(optsc, low_n_diffuse_perp, false);

  OPTION(optsc, resistivity_multiply, 1.0);

  OPTION(optsc, sheath_model, 0);
  OPTION(optsc, sheath_gamma_e, 5.5);
  OPTION(optsc, sheath_gamma_i, 1.0);

  OPTION(optsc, neutral_vwall, 1. / 3);  // 1/3rd Franck-Condon energy at wall
  OPTION(optsc, sheath_yup, true);       // Apply sheath at yup?
  OPTION(optsc, sheath_ydown, true);     // Apply sheath at ydown?
  OPTION(optsc, test_boundaries, false); // Test boundary conditions
  OPTION(optsc, parallel_sheaths, false); // Apply parallel sheath conditions?
  OPTION(optsc, par_sheath_model, 0);
  OPTION(optsc, par_sheath_ve, true);
  OPTION(optsc, electron_weight, 1.0);

  OPTION(optsc, Div_parP_n_sheath_extra, Div_parP_n_sheath_extra);

  sheath_allow_supersonic =
      optsc["sheath_allow_supersonic"]
          .doc("If plasma is faster than sound speed, go to plasma velocity")
          .withDefault<bool>(true);

  radial_buffers = optsc["radial_buffers"]
    .doc("Turn on radial buffer regions?").withDefault<bool>(false);
  OPTION(optsc, radial_inner_width, 4);
  OPTION(optsc, radial_outer_width, 1);
  OPTION(optsc, radial_buffer_D, 1.0);

  OPTION(optsc, phi_smoothing, false);
  OPTION(optsc, phi_sf, 0.0);

  resistivity_boundary = optsc["resistivity_boundary"]
    .doc("Normalised resistivity in radial boundary region")
    .withDefault(1.0);

  resistivity_boundary_width = optsc["resistivity_boundary_width"]
    .doc("Number of grid cells in radial (x) direction")
    .withDefault(0);

  // Output additional information
  OPTION(optsc, verbose, false);    // Save additional fields
  OPTION(optsc, output_ddt, false); // Save time derivatives

  // Normalisation
  OPTION(optsc, Tnorm, 100);  // Reference temperature [eV]
  OPTION(optsc, Nnorm, 1e19); // Reference density [m^-3]
  OPTION(optsc, Bnorm, 1.0);  // Reference magnetic field [T]

  OPTION(optsc, AA, 2.0); // Ion mass (2 = Deuterium)

  output.write("Normalisation Te={:e}, Ne={:e}, B={:e}\n", Tnorm, Nnorm, Bnorm);
  SAVE_ONCE(Tnorm, Nnorm, Bnorm, AA); // Save

  Cs0 = sqrt(qe * Tnorm / (AA * Mp)); // Reference sound speed [m/s]
  Omega_ci = qe * Bnorm / (AA * Mp);  // Ion cyclotron frequency [1/s]
  rho_s0 = Cs0 / Omega_ci;

  mi_me = AA * Mp / (electron_weight * Me);
  me_mi = (electron_weight * Me) / (AA * Mp);
  beta_e = qe * Tnorm * Nnorm / (SQ(Bnorm) / (2. * SI::mu0));

  output.write("\tmi_me={}, beta_e={}\n", mi_me, beta_e);
  SAVE_ONCE(mi_me, beta_e, me_mi);

  output.write("\t Cs={:e}, rho_s={:e}, Omega_ci={:e}\n", Cs0, rho_s0, Omega_ci);
  SAVE_ONCE(Cs0, rho_s0, Omega_ci);

  // Collision times
  BoutReal lambda_ei = 24. - log(sqrt(Nnorm / 1e6) / Tnorm);
  BoutReal lambda_ii = 23. - log(sqrt(2. * Nnorm / 1e6) / pow(Tnorm, 1.5));

  tau_e0 = 1. / (2.91e-6 * (Nnorm / 1e6) * lambda_ei * pow(Tnorm, -3. / 2));
  tau_i0 =
      sqrt(AA) / (4.78e-8 * (Nnorm / 1e6) * lambda_ii * pow(Tnorm, -3. / 2));

  output.write("\ttau_e0={:e}, tau_i0={:e}\n", tau_e0, tau_i0);

  if (anomalous_D > 0.0) {
    // Normalise
    anomalous_D /= rho_s0 * rho_s0 * Omega_ci; // m^2/s
    output.write("\tnormalised anomalous D_perp = {:e}\n", anomalous_D);
    a_d3d = anomalous_D;
    mesh->communicate(a_d3d);
    a_d3d.yup() = anomalous_D;
    a_d3d.ydown() = anomalous_D;
  }
  if (anomalous_chi > 0.0) {
    // Normalise
    anomalous_chi /= rho_s0 * rho_s0 * Omega_ci; // m^2/s
    output.write("\tnormalised anomalous chi_perp = {:e}\n", anomalous_chi);
    a_chi3d = anomalous_chi;
    mesh->communicate(a_chi3d);
    a_chi3d.yup() = anomalous_D;
    a_chi3d.ydown() = anomalous_D;
  }
  if (anomalous_nu > 0.0) {
    // Normalise
    anomalous_nu /= rho_s0 * rho_s0 * Omega_ci; // m^2/s
    output.write("\tnormalised anomalous nu_perp = {:e}\n", anomalous_nu);
    a_nu3d = anomalous_nu;
    mesh->communicate(a_nu3d);
    a_nu3d.yup() = anomalous_D;
    a_nu3d.ydown() = anomalous_D;
  }

  if (ramp_mesh) {
    Jpar0 = 0.0;
  } else {
    // Read equilibrium current density
    // GRID_LOAD(Jpar0);
    // Jpar0 /= qe*Nnorm*Cs0;
    Jpar0 = 0.0;
  }

  FieldFactory fact(mesh);

  if (sinks) {
    std::string source = optsc["sink_invlpar"].withDefault<std::string>("0.05"); // 20 m
    sink_invlpar = fact.create3D(source);
    sink_invlpar *= rho_s0; // Normalise
    SAVE_ONCE(sink_invlpar);

    if (drift_wave) {
      alpha_dw = fact.create2D("Hermes:alpha_dw");
      SAVE_ONCE(alpha_dw);
    }
  } else {
    optsc["sink_invlpar"].setConditionallyUsed();
  }

  // Get switches from each variable section
  auto& optne = opt["Ne"];
  NeSource = optne["source"].doc("Source term in ddt(Ne)").withDefault(Field3D{0.0});
  NeSource /= Omega_ci;
  Sn = NeSource;

  // Inflowing density carries momentum
  OPTION(optne, density_inflow, false);

  auto& optpe = opt["Pe"];
  PeSource = optpe["source"].withDefault(Field3D{0.0});
  PeSource /= Omega_ci;
  Spe = PeSource;

  auto& optpi = opt["Pi"];
  PiSource = optpi["source"].withDefault(Field3D{0.0});
  PiSource /= Omega_ci;
  Spi = PiSource;

  OPTION(optsc, core_sources, false);
  if (core_sources) {
    for (int x = mesh->xstart; x <= mesh->xend; x++) {
      if (!mesh->periodicY(x)) {
        // Not periodic, so not in core
        for (int y = mesh->ystart; y <= mesh->yend; y++) {
          for (int z = 0; z <= mesh->LocalNz; z++) {
            Sn(x, y, z) = 0.0;
            Spe(x, y, z) = 0.0;
            Spi(x, y, z) = 0.0;
          }
        }
      }
    }
  }

  // Mid-plane power flux q_||
  // Midplane power specified in Watts per m^2
  Field2D qfact;
  GRID_LOAD(qfact); // Factor to multiply to get volume source
  Field2D qmid = optpe["midplane_power"].withDefault(Field2D{0.0}) * qfact;
  // Normalise from W/m^3
  qmid /= qe * Tnorm * Nnorm * Omega_ci;
  Spe += (2. / 3) * qmid;

  // Add variables to solver
  SOLVE_FOR(Ne);
  EvolvingVars.add(Ne);

  if (output_ddt) {
    SAVE_REPEAT(ddt(Ne));
  }

  // Evolving n_i instead of n_e
  evolve_ni = optsc["evolve_ni"].doc("Evolve ion density instead?")
    .withDefault<bool>(true);

  // Temperature evolution can be turned off
  // so that Pe = Ne and/or Pi = Ne
  evolve_te = optsc["evolve_te"].doc("Evolve electron temperature?")
    .withDefault<bool>(true);
  if (evolve_te) {
    SOLVE_FOR(Pe);
    EvolvingVars.add(Pe);
    if (output_ddt) {
      SAVE_REPEAT(ddt(Pe));
    }
  } else {
    Pe = Ne;
  }
  evolve_ti = optsc["evolve_ti"].doc("Evolve ion temperature?")
    .withDefault<bool>(true);
  if (evolve_ti) {
    SOLVE_FOR(Pi);
    EvolvingVars.add(Pi);
    if (output_ddt) {
      SAVE_REPEAT(ddt(Pi));
    }
  } else {
    Pi = Ne;
  }

  fall_off_Ne = optsc["fall_off_ne"].doc("outer radial fall off length BC for density [m]")
    .withDefault<BoutReal>(-1);
  fall_off_Pe = optsc["fall_off_pe"].doc("outer radial fall off length BC for electron pressure [m]")
    .withDefault<BoutReal>(-1);
  fall_off_Pi = optsc["fall_off_pi"].doc("outer radial fall off length BC for ion pressure [m]")
    .withDefault<BoutReal>(-1);

  fall_off = fall_off_Ne > 0 || fall_off_Pe > 0 || fall_off_Pi > 0;
  if (fall_off) {
    xdist = BoutNaN;
    Field3D R, Z;
    mesh->get(R, "R");
    mesh->get(Z, "Z");
    const int x0 = mesh->xend;
    for (int y = mesh->ystart; y <= mesh->yend; ++y) {
      for (int z = mesh->zstart; z <= mesh->zend; ++z) {
        for (int x = mesh->xend + 1; x < mesh->LocalNx; ++x) {
          xdist(x, y, z) =
              sqrt(SQ(R(x0, y, z) - R(x, y, z)) + SQ(Z(x0, y, z) - Z(x, y, z)));
          // printf("%d %d %d %e\n", x,y,z,xdist(x,y,z));
        }
      }
    }
  }

  evolve_vort = optsc["evolve_vort"].doc("Evolve Vorticity?")
    .withDefault<bool>(true);

  if (relaxation) {
    SOLVE_FOR(phi_1);
    EvolvingVars.add(phi_1);
    if (output_ddt) {
      SAVE_REPEAT(ddt(phi_1));
    }
  }

  if ((j_par || j_diamag || relaxation) && evolve_vort) {
    // Have a source of vorticity
    solver->add(Vort, "Vort");
    EvolvingVars.add(Vort);
    if (output_ddt) {
      SAVE_REPEAT(ddt(Vort));
    }
  } else {
    zero_all(Vort);
  }

  if (electromagnetic || FiniteElMass) {
    solver->add(VePsi, "VePsi");
    EvolvingVars.add(VePsi);
    if (output_ddt) {
      SAVE_REPEAT(ddt(VePsi));
    }
  } else {
    // If both electrostatic and zero electron mass,
    // then Ohm's law has no time-derivative terms,
    // but is calculated from other evolving quantities
    zero_all(VePsi);
  }

  if (ion_velocity) {
    solver->add(NVi, "NVi");
    EvolvingVars.add(NVi);
    if (output_ddt) {
      SAVE_REPEAT(ddt(NVi));
    }
  } else {
    zero_all(NVi);
  }

  if (verbose) {
    SAVE_REPEAT(Ti);
    if (electron_ion_transfer && evolve_ti) {
      SAVE_REPEAT(Wi);
    }
    if (ion_velocity) {
      SAVE_REPEAT(Vi);
    }
  }

  OPTION(optsc, adapt_source, false);
  if (adapt_source) {
    // Adaptive sources to match profiles

    // PI controller, including an integrated difference term
    OPTION(optsc, source_p, 1e-2);
    OPTION(optsc, source_i, 1e-6);

    Coordinates::FieldMetric Snsave = copy(Sn);
    Coordinates::FieldMetric Spesave = copy(Spe);
    Coordinates::FieldMetric Spisave = copy(Spi);
    SOLVE_FOR(Sn, Spe, Spi);
    Sn = Snsave;
    Spe = Spesave;
    Spi = Spisave;
  } else {
    SAVE_ONCE(Sn, Spe, Spi);
  }

  /////////////////////////////////////////////////////////
  // Load metric tensor from the mesh, passing length and B
  // field normalisations
  Coordinates *coord = mesh->getCoordinates();
  // To use non-orthogonal metric
  // Normalise
  coord->Bxy /= Bnorm;
  // Metric is in grid file - just need to normalise

  // coord->dx /= rho_s0;
  // coord->dy /= rho_s0;
  // coord->dz /= rho_s0;

  // CONTRAVARIANT
  mul_all_inp(coord->g11, rho_s0 * rho_s0);
  mul_all_inp(coord->g22, rho_s0 * rho_s0);
  mul_all_inp(coord->g33, rho_s0 * rho_s0);
  mul_all_inp(coord->g12, rho_s0 * rho_s0);
  mul_all_inp(coord->g13, rho_s0 * rho_s0);
  mul_all_inp(coord->g23, rho_s0 * rho_s0);

  // Jacobi matrix
  div_all_inp(coord->J, rho_s0 * rho_s0 * rho_s0);

  // LIKE IN D'haeseleer

  // subscripts = ()_i -> covariant

  // superscripts = ()^j -> contravariant

  // COVARIANT
  div_all_inp(coord->g_11, rho_s0 * rho_s0);
  div_all_inp(coord->g_22, rho_s0 * rho_s0); // In m^2
  div_all_inp(coord->g_33, rho_s0 * rho_s0);
  div_all_inp(coord->g_12, rho_s0 * rho_s0);
  div_all_inp(coord->g_13, rho_s0 * rho_s0);
  div_all_inp(coord->g_23, rho_s0 * rho_s0);

  coord->geometry(); // Calculate other metrics

  _FCIDiv_a_Grad_perp = std::make_unique<FCI::dagp_fv>(*mesh);
  *_FCIDiv_a_Grad_perp *= rho_s0;

  if (Options::root()["mesh:paralleltransform"]["type"].as<std::string>() == "fci") {
    fci_transform = true;
  }else{
    fci_transform = false;
  }
  ASSERT0(fci_transform);

  if(fci_transform){
    poloidal_flows = false;
    mesh->get(Bxyz, "B",1.0);
    mesh->get(coord->Bxy, "Bxy", 1.0);
    Bxyz /= Bnorm;
    coord->Bxy /= Bnorm;
    // mesh->communicate(Bxyz, coord->Bxy); // To get yup/ydown fields
    //  Note: A Neumann condition simplifies boundary conditions on fluxes
    //  where the condition e.g. on J should be on flux (J/B)

    auto logBxy = log(coord->Bxy);
    auto logBxyz = log(Bxyz);
    logBxy.applyBoundary("neumann");
    logBxyz.applyBoundary("neumann");
    mesh->communicate(logBxy, logBxyz);
    logBxy.applyParallelBoundary(parbc);
    logBxyz.applyParallelBoundary(parbc);
    output_info.write("Setting from log");
    coord->Bxy = exp_all(logBxy);
    Bxyz = exp_all(logBxyz);
    SAVE_ONCE(Bxyz);
    ASSERT1(min(Bxyz) > 0.0);

    fwd_bndry_mask = BoutMask(mesh, false);
    bwd_bndry_mask = BoutMask(mesh, false);
    for (const auto &bndry_par : mesh->getBoundariesPar(BoundaryParType::fwd)) {
      for (bndry_par->first(); !bndry_par->isDone(); bndry_par->next()) {
        fwd_bndry_mask[bndry_par->ind()] = true;
      }
    }
    for (const auto &bndry_par : mesh->getBoundariesPar(BoundaryParType::bwd)) {
      for (bndry_par->first(); !bndry_par->isDone(); bndry_par->next()) {
        bwd_bndry_mask[bndry_par->ind()] = true;
      }
    }

    bout::checkPositive(coord->Bxy, "f", "RGN_NOCORNERS");
    bout::checkPositive(coord->Bxy.yup(), "fyup", "RGN_YPAR_+1");
    bout::checkPositive(coord->Bxy.ydown(), "fdown", "RGN_YPAR_-1");
    logB = log(Bxyz);

    bracket_factor = sqrt(coord->g_22) / (coord->J * Bxyz);
    SAVE_ONCE(bracket_factor);
  }else{
    mesh->communicate(coord->Bxy);
    bracket_factor = sqrt(coord->g_22) / (coord->J * coord->Bxy);
    SAVE_ONCE(bracket_factor);
  }

  B12 = sqrt_all(coord->Bxy);     // B^(1/2)
  B32 = mul_all(B12, coord->Bxy); // B^(3/2)
  B42 = SQ_all(coord->Bxy);

  /////////////////////////////////////////////////////////
  // Neutral models

  TRACE("Initialising neutral models");
  neutrals = NeutralModel::create(solver, mesh, Options::root()["neutral"]);

  // Set normalisations
  if (neutrals) {
    neutrals->setNormalisation(Tnorm, Nnorm, Bnorm, rho_s0, Omega_ci);
  }

  /////////////////////////////////////////////////////////
  // Impurities
  TRACE("Impurities");

  impurity_adas = optsc["impurity_adas"]
                      .doc("Use Atomic++ interface to ADAS")
                      .withDefault<bool>(false);

  if (impurity_adas) {

    fimp = optsc["impurity_fraction"]
               .doc("Fixed fraction ADAS impurity, multiple of electron density")
               .withDefault(0.0);

    string impurity_species =
        optsc["impurity_species"]
            .doc("Short name of the ADAS species e.g. 'c' or 'ne'")
            .withDefault("c");

    impurity = new ImpuritySpecies(impurity_species);
  }

  carbon_fraction = optsc["carbon_fraction"]
          .doc("Include a fixed fraction carbon impurity. < 0 means none.")
          .withDefault(-1.);
  if (carbon_fraction > 0.0) {
    SAVE_REPEAT(Rzrad);
    SAVE_ONCE(carbon_fraction);
    carbon_rad = new HutchinsonCarbonRadiation();
  }

  if ((carbon_fraction > 0.0) || impurity_adas) {
    // Save impurity radiation
    SAVE_REPEAT(Rzrad);
  }

  /////////////////////////////////////////////////////////
  // Read profiles from the mesh
  TRACE("Reading profiles");

  Field3D NeMesh, TeMesh, TiMesh;
  if (mesh->get(NeMesh, "Ne0")) {
    // No Ne0. Try Ni0
    if (mesh->get(NeMesh, "Ni0")) {
      output << "WARNING: Neither Ne0 nor Ni0 found in mesh input\n";
    }
  }
  NeMesh *= 1e20; // Convert to m^-3

  NeMesh /= Nnorm; // Normalise

  if (mesh->get(TeMesh, "Te0")) {
    // No Te0
    output << "WARNING: Te0 not found in mesh\n";
    // Try to read Ti0
    if (mesh->get(TeMesh, "Ti0")) {
      // No Ti0 either
      output << "WARNING: No Te0 or Ti0. Setting TeMesh to 0.0\n";
      TeMesh = 0.0;
    }
  }

  TeMesh /= Tnorm; // Normalise

  if (mesh->get(TiMesh, "Ti0")) {
    // No Ti0
    output << "WARNING: Ti0 not found in mesh. Setting to TeMesh\n";
    TiMesh = TeMesh;
  }
  TiMesh /= Tnorm; // Normalise
  PiTarget = NeMesh * TiMesh;

  NeTarget = NeMesh;
  PeTarget = NeMesh * TeMesh;

  if (!restarting && !ramp_mesh) {
    if (optsc["startprofiles"].withDefault(true)) {
      Ne += NeMesh; // Add profiles in the mesh file

      Pe += NeMesh * TeMesh;

      Pi += NeMesh * TiMesh;
      // Check for negatives
      if (min(Pi, true) < 0.0) {
        throw BoutException("Starting ion pressure is negative");
      }
      if (max(Pi, true) < 1e-5) {
        throw BoutException("Starting ion pressure is too small");
      }
      mesh->communicateXZ(Pi);
    }

    // Check for negatives
    if (min(Ne, true) < 0.0) {
      throw BoutException("Starting density is negative");
    }
    if (max(Ne, true) < 1e-5) {
      throw BoutException("Starting density is too small");
    }

    if (min(Pe, true) < 0.0) {
      throw BoutException("Starting pressure is negative");
    }
    if (max(Pe, true) < 1e-5) {
      throw BoutException("Starting pressure is too small");
    }

    mesh->communicateXZ(Ne, Pe);
  }

  /////////////////////////////////////////////////////////
  // Sources (after metric)

  // Multiply sources by g11
  OPTION(optsc, source_vary_g11, false);
  if (source_vary_g11) {
    // Average metric tensor component
    g11norm = coord->g11 / averageY(coord->g11);

    NeSource *= g11norm;
    PeSource *= g11norm;
    PiSource *= g11norm;
  }

  /////////////////////////////////////////////////////////
  // Read curvature components
  TRACE("Reading curvature");

  try {
    Curlb_B.covariant = false; // Contravariant
    mesh->get(Curlb_B, "bxcv");
    // SAVE_ONCE(Curlb_B);
  } catch (BoutException &e) {
    try {
      // May be 2D, reading as 3D
      Vector2D curv2d;
      curv2d.covariant = false;
      mesh->get(curv2d, "bxcv");
      Curlb_B = curv2d;
    } catch (BoutException &e) {
      if (j_diamag) {
        // Need curvature
        throw;
      } else {
        output_warn.write("No curvature vector in input grid");
        Curlb_B = 0.0;
      }
    }
  }

  if (Options::root()["mesh:paralleltransform"]["type"].as<std::string>() == "shifted") {
    Field2D I;
    mesh->get(I, "sinty");
    Curlb_B.z += I * Curlb_B.x;
  }

  Curlb_B.x /= Bnorm;
  Curlb_B.y *= rho_s0 * rho_s0;
  Curlb_B.z *= rho_s0 * rho_s0;

  Curlb_B *= 2. / coord->Bxy;

  //////////////////////////////////////////////////////////////
  // Electromagnetic fields

  opt["phiSolver"].setConditionallyUsed();

  optsc["newXZsolver"].setConditionallyUsed();
  optsc["split_n0"].setConditionallyUsed();
  optsc["split_n0_psi"].setConditionallyUsed();
  if (j_par | j_diamag | relaxation) {
    // Only needed if there are any currents
    SAVE_REPEAT(phi);
    if (relaxation){
      SAVE_REPEAT(phi_1);
    }

    if (j_par) {
      SAVE_REPEAT(Ve);

      if (electromagnetic)
        SAVE_REPEAT(psi);
    }

    OPTION(optsc, split_n0, false); // Split into n=0 and n~=0
    OPTION(optsc, split_n0_psi, split_n0);
    // Phi solver
    if (phi3d) {
#ifdef PHISOLVER
      phiSolver3D = Laplace3D::create();
#endif
    } else {
      if (!relaxation) {
        if (split_n0) {
          // Create an XY solver for n=0 component
          laplacexy = new LaplaceXY(mesh);
          // Set coefficients for Boussinesq solve
          laplacexy->setCoefs(1. / SQ(DC(coord->Bxy)), 0.0);
          phi2D = 0.0; // Starting guess
        }

        // Create an XZ solver
        OPTION(optsc, newXZsolver, false);
        if (newXZsolver) {
          // Test new LaplaceXZ solver
          newSolver = LaplaceXZ::create(bout::globals::mesh);
          // Set coefficients for Boussinesq solve
          newSolver->setCoefs(1. / SQ(coord->Bxy), Field3D(0.0));
        } else {
          // Use older Laplacian solver
          phiSolver = Laplacian::create(&opt["phiSolver"]);
          // Set coefficients for Boussinesq solve
          phiSolver->setCoefC(1./ SQ(coord->Bxy));
        }
      }else{
        // Relaxation method for steady-state potential
        phi_1 = 0.;
        phi_1.setBoundary("phi_1");
      }

      phi = 0.0;
      phi.setBoundary("phi"); // For y boundaries

      phi_boundary_relax = optsc["phi_boundary_relax"]
        .doc("Relax x boundaries of phi towards Neumann?")
        .withDefault<bool>(false);

      // Add phi to restart files so that the value in the boundaries
      // is restored on restart. This is done even when phi is not evolving,
      // so that phi can be saved and re-loaded
      restart.addOnce(phi, "phi");

      if (phi_boundary_relax) {

        if (!restarting) {
          // Start by setting to the sheath current = 0 boundary value

          Ne = floor(Ne, 1e-5);
          Te = floor(Pe / Ne, 0.1 / Tnorm);
          Ti = floor(Pi / Ne, 0.1 / Tnorm);

          phi.setBoundaryTo(DC(
                               (log(0.5 * sqrt(mi_me / PI)) + log(sqrt(Te / (Te + Ti)))) * Te));
        }

        // Set the last update time to -1, so it will reset
        // the first time RHS function is called
        phi_boundary_last_update = -1.;

        phi_boundary_timescale = optsc["phi_boundary_timescale"]
          .doc("Timescale for phi boundary relaxation [seconds]")
          .withDefault(1e-4)
          * Omega_ci; // Normalise to internal time units
      }

      // Apar (Psi) solver
      aparSolver = Laplacian::create(&opt["aparSolver"]);
      if (split_n0_psi) {
        // Use another XY solver for n=0 psi component
        aparXY = new LaplaceXY(mesh);
        psi2D = 0.0;
      }

      Ve.setBoundary("Ve");
      nu.setBoundary("nu");
      Jpar.setBoundary("Jpar");

      psi = 0.0;
    }
  }
  nu = 0.0;
  kappa_epar = 0.0;
  kappa_ipar = 0.0;
  Dn = 0.0;

  SAVE_REPEAT(a,b,d);
  SAVE_REPEAT(Te, Ti);

  if (verbose) {
    // Save additional fields
    SAVE_REPEAT(Jpar); // Parallel current

    SAVE_REPEAT(tau_e, tau_i);

    SAVE_REPEAT(kappa_epar); // Parallel electron heat conductivity
    SAVE_REPEAT(kappa_ipar); // Parallel ion heat conductivity

    SAVE_REPEAT(nu);

    /*
    if (resistivity) {
      SAVE_REPEAT(nu); // Parallel resistivity
    }
    */

    // SAVE_REPEAT2(wall_flux, wall_power);

    if (ion_viscosity) {
      // Ion parallel stress tensor
      SAVE_REPEAT(Pi_ci, Pi_ciperp, Pi_cipar);
    }

    // Sources added to Ne, Pe and Pi equations
    SAVE_REPEAT(NeSource, PeSource, PiSource);
  }

  zero_all(phi);
  zero_all(psi);

  if (relaxation) {
    zero_all(phi_1);
  }
  // Preconditioner
  setPrecon((preconfunc)&Hermes::precon);

  if (evolve_te && evolve_ti && parallel_sheaths){
    SAVE_REPEAT(sheath_dpe, sheath_dpi);
  }
  // Magnetic field in boundary
  auto& Bxy = mesh->getCoordinates()->Bxy;

  for (RangeIterator r = mesh->iterateBndryLowerY(); !r.isDone(); r++) {
    for (int jz = 0; jz < mesh->LocalNz; jz++) {
      Bxy.ydown()(r.ind, mesh->ystart - 1, jz) = Bxy(r.ind, mesh->ystart, jz);
      Bxy(r.ind, mesh->ystart - 1, jz) = Bxy(r.ind, mesh->ystart, jz);
    }
  }
  for (RangeIterator r = mesh->iterateBndryUpperY(); !r.isDone(); r++) {
    for (int jz = 0; jz < mesh->LocalNz; jz++) {
      Bxy.yup()(r.ind, mesh->yend + 1, jz) = Bxy(r.ind, mesh->yend, jz);
      Bxy(r.ind, mesh->yend + 1, jz) = Bxy(r.ind, mesh->yend, jz);
    }
  }

  opt["Pn"].setConditionallyUsed();
  opt["Nn"].setConditionallyUsed();
  opt["NVn"].setConditionallyUsed();
  opt["Pe"].setConditionallyUsed();
  opt["Pi"].setConditionallyUsed();
  opt["Vn"].setConditionallyUsed();
  opt["Vn_x"].setConditionallyUsed();
  opt["Vn_y"].setConditionallyUsed();
  opt["Vn_z"].setConditionallyUsed();
  opt["phi"].setConditionallyUsed();
  opt["phiSolver"].setConditionallyUsed();
  opt["Vort"].setConditionallyUsed();
  opt["VePsi"].setConditionallyUsed();
  optsc["neutral_gamma"].setConditionallyUsed();

  alloc_all(Te);
  alloc_all(Ti);
  alloc_all(Vi);
  alloc_all(a);
  alloc_all(b);
  alloc_all(d);
  return 0;
}

int Hermes::rhs(BoutReal t) {
  if (show_timesteps) {
    printf("TIME = %e\r", t);
  }

  Coordinates *coord = mesh->getCoordinates();
  if (!evolve_plasma) {
    Ne = 0.0;
    Pe = 0.0;
    Pi = 0.0;
    Vort = 0.0;
    VePsi = 0.0;
    NVi = 0.0;
    sheath_model = 0;
  }

  if (fall_off and mesh->lastX()) {
    auto coord = mesh->getCoordinates();
    for (int y = mesh->ystart ; y <= mesh->yend ; ++y) {
      for (int z = mesh->zstart; z <= mesh->zend; ++z) {
        for (int x = mesh->xend + 1; x < mesh->LocalNx; ++x) {
          if (fall_off_Ne > 0) {
            const auto fac = exp(-xdist(x, y, z) / fall_off_Ne);
            // printf("Ne %d %d %d %e -> %e\n", x, y, z, fac, Ne(mesh->xend, y,
            // z) * fac);
            Ne(x, y, z) = Ne(mesh->xend, y, z) * fac;
          }
          if (fall_off_Pe > 0) {
            const auto fac = exp(-xdist(x, y, z) / fall_off_Pe);
            Pe(x, y, z) = Pe(mesh->xend, y, z) * fac;
          }
          if (fall_off_Pi > 0) {
            const auto fac = exp(-xdist(x, y, z) / fall_off_Pi);
            Pi(x, y, z) = Pi(mesh->xend, y, z) * fac;
          }
        }
      }
    }
  }
  // Communicate evolving variables
  // Note: Parallel slices are not calculated because parallel derivatives
  // are calculated using field aligned quantities
  mesh->communicate(EvolvingVars);
  Ne.applyParallelBoundary();
  Vort.applyParallelBoundary();
  if (evolve_te){
    Pe.applyParallelBoundary();
  }
  if (evolve_ti){
    Pi.applyParallelBoundary();
  }
  NVi.applyParallelBoundary();
  if (relaxation) {
    phi_1.applyParallelBoundary();
  }

  // Are there any currents? If not, then there is no source
  // for vorticity, phi = 0 and jpar = 0
  bool currents = j_par | j_diamag;

  // Local sound speed. Used for parallel advection operator
  // Assumes isothermal electrons, adiabatic ions
  // The factor scale_num_cs can be used to test sensitity
  Field3D sound_speed;
  sound_speed.allocate();

  alloc_all(Te);
  alloc_all(Ti);
  alloc_all(Vi);
  alloc_all(Pi);
  alloc_all(Pe);
  BOUT_FOR(i, Ne.getRegion("RGN_ALL")) {
    // Field3D Ne = floor_all(Ne, 1e-5);
    floor_all(Ne, 1e-5, i);

    if (!evolve_te) {
      copy_all(Pe, Ne, i); // Fixed electron temperature
    }

    div_all(Te, Pe, Ne, i);
    // ASSERT0(Te[i] > 1e-10);
    /// printf("%f\n", Te[i]);
    div_all(Vi, NVi, Ne, i);

    floor_all(Te, 0.1 / Tnorm, i);
    // ASSERT0(Te[i] > 1e-10);

    mul_all(Pe, Te, Ne, i);

    if (!evolve_ti) {
      copy_all(Pi, Ne, i); // Fixed ion temperature
    }

    div_all(Ti, Pi, Ne, i);
    floor_all(Ti, 0.1 / Tnorm, i);
    mul_all(Pi, Ti, Ne, i);
    div_all(Te, Pe, Ne, i);
    // ASSERT0(Te[i] > 1e-10);

    sound_speed[i] = scale_num_cs * sqrt(Te[i] + Ti[i] * (5. / 3));
    if (floor_num_cs > 0.0) {
      // Apply a floor function to the sound speed
      sound_speed[i] = floor(sound_speed[i], floor_num_cs);
    }
  }

  // Set radial boundary conditions on Te, Ti, Vi
  //
  if (mesh->firstX()) {
    for (int j = mesh->ystart; j <= mesh->yend; j++) {
      for (int k = 0; k < mesh->LocalNz; k++) {
        BoutReal ne_bndry = 0.5 * (Ne(1, j, k) + Ne(2, j, k));
        if (ne_bndry < 1e-5)
          ne_bndry = 1e-5;
        BoutReal pe_bndry = 0.5 * (Pe(1, j, k) + Pe(2, j, k));
        BoutReal pi_bndry = 0.5 * (Pi(1, j, k) + Pi(2, j, k));

        BoutReal te_bndry = pe_bndry / ne_bndry;
        BoutReal ti_bndry = pi_bndry / ne_bndry;

        Te(1, j, k) = 2. * te_bndry - Te(2, j, k);
        Ti(1, j, k) = 2. * ti_bndry - Ti(2, j, k);
        Vi(0, j, k) = Vi(1, j, k) = Vi(2, j, k);

        if (te_bndry < 0.1 / Tnorm)
          te_bndry = 0.1 / Tnorm;
        if (ti_bndry < 0.1 / Tnorm)
          ti_bndry = 0.1 / Tnorm;

        Te(1, j, k) = 2. * te_bndry - Te(2, j, k);
        Ti(1, j, k) = 2. * ti_bndry - Ti(2, j, k);
      }
    }
  }
  if (mesh->lastX()) {
    int n = mesh->LocalNx;
    for (int j = mesh->ystart; j <= mesh->yend; j++) {
      for (int k = 0; k < mesh->LocalNz; k++) {
        BoutReal ne_bndry = 0.5 * (Ne(n - 1, j, k) + Ne(n - 2, j, k));
        if (ne_bndry < 1e-5)
          ne_bndry = 1e-5;
        BoutReal pe_bndry = 0.5 * (Pe(n - 1, j, k) + Pe(n - 2, j, k));
        BoutReal pi_bndry = 0.5 * (Pi(n - 1, j, k) + Pi(n - 2, j, k));

        BoutReal te_bndry = pe_bndry / ne_bndry;
        BoutReal ti_bndry = pi_bndry / ne_bndry;

        Te(n - 1, j, k) = 2. * te_bndry - Te(n - 2, j, k);
        Ti(n - 1, j, k) = 2. * ti_bndry - Ti(n - 2, j, k);
        Vi(n - 1, j, k) = Vi(n - 2, j, k);

        if (te_bndry < 0.1 / Tnorm)
          te_bndry = 0.1 / Tnorm;
        if (ti_bndry < 0.1 / Tnorm)
          ti_bndry = 0.1 / Tnorm;

        Te(n - 1, j, k) = 2. * te_bndry - Te(n - 2, j, k);
        Ti(n - 1, j, k) = 2. * ti_bndry - Ti(n - 2, j, k);
      }
    }
  }
  sound_speed.applyBoundary("neumann");

  //////////////////////////////////////////////////////////////
  // Calculate electrostatic potential phi
  //
  //

  TRACE("Electrostatic potential");
  if (!currents && !relaxation) {
    // Disabling electric fields
    // phi = 0.0; // Already set in initialisation
  } else {
    // Solve phi from Vorticity

    // Set the boundary of phi. Both 2D and 3D fields are kept, though the 3D field
    // is constant in Z. This is for efficiency, to reduce the number of conversions.
    // Note: For now the boundary values are all at the midpoint,
    //       and only phi is considered, not phi + Pi which is handled in Boussinesq solves
    Field2D phi_boundary2d;
    Field3D phi_boundary3d;

    if (phi_boundary_relax) {
      // Update the boundary regions by relaxing towards zero gradient
      // on a given timescale.

      if (phi_boundary_last_update < 0.0) {
        // First time this has been called.
        phi_boundary_last_update = t;

      } else if (t > phi_boundary_last_update) {
        // Only update if time has advanced
        // Uses an exponential decay of the weighting of the value in the boundary
        // so that the solution is well behaved for arbitrary steps
        BoutReal weight = exp(-(t - phi_boundary_last_update) / phi_boundary_timescale);
        // output.write("weight: {}\n", weight);
        phi_boundary_last_update = t;

        if (mesh->firstX()) {
          for (int j = mesh->ystart; j <= mesh->yend; j++) {
            BoutReal phivalue = 0.0;
            for (int k = 0; k < mesh->LocalNz; k++) {
              phivalue += phi(mesh->xstart, j, k);
            }
            phivalue /= mesh->LocalNz; // Average in Z of point next to boundary

            for (int k = 0; k < mesh->LocalNz; k++) {
              phivalue = phi(mesh->xstart, j, k);
              // Old value of phi at boundary
              BoutReal oldvalue =  phi(mesh->xstart,j,k);//0.5 * (phi(mesh->xstart - 1, j, k) + phi(mesh->xstart, j, k));

              // New value of phi at boundary, relaxing towards phivalue
              BoutReal newvalue = weight * oldvalue + (1. - weight) * phivalue;

              // Set phi at the boundary to this value
              phi(mesh->xstart - 1, j, k) = 2.*newvalue - phi(mesh->xstart, j, k);

              // Note: This seems to make a difference, but don't know why.
              // Without this, get convergence failures with no apparent instability
              // (all fields apparently smooth, well behaved)
              phi(mesh->xstart - 2, j, k) = phi(mesh->xstart - 1, j, k);
            }
          }
        }

        if (mesh->lastX()) {
          for (int j = mesh->ystart; j <= mesh->yend; j++) {
            BoutReal phivalue = 0.0;
            for (int k = 0; k < mesh->LocalNz; k++) {
              phivalue += phi(mesh->xend, j, k);
            }
            phivalue /= mesh->LocalNz; // Average in Z of point next to boundary

            for (int k = 0; k < mesh->LocalNz; k++) {
              phivalue = phi(mesh->xend, j, k);


              // Old value of phi at boundary
              BoutReal oldvalue = phi(mesh->xend,j,k); //0.5 * (phi(mesh->xend + 1, j, k) + phi(mesh->xend, j, k));

              // New value of phi at boundary, relaxing towards phivalue
              BoutReal newvalue = weight * oldvalue + (1. - weight) * phivalue;

              // Set phi at the boundary to this value
              phi(mesh->xend + 1, j, k) = 2.* newvalue - phi(mesh->xend, j, k);

              // Note: This seems to make a difference, but don't know why.
              // Without this, get convergence failures with no apparent instability
              // (all fields apparently smooth, well behaved)
              phi(mesh->xend + 2, j, k) = phi(mesh->xend + 1, j, k);
            }
          }
        }
      }
      phi_boundary3d = phi;
    } else {
      // phi_boundary_relax = false
      //
      // Set boundary from temperature, to be consistent with j=0 at sheath

      // Sheath multiplier Te -> phi (2.84522 for Deuterium if Ti = 0)
      phi_boundary2d =
          DC((log(0.5 * sqrt(mi_me / PI)) + log(sqrt(Te / (Te + Ti)))) * Te);

      phi_boundary3d = phi_boundary2d;
    }

    if (phi3d) {
#ifdef PHISOLVER
      phiSolver3D->setCoefC(Ne / SQ(coord->Bxy));
      // phi.setBoundaryTo(3.*Te);
      if (mesh->lastX()) {
        for (int i = mesh->xend + 1; i < mesh->LocalNx; i++)
          for (int j = mesh->ystart; j <= mesh->yend; j++)
            for (int k = 0; k < mesh->LocalNz; k++) {
              phi(i, j, k) = 3. * Te(i, j, k);
            }
      }
      phi = phiSolver3D->solve(Vort, phi);
#endif
    } else {
      // Phi flags should be set in BOUT.inp
      // phiSolver->setInnerBoundaryFlags(INVERT_DC_GRAD);
      // phiSolver->setOuterBoundaryFlags(INVERT_SET);

      if (boussinesq) {

        // Update boundary conditions. Two issues:
        // 1) Solving here for phi + Pi, and then subtracting Pi from the result
        //    The boundary values should therefore include Pi
        // 2) The INVERT_SET flag takes the value in the guard (boundary) cell
        //    and sets the boundary between cells to this value.
        //    This shift by 1/2 grid cell is important.

        if (mesh->firstX()) {
          for (int j = mesh->ystart; j <= mesh->yend; j++) {
            for (int k = 0; k < mesh->LocalNz; k++) {
              // Average phi + Pi at the boundary, and set the boundary cell
              // to this value. The phi solver will then put the value back
              // onto the cell mid-point
              phi_boundary3d(mesh->xstart - 1, j, k) =
                  0.5
                  * (phi_boundary3d(mesh->xstart - 1, j, k) +
                     phi_boundary3d(mesh->xstart, j, k) +
                     Pi(mesh->xstart - 1, j, k) +
                     Pi(mesh->xstart, j, k));
            }
          }
        }

        if (mesh->lastX()) {
          for (int j = mesh->ystart; j <= mesh->yend; j++) {
            for (int k = 0; k < mesh->LocalNz; k++) {
              phi_boundary3d(mesh->xend + 1, j, k) =
                  0.5
                  * (phi_boundary3d(mesh->xend + 1, j, k) +
                     phi_boundary3d(mesh->xend, j, k) +
                     Pi(mesh->xend + 1, j, k) +
                     Pi(mesh->xend, j, k));
            }
          }
        }
        if (relaxation) {
          phi = div_all(phi_1,lambda_2);
        } else{
          if (split_n0) {
            ////////////////////////////////////////////
            // Boussinesq, split
            // Split into axisymmetric and non-axisymmetric components
            Field2D Vort2D = DC(Vort); // n=0 component

            if (!phi_boundary2d.isAllocated()) {
              // Make sure that the 2D boundary field is set
              phi_boundary2d = DC(phi_boundary3d);
            }

            // Set the boundary
            phi2D.setBoundaryTo(phi_boundary2d);

            phi2D = laplacexy->solve(Vort2D, phi2D);

            // Solve non-axisymmetric part using X-Z solver
            if (newXZsolver) {
              newSolver->setCoefs(1. / SQ(coord->Bxy), 0.0);
              phi = newSolver->solve(Vort - Vort2D,
                                     // Second argument is initial guess. Use current phi, and update boundary
                                     withBoundary(phi + Pi - phi2D, // Value in domain
                                                  phi_boundary3d - phi_boundary2d)); // boundary
            } else {
              phiSolver->setCoefC(div_all(1. , mul_all(coord->Bxy, coord->Bxy)));
              phi = phiSolver->solve((Vort - Vort2D) * SQ(coord->Bxy),
                                     phi_boundary3d - phi_boundary2d); // Note: non-zero due to Pi variation
            }
            phi += phi2D; // Add axisymmetric part
          } else {
            ////////////////////////////////////////////
            // Boussinesq, non-split
            // Solve all components using X-Z solver

            if (newXZsolver) {
              // Use the new LaplaceXZ solver
              // newSolver->setCoefs(1./SQ(coord->Bxy), 0.0); // Set when initialised
              phi = newSolver->solve(Vort, phi + Pi);
            } else {
              // Use older Laplacian solver
              // phiSolver->setCoefC(1./SQ(coord->Bxy)); // Set when initialised
              mesh->communicate(phi_boundary3d);
              phi = phiSolver->solve(mul_all(Vort , mul_all(coord->Bxy, coord->Bxy)), phi_boundary3d);//_boundary3d);
              //phi = phiSolver->solve(Vort, phi);
            }
          }
        }
        // Hot ion term in vorticity
        mesh->communicate(phi);
        phi.applyParallelBoundary(parbc);
        phi = sub_all(phi, Pi);
      } else {
        ////////////////////////////////////////////
        // Non-Boussinesq
        //
        throw BoutException("Non-Boussinesq not implemented yet");
      }
    }
  }

  //////////////////////////////////////////////////////////////
  // Calculate perturbed magnetic field psi
  TRACE("Calculating psi");

  if (!currents) {
    // No magnetic fields or currents
    zero_all(psi);
    zero_all(Jpar);
    zero_all(Ve); // Ve will be set after the sheath boundaries below
  } else {
    // Calculate electomagnetic potential psi from VePsi
    // VePsi = Ve - Vi + 0.5 * mi_me * beta_e * psi
    // where the first term comes from finite electron mass, and the second
    // from the parallel component of the electric field
    // Note that psi is -A_|| so Jpar = Delp2(psi)
    if (electromagnetic) {
      if (FiniteElMass) {
        // Solve Helmholtz equation for psi

        aparSolver->setCoefA(-Ne*0.5*mi_me*beta_e);
        aparSolver->setCoefC(Field3D(1.0));
        // aparSolver->setCoefs(1.0, -Ne*0.5*mi_me*beta_e);

        psi = aparSolver->solve(Field3D(-Ne * VePsi), Field3D(psi));
        // psi = aparSolver->solve(-Ne*VePsi, psi);
        mesh->communicate(psi);
        psi.applyParallelBoundary(parbc);

        // Ve = VePsi - 0.5 * beta_e * mi_me * psi + Vi;
        Ve = VePsi - 0.5 * beta_e * mi_me * psi + Vi;
        // Field3D vepsi_betapsi = sub_all(VePsi , 0.5 * beta_e * mi_me * psi);
        // mesh->communicate(vepsi_betapsi);
        // vepsi_betapsi.applyParallelBoundary(parbc);
        // Ve = add_all(vepsi_betapsi , Vi);

        Ve.applyBoundary(t);
        mesh->communicate(Ve, psi);
        Ve.applyParallelBoundary(parbc);

        Jpar = mul_all(Ne, sub_all(Vi, Ve));
        mesh->communicate(Jpar);
        Jpar.applyParallelBoundary(parbc);
        // Jpar.applyBoundary();
      } else {
        // Zero electron mass
        // No Ve term in VePsi, only electromagnetic term
        psi = div_all(VePsi, 0.5 * mi_me * beta_e);

        // Ve = (NVi - Delp2(psi)) / Ne;
	Field3D one;
	set_all(one, 1.0);
	Jpar = FCIDiv_a_Grad_perp(one, psi);

        mesh->communicate(Jpar);

        Jpar.applyBoundary(t);
        Ve = div_all(sub_all(NVi, Jpar), Ne);
      }

      // psi -= psi.DC(); // Remove toroidal average, only keep fluctuations
    } else {
      // Electrostatic
      zero_all(psi);
      if (FiniteElMass) {
        // No psi contribution to VePsi
        Ve = add_all(VePsi , Vi);
      } else {
        // Zero electron mass and electrostatic.
        // Special case where Ohm's law has no time-derivatives
        // mesh->communicate(phi,Pe);

        // tau_e = (Cs0 / rho_s0) * tau_e0 * pow(Te, 1.5) / Ne;
        Field3D Te32= pow(Te,1.5);
        mesh->communicate(Te32, Ne, phi, Pe, Vi);
        tau_e =
            div_all(mul_all(mul_all(div_all(Cs0, rho_s0), tau_e0), Te32), Ne);
        nu = resistivity_multiply / (1.96 * tau_e * mi_me);
        mesh->communicate(nu);

        Field3D gparpe = Grad_par(Pe);
        Field3D gparphi = Grad_par(phi);
        gparpe.applyBoundary("neumann");
        gparphi.applyBoundary("neumann");
        mesh->communicate(gparphi, gparpe);
        Field3D gparpe_n = div_all(gparpe, Ne);

        Field3D gparphi_gparpe_nu = div_all(sub_all(gparphi, gparpe_n), nu);

        // Ve = add_all(Vi,gparphi);
        Ve = add_all(Vi, gparphi_gparpe_nu);
        if (thermal_force) {
          Ve -= 0.71 * Grad_parP(Te) / nu;
        }
      }

      Ve.applyBoundary(t);
      // Communicate auxilliary variables
      mesh->communicate(Ve);
      Field3D neve = mul_all(Ne,Ve);
      mesh->communicate(NVi,neve);
      Jpar = sub_all(NVi, neve);

    }
    // Ve -= Jpar0 / Ne; // Equilibrium current density
  }

  //////////////////////////////////////////////////////////////
  // Sheath boundary conditions on Y up and Y down
  //
  // NOTE: Have to apply parallel boundary conditions in field aligned coordinates
  // so shift to and then from field aligned

  TRACE("Sheath boundaries");
  if (parallel_sheaths){
    switch (par_sheath_model) {
    case 0 :{
      for (const auto &bndry_par :
           mesh->getBoundariesPar(BoundaryParType::xout)) {
        for (bndry_par->first(); !bndry_par->isDone(); bndry_par->next()) {
          int x = bndry_par->ind().x();
          int y = bndry_par->ind().y();
          int z = bndry_par->ind().z();
          // Zero-gradient density
          BoutReal nesheath = floor(Ne(x, y, z), 0.0);

          // Temperature at the sheath entrance
          BoutReal tesheath = floor(Te(x, y, z), 0.0);
          BoutReal tisheath = floor(Ti(x, y, z), 0.0);

          // Zero-gradient potential
          BoutReal phisheath = phi(x, y, z);
          BoutReal visheath = bndry_par->dir * sqrt(tisheath + tesheath);

          if (sheath_allow_supersonic) {
            if (bndry_par->dir == 1){
              if (Vi(x, y, z) > visheath){
                // If plasma is faster, go to plasma velocity
                visheath = Vi(x, y, z);
              }
            } else {
              if (Vi(x, y, z) < visheath){
                visheath = Vi(x, y, z);
              }
            }
          }

          // Sheath current
          // Note that phi/Te >= 0.0 since for phi < 0
          // vesheath is the electron saturation current
          BoutReal phi_te =
            floor(phisheath / tesheath, 0.0);

          BoutReal vesheath =
            bndry_par->dir * sqrt(tesheath) * (sqrt(mi_me) / (2. * sqrt(PI))) * exp(-phi_te);

          // J = n*(Vi - Ve)
          BoutReal jsheath = nesheath * (visheath - vesheath);
          if (nesheath < 1e-10) {
            vesheath = visheath;
            jsheath = 0.0;
          }

          // Neumann conditions
          Ne.ynext(bndry_par->dir)(x, y+bndry_par->dir, z) = nesheath;
          phi.ynext(bndry_par->dir)(x, y+bndry_par->dir, z) = phisheath;
          Vort.ynext(bndry_par->dir)(x, y+bndry_par->dir, z) = Vort(x, y, z);

          // Here zero-gradient Te, heat flux applied later
          Te.ynext(bndry_par->dir)(x, y+bndry_par->dir, z) = Te(x, y, z);
          Ti.ynext(bndry_par->dir)(x, y+bndry_par->dir, z) = Ti(x, y, z);

          Pe.ynext(bndry_par->dir)(x, y+bndry_par->dir, z) = Pe(x, y, z);
          Pi.ynext(bndry_par->dir)(x, y+bndry_par->dir, z) = Pi(x, y, z);

          // Dirichlet conditions
          Vi.ynext(bndry_par->dir)(x, y+bndry_par->dir, z) = visheath;//2. * visheath - Vi(x, y, z);
          if (par_sheath_ve){
            Ve.ynext(bndry_par->dir)(x, y+bndry_par->dir, z) = vesheath;//2. * vesheath - Ve(x, y, z);
          }
          Jpar.ynext(bndry_par->dir)(x, y+bndry_par->dir, z) = jsheath;
            // 2. * jsheath - Jpar(x, y, z);
          NVi.ynext(bndry_par->dir)(x, y+bndry_par->dir, z) = nesheath * visheath;//
            // 1. * nesheath * visheath;// - NVi(x, y, z);
        }
      }
      break;
    }
    case 1: { // insulating boundary      break;
      throw BoutException("Not implemented");
      break;
    }
    default: {
      throw BoutException("Not implemented");
      break;
    }
    }
  }


  if (!currents) {
    // No currents, so reset Ve to be equal to Vi
    // VePsi also reset, so saved in restart file correctly
    Ve = Vi;
    VePsi = Ve;
  }

  //////////////////////////////////////////////////////////////
  // Plasma quantities calculated.
  // At this point we have calculated all boundary conditions,
  // and auxilliary variables like jpar, phi, psi

  // Now can update the neutral gas model, so that we can use neutral
  // gas densities and interaction frequencies in the collision processes.

  // Neutral gas
  if (neutrals) {
    TRACE("Neutral gas model update");

    // Update neutral gas model
    neutrals->update(Ne, Te, Ti, Vi);
  }

  //////////////////////////////////////////////////////////////
  // Collisions and stress tensor
  TRACE("Collisions");

  const BoutReal tau_e1 = (Cs0 / rho_s0) * tau_e0;
  const BoutReal tau_i1 = (Cs0 / rho_s0) * tau_i0;

  Field3D neutral_rate;
  if (ion_neutral && ( neutrals || (ion_neutral_rate > 0.0))) {
    // Include ion-neutral collisions in collision time
    neutral_rate = neutrals ? neutrals->Fperp : ion_neutral_rate;
  }
  alloc_all(tau_e);
  alloc_all(tau_i);
  BOUT_FOR(i, Te.getRegion("RGN_ALL")) {
    // Normalised electron collision time
    // tau_e[i] = mul_all((Cs0 / rho_s0) * tau_e0, div_all(mul_all(Te,
    // sqrt_all(Te)), Ne));
    tau_e[i] = tau_e1 * (Te[i] * sqrt(Te[i]) / Ne[i]);
    tau_e.yup()[i] = tau_e1 * (Te.yup()[i] * sqrt(Te.yup()[i]) / Ne.yup()[i]);
    tau_e.ydown()[i] =
        tau_e1 * (Te.ydown()[i] * sqrt(Te.ydown()[i]) / Ne.ydown()[i]);

    // Normalised ion-ion collision time
    tau_i[i] = tau_i1 * (Ti[i] * sqrt(Ti[i])) / Ne[i];
    tau_i.yup()[i] = tau_i1 * (Ti.yup()[i] * sqrt(Ti.yup()[i])) / Ne.yup()[i];
    tau_i.ydown()[i] =
        tau_i1 * (Ti.ydown()[i] * sqrt(Ti.ydown()[i])) / Ne.ydown()[i];

    if (ion_neutral && (neutrals || (ion_neutral_rate > 0.0))) {
      // Include ion-neutral collisions in collision time
      // Add collision frequencies (1/tau_i + neutral rate)
      tau_i[i] = tau_i[i] / (1 + (tau_i[i] * neutral_rate[i]));
      tau_i.yup()[i] =
          tau_i.yup()[i] / (1 + (tau_i.yup()[i] * neutral_rate.yup()[i]));
      tau_i.ydown()[i] =
          tau_i.ydown()[i] / (1 + (tau_i.ydown()[i] * neutral_rate.ydown()[i]));
    }
  }
  // tau_e = mul_all((Cs0 / rho_s0) * tau_e0, div_all(mul_all(Te, sqrt_all(Te)),
  // Ne)); tau_i = mul_all((Cs0 / rho_s0) * tau_i0, div_all(mul_all(Ti,
  // sqrt_all(Ti)), Ne)); if (ion_neutral && (neutrals || (ion_neutral_rate >
  // 0.0))) {
  //   tau_i = div_all(tau_i, add_all(1, mul_all(tau_i, neutral_rate)));
  // }

  // Collisional damping (normalised)
  if (resistivity || (!electromagnetic && !FiniteElMass)) {
    // Need to calculate nu if electrostatic and zero electron mass
    nu = resistivity_multiply / (1.96 * tau_e * mi_me);

    if (electron_neutral && neutrals) {
      /*
       * Include electron-neutral collisions. These can dominate
       * the resistivity at low temperatures (~1eV)
       *
       * This assumes a fixed cross-section, independent of energy
       *
       */
      BoutReal a0 = PI*SQ(5.29e-11); // Cross-section [m^2]

      // Electron thermal speed
      Field3D vth_e = sqrt(mi_me*Te);

      // Electron-neutral collision rate
      Field3D nu_ne = vth_e * Nnorm * neutrals->getDensity() * a0 * rho_s0;

      // Add collision rate to the electron-ion rate
      nu += nu_ne;
    }

    if (resistivity_boundary_width > 0 &&
        ((mesh->getGlobalXIndex(mesh->xstart) - mesh->xstart)
         < resistivity_boundary_width)) {
      // A region near the boundary with different resistivity

      int imax = mesh->xstart + resistivity_boundary_width - 1 -
        (mesh->getGlobalXIndex(mesh->xstart) - mesh->xstart);
      if (imax > mesh->xend) {
        imax = mesh->xend;
      }
      int imin = mesh->xstart;
      if (!mesh->firstX()) {
        --imin; // Calculate in guard cells, for radial fluxes
      }
      for (int i = imin; i <= imax; ++i) {
        for (int j = mesh->ystart; j <= mesh->yend; ++j) {
          for (int k = 0; k < mesh->LocalNz; ++k) {
            nu(i, j, k) = resistivity_boundary;
          }
        }
      }
    }

    // Number of points in outer guard cells
    int nguard = mesh->LocalNx - mesh->xend - 1;

    if (resistivity_boundary_width > 0 &&
        (mesh->GlobalNx - nguard - mesh->getGlobalXIndex(mesh->xend) <=
         resistivity_boundary_width)) {
      // Outer boundary
      int imin =
        mesh->GlobalNx - nguard - resistivity_boundary_width - mesh->getGlobalXIndex(0);
      if (imin < mesh->xstart) {
        imin = mesh->xstart;
      }
      for (int i = imin; i <= mesh->xend; ++i) {
        for (int j = mesh->ystart; j <= mesh->yend; ++j) {
          for (int k = 0; k < mesh->LocalNz; ++k) {
            nu(i, j, k) = resistivity_boundary;
          }
        }
      }
    }
  }

  if (thermal_conduction || sinks) {
    // Braginskii expression for electron parallel conduction
    // kappa ~ n * v_th^2 * tau
    kappa_epar = mul_all(mul_all(mul_all(mul_all(3.16, mi_me), Te), Ne), tau_e);

    if (kappa_limit_alpha > 0.0) {
      /*
       * Flux limiter, as used in SOLPS.
       *
       * Calculate the heat flux from Spitzer-Harm and flux limit
       *
       * Typical value of alpha ~ 0.2 for electrons
       *
       * R.Schneider et al. Contrib. Plasma Phys. 46, No. 1-2, 3 – 191 (2006)
       * DOI 10.1002/ctpp.200610001
       */
       kappa_epar.applyBoundary("neumann");
       mesh->communicate(kappa_epar);
       kappa_epar.applyParallelBoundary(parbc);
       Field3D gradTe = Grad_parP(Te);
       mesh->communicate(gradTe);
       gradTe.applyParallelBoundary(parbc);
       Field3D Te32 = pow(Te,1.5);
       mesh->communicate(Te32);
       
       // Spitzer-Harm heat flux
       Field3D q_SH = mul_all(kappa_epar,gradTe);      
       Field3D q_fl = mul_all(kappa_limit_alpha,mul_all(sqrt(mi_me),mul_all(Ne,Te32)));
       Field3D one;
       set_all(one, 1.0);
       Field3D denom = one + abs(div_all(q_SH,q_fl));
       
       denom.applyBoundary("neumann");
       mesh->communicate(denom);
       denom.applyParallelBoundary(parbc);
       
       kappa_epar = div_all(kappa_epar,denom);
    }

    // Ion parallel heat conduction
    kappa_ipar = mul_all(mul_all(mul_all(3.9, Ti), Ne), tau_i);

    // Boundary conditions on heat conduction coefficients
    for (RangeIterator r = mesh->iterateBndryLowerY(); !r.isDone(); r++) {
      for (int jz = 0; jz < mesh->LocalNz; jz++) {
        ASSERT0(fci_transform == false);
        kappa_epar(r.ind, mesh->ystart - 1, jz) = kappa_epar(r.ind, mesh->ystart, jz);
        kappa_ipar(r.ind, mesh->ystart - 1, jz) = kappa_ipar(r.ind, mesh->ystart, jz);
      }
    }

    for (RangeIterator r = mesh->iterateBndryUpperY(); !r.isDone(); r++) {
      for (int jz = 0; jz < mesh->LocalNz; jz++) {
        ASSERT0(fci_transform == false);
        kappa_epar(r.ind, mesh->yend + 1, jz) = kappa_epar(r.ind, mesh->yend, jz);
        kappa_ipar(r.ind, mesh->yend + 1, jz) = kappa_ipar(r.ind, mesh->yend, jz);
      }
    }
  }

  if(currents){ nu.applyBoundary(t); }

  if (ion_viscosity) {
    ///////////////////////////////////////////////////////////
    // Ion stress tensor. Split into
    // Pi_ci = Pi_ciperp + Pi_cipar
    //
    // In the parallel ion momentum equation the Pi_cipar term
    // is solved as a parallel diffusion, so is treated separately
    // All other terms are added to Pi_ciperp, even if they are
    // not really parallel parts
    throw BoutException("Non-Boussinesq not implemented yet");

    ASSERT0(false); // not implemented - ask Brendan
  } // ion visocsity

  ///////////////////////////////////////////////////////////
  // Relaxation potential
  //
  if (relaxation) {
    TRACE("relaxation");
    Field3D inv_b2 = div_all(1 , mul_all(coord->Bxy , coord->Bxy));
    ddt(phi_1) = lambda_0 * lambda_2 *
                 ((1 / lambda_2 * FCIDiv_a_Grad_perp(inv_b2, phi_1) +
                   FCIDiv_a_Grad_perp(inv_b2, Pi)) -
                  Vort);
  }

  ///////////////////////////////////////////////////////////
  // Density
  // This is the electron density equation
  TRACE("density");

  if (currents) {
    // ExB drift, only if electric field is evolved
    // ddt(Ne) = bracket(Ne, phi, BRACKET_ARAKAWA) * bracket_factor;
    ddt(Ne) = SETNAME(-Div_n_bxGrad_f_B_XPPM(Ne, phi, ne_bndry_flux,
                                             poloidal_flows, true,
                                             bracket_factor)); // ExB drift
    // a += -Div_n_bxGrad_f_B_XPPM(Ne, phi, ne_bndry_flux, poloidal_flows,
    //                                      true) * bracket_factor; // ExB drift
  } else {
    ddt(Ne) = 0.0;
  }

  // Parallel flow
  if (parallel_flow) {
    // if (!fci_transform){
    //   if (currents) {
    //         // Parallel wave speed increased to electron sound speed
    //         // since electrostatic & electromagnetic waves are supported
    //         ddt(Ne) -= FV::Div_par(Ne, Ve, sqrt(mi_me) * sound_speed);
    //   }else {
    //         // Parallel wave speed is ion sound speed
    //         ddt(Ne) -= FV::Div_par(Ne, Ve, sound_speed);
    //   }
    // }else{

    check_all(Ne);

    if (!evolve_ni) {
      check_all(Ve);
      Field3D neve = mul_all(Ne, Ve);
      check_all(neve);
      ddt(Ne) -= SETNAME(Div_parP(neve));
    } else {
      check_all(Vi);
      Field3D nevi = mul_all(Ne, Vi);
      check_all(nevi);
      ddt(Ne) -= SETNAME(Div_parP(nevi));
    }

    //Skew-symmetric form

    // Field3D gparne = Grad_par(Ne);
    // Field3D dparve = Div_parP(Ve);
    // gparne.applyBoundary("neumann");
    // dparve.applyBoundary("neumann");
    // mesh->communicate(gparne,dparve);
    // gparne.applyParallelBoundary(parbc);
    // dparve.applyParallelBoundary(parbc);

    // check_all(gparne);
    // check_all(dparve);

    // ddt(Ne) -= 0.5 * (Div_par(neve) + mul_all(Ve,gparne) + mul_all(Ne,
    // dparve));
    // b -= 0.5 * (Div_par(neve) + mul_all(Ve,gparne) + mul_all(Ne,
    // dparve));
    // //ddt(Ne) -= 0.5 * (Div_par(neve) + Ve * Grad_par(Ne) + Ne *
    // Div_par(Ve)); a = -0.5 * (Div_parP(neve) + Ve * Grad_par(Ne) + Ne *
    // Div_par(Ve));
    // auto* coords = mesh->getCoordinates();
    // for(auto &i :Ne.getRegion("RGN_NOBNDRY")) {
    //   ddt(Ne)[i] -= (Ne.yup()[i.yp()] * Ve.yup()[i.yp()] /
    //   coords->Bxy[i.yp()]
    //               - Ne.ydown()[i.ym()] * Ve.ydown()[i.ym()] /
    //               coords->Bxy[i.ym()])
    //     * coords->Bxy[i] / (coords->dy[i] * sqrt(coords->g_22[i]));
    // }
  }

  if (j_diamag) {
    // Diamagnetic drift, formulated as a magnetic drift
    // i.e Grad-B + curvature drift
    if (!evolve_ni) {
      mesh->communicate(Pe);
      ddt(Ne) -= SETNAME(fci_curvature(Pe));
    } else {
      mesh->communicate(Pi);
      ddt(Ne) += SETNAME(fci_curvature(Pi));
    }
  }

  if (ramp_mesh && (t < ramp_timescale)) {
    ddt(Ne) += NeTarget / ramp_timescale;
  }

  Field3D TiTediff, tauemimeSQB;
  if (classical_diffusion) {
    // Classical perpendicular diffusion
    // The only term here comes from the resistive drift
    Field3D Ne_tauB2;
    alloc_all(TiTediff);
    alloc_all(tauemimeSQB);
    alloc_all(Ne_tauB2);
    alloc_all(Dn);
    BOUT_FOR(i, Ne.getRegion("RGN_ALL")) {
      tauemimeSQB[i] = tau_e[i] * mi_me * B42[i];
      Dn[i] = (Te[i] + Ti[i]) / tauemimeSQB[i];
      Ne_tauB2[i] = Ne[i] / tauemimeSQB[i];
      TiTediff[i] = Ti[i] - (0.5 * Te[i]);

      tauemimeSQB.yup()[i] = tau_e.yup()[i] * mi_me * B42.yup()[i];
      Dn.yup()[i] = (Te.yup()[i] + Ti.yup()[i]) / tauemimeSQB.yup()[i];
      Ne_tauB2.yup()[i] = Ne.yup()[i] / tauemimeSQB.yup()[i];
      TiTediff.yup()[i] = Ti.yup()[i] - (0.5 * Te.yup()[i]);

      tauemimeSQB.ydown()[i] = tau_e.ydown()[i] * mi_me * B42.ydown()[i];
      Dn.ydown()[i] = (Te.ydown()[i] + Ti.ydown()[i]) / tauemimeSQB.ydown()[i];
      Ne_tauB2.ydown()[i] = Ne.ydown()[i] / tauemimeSQB.ydown()[i];
      TiTediff.ydown()[i] = Ti.ydown()[i] - (0.5 * Te.ydown()[i]);
    }
    ddt(Ne) += SETNAME(FCIDiv_a_Grad_perp(Dn, Ne));
    ddt(Ne) += SETNAME(FCIDiv_a_Grad_perp(Ne_tauB2, TiTediff));
  }
  if (anomalous_D > 0.0) {
    ddt(Ne) += SETNAME(FCIDiv_a_Grad_perp(a_d3d, Ne));
  }

  // Source
  if (adapt_source) {
    // Add source. Ensure that sink will go to zero as Ne -> 0
    Field3D NeErr = averageY(DC(Ne) - NeTarget);

    if (core_sources) {
      // Sources only in core (periodic Y) domain
      // Try to keep NeTarget

      ddt(Sn) = 0.0;
      for (int x = mesh->xstart; x <= mesh->xend; x++) {
        if (!mesh->periodicY(x))
          continue; // Not periodic, so skip

        for (int y = mesh->ystart; y <= mesh->yend; y++) {
          for (int z = 0; z <= mesh->LocalNz; z++){
            Sn(x, y, z) -= source_p * NeErr(x, y, z);
            ddt(Sn)(x, y, z) = -source_i * NeErr(x, y, z);

            if (Sn(x, y, z) < 0.0) {
              Sn(x, y, z) = 0.0;
              if (ddt(Sn)(x, y, z) < 0.0)
                ddt(Sn)(x, y, z) = 0.0;
            }
          }
        }
      }

      NeSource = Sn;
    } else {
      // core_sources = false
      NeSource = Sn * where(Sn, NeTarget, Ne);
      NeSource -= source_p * NeErr / NeTarget;

      ddt(Sn) = -source_i * NeErr;
    }

    if (source_vary_g11) {
      NeSource *= g11norm;
    }
  }
  NeSource.name = "NeSource";
  ddt(Ne) += NeSource;

  if (low_n_diffuse) {
    // Diffusion which kicks in at very low density, in order to
    // help prevent negative density regions
    ddt(Ne) += SETNAME(
        Div_par_K_Grad_par(SQ(coord->dy) * coord->g_22 * 1e-4 / Ne, Ne));
  }
  if (low_n_diffuse_perp) {
    ddt(Ne) += (Div_Perp_Lap_FV_Index(1e-4 / Ne, Ne, ne_bndry_flux));
  }

  if (ne_hyper_z > 0.) {
    ddt(Ne) -= ne_hyper_z * SQ(SQ(coord->dz)) * D4DZ4(Ne);
  }

  if (ne_num_diff > 0.0) {
    // Numerical perpendicular diffusion
    ddt(Ne) += (Div_Perp_Lap_FV_Index(ne_num_diff, Ne, ne_bndry_flux));
  }

  if (ne_num_hyper > 0.0) {
    ddt(Ne) -=
        SETNAME(ne_num_hyper * (D4DX4_FV_Index(Ne, true) + D4DZ4_Index(Ne)));
  }

  if (numdiff > 0.0) {
    BOUT_FOR(i, Ne.getRegion("RGN_NOY")) {
      ddt(Ne)[i] += numdiff*(Ne.ydown()[i.ym()] - 2.*Ne[i] + Ne.yup()[i.yp()]);
    }
  }
  ///////////////////////////////////////////////////////////
  // Vorticity
  // This is the current continuity equation

  TRACE("vorticity");

  ddt(Vort) = 0.0;

  if (currents && evolve_vort) {
    // Only evolve vorticity if any diamagnetic or parallel currents
    // are included.

    if (j_par) {
      TRACE("Vort:j_par");
      // Parallel current

      // This term is central differencing so that it balances the parallel gradient
      // of the potential in Ohm's law
      // Jpar.applyParallelBoundary(parbc);
      ddt(Vort) += Div_parP(Jpar);
      // a += Div_parP(Jpar);
    }

    if (j_diamag) {
      // Electron diamagnetic current

      // Note: This term is central differencing so that it balances
      // the corresponding compression term in the pressure equation
      ddt(Vort) += fci_curvature(add_all(Pi , Pe));
      // b += fci_curvature(add_all(Pi , Pe));
    }

    // Advection of vorticity by ExB
    if (boussinesq) {
      TRACE("Vort:boussinesq");
      // Using the Boussinesq approximation
      if (j_pol_pi){
        ddt(Vort) -=
            Div_n_bxGrad_f_B_XPPM(0.5 * Vort, phi, vort_bndry_flux,
                                  poloidal_flows, false, bracket_factor);

        // V_ExB dot Grad(Pi)
        Field3D vEdotGradPi =
            bracket(phi, Pi, BRACKET_ARAKAWA) * bracket_factor;
        vEdotGradPi.applyBoundary("free_o2");
        // delp2(phi) term
	Field3D DelpPhi_2B2 = 0.5 * Delp2(phi) / SQ(Bxyz);
	DelpPhi_2B2.applyBoundary("free_o2");

	Field3D inv_2sqb = 0.5 / SQ(Bxyz);
	ddt(Vort) -= FCIDiv_a_Grad_perp(inv_2sqb, vEdotGradPi);

	// delp2 phi v_ExB term
        ddt(Vort) -=
            Div_n_bxGrad_f_B_XPPM(DelpPhi_2B2, phi + Pi, vort_bndry_flux,
                                  poloidal_flows, false, bracket_factor);
      }
    } else {
      // When the Boussinesq approximation is not made,
      // then the changing ion density introduces a number
      // of other terms.

      throw BoutException("Hot ion non-Boussinesq not implemented yet\n");
    }

    if (classical_diffusion) {
      TRACE("Vort:classical_diffusion");
      // Perpendicular viscosity
      Field3D tilim_3 = 0.3*Ti;
      Field3D tauisqB = tau_i * SQ(coord->Bxy);

      Field3D mu = div_all(tilim_3 , tauisqB);
      ddt(Vort) += FCIDiv_a_Grad_perp(mu, Vort);
    }

    if (ion_viscosity) {
      TRACE("Vort:ion_viscosity");
      // Ion collisional viscosity.
      // Contains poloidal viscosity
      // if(fci_transform){
      //         throw BoutException("Ion viscosity not implemented for FCI yet\n");
      // }
      // Vector3D Pi_ciCb_B_2 = 0.5 * Pi_ci * Curlb_B;
      // mesh->communicate(Pi_ciCb_B_2);
      ddt(Vort) += 0.5 * fci_curvature(Pi_ci) -
                   Div_n_bxGrad_f_B_XPPM(1. / 3, Pi_ci, vort_bndry_flux, false,
                                         false, bracket_factor);
    }

    if (anomalous_nu > 0.0) {
      TRACE("Vort:anomalous_nu");
      // Perpendicular anomalous momentum diffusion
      ddt(Vort) += FCIDiv_a_Grad_perp(a_nu3d, Vort);
    }

    if (ion_neutral_rate > 0.0) {
      // Sink of vorticity due to ion-neutral friction
      ddt(Vort) -= ion_neutral_rate * Vort;
    }

    if (x_hyper_viscos > 0) {
      // Form of hyper-viscosity to suppress zig-zags in X
      ddt(Vort) -= x_hyper_viscos * D4DX4_FV_Index(Vort);
     }
    if (y_hyper_viscos > 0) {
      // Form of hyper-viscosity to suppress zig-zags in Y
      ddt(Vort) -= y_hyper_viscos * FV::D4DY4_Index(Vort, false);
    }
    if (z_hyper_viscos > 0) {
      // Form of hyper-viscosity to suppress zig-zags in Z
      ddt(Vort) -= z_hyper_viscos * SQ(SQ(coord->dz)) * D4DZ4(Vort);
    }

    if (vort_dissipation) {
      ddt(Vort) += mul_all(mul_all(coord->dy,coord->dy),D2DY2(Vort));
    }
    if (phi_dissipation) {
      // Adds dissipation term like in other equations, but depending on gradient of potential
      // Note: Doesn't seem to need faster than sound speed
      ddt(Vort) -= mul_all(mul_all(coord->dy,coord->dy),D2DY2(phi));
      //ddt(Vort) -= SQ(coord->dy)*D2DY2(phi);
    }
  }

  ///////////////////////////////////////////////////////////
  // Ohm's law
  // VePsi = Ve - Vi + 0.5*mi_me*beta_e*psi
  TRACE("Ohm's law");

  ddt(VePsi) = 0.0;

  if (currents && (electromagnetic || FiniteElMass)) {
    // Evolve VePsi except for electrostatic and zero electron mass case

    if (resistivity) {
      ddt(VePsi) -= mi_me * nu * (Ve - Vi);
      // External electric field
      // ddt(VePsi) += mi_me*nu*(Jpar - Jpar0)/NeVe;
    }

    // Parallel electric field
    if (j_par) {
      ddt(VePsi) += mi_me * Grad_parP(phi);
    }

    // Parallel electron pressure
    if (pe_par) {
      ddt(VePsi) -= mi_me * Grad_parP(Pe) / Ne;
    }

    if (thermal_force) {
      ddt(VePsi) -= mi_me * 0.71 * Grad_parP(Te);
    }

    if (electron_viscosity) {
      // Electron parallel viscosity (Braginskii)
      Field3D ve_eta = 0.973 * mi_me * tau_e * Te;

      if (eta_limit_alpha > 0.) {
        // SOLPS-style flux limiter
        // Values of alpha ~ 0.5 typically
        Field3D q_cl = ve_eta * Grad_par(Ve);           // Collisional value
        Field3D q_fl = eta_limit_alpha * Pe * mi_me; // Flux limit

        ve_eta = ve_eta / (1. + abs(q_cl / q_fl));

	ASSERT0("not implemented: FCI comm");
        mesh->communicate(ve_eta);
        ve_eta.applyBoundary("neumann");
      }
      ddt(VePsi) += Div_par_K_Grad_par(ve_eta, Ve);
    }

    if (FiniteElMass) {
      // Finite Electron Mass. Small correction needed to conserve energy
      Field3D vdiff = sub_all(Ve,Vi);
      // Comm breaks sheath par BCs!
      //mesh->communicate(vdiff);
      //vdiff.applyParallelBoundary(parbc);
      ddt(VePsi) -= Vi * Grad_par(vdiff); // Parallel advection
      ddt(VePsi) -= bracket(phi, vdiff, BRACKET_ARAKAWA)*bracket_factor;  // ExB advection
      // Should also have ion polarisation advection here
    }

    if (numdiff > 0.0) {
      // ddt(VePsi) += sqrt(mi_me) * numdiff * Div_par_diffusion_index(Ve);
      for(auto &i : VePsi.getRegion("RGN_ALL")) {
        ddt(VePsi)[i] += sqrt(mi_me) * numdiff*(Ve.ydown()[i.ym()] - 2.*Ve[i] + Ve.yup()[i.yp()]);
      }

    }

    if (hyper > 0.0) {
      ddt(VePsi) -= hyper * mi_me * nu * Delp2(Jpar) / Ne;
    }

    if (hyperpar > 0.0) {
      ddt(VePsi) -= hyperpar * FV::D4DY4_Index(Ve - Vi);
    }

    if (ve_num_diff > 0.0) {
      // Numerical perpendicular diffusion
      ddt(VePsi) += Div_Perp_Lap_FV_Index(ve_num_diff, Ve, ne_bndry_flux);
    }
    if (ve_num_hyper > 0.0) {
      ddt(VePsi) -= ve_num_hyper * (D4DX4_FV_Index(Ve, true) + D4DZ4_Index(Ve));
    }

    if (vepsi_dissipation) {
      // Adds dissipation term like in other equations
      // Maximum speed either electron sound speed or Alfven speed
      Field3D max_speed = Bnorm * coord->Bxy / sqrt(SI::mu0 * AA * SI::Mp * Nnorm * Ne)
                          / Cs0;                      // Alfven speed (normalised by Cs0)
      Field3D elec_sound = sqrt(mi_me) * sound_speed; // Electron sound speed
      for (auto& i : max_speed.getRegion("RGN_NOBNDRY")) {
        // Maximum of Alfven or thermal electron speed
        if (elec_sound[i] > max_speed[i]) {
          max_speed[i] = elec_sound[i];
        }

        // Limit to 100x reference sound speed or light speed
        BoutReal lim = BOUTMIN(100., 3e8 / Cs0);
        if (max_speed[i] > lim) {
          max_speed[i] = lim;
        }
      }
      ddt(VePsi) += SQ(coord->dy) * (D2DY2(Ve) - D2DY2(Vi));
    }
  }

  ///////////////////////////////////////////////////////////
  // Ion velocity
  if (ion_velocity) {
    TRACE("Ion velocity");

    if (currents) {
      // ddt(NVi) = bracket(NVi, phi, BRACKET_ARAKAWA) * bracket_factor;
      // ExB drift, only if electric field calculated
      ddt(NVi) =
          setName(-Div_n_bxGrad_f_B_XPPM(NVi, phi, ne_bndry_flux,
                                         poloidal_flows, false, bracket_factor),
                  "ExB drift");
    } else {
      ddt(NVi) = 0.0;
    }

    if (j_diamag) {
      // Magnetic drift
      ddt(NVi) -= setName(fci_curvature(mul_all(NVi, Ti)), "fci_curvature");
    }

    // FV with added dissipation
    ddt(NVi) -= setName(
        Div_parP_n(Ne, Vi, sound_speed, fwd_bndry_mask, bwd_bndry_mask),
        "Div_parP_n(Ne, Vi, sound_speed, fwd_bndry_mask, bwd_bndry_mask)");
    // // straight forward
    // Field3D nvivi = mul_all(NVi, Vi);
    // ddt(NVi) -= Div_parP(nvivi);
    // //  Skew-symmetric form
    // ddt(NVi) -= 0.5 * (Div_par(mul_all(NVi, Vi)) + Vi * Grad_par(NVi) + NVi *
    //                        Div_par(Vi));

    // Ignoring polarisation drift for now
    if (pe_par) {
      Field3D peppi = add_all(Pe, Pi);
      ddt(NVi) -= setName(Grad_parP(peppi), "Grad_parP(peppi)");
    }

    if (ion_viscosity) {
      TRACE("NVi:ion viscosity");
      // Poloidal flow damping
      // The parallel part is solved as a diffusion term
      Coordinates::FieldMetric sqrtBVi = mul_all(B12, Vi);
      Coordinates::FieldMetric Pitau_i_B =
	div_all(mul_all(Pi, tau_i), (coord->Bxy));
      ddt(NVi) += 1.28 * B12 *
	SETNAME(Div_par_K_Grad_par(Pitau_i_B, sqrtBVi));
      if (currents) {
        // Perpendicular part. B32 = B^{3/2}
        // This is only included if ExB flow is included
        Field3D Piciperp_B32 = div_all(Pi_ciperp, B32);
        ddt(NVi) -= (2. / 3) * B32 *
                    setName(Grad_par(Piciperp_B32), "Grad_par(Piciperp_B32)");
      }
    }

    // Ion-neutral friction
    if (ion_neutral_rate > 0.0) {
      ddt(NVi) -= SETNAME(ion_neutral_rate * NVi);
    }

    if (numdiff > 0.0) {
      for(auto &i : NVi.getRegion("RGN_NOY")) {
        ddt(NVi)[i] += numdiff*(Vi.ydown()[i.ym()] - 2.*Vi[i] + Vi.yup()[i.yp()]);
      }
      // ddt(NVi) += numdiff * Div_par_diffusion_index(NVi);

    }

    if (density_inflow) {
      // Particles arrive in cell at rate NeSource
      // This should come from a flow through the cell edge
      // with a flow velocity, and hence momentum

      ddt(NVi) +=
          SETNAME(NeSource * (NeSource / Ne) * coord->dy * sqrt(coord->g_22));
    }

    if (classical_diffusion) {
      // Using same cross-field drift as in density equation
      Field3D ViDn = mul_all(Vi,Dn);
      ddt(NVi) += FCIDiv_a_Grad_perp(ViDn, Ne);
      Field3D NVi_tauB2 = div_all(NVi, tauemimeSQB);
      ddt(NVi) += SETNAME(FCIDiv_a_Grad_perp(NVi_tauB2, TiTediff));
    }

    if ((anomalous_D > 0.0) && anomalous_D_nvi) {
      ddt(NVi) += SETNAME(FCIDiv_a_Grad_perp(mul_all(Vi, a_d3d), Ne));
    }

    if (anomalous_nu > 0.0) {
      ddt(NVi) += SETNAME(FCIDiv_a_Grad_perp(mul_all(Ne, a_nu3d), Vi));
    }

    if (hyperpar > 0.0) {
      ddt(NVi) -= hyperpar * FV::D4DY4_Index(Vi) / mi_me;
     }

    if (low_n_diffuse) {
      // Diffusion which kicks in at very low density, in order to
      // help prevent negative density regions
      ASSERT0(false);
      ddt(NVi) += Div_par_K_Grad_par(SQ(coord->dy) * coord->g_22 * 1e-4 / Ne, NVi);
    }
    if (low_n_diffuse_perp) {
      ddt(NVi) += Div_Perp_Lap_FV_Index(1e-4 / Ne, NVi, ne_bndry_flux);
    }

    if (vi_num_diff > 0.0) {
      // Numerical perpendicular diffusion
      ddt(NVi) += Div_Perp_Lap_FV_Index(vi_num_diff * Ne, Vi, ne_bndry_flux);
    }
  }

  ///////////////////////////////////////////////////////////
  // Pressure equation
  TRACE("Electron pressure");

  if (evolve_te) {

    if (currents) {
      // Divergence of heat flux due to ExB advection
      // ddt(Pe) = bracket(Pe, phi, BRACKET_ARAKAWA) * bracket_factor;
      ddt(Pe) = SETNAME(-Div_n_bxGrad_f_B_XPPM(
          Pe, phi, pe_bndry_flux, poloidal_flows, true, bracket_factor));
    } else {
      ddt(Pe) = 0.0;
    }

    if (parallel_flow_p_term) {
      // Parallel flow
      check_all(Pe);
      check_all(Ve);
      Field3D peve = mul_all(Pe,Ve);
      ddt(Pe) -= SETNAME(Div_parP(peve));
    }

    if (j_diamag) { // Diamagnetic flow
      // Magnetic drift (curvature) divergence.
      ddt(Pe) += SETNAME((5. / 3) * fci_curvature(mul_all(Pe, Te)));

      // This term energetically balances diamagnetic term
      // in the vorticity equation
      // ddt(Pe) -= (2. / 3) * Pe * (Curlb_B * Grad(phi));
      ddt(Pe) -= SETNAME((2. / 3) * Pe * fci_curvature(phi));
    }

    // Parallel heat conduction
    if (thermal_conduction) {
      check_all(kappa_epar);
      ddt(Pe) += SETNAME((2. / 3) * Div_par_K_Grad_par(kappa_epar, Te));
    }

    if (thermal_flux) {
      // Parallel heat convection
      Field3D tejpar = mul_all(Te,Jpar);
      ddt(Pe) += SETNAME((2. / 3) * 0.71 * Div_parP(tejpar));
    }

    if (currents && resistivity) {
      // Ohmic heating
      ddt(Pe) += SETNAME(nu * Jpar * (Jpar - Jpar0) / Ne);
    }

    if (pe_hyper_z > 0.0) {
      ddt(Pe) -= SETNAME(pe_hyper_z * SQ(SQ(coord->dz)) * D4DZ4(Pe));
    }

    ///////////////////////////////////
    // Heat transmission through sheath

    wall_power = 0.0; // Diagnostic output
    if (parallel_sheaths){
      sheath_dpe = 0.;

      for (const auto &bndry_par :
           mesh->getBoundariesPar(BoundaryParType::xout)) {
        for (bndry_par->first(); !bndry_par->isDone(); bndry_par->next()) {
          int x = bndry_par->ind().x();
          int y = bndry_par->ind().y();
          int z = bndry_par->ind().z();
          // Temperature and density at the sheath entrance
          BoutReal tesheath =
              floor(0.5 * (Te(x, y, z) +
                           Te.ynext(bndry_par->dir)(x, y + bndry_par->dir, z)),
                    0.0);
          BoutReal nesheath =
              floor(0.5 * (Ne(x, y, z) +
                           Ne.ynext(bndry_par->dir)(x, y + bndry_par->dir, z)),
                    0.0);
          BoutReal vesheath =
              floor(0.5 * (Ve(x, y, z) +
                           Ve.ynext(bndry_par->dir)(x, y + bndry_par->dir, z)),
                    0.0);
          // BoutReal tisheath = floor(
          //                               0.5 * (Ti(x, y, z) +
          // Ti.ynext(bndry_par->dir)(x, y + bndry_par->dir, z)),
          // 0.0);

          // Sound speed (normalised units)
          // BoutReal Cs =bndry_par->dir* sqrt(tesheath + tisheath);

          // Heat flux
          BoutReal q = (sheath_gamma_e - 1.5) * tesheath * nesheath * vesheath *
                       bndry_par->dir;
          // Multiply by cell area to get power
          BoutReal flux = q * coord->J(x, y, z) / sqrt(coord->g_22(x, y, z));

          // Divide by volume of cell, and 2/3 to get pressure
          BoutReal power =
            flux
            / (coord->dy(x, y, z) * coord->J(x, y, z));
          // ddt(Pe)(x, y, z) -= (2. / 3) * power;
          sheath_dpe(x, y, z) -= (2. / 3) * power;
        }
      }
      sheath_dpe.name = "sheath physics";
      ddt(Pe) += sheath_dpe;
    }


    // Transfer and source terms
    if (thermal_force) {
      ddt(Pe) -= SETNAME((2. / 3) * 0.71 * Jpar * Grad_parP(Te));
    }

    if (pe_par_p_term) {
      // This term balances energetically the pressure term
      // in Ohm's law
      ddt(Pe) -= SETNAME((2. / 3) * Pe * Div_parP(Ve));
    }
    if (ramp_mesh && (t < ramp_timescale)) {
      ddt(Pe) += PeTarget / ramp_timescale;
    }

    //////////////////////
    // Classical diffusion

    if (classical_diffusion) {

      // Combined resistive drift and cross-field heat diffusion
      // nu_rho2 = nu_ei * rho_e^2 in normalised units
      Field3D nu_rho2 = div_all(Te, mul_all(mul_all(tau_e, mi_me), B42));
      Field3D PePi = add_all(Pe, Pi);
      Field3D nu_rho2Ne = mul_all(nu_rho2, Ne);
      ddt(Pe) +=
          (2. / 3) * SETNAME(FCIDiv_a_Grad_perp(nu_rho2, PePi) +
                             (11. / 12) * FCIDiv_a_Grad_perp(nu_rho2Ne, Te));
    }

    //////////////////////
    // Anomalous diffusion

    if ((anomalous_D > 0.0) && anomalous_D_pepi) {
      ddt(Pe) += SETNAME(FCIDiv_a_Grad_perp(mul_all(a_d3d, Te), Ne));
    }
    if (anomalous_chi > 0.0) {
      ddt(Pe) +=
          (2. / 3) * SETNAME(FCIDiv_a_Grad_perp(mul_all(a_chi3d, Ne), Te));
    }

    // hyper diffusion
    if (numdiff > 0.0) {
      BOUT_FOR(i, Pe.getRegion("RGN_NOY")) {
        ddt(Pe)[i] += numdiff*(Pe.ydown()[i.ym()] - 2.*Pe[i] + Pe.yup()[i.yp()]);
      }
    }

    //////////////////////
    // Sources

    if (adapt_source) {
      // Add source. Ensure that sink will go to zero as Pe -> 0
      Field3D PeErr = averageY(DC(Pe) - PeTarget);

      if (core_sources) {
        // Sources only in core

        ddt(Spe) = 0.0;
        for (int x = mesh->xstart; x <= mesh->xend; x++) {
          if (!mesh->periodicY(x))
            continue; // Not periodic, so skip

          for (int y = mesh->ystart; y <= mesh->yend; y++) {
                for (int z = 0; z <= mesh->LocalNz; z++) {
                  Spe(x, y, z) -= source_p * PeErr(x, y, z);
                  ddt(Spe)(x, y, z) = -source_i * PeErr(x, y, z);

                  if (Spe(x, y, z) < 0.0) {
                    Spe(x, y, z) = 0.0;
                    if (ddt(Spe)(x, y, z) < 0.0)
                      ddt(Spe)(x, y, z) = 0.0;
                  }
            }
          }
        }

        if (energy_source) {
          // Add the same amount of energy to each particle
          PeSource = Spe * Ne / DC(Ne);
        } else {
          PeSource = Spe;
        }
      } else {

        Spe -= source_p * PeErr / PeTarget;
        ddt(Spe) = -source_i * PeErr;

        if (energy_source) {
          // Add the same amount of energy to each particle
          PeSource = Spe * Ne / DC(Ne);
        } else {
          PeSource = Spe * where(Spe, PeTarget, Pe);
        }
      }

      if (source_vary_g11) {
        PeSource *= g11norm;
      }

    } else {
      // Not adapting sources

      if (energy_source) {
        // Add the same amount of energy to each particle
        PeSource = Spe * Ne / DC(Ne);

        if (source_vary_g11) {
          PeSource *= g11norm;
        }
      } else {
        // Add the same amount of energy per volume
        // If no particle source added, then this can lead to
        // a small number of particles with a lot of energy!
      }
    }
    PeSource.name = "PeSource";
    ddt(Pe) += PeSource;
  } else {
    ddt(Pe) = 0.0;
  }

  ///////////////////////////////////////////////////////////
  // Ion pressure equation
  // Similar to electron pressure equation
  TRACE("Ion pressure");

  if (evolve_ti) {

    if (currents) {
      // Divergence of heat flux due to ExB advection
      ddt(Pi) = SETNAME(-Div_n_bxGrad_f_B_XPPM(
          Pi, phi, pe_bndry_flux, poloidal_flows, true, bracket_factor));
    } else {
      ddt(Pi) = 0.0;
    }

    // Parallel flow
    if (parallel_flow_p_term) {
      check_all(Pi);
      check_all(Vi);
      Field3D pivi = mul_all(Pi,Vi);
      ddt(Pi) -= SETNAME(Div_parP(pivi));
    }

    if (j_diamag) { // Diamagnetic flow
      // Magnetic drift (curvature) divergence
      ddt(Pi) -= (5. / 3) * SETNAME(fci_curvature(mul_all(Pi, Ti)));

      // Compression of ExB flow
      // These terms energetically balances diamagnetic term
      // in the vorticity equation
      // ddt(Pi) -= (2. / 3) * Pi * (Curlb_B * Grad(phi));
      ddt(Pi) -= (2. / 3) * SETNAME(Pi * fci_curvature(phi));

      ddt(Pi) += SETNAME(Pi * fci_curvature(Pi + Pe));
    }

    if (j_par) {
      if (boussinesq) {
        ddt(Pi) -= (2. / 3) * SETNAME(Jpar * Grad_parP(Pi));
      } else {
        ddt(Pi) -= (2. / 3) * SETNAME(Jpar * Grad_parP(Pi) / Ne);
      }
    }

    // Parallel heat conduction
    if (thermal_conduction) {
      ddt(Pi) += (2. / 3) * SETNAME(Div_par_K_Grad_par(kappa_ipar, Ti));
    }

    // Parallel pressure gradients (sound waves)
    if (pe_par_p_term) {
      // This term balances energetically the pressure term
      // in the parallel momentum equation
      ddt(Pi) -= (2. / 3) * SETNAME(Pi * Div_parP(Vi));
    }

    if (electron_ion_transfer) {
      // Electron-ion heat transfer
      Wi = SETNAME((3. / mi_me) * Ne * (Te - Ti) / tau_e);
      ddt(Pi) += (2. / 3) * Wi;
      ddt(Pe) -= (2. / 3) * Wi;
    }

    //////////////////////
    // Classical diffusion

    if (classical_diffusion) {
      Field3D Pi_B2tau, PePi, nu_rho2, nu_rho2Ne;
      alloc_all(Pi_B2tau);
      alloc_all(PePi);
      alloc_all(nu_rho2);
      alloc_all(nu_rho2Ne);
      BOUT_FOR(i, Pi.getRegion("RGN_ALL")) {
        // Cross-field heat conduction
        // kappa_perp = 2 * n * nu_ii * rho_i^2
        Pi_B2tau[i] = (2. * Pi[i]) / (B42[i] * tau_i[i]);
        nu_rho2[i] = Te[i] / (tau_e[i] * mi_me * B42[i]);
        PePi[i] = Pe[i] + Pi[i];
        nu_rho2Ne[i] = nu_rho2[i] * Ne[i];

        Pi_B2tau.yup()[i] =
            (2. * Pi.yup()[i]) / (B42.yup()[i] * tau_i.yup()[i]);
        nu_rho2.yup()[i] =
            Te.yup()[i] / (tau_e.yup()[i] * mi_me * B42.yup()[i]);
        PePi.yup()[i] = Pe.yup()[i] + Pi.yup()[i];
        nu_rho2Ne.yup()[i] = nu_rho2.yup()[i] * Ne.yup()[i];

        Pi_B2tau.ydown()[i] =
            (2. * Pi.ydown()[i]) / (B42.ydown()[i] * tau_i.ydown()[i]);
        nu_rho2.ydown()[i] =
            Te.ydown()[i] / (tau_e.ydown()[i] * mi_me * B42.ydown()[i]);
        PePi.ydown()[i] = Pe.ydown()[i] + Pi.ydown()[i];
        nu_rho2Ne.ydown()[i] = nu_rho2.ydown()[i] * Ne.ydown()[i];
      }

      // BOUT_FOR(i, Pi.getRegion("RGN_NOBNDRY")) {
      ddt(Pi) += (2. / 3) * SETNAME(FCIDiv_a_Grad_perp(Pi_B2tau, Ti));

      // Resistive drift terms

      // mesh->communicate(nu_rho2Ne,Te);
      ddt(Pi) += (5. / 3) * SETNAME(FCIDiv_a_Grad_perp(nu_rho2, PePi) -
                                    (1.5) * FCIDiv_a_Grad_perp(nu_rho2Ne, Te));

      // Collisional heating from perpendicular viscosity
      // in the vorticity equation

      if (currents) {
        Vector3D Grad_perp_vort = Grad(Vort);
        Field3D phiPi = add_all(phi, Pi);
        Grad_perp_vort.y = 0.0; // Zero parallel component
        ddt(Pi) -= (2. / 3) * (3. / 10) *
                   SETNAME(Ti / (SQ(coord->Bxy) * tau_i) *
                           (Grad_perp_vort * Grad(phiPi)));
      }
    }

    if (ion_viscosity) {
      // Collisional heating due to parallel viscosity
      Field3D sqrtBVi = mul_all(B12, Vi);
      ddt(Pi) += (2. / 3) * 1.28 *
                 SETNAME((Pi * tau_i / B12) * Grad_par(sqrtBVi) * Div_parP(Vi));

      if (currents) {
        ddt(Pi) -= (4. / 9) * Pi_ciperp * Div_parP(Vi);
        //(4. / 9) * Vi * B32 * Grad_par(Pi_ciperp / B32);
        Field3D phiPi = add_all(phi, Pi);
        ddt(Pi) -=
            (2. / 6) *
            SETNAME(Pi_ci * fci_curvature(phiPi)); // Curlb_B * Grad(phiPi);
        ddt(Pi) += (2. / 9) * SETNAME(bracket(Pi_ci, phiPi, BRACKET_ARAKAWA) *
                                      bracket_factor);
      }
    }

    //////////////////////
    // Anomalous diffusion

    if ((anomalous_D > 0.0) && anomalous_D_pepi) {
      ddt(Pi) += SETNAME(FCIDiv_a_Grad_perp(mul_all(a_d3d, Ti), Ne));
    }

    if (anomalous_chi > 0.0) {
      ddt(Pi) +=
          (2. / 3) * SETNAME(FCIDiv_a_Grad_perp(mul_all(a_chi3d, Ne), Ti));
    }

    // hyper diffusion
    if (numdiff > 0.0) {
      BOUT_FOR(i, Pi.getRegion("RGN_NOY")) {
        ddt(Pi)[i] += numdiff*(Pi.ydown()[i.ym()] - 2.*Pi[i] + Pi.yup()[i.yp()]);
      }
    }

    ///////////////////////////////////
    // Heat transmission through sheath

    if (parallel_sheaths){
      sheath_dpi = 0.0;
      for (const auto &bndry_par :
           mesh->getBoundariesPar(BoundaryParType::xout)) {
        for (bndry_par->first(); !bndry_par->isDone(); bndry_par->next()) {
          int x = bndry_par->ind().x();
          int y = bndry_par->ind().y();
          int z = bndry_par->ind().z();
          // Temperature and density at the sheath entrance
          BoutReal tisheath =
              floor(0.5 * (Ti(x, y, z) +
                           Ti.ynext(bndry_par->dir)(x, y + bndry_par->dir, z)),
                    0.0);
          BoutReal tesheath =
              floor(0.5 * (Te(x, y, z) +
                           Te.ynext(bndry_par->dir)(x, y + bndry_par->dir, z)),
                    0.0);
          BoutReal nesheath =
              floor(0.5 * (Ne(x, y, z) +
                           Ne.ynext(bndry_par->dir)(x, y + bndry_par->dir, z)),
                    0.0);
          BoutReal visheath =
              0.5 * (Vi(x, y, z) +
                     Vi.ynext(bndry_par->dir)(x, y + bndry_par->dir, z));

          // Sound speed (normalisexd units)
          // BoutReal Cs = bndry_par->dir * sqrt(tesheath + tisheath);

          // Heat flux
          BoutReal q = (sheath_gamma_i - 1.5) * tisheath * nesheath * visheath *
                       bndry_par->dir;

          // Multiply by cell area to get power
          BoutReal flux = q * coord->J(x, y, z) / sqrt(coord->g_22(x, y, z));

          // Divide by volume of cell, and 2/3 to get pressure
          BoutReal power =
            flux
            / (coord->dy(x, y, z) * coord->J(x, y, z));
          sheath_dpi(x, y, z) -= (3. / 2) * power;
        }
      }
      sheath_dpi.name = "sheath_dpi";
      ddt(Pi) += sheath_dpi;
    }

    //////////////////////
    // Sources

    if (adapt_source) {
      // Add source. Ensure that sink will go to zero as Pe -> 0
      Field3D PiErr = averageY(DC(Pi) - PiTarget);

      if (core_sources) {
        // Sources only in core

        ddt(Spi) = 0.0;
        for (int x = mesh->xstart; x <= mesh->xend; x++) {
          if (!mesh->periodicY(x))
            continue; // Not periodic, so skip

          for (int y = mesh->ystart; y <= mesh->yend; y++) {
                for (int z = 0; z <= mesh->LocalNz; z++) {
                  Spi(x, y, z) -= source_p * PiErr(x, y, z);
                  ddt(Spi)(x, y, z) = -source_i * PiErr(x, y, z);

                  if (Spi(x, y, z) < 0.0) {
                    Spi(x, y, z) = 0.0;
                    if (ddt(Spi)(x, y, z) < 0.0)
                      ddt(Spi)(x, y, z) = 0.0;
                  }
            }
          }
        }

        if (energy_source) {
          // Add the same amount of energy to each particle
          PiSource = Spi * Ne / DC(Ne);
        } else {
          PiSource = Spi;
        }
      } else {

        Spi -= source_p * PiErr / PiTarget;
        ddt(Spi) = -source_i * PiErr;

        if (energy_source) {
          // Add the same amount of energy to each particle
          PiSource = Spi * Ne / DC(Ne);
        } else {
          PiSource = Spi * where(Spi, PiTarget, Pi);
        }
      }

      if (source_vary_g11) {
        PiSource *= g11norm;
      }

    } else {
      // Not adapting sources

      if (energy_source) {
        // Add the same amount of energy to each particle
        PiSource = Spi * Ne / DC(Ne);

        if (source_vary_g11) {
          PiSource *= g11norm;
        }

      } else {
        // Add the same amount of energy per volume
        // If no particle source added, then this can lead to
        // a small number of particles with a lot of energy!
      }
    }
    PiSource.name = "PiSource";
    ddt(Pi) += PiSource;

  } else {
    ddt(Pi) = 0.0;
  }

  ///////////////////////////////////////////////////////////
  // Radial buffer regions for turbulence simulations

  if (radial_buffers) {
    /// Radial buffer regions

    // Calculate flux sZ averages
    Field2D PeDC = averageY(DC(Pe));
    Field2D PiDC = averageY(DC(Pi));
    Field2D NeDC = averageY(DC(Ne));
    Field2D VortDC = averageY(DC(Vort));

    if ((mesh->getGlobalXIndex(mesh->xstart) - mesh->xstart) < radial_inner_width) {
      // This processor contains points inside the inner radial boundary

      int imax = mesh->xstart + radial_inner_width - 1
                 - (mesh->getGlobalXIndex(mesh->xstart) - mesh->xstart);
      if (imax > mesh->xend) {
        imax = mesh->xend;
      }

      int imin = mesh->xstart;
      if (!mesh->firstX()) {
        --imin; // Calculate in guard cells, for radial fluxes
      }
      int ncz = mesh->LocalNz;

      for (int i = imin; i <= imax; ++i) {
        // position inside the boundary (0 = on boundary, 0.5 = first cell)
        BoutReal pos =
            static_cast<BoutReal>(mesh->getGlobalXIndex(i) - mesh->xstart) + 0.5;

        // Diffusion coefficient which increases towards the boundary
        BoutReal D = radial_buffer_D * (1. - pos / radial_inner_width);

        for (int j = mesh->ystart; j <= mesh->yend; ++j) {
          for (int k = 0; k < ncz; ++k) {
            BoutReal dx = coord->dx(i, j, k);
            BoutReal dx_xp = coord->dx(i + 1, j, k);
            BoutReal J = coord->J(i, j, k);
            BoutReal J_xp = coord->J(i + 1, j, k);

            // Calculate metric factors for radial fluxes
            BoutReal rad_flux_factor = 0.25 * (J + J_xp) * (dx + dx_xp);
            BoutReal x_factor = rad_flux_factor / (J * dx);
            BoutReal xp_factor = rad_flux_factor / (J_xp * dx_xp);
            // Relax towards constant value on flux surface

            ddt(Pe)(i, j, k) -= D * (Pe(i, j, k) - PeDC(i, j));
            ddt(Pi)(i, j, k) -= D * (Pi(i, j, k) - PiDC(i, j));
            ddt(Ne)(i, j, k) -= D * (Ne(i, j, k) - NeDC(i, j));
            ddt(Vort)(i, j, k) -= D * (Vort(i, j, k) - VortDC(i, j));
            ddt(NVi)(i, j, k) -= D * NVi(i, j, k);

            // Radial fluxes
            BoutReal f = D * (Ne(i + 1, j, k) - Ne(i, j, k));
            ddt(Ne)(i, j, k) += f * x_factor;
            ddt(Ne)(i + 1, j, k) -= f * xp_factor;

            f = D * (Pe(i + 1, j, k) - Pe(i, j, k));
            ddt(Pe)(i, j, k) += f * x_factor;
            ddt(Pe)(i + 1, j, k) -= f * xp_factor;

            f = D * (Pi(i + 1, j, k) - Pi(i, j, k));
            ddt(Pi)(i, j, k) += f * x_factor;
            ddt(Pi)(i + 1, j, k) -= f * xp_factor;

            f = D * (Vort(i + 1, j, k) - Vort(i, j, k));
            ddt(Vort)(i, j, k) += f * x_factor;
            ddt(Vort)(i + 1, j, k) -= f * xp_factor;
          }
        }
      }
    }
    // Number of points in outer guard cells
    int nguard = mesh->LocalNx - mesh->xend - 1;

    if (mesh->GlobalNx - nguard - mesh->getGlobalXIndex(mesh->xend)
        <= radial_outer_width) {

      // Outer boundary
      int imin = mesh->GlobalNx - nguard - radial_outer_width - mesh->getGlobalXIndex(0);
      if (imin < mesh->xstart) {
        imin = mesh->xstart;
      }
      int ncz = mesh->LocalNz;
      for (int i = imin; i <= mesh->xend; ++i) {

        // position inside the boundary
        BoutReal pos =
            static_cast<BoutReal>(mesh->GlobalNx - nguard - mesh->getGlobalXIndex(i))
            - 0.5;

        // Diffusion coefficient which increases towards the boundary
        BoutReal D = radial_buffer_D * (1. - pos / radial_outer_width);

        for (int j = mesh->ystart; j <= mesh->yend; ++j) {
          for (int k = 0; k < ncz; ++k) {
            BoutReal dx = coord->dx(i, j, k);
            BoutReal dx_xp = coord->dx(i + 1, j, k);
            BoutReal J = coord->J(i, j, k);
            BoutReal J_xp = coord->J(i + 1, j, k);

            // Calculate metric factors for radial fluxes
            BoutReal rad_flux_factor = 0.25 * (J + J_xp) * (dx + dx_xp);
            BoutReal x_factor = rad_flux_factor / (J * dx);
            BoutReal xp_factor = rad_flux_factor / (J_xp * dx_xp);

            ddt(Pe)(i, j, k) -= D * (Pe(i, j, k) - PeDC(i, j));
            ddt(Pi)(i, j, k) -= D * (Pi(i, j, k) - PiDC(i, j));
            ddt(Ne)(i, j, k) -= D * (Ne(i, j, k) - NeDC(i, j));
            ddt(Vort)(i, j, k) -= D * (Vort(i, j, k) - VortDC(i, j));
            // ddt(Vort)(i,j,k) -= D*Vort(i,j,k);

            BoutReal f = D * (Vort(i + 1, j, k) - Vort(i, j, k));
            ddt(Vort)(i, j, k) += f * x_factor;
            ddt(Vort)(i + 1, j, k) -= f * xp_factor;
          }
        }
      }
    }
  }

  ///////////////////////////////////////////////////////////
  // Neutral gas
  if (neutrals) {
    TRACE("Neutral gas model add sources");

    // Add sources/sinks to plasma equations
    ddt(Ne) -= neutrals->S;             // Sink of plasma density
    ddt(NVi) -= neutrals->F;            // Plasma momentum
    ddt(Pe) -= (2. / 3) * neutrals->Rp; // Plasma radiated energy
    ddt(Pi) -= (2. / 3) * neutrals->Qi; // Ion energy transferred to neutrals

    // Calculate atomic rates

    if (neutral_friction) {
      // Vorticity
      if (boussinesq) {
	Field3D tmp = neutrals->Fperp / (Ne * SQ(coord->Bxy));
	mesh->communicate(tmp);
	tmp.applyParallelBoundary(parbc);
	ddt(Vort) -= FCIDiv_a_Grad_perp(tmp, phi);
      } else {
        ddt(Vort) -= FCIDiv_a_Grad_perp(neutrals->Fperp / SQ(coord->Bxy), phi);
      }
    }

    ///////////////////////////////////////////////////////////////////
    // Recycling at the boundary
    TRACE("Neutral recycling fluxes");
    wall_flux = 0.0;

    if (sheath_ydown) {
      for (RangeIterator r = mesh->iterateBndryLowerY(); !r.isDone(); r++) {
        // Calculate flux of ions into target from Ne and Vi boundary
        // This calculation is supposed to be consistent with the flow
        // of plasma from Div_par_FV(Ne, Ve)

        for (int jz = 0; jz < mesh->LocalNz; jz++) {
          BoutReal flux_ion =
              -0.5 *
              (Ne(r.ind, mesh->ystart, jz) + Ne(r.ind, mesh->ystart - 1, jz)) *
              0.5 * (Ve(r.ind, mesh->ystart, jz) +
                     Ve(r.ind, mesh->ystart - 1, jz)); // Flux through surface
                                                       // [m^-2 s^-1], should be
                                                       // positive since Ve <
                                                       // 0.0

          // Flow of neutrals inwards
          BoutReal flow = frecycle * flux_ion *
            (coord->J(r.ind, mesh->ystart, jz) +
             coord->J(r.ind, mesh->ystart - 1, jz)) /
            (sqrt(coord->g_22(r.ind, mesh->ystart, jz)) +
             sqrt(coord->g_22(r.ind, mesh->ystart - 1, jz)));

          // Rate of change of neutrals in final cell
          BoutReal dndt = flow / (coord->J(r.ind, mesh->ystart, jz) *
                                  coord->dy(r.ind, mesh->ystart, jz));

          // Add mass, momentum and energy to the neutrals

          neutrals->addDensity(r.ind, mesh->ystart, jz, dndt);
          neutrals->addPressure(r.ind, mesh->ystart, jz,
                                dndt * (3.5 / Tnorm)); // Franck-Condon energy
          neutrals->addMomentum(r.ind, mesh->ystart, jz,
                                dndt * neutral_vwall * sqrt(3.5 / Tnorm));

          // Power deposited onto the wall due to surface recombination
          wall_power(r.ind, mesh->ystart) += (13.6 / Tnorm) * dndt;
        }
      }
    }

    if (sheath_yup) {
      for (RangeIterator r = mesh->iterateBndryUpperY(); !r.isDone(); r++) {
        // Calculate flux of ions into target from Ne and Vi boundary
        // This calculation is supposed to be consistent with the flow
        // of plasma from FV::Div_par(Ne, Ve)

        for (int jz = 0; jz < mesh->LocalNz; jz++) {
          // Flux through surface [m^-2 s^-1], should be positive
          BoutReal flux_ion =
              frecycle * 0.5 *
              (Ne(r.ind, mesh->yend, jz) + Ne(r.ind, mesh->yend + 1, jz)) *
              0.5 * (Ve(r.ind, mesh->yend, jz) + Ve(r.ind, mesh->yend + 1, jz));

          // Flow of neutrals inwards
          BoutReal flow = flux_ion * (coord->J(r.ind, mesh->yend, jz) +
                                      coord->J(r.ind, mesh->yend + 1, jz)) /
            (sqrt(coord->g_22(r.ind, mesh->yend, jz)) +
             sqrt(coord->g_22(r.ind, mesh->yend + 1, jz)));

          // Rate of change of neutrals in final cell
          BoutReal dndt =
            flow / (coord->J(r.ind, mesh->yend, jz) * coord->dy(r.ind, mesh->yend, jz));

          // Add mass, momentum and energy to the neutrals

          neutrals->addDensity(r.ind, mesh->yend, jz, dndt);
          neutrals->addPressure(r.ind, mesh->yend, jz,
                                dndt * (3.5 / Tnorm)); // Franck-Condon energy
          neutrals->addMomentum(r.ind, mesh->yend, jz,
                                -dndt * neutral_vwall * sqrt(3.5 / Tnorm));

          // Power deposited onto the wall due to surface recombination
          wall_power(r.ind, mesh->yend) += (13.6 / Tnorm) * dndt;
        }
      }
    }
  }

  //////////////////////////////////////////////////////////////
  // Impurities

  if ((carbon_fraction > 0.0) || impurity_adas) {

    if (carbon_fraction > 0.0) {
      TRACE("Carbon impurity radiation");
      Rzrad = carbon_rad->power(Te * Tnorm, Ne * Nnorm,
                                Ne * (Nnorm * carbon_fraction)); // J / m^3 / s
    } else {
      Rzrad = 0.0;
    }

    if (impurity_adas) {
      for (auto &i : Rzrad.getRegion("RGN_NOY")) {
        // Calculate cell centre (C), left (L) and right (R) values

        BoutReal Te_C = Te[i],
          Te_L = 0.5 * (Te[i.ym()] + Te[i]),
          Te_R = 0.5 * (Te[i] + Te[i.yp()]);
        BoutReal Ne_C = Ne[i],
          Ne_L = 0.5 * (Ne[i.ym()] + Ne[i]),
          Ne_R = 0.5 * (Ne[i] + Ne[i.yp()]);

        BoutReal Rz_L = computeRadiatedPower(*impurity,
                                             Te_L * Tnorm,        // electron temperature [eV]
                                             Ne_L * Nnorm,        // electron density [m^-3]
                                             fimp * Ne_L * Nnorm, // impurity density [m^-3]
                                             0.0);       // Neutral density [m^-3]

        BoutReal Rz_C = computeRadiatedPower(*impurity,
                                             Te_C * Tnorm,        // electron temperature [eV]
                                             Ne_C * Nnorm,        // electron density [m^-3]
                                             fimp * Ne_C * Nnorm, // impurity density [m^-3]
                                             0.0);       // Neutral density [m^-3]

        BoutReal Rz_R = computeRadiatedPower(*impurity,
                                             Te_R * Tnorm,        // electron temperature [eV]
                                             Ne_R * Nnorm,        // electron density [m^-3]
                                             fimp * Ne_R * Nnorm, // impurity density [m^-3]
                                             0.0);       // Neutral density [m^-3]

        // Jacobian (Cross-sectional area)
        BoutReal J_C = coord->J[i],
          J_L = 0.5 * (coord->J[i.ym()] + coord->J[i]),
          J_R = 0.5 * (coord->J[i] + coord->J[i.yp()]);

        // Simpson's rule, calculate average over cell
        Rzrad[i] += (J_L * Rz_L +
                     4. * J_C * Rz_C +
                     J_R * Rz_R) / (6. * J_C);
      }
    }

    Rzrad /= SI::qe * Tnorm * Nnorm * Omega_ci;                // Normalise
    ddt(Pe) -= (2. / 3) * Rzrad;
  }

  //////////////////////////////////////////////////////////////
  // Parallel closures for 2D simulations

  if (sinks) {
    // Sink terms for 2D simulations

    // Field3D nsink = 0.5*Ne*sqrt(Te)*sink_invlpar;   // n C_s/ (2L)  //
    // Sound speed flow to targets
    Field3D nsink = SETNAME(0.5 * sqrt(Ti) * Ne * sink_invlpar);
    nsink = floor(nsink, 0.0);

    ddt(Ne) -= nsink;

    ASSERT0(not fci_transform);
    Field3D conduct = (2. / 3) * kappa_epar * Te * SQ(sink_invlpar);
    conduct = floor(conduct, 0.0);
    ddt(Pe) -= SETNAME(conduct      // Heat conduction
                       + Te * nsink // Advection
    );

    if (sheath_closure) {
      ///////////////////////////
      // Sheath dissipation closure

      Field3D phi_te = floor(phi / Te, 0.0);
      Field3D jsheath = Ne * sqrt(Te) *
                        (1 - (sqrt(mi_me) / (2. * sqrt(PI))) * exp(-phi_te));

      ddt(Vort) += SETNAME(jsheath * sink_invlpar);
    } else {
      ///////////////////////////
      // Vorticity closure
      ddt(Vort) -= SETNAME(FCIDiv_a_Grad_perp(nsink / SQ(coord->Bxy), phi));
      // ddt(Vort) -= (nsink / Ne)*Vort;
      // Note: If nsink = n * nu then this reduces to
      // ddt(Vort) -= nu * Vort
    }

    if (drift_wave) {
      // Include a drift-wave closure in the core region
      // as in Hasegawa-Wakatani and SOLT models

      Field2D Tedc = DC(Te); // Zonal average
      Field3D Q = phi - Te * log(Ne);

      // Drift-wave operator
      Field3D Adw = SETNAME(alpha_dw * pow(Tedc, 1.5) * (Q - DC(Q)));

      ddt(Ne) += Adw;
      ddt(Vort) += Adw;
    }

    // Electron and ion parallel dynamics not evolved
  }

  if (low_pass_z >= 0) {
    // Low pass Z filtering, keeping up to and including low_pass_z
    ddt(Ne) = lowPass(ddt(Ne), low_pass_z);
    ddt(Pe) = lowPass(ddt(Pe), low_pass_z);

    if (currents) {
      ddt(Vort) = lowPass(ddt(Vort), low_pass_z);
      if (electromagnetic || FiniteElMass) {
        ddt(VePsi) = lowPass(ddt(VePsi), low_pass_z);
      }
    }
    if (ion_velocity) {
      ddt(NVi) = lowPass(ddt(NVi), low_pass_z);
    }
  }

  if (!evolve_plasma) {
    ddt(Ne) = 0.0;
    ddt(Pe) = 0.0;
    ddt(Vort) = 0.0;
    ddt(VePsi) = 0.0;
    ddt(NVi) = 0.0;
  }

  return 0;
} // rhs

/*!
 * Preconditioner. Solves the heat conduction
 *
 * @param[in] t  The simulation time
 * @param[in] gamma   Factor in front of the Jacobian in (I - gamma*J). Related
 * to timestep
 * @param[in] delta   Not used here
 */
int Hermes::precon(BoutReal t, BoutReal gamma, BoutReal delta) {
  static std::unique_ptr<InvertPar> inv{nullptr};
  if (!inv) {
    // Initialise parallel inversion class
    auto inv = InvertPar::create();
    inv->setCoefA(1.0);
  }
  if (thermal_conduction) {
    // Set the coefficient in front of Grad2_par2
    inv->setCoefB(-(2. / 3) * gamma * kappa_epar);
    Field3D dT = ddt(Pe);
    dT.applyBoundary("neumann");
    ddt(Pe) = inv->solve(dT);
  }

  // Neutral gas preconditioning
  if (neutrals)
    neutrals->precon(t, gamma, delta);

  return 0;
}

Field3D Hermes::fci_curvature(const Field3D &f) {
  // Field3D result = mul_all(bracket(logB, f, BRACKET_ARAKAWA), bracket_factor);
  // mesh->communicate(result);
  return 2 * bracket(logB, f, BRACKET_ARAKAWA) * bracket_factor;
}

Field3D Hermes::Grad_parP(const Field3D &f) {
  return Grad_par(f); //+ 0.5*beta_e*bracket(psi, f, BRACKET_ARAKAWA);
}

Field3D Hermes::Div_parP(const Field3D &f) {
  return Div_par(f);
  auto* coords = mesh->getCoordinates();
  Field3D result;
  result.allocate();
  const auto fup = f.yup();
  const auto fdown = f.ydown();
  BOUT_FOR(i, f.getRegion("RGN_NOBNDRY")) {
    // for(auto &i : f.getRegion("RGN_NOBNDRY")) {
    auto yp = i.yp();
    auto ym = i.ym();
    result[i] = (fup[yp] / coords->Bxy.yup()[yp] -
                 fdown[ym] / coords->Bxy.ydown()[ym]) *
                coords->Bxy[i] / (coords->dy[i] * sqrt(coords->g_22[i]));
  }
  return result;
  //return Div_par(f) + 0.5*beta_e*coord->Bxy*bracket(psi, f/coord->Bxy, BRACKET_ARAKAWA);
}

Field3D MinMod(const Field3D &f) {
  // get gradient in y direction, avoiding numerical issues
  Field3D result;
  result.allocate();
  BOUT_FOR(i, f.getRegion("RGN_NOBNDRY")) {
    const BoutReal fp = f.yup()[i.yp()];
    const BoutReal fm = f.ydown()[i.ym()];
    const BoutReal fi = f[i];
    const BoutReal gp = fp - fi;
    const BoutReal gm = fi - fm;
    if ((gp * gm) < 0) {
      result[i] = 0;
    } else if (abs(gp) < abs(gm)) {
      result[i] = gp;
    } else {
      result[i] = gm;
    }
    ASSERT2(std::isfinite(result[i]));
  }
  result.applyBoundary("neumann_o2");
  return result;
}

Field3D Hermes::Div_parP_f(const Field3D &f, const Field3D &v,
                           Field3D &sound_speed) {
  throw BoutException("NI");
}

Field3D Hermes::Div_parP_n(const Field3D &n, const Field3D &v,
                           Field3D &sound_speed, const BoutMask &fwd,
                           const BoutMask &bwd) {
  Field3D gn = MinMod(n);
  Field3D gv = MinMod(v);
  n.getMesh()->communicate(gn, gv, sound_speed);
  gn.applyParallelBoundary(parbc);
  gv.applyParallelBoundary(parbc);
  sound_speed.applyParallelBoundary(parbc);
  Field3D result{0.0};

  auto coord = n.getCoordinates();
  BOUT_FOR(i, n.getRegion("RGN_NOBNDRY")) {
    const auto ip = i.yp();
    const auto im = i.ym();

    // const BoutReal iVi =
    //     1 / (coord->dx[i] * coord->dy[i] * coord->dz[i] * coord->J[i]);
    // const BoutReal Ai =
    //     coord->dx[i] * coord->dz[i] * coord->J[i] / sqrt(coord->g_22[i]);
    // Area / Volume
    const BoutReal AoVi = 1 / (coord->dy[i] * sqrt(coord->g_22[i]));

    const BoutReal niR = n[i] + gn[i] / 2;
    const BoutReal viR = v[i] + gv[i] / 2;
    const BoutReal npL = n.yup()[ip] - gn.yup()[ip];
    const BoutReal vpL = v.yup()[ip] - gv.yup()[ip];
    const BoutReal niL = n[i] - gn[i] / 2;
    const BoutReal viL = v[i] - gv[i] / 2;
    const BoutReal nmR = n.ydown()[im] - gn.ydown()[im];
    const BoutReal vmR = v.ydown()[im] - gv.ydown()[im];
    const BoutReal amaxp = std::max(
        {abs(v[i]), abs(v.yup()[ip]), sound_speed[i], sound_speed.yup()[ip]});
    const BoutReal amaxm = std::max({abs(v[i]), abs(v.ydown()[im]),
                                     sound_speed[i], sound_speed.ydown()[im]});
    BoutReal Gnvp = 0.5 * (niR * SQ(viR) + npL * SQ(vpL)) +
                    0.5 * amaxp * (niR * viR - npL * vpL);
    BoutReal Gnvm = 0.5 * (nmR * SQ(vmR) + niL * SQ(viL)) +
                    0.5 * amaxm * (nmR * vmR - niL * viL);
    if (Div_parP_n_sheath_extra) {
      if (fwd[i]) {
        const BoutReal vip = 0.5 * (v[i] + v.yup()[ip]);
        const BoutReal nip = 0.5 * (n[i] + n.yup()[ip]);
        Gnvp = niR * viR * vip + amaxp * (niR * viR - nip * vip);
      }
      if (bwd[i]) {
        const BoutReal vim = 0.5 * (v[i] + v.ydown()[im]);
        const BoutReal nim = 0.5 * (n[i] + n.ydown()[im]);
        Gnvp = niL * viL * vim + amaxm * (niL * viL - nim * vim);
      }
    }
    ASSERT1(std::isfinite(Gnvp));
    ASSERT1(std::isfinite(Gnvm));
    result[i] = AoVi * (Gnvp - Gnvm);
  }
  return result;
}

Field3D Hermes::FCIDiv_a_Grad_perp(const Field3D &a, const Field3D &f) {
  return (*_FCIDiv_a_Grad_perp)(a, f);
}
