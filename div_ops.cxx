/*
    Copyright B.Dudson, J.Leddy, University of York, September 2016
              email: benjamin.dudson@york.ac.uk

    This file is part of Hermes.

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

#include <mpi.h>

#include "div_ops.hxx"

#include <bout/fv_ops.hxx>

#include <bout/assert.hxx>
#include <bout/mesh.hxx>
#include <derivs.hxx>
#include <globals.hxx>
#include <output.hxx>
#include <utils.hxx>

#include <cmath>

using bout::globals::mesh;

const Field3D Div_par_diffusion_index(const Field3D &f, bool bndry_flux) {
  Field3D result;
  result = 0.0;

  Coordinates *coord = mesh->getCoordinates();
  
  for (int i = mesh->xstart; i <= mesh->xend; i++)
    for (int j = mesh->ystart - 1; j <= mesh->yend; j++)
      for (int k = 0; k < mesh->LocalNz; k++) {
        // Calculate flux at upper surface

        if (!bndry_flux && !mesh->periodicY(i)) {
          if ((j == mesh->yend) && mesh->lastY(i))
            continue;

          if ((j == mesh->ystart - 1) && mesh->firstY(i))
            continue;
        }
        BoutReal J =
	  0.5 * (coord->J(i, j, k) + coord->J(i, j + 1, k)); // Jacobian at boundary

        BoutReal gradient = f(i, j + 1, k) - f(i, j, k);

        BoutReal flux = J * gradient;

        result(i, j, k) += flux / coord->J(i, j, k);
        result(i, j + 1, k) -= flux / coord->J(i, j + 1, k);
      }
  return result;
}

////////////////////////////////////////////////////////////////////////////////////////////////
// XPPM methods

BoutReal BOUTMIN(const BoutReal &a, const BoutReal &b, const BoutReal &c,
                 const BoutReal &d) {
  BoutReal r1 = (a < b) ? a : b;
  BoutReal r2 = (c < d) ? c : d;
  return (r1 < r2) ? r1 : r2;
}

struct Stencil1D {
  // Cell centre values
  BoutReal c, m, p, mm, pp;

  // Left and right cell face values
  BoutReal L, R;
};

// First order upwind for testing
void Upwind(Stencil1D &n) { n.L = n.R = n.c; }

// Fromm method
void Fromm(Stencil1D &n) {
  n.L = n.c - 0.25 * (n.p - n.m);
  n.R = n.c + 0.25 * (n.p - n.m);
}

/// The minmod function returns the value with the minimum magnitude
/// If the inputs have different signs then returns zero
BoutReal minmod(BoutReal a, BoutReal b) {
  if (a * b <= 0.0)
    return 0.0;

  if (fabs(a) < fabs(b))
    return a;
  return b;
}

BoutReal minmod(BoutReal a, BoutReal b, BoutReal c) {
  // If any of the signs are different, return zero gradient
  if ((a * b <= 0.0) || (a * c <= 0.0)) {
    return 0.0;
  }

  // Return the minimum absolute value
  return SIGN(a) * BOUTMIN(fabs(a), fabs(b), fabs(c));
}

void MinMod(Stencil1D &n) {
  // Choose the gradient within the cell
  // as the minimum (smoothest) solution
  BoutReal slope = minmod(n.p - n.c, n.c - n.m);
  n.L = n.c - 0.5 * slope; // 0.25*(n.p - n.m);
  n.R = n.c + 0.5 * slope; // 0.25*(n.p - n.m);
}

// Monotonized Central limiter (Van-Leer)
void MC(Stencil1D &n) {
  BoutReal slope =
      minmod(2. * (n.p - n.c), 0.5 * (n.p - n.m), 2. * (n.c - n.m));
  n.L = n.c - 0.5 * slope;
  n.R = n.c + 0.5 * slope;
}

void XPPM(Stencil1D &n, const BoutReal h) {
  // 4th-order PPM interpolation in X

  const BoutReal C = 1.25; // Limiter parameter

  BoutReal h2 = h * h;

  n.R = (7. / 12) * (n.c + n.p) - (1. / 12) * (n.m + n.pp);
  n.L = (7. / 12) * (n.c + n.m) - (1. / 12) * (n.mm + n.p);

  // Apply limiters
  if ((n.c - n.R) * (n.p - n.R) > 0.0) {
    // Calculate approximations to second derivative

    BoutReal D2 = (3. / h2) * (n.c - 2 * n.R + n.p);
    BoutReal D2L = (1. / h2) * (n.m - 2 * n.c + n.p);
    BoutReal D2R = (1. / h2) * (n.c - 2. * n.p + n.pp);

    BoutReal D2lim; // Value to be used in limiter

    // Check if they all have the same sign
    if ((D2 * D2L > 0.0) && (D2 * D2R > 0.0)) {
      // Same sign

      D2lim = SIGN(D2) * BOUTMIN(C * fabs(D2L), C * fabs(D2R), fabs(D2));
    } else {
      // Different sign
      D2lim = 0.0;
    }

    n.R = 0.5 * (n.c + n.p) - (h2 / 6) * D2lim;
  }

  if ((n.m - n.L) * (n.c - n.L) > 0.0) {
    // Calculate approximations to second derivative

    BoutReal D2 = (3. / h2) * (n.m - 2 * n.L + n.c);
    BoutReal D2L = (1. / h2) * (n.mm - 2 * n.m + n.c);
    BoutReal D2R = (1. / h2) * (n.m - 2. * n.c + n.p);

    BoutReal D2lim; // Value to be used in limiter

    // Check if they all have the same sign
    if ((D2 * D2L > 0.0) && (D2 * D2R > 0.0)) {
      // Same sign

      D2lim = SIGN(D2) * BOUTMIN(C * fabs(D2L), C * fabs(D2R), fabs(D2));
    } else {
      // Different sign
      D2lim = 0.0;
    }

    n.L = 0.5 * (n.m + n.c) - (h2 / 6) * D2lim;
  }

  if (((n.R - n.c) * (n.c - n.L) <= 0.0) ||
      ((n.m - n.c) * (n.c - n.p) <= 0.0)) {
    // At a local maximum or minimum

    BoutReal D2 = (6. / h2) * (n.L - 2. * n.c + n.R);

    if (fabs(D2) < 1e-10) {
      n.R = n.L = n.c;
    } else {
      BoutReal D2C = (1. / h2) * (n.m - 2. * n.c + n.p);
      BoutReal D2L = (1. / h2) * (n.mm - 2 * n.m + n.c);
      BoutReal D2R = (1. / h2) * (n.c - 2. * n.p + n.pp);

      BoutReal D2lim;
      // Check if they all have the same sign
      if ((D2 * D2C > 0.0) && (D2 * D2L > 0.0) && (D2 * D2R > 0.0)) {
        // Same sign

        D2lim = SIGN(D2) *
                BOUTMIN(C * fabs(D2L), C * fabs(D2R), C * fabs(D2C), fabs(D2));
        n.R = n.c + (n.R - n.c) * D2lim / D2;
        n.L = n.c + (n.L - n.c) * D2lim / D2;
      } else {
        // Different signs
        n.R = n.L = n.c;
      }
    }
  }
}

/* ***USED***
 *  Div (n * b x Grad(f)/B)
 *
 *
 * poloidal   - If true, includes X-Y flows
 * positive   - If true, limit advected quantity (n_in) to be positive
 */
const Field3D Div_n_bxGrad_f_B_XPPM(const Field3D &n, const Field3D &f,
                                    bool bndry_flux, bool poloidal,
                                    bool positive) {
  Field3D result{0.0};

  Coordinates *coord = mesh->getCoordinates();
  
  //////////////////////////////////////////
  // X-Z advection.
  //
  //             Z
  //             |
  //
  //    fmp --- vU --- fpp
  //     |      nU      |
  //     |               |
  //    vL nL        nR vR    -> X
  //     |               |
  //     |      nD       |
  //    fmm --- vD --- fpm
  //

  int nz = mesh->LocalNz;
  for (int i = mesh->xstart; i <= mesh->xend; i++)
    for (int j = mesh->ystart; j <= mesh->yend; j++)
      for (int k = 0; k < nz; k++) {
        int kp = (k + 1) % nz;
        int kpp = (kp + 1) % nz;
        int km = (k - 1 + nz) % nz;
        int kmm = (km - 1 + nz) % nz;

        // 1) Interpolate stream function f onto corners fmp, fpp, fpm

        BoutReal fmm = 0.25 * (f(i, j, k) + f(i - 1, j, k) + f(i, j, km) +
                               f(i - 1, j, km));
        BoutReal fmp = 0.25 * (f(i, j, k) + f(i, j, kp) + f(i - 1, j, k) +
                               f(i - 1, j, kp)); // 2nd order accurate
        BoutReal fpp = 0.25 * (f(i, j, k) + f(i, j, kp) + f(i + 1, j, k) +
                               f(i + 1, j, kp));
        BoutReal fpm = 0.25 * (f(i, j, k) + f(i + 1, j, k) + f(i, j, km) +
                               f(i + 1, j, km));

        // 2) Calculate velocities on cell faces

        BoutReal vU = 0.5 * (coord->J(i, j, k) + coord->J(i, j , kp)) * (fmp - fpp) /
	  coord->dx(i, j, k); // -J*df/dx
        BoutReal vD = 0.5 * (coord->J(i, j, k) + coord->J(i, j , km)) * (fmm - fpm) /
	  coord->dx(i, j, k); // -J*df/dx

        BoutReal vR = 0.5 * (coord->J(i, j, k) + coord->J(i + 1, j, k)) * (fpp - fpm) /
	  coord->dz(i,j,k); // J*df/dz
        BoutReal vL = 0.5 * (coord->J(i, j, k) + coord->J(i - 1, j, k)) * (fmp - fmm) /
	  coord->dz(i,j,k); // J*df/dz

        // output.write("NEW: (%d,%d,%d) : (%e/%e, %e/%e)\n", i,j,k,vL,vR,
        // vU,vD);

        // 3) Calculate n on the cell faces. The sign of the
        //    velocity determines which side is used.

        // X direction
        Stencil1D s;
        s.c = n(i, j, k);
        s.m = n(i - 1, j, k);
        s.mm = n(i - 2, j, k);
        s.p = n(i + 1, j, k);
        s.pp = n(i + 2, j, k);

        // Upwind(s, mesh->dx(i,j));
        // XPPM(s, mesh->dx(i,j));
        // Fromm(s, coord->dx(i, j));
        MC(s);

        // Right side
        if ((i == mesh->xend) && (mesh->lastX())) {
          // At right boundary in X

          if (bndry_flux) {
            BoutReal flux;
            if (vR > 0.0) {
              // Flux to boundary
              flux = vR * s.R;
            } else {
              // Flux in from boundary
              flux = vR * 0.5 * (n(i + 1, j, k) + n(i, j, k));
            }
            result(i, j, k) += flux / (coord->dx(i, j, k) * coord->J(i, j, k));
            result(i + 1, j, k) -=
	      flux / (coord->dx(i + 1, j, k) * coord->J(i + 1, j, k));
          }
        } else {
          // Not at a boundary
          if (vR > 0.0) {
            // Flux out into next cell
            BoutReal flux = vR * s.R;
            result(i, j, k) += flux / (coord->dx(i, j, k) * coord->J(i, j, k));
            result(i + 1, j, k) -=
	      flux / (coord->dx(i + 1, j, k) * coord->J(i + 1, j, k));

            // if(i==mesh->xend)
            //  output.write("Setting flux (%d,%d) : %e\n",
            //  j,k,result(i+1,j,k));
          }
        }

        // Left side

        if ((i == mesh->xstart) && (mesh->firstX())) {
          // At left boundary in X

          if (bndry_flux) {
            BoutReal flux;

            if (vL < 0.0) {
              // Flux to boundary
              flux = vL * s.L;

            } else {
              // Flux in from boundary
              flux = vL * 0.5 * (n(i - 1, j, k) + n(i, j, k));
            }
            result(i, j, k) -= flux / (coord->dx(i, j, k) * coord->J(i, j, k));
            result(i - 1, j, k) +=
	      flux / (coord->dx(i - 1, j, k) * coord->J(i - 1, j, k));
          }
        } else {
          // Not at a boundary

          if (vL < 0.0) {
            BoutReal flux = vL * s.L;
            result(i, j, k) -= flux / (coord->dx(i, j, k) * coord->J(i, j, k));
            result(i - 1, j, k) +=
	      flux / (coord->dx(i - 1, j, k) * coord->J(i - 1, j, k));
          }
        }

        /// NOTE: Need to communicate fluxes

        // Z direction
        s.m = n(i, j, km);
        s.mm = n(i, j, kmm);
        s.p = n(i, j, kp);
        s.pp = n(i, j, kpp);

        // Upwind(s, coord->dz);
        // XPPM(s, coord->dz);
        // Fromm(s, coord->dz);
        MC(s);

        if (vU > 0.0) {
          BoutReal flux = vU * s.R; 
          result(i, j, k) += flux / (coord->J(i, j, k) * coord->dz(i, j, k));
          result(i, j, kp) -= flux / (coord->J(i, j, kp) * coord->dz(i, j, kp));
        }
        if (vD < 0.0) {
          BoutReal flux = vD * s.L; 
          result(i, j, k) -= flux / (coord->J(i, j, k) * coord->dz(i, j, k));
          result(i, j, km) += flux  / (coord->J(i, j, km) * coord->dz(i, j, km));
        }
      }
  FV::communicateFluxes(result);

  //////////////////////////////////////////
  // X-Y advection.
  //
  //
  //  This code does not deal with corners correctly. This may or may not be
  //  important.
  //
  // 1/J d/dx ( J n (g^xx g^yz / B^2) df/dy) - 1/J d/dy( J n (g^xx g^yz / B^2)
  // df/dx )
  //
  // Interpolating stream function f_in onto corners fmm, fmp, fpp, fpm
  // is complicated because the corner point in X-Y is not communicated
  // and at an X-point it is shared with 8 cells, rather than 4
  // (being at the X-point itself)
  // Corners also need to be shifted to the correct toroidal angle

  if (poloidal) {
    // X flux

    Field3D dfdy = DDY(f);
    mesh->communicate(dfdy);
    dfdy.applyBoundary("neumann");

    int xs = mesh->xstart - 1;
    int xe = mesh->xend;
    if (!bndry_flux) {
      // No boundary fluxes
      if (mesh->firstX()) {
        // At an inner boundary
        xs = mesh->xstart;
      }
      if (mesh->lastX()) {
        // At outer boundary
        xe = mesh->xend - 1;
      }
    }

    for (int i = xs; i <= xe; i++)
      for (int j = mesh->ystart - 1; j <= mesh->yend; j++)
        for (int k = 0; k < mesh->LocalNz; k++) {

          // Average dfdy to right X boundary
          BoutReal f_R =
	    0.5 * ((coord->g11(i + 1, j, k) * coord->g23(i + 1, j, k) /
		    SQ(coord->Bxy(i + 1, j, k))) *
		   dfdy(i + 1, j, k) +
		   (coord->g11(i, j, k) * coord->g23(i, j, k) / SQ(coord->Bxy(i, j, k))) *
		   dfdy(i, j, k));

          // Advection velocity across cell face
          BoutReal Vx = 0.5 * (coord->J(i + 1, j, k) + coord->J(i, j, k)) * f_R;

          // Fromm method
          BoutReal flux = Vx;
          if (Vx > 0) {
            // Right boundary of cell (i,j,k)
            BoutReal nval =
                n(i, j, k) + 0.25 * (n(i + 1, j, k) - n(i - 1, j, k));
            if (positive && (nval < 0.0)) {
              // Limit value to positive
              nval = 0.0;
            }
            flux *= nval;
          } else {
            // Left boundary of cell (i+1,j,k)
            BoutReal nval =
                n(i + 1, j, k) - 0.25 * (n(i + 2, j, k) - n(i, j, k));
            if (positive && (nval < 0.0)) {
              nval = 0.0;
            }
            flux *= nval;
          }

          result(i, j, k) += flux / (coord->dx(i, j, k) * coord->J(i, j, k));
          result(i + 1, j, k) -=
	    flux / (coord->dx(i + 1, j, k) * coord->J(i + 1, j, k));
        }
  }

  if (poloidal) {
    // Y flux
    
    Field3D dfdx = DDX(f);
    mesh->communicate(dfdx);
    dfdx.applyBoundary("neumann");

    // This calculation is in field aligned coordinates
    dfdx = toFieldAligned(dfdx);
    Field3D n_fa = toFieldAligned(n);
    
    Field3D yresult{zeroFrom(n_fa)};
    
    for (int i = mesh->xstart; i <= mesh->xend; i++) {
      int ys = mesh->ystart - 1;
      int ye = mesh->yend;

      if (!bndry_flux && !mesh->periodicY(i)) {
        // No boundary fluxes
        if (mesh->firstY(i)) {
          // At an inner boundary
          ys = mesh->ystart;
        }
        if (mesh->lastY(i)) {
          // At outer boundary
          ye = mesh->yend - 1;
        }
      }

      for (int j = ys; j <= ye; j++) {
        for (int k = 0; k < mesh->LocalNz; k++) {
          // Y flow

          // Average dfdx to upper Y boundary
          BoutReal f_U =
	    0.5 * ((coord->g11(i, j + 1, k) * coord->g23(i, j + 1, k) /
		    SQ(coord->Bxy(i, j + 1, k))) *
		   dfdx(i, j + 1, k) +
		   (coord->g11(i, j, k) * coord->g23(i, j, k) / SQ(coord->Bxy(i, j, k))) *
		   dfdx(i, j, k));

          BoutReal Vy = -0.5 * (coord->J(i, j + 1, k) + coord->J(i, j, k)) * f_U;

          if (mesh->firstY(i) && !mesh->periodicY(i) &&
              (j == mesh->ystart - 1)) {
            // Lower y boundary. Allow flows out of the domain only
            if (Vy > 0.0)
              Vy = 0.0;
          }
          if (mesh->lastY(i) && !mesh->periodicY(i) && (j == mesh->yend)) {
            // Upper y boundary
            if (Vy < 0.0)
              Vy = 0.0;
          }

          // Fromm method
          BoutReal flux = Vy;
          if (Vy > 0) {
            // Right boundary of cell (i,j,k)
            BoutReal nval =
                n_fa(i, j, k) + 0.25 * (n_fa(i, j + 1, k) - n_fa(i, j - 1, k));
            if (positive && (nval < 0.0)) {
              nval = 0.0;
            }
            flux *= nval;
          } else {
            // Left boundary of cell (i,j+1,k)
            BoutReal nval =
                n_fa(i, j + 1, k) - 0.25 * (n_fa(i, j + 2, k) - n_fa(i, j, k));
            if (positive && (nval < 0.0)) {
              nval = 0.0;
            }
            flux *= nval;
          }

          yresult(i, j, k) += flux / (coord->dy(i, j, k) * coord->J(i, j, k));
          yresult(i, j + 1, k) -= flux / (coord->dy(i, j + 1, k) * coord->J(i, j + 1, k));
        }
      }
    }
    result += fromFieldAligned(yresult);
  }
  
  return result;
}

/// *** USED ***
const Field3D Div_Perp_Lap_FV_Index(const Field3D &as, const Field3D &fs,
                                    bool xflux) {

  Field3D result = 0.0;

  //////////////////////////////////////////
  // X-Z diffusion in index space
  //
  //            Z
  //            |
  //
  //     o --- gU --- o
  //     |     nU     |
  //     |            |
  //    gL nL      nR gR    -> X
  //     |            |
  //     |     nD     |
  //     o --- gD --- o
  //

  Coordinates *coord = mesh->getCoordinates();
  
  for (int i = mesh->xstart; i <= mesh->xend; i++)
    for (int j = mesh->ystart; j <= mesh->yend; j++)
      for (int k = 0; k < mesh->LocalNz; k++) {
        int kp = (k + 1) % mesh->LocalNz;
        int km = (k - 1 + mesh->LocalNz) % mesh->LocalNz;

        // Calculate gradients on cell faces

        BoutReal gR = fs(i + 1, j, k) - fs(i, j, k);

        BoutReal gL = fs(i, j, k) - fs(i - 1, j, k);

        BoutReal gD = fs(i, j, k) - fs(i, j, km);

        BoutReal gU = fs(i, j, kp) - fs(i, j, k);

        // Flow right
        BoutReal flux = gR * 0.25 * (coord->J(i + 1, j, k) + coord->J(i, j, k)) *
	  (coord->dx(i + 1, j, k) + coord->dx(i, j, k)) *
	  (as(i + 1, j, k) + as(i, j, k));

        result(i, j, k) += flux / (coord->dx(i, j, k) * coord->J(i, j, k));

        // Flow left
        flux = gL * 0.25 * (coord->J(i - 1, j, k) + coord->J(i, j, k)) *
	  (coord->dx(i - 1, j, k) + coord->dx(i, j, k)) *
	  (as(i - 1, j, k) + as(i, j, k));

        result(i, j, k) -= flux / (coord->dx(i, j, k) * coord->J(i, j, k));

        // Flow up

        flux = gU * 0.25 *  (coord->J(i, j, k) + coord->J(i, j, kp)) *
	  (coord->dz(i, j, k) + coord->dz(i, j, kp)) *
	  (as(i, j, k) + as(i, j, kp));
	
        result(i, j, k) += flux / (coord->dz(i, j, k) * coord->J(i, j, k));

	// Flow down
        flux = gD * 0.25 *  (coord->J(i, j, km) + coord->J(i, j, k)) *
	  (coord->dz(i, j, km) + coord->dz(i, j, k)) *
	  (as(i, j, km) + as(i, j, k));

        result(i, j, k) -= flux / (coord->dz(i, j, k) * coord->J(i, j, k));
      }
  
  return result;
}

// *** USED ***
const Field3D D4DX4_FV_Index(const Field3D &f, bool bndry_flux) {
  Field3D result = 0.0;

  Coordinates *coord = mesh->getCoordinates();
  
  for (int i = mesh->xstart; i <= mesh->xend; i++)
    for (int j = mesh->ystart; j <= mesh->yend; j++) {
      for (int k = 0; k < mesh->LocalNz; k++) {

        // 3rd derivative at right boundary

        BoutReal d3fdx3 = (f(i + 2, j, k) - 3. * f(i + 1, j, k) +
                           3. * f(i, j, k) - f(i - 1, j, k));

        BoutReal flux = 0.25 * (coord->dx(i, j, k) + coord->dx(i + 1, j, k)) *
	  (coord->J(i, j, k) + coord->J(i + 1, j, k)) * d3fdx3;

        if (mesh->lastX() && (i == mesh->xend)) {
          // Boundary

          if (bndry_flux) {
            // Use a one-sided difference formula

            d3fdx3 = -((16. / 5) * 0.5 *
                           (f(i + 1, j, k) + f(i, j, k)) // Boundary value f_b
                       - 6. * f(i, j, k)                 // f_0
                       + 4. * f(i - 1, j, k)             // f_1
                       - (6. / 5) * f(i - 2, j, k)       // f_2
                       );

            flux = 0.25 * (coord->dx(i, j, k) + coord->dx(i + 1, j, k)) *
	      (coord->J(i, j, k) + coord->J(i + 1, j, k)) * d3fdx3;

          } else {
            // No fluxes through boundary
            flux = 0.0;
          }
        }

        result(i, j, k) += flux / (coord->J(i, j, k) * coord->dx(i, j, k));
        result(i + 1, j, k) -= flux / (coord->J(i + 1, j, k) * coord->dx(i + 1, j, k));

        if (j == mesh->xstart) {
          // Left cell boundary, no flux through boundaries

          if (mesh->firstX()) {
            // On an X boundary

            if (bndry_flux) {
              d3fdx3 = -(-(16. / 5) * 0.5 *
                             (f(i - 1, j, k) + f(i, j, k)) // Boundary value f_b
                         + 6. * f(i, j, k)                 // f_0
                         - 4. * f(i + 1, j, k)             // f_1
                         + (6. / 5) * f(i + 2, j, k)       // f_2
                         );

              flux = 0.25 * (coord->dx(i, j, k) + coord->dx(i + 1, j, k)) *
		(coord->J(i, j, k) + coord->J(i + 1, j, k)) * d3fdx3;

              result(i, j, k) -= flux / (coord->J(i, j, k) * coord->dx(i, j, k));
              result(i - 1, j, k) +=
		flux / (coord->J(i - 1, j, k) * coord->dx(i - 1, j, k));
            }

          } else {
            // Not on a boundary
            d3fdx3 = (f(i + 1, j, k) - 3. * f(i, j, k) + 3. * f(i - 1, j, k) -
                      f(i - 2, j, k));

            flux = 0.25 * (coord->dx(i, j, k) + coord->dx(i + 1, j, k)) *
	      (coord->J(i, j, k) + coord->J(i + 1, j, k)) * d3fdx3;

            result(i, j, k) -= flux / (coord->J(i, j, k) * coord->dx(i, j, k));
            result(i - 1, j, k) +=
	      flux / (coord->J(i - 1, j, k) * coord->dx(i - 1, j, k));
          }
        }
      }
    }

  return result;
}

const Field3D D4DZ4_Index(const Field3D &f) {
  Field3D result{emptyFrom(f)};

  BOUT_FOR(i, f.getRegion("RGN_NOBNDRY")) {
    result[i] = f[i.zpp()] - 4. * f[i.zp()] + 6. * f[i] - 4. * f[i.zm()] + f[i.zmm()];
  }
  return result;
}

/*! *** USED ***
 * X-Y diffusion
 *
 * NOTE: Assumes g^12 = 0, so X and Y are orthogonal. Otherwise
 * we would need the corner cell values to take Y derivatives along X edges
 *
 */
const Field2D Laplace_FV(const Field2D &k, const Field2D &f) {
  Field2D result;
  result.allocate();

  Coordinates *coord = mesh->getCoordinates();
  Field2D g11_2D = DC(coord->g11);
  Field2D g22_2D = DC(coord->g22);
  Field2D dx_2D = DC(coord->dx);
  Field2D J_2D = DC(coord->J);

  for (int i = mesh->xstart; i <= mesh->xend; i++)
    for (int j = mesh->ystart; j <= mesh->yend; j++) {

      // Calculate gradients on cell faces

      BoutReal gR = (g11_2D(i, j) + g11_2D(i + 1, j)) *
                    (f(i + 1, j) - f(i, j)) /
                    (dx_2D(i + 1, j) + dx_2D(i, j));

      BoutReal gL = (g11_2D(i - 1, j) + g11_2D(i, j)) *
                    (f(i, j) - f(i - 1, j)) /
                    (dx_2D(i - 1, j) + dx_2D(i, j));

      BoutReal gU = (g22_2D(i, j) + g22_2D(i, j + 1)) *
                    (f(i, j + 1) - f(i, j)) /
                    (dx_2D(i, j + 1) + dx_2D(i, j));

      BoutReal gD = (g22_2D(i, j - 1) + g22_2D(i, j)) *
                    (f(i, j) - f(i, j - 1)) /
                    (dx_2D(i, j) + dx_2D(i, j - 1));

      // Flow right

      BoutReal flux = gR * 0.25 * (J_2D(i + 1, j) + J_2D(i, j)) *
                      (k(i + 1, j) + k(i, j));

      result(i, j) = flux / (dx_2D(i, j) * J_2D(i, j));

      // Flow left

      flux = gL * 0.25 * (J_2D(i - 1, j) + J_2D(i, j)) *
             (k(i - 1, j) + k(i, j));
      result(i, j) -= flux / (dx_2D(i, j) * J_2D(i, j));

      // Flow up

      flux = gU * 0.25 * (J_2D(i, j + 1) + J_2D(i, j)) *
             (k(i, j + 1) + k(i, j));
      result(i, j) += flux / (dx_2D(i, j) * J_2D(i, j));

      // Flow down

      flux = gD * 0.25 * (J_2D(i, j - 1) + J_2D(i, j)) *
             (k(i, j - 1) + k(i, j));
      result(i, j) -= flux / (dx_2D(i, j) * J_2D(i, j));
    }
  return result;
}

namespace FCI {
Field3D Div_a_Grad_perp(const Field3D &a, const Field3D &f) {
  ASSERT1_FIELDS_COMPATIBLE(a, f);
  auto coord = a.getCoordinates();
  auto J = coord->J;
  auto Jgxx = coord->g11 * J;
  auto Jgzz = coord->g33 * J;
  auto Jgxz = coord->g13 * J;

  auto result = DDX(a * Jgxx) * DDX(f) + a * Jgxx * D2DX2(f);
  result += DDX(a * Jgxz * DDZ(f, CELL_DEFAULT, "DEFAULT", "RGN_NOY"));
  result += DDZ(a * Jgxz * DDX(f));
  result += DDZ(a * Jgzz) * DDZ(f) + a * Jgzz * D2DZ2(f);
  result /= J;
  result.name = "FCI::Div_a_Grad_perp()";
  return result;
}
} // namespace FCI
