#include "acc_add.h"
#include "acc_image.h"
#include "acc_mathfunc.h"
#include "acc_switch.h"
#include "e_vdw.h"
#include "gpu_card.h"
#include "md.h"
#include "nblist.h"

// MAYBE_UNUSED static const int BLOCK_DIM = 32;
// MAYBE_UNUSED static const int BLOCK_DIM = 64;
MAYBE_UNUSED static const int BLOCK_DIM = 128;

// TODO: test lj, buck, mm3hb, gauss, and mutant
// TODO: add vdw correction

TINKER_NAMESPACE_BEGIN
void evdw_reduce_xyz() {
  #pragma acc parallel deviceptr(x,y,z,ired,kred,xred,yred,zred)
  #pragma acc loop independent
  for (int i = 0; i < n; ++i) {
    int iv = ired[i];
    real rdn = kred[i];
    xred[i] = rdn * (x[i] - x[iv]) + x[iv];
    yred[i] = rdn * (y[i] - y[iv]) + y[iv];
    zred[i] = rdn * (z[i] - z[iv]) + z[iv];
  }
}

void evdw_resolve_gradient() {
  #pragma acc parallel deviceptr(ired,kred,gxred,gyred,gzred,gx,gy,gz)
  #pragma acc loop independent
  for (int ii = 0; ii < n; ++ii) {
    int iv = ired[ii];
    real redii = kred[ii];
    real rediv = 1 - redii;
    gx[ii] += redii * gxred[ii];
    gy[ii] += redii * gyred[ii];
    gz[ii] += redii * gzred[ii];
    gx[iv] += rediv * gxred[iv];
    gy[iv] += rediv * gyred[iv];
    gz[iv] += rediv * gzred[iv];
  }
}

#pragma acc routine seq
template <int DO_G>
inline void ehal_pair(real rik,                                             //
                      real rv, real eps, real ghal, real dhal,              //
                      real vscalek, real vlambda, real scexp, real scalpha, //
                      real& __restrict__ e, real& __restrict__ de) {
  eps *= vscalek;
  real rho = rik * REAL_RECIP(rv);
  real rho6 = REAL_POW(rho, 6);
  real rho7 = rho6 * rho;
  eps *= REAL_POW(vlambda, scexp);
  real scal = scalpha * REAL_POW(1 - vlambda, 2);
  real s1 = REAL_RECIP(scal + REAL_POW(rho + dhal, 7));
  real s2 = REAL_RECIP(scal + rho7 + ghal);
  real t1 = REAL_POW(1 + dhal, 7) * s1;
  real t2 = (1 + ghal) * s2;
  e = eps * t1 * (t2 - 2);
  if_constexpr(DO_G) {
    real dt1drho = -7 * REAL_POW(rho + dhal, 6) * t1 * s1;
    real dt2drho = -7 * rho6 * t2 * s2;
    de = eps * (dt1drho * (t2 - 2) + t1 * dt2drho) * REAL_RECIP(rv);
  }
}

template <int USE, evdw_t VDWTYP>
void evdw_tmpl() {
  constexpr int do_e = USE & calc::energy;
  constexpr int do_a = USE & calc::analyz;
  constexpr int do_g = USE & calc::grad;
  constexpr int do_v = USE & calc::virial;
  static_assert(do_v ? do_g : true, "");
  static_assert(do_a ? do_e : true, "");

  const real cut = vdw_switch_cut;
  const real off = vdw_switch_off;
  const real cut2 = cut * cut;
  const real off2 = off * off;
  const int maxnlst = vlist_unit->maxnlst;
  const auto* vlst = vlist_unit.deviceptr();

  auto* nev = ev_handle.ne()->buffer();
  auto* ev = ev_handle.e()->buffer();
  auto* vir_ev = ev_handle.vir()->buffer();
  auto bufsize = ev_handle.buffer_size();

  if_constexpr(do_g) { device_array::zero(n, gxred, gyred, gzred); }

#define DEVICE_PTRS_                                                           \
  x, y, z, gxred, gyred, gzred, box, xred, yred, zred, jvdw, radmin, epsilon,  \
      vlam, nev, ev, vir_ev

  MAYBE_UNUSED int GRID_DIM = get_grid_size(BLOCK_DIM);
  #pragma acc parallel num_gangs(GRID_DIM) vector_length(BLOCK_DIM)\
               deviceptr(DEVICE_PTRS_,vlst)
  #pragma acc loop gang independent
  for (int i = 0; i < n; ++i) {
    int it = jvdw[i];
    real xi = xred[i];
    real yi = yred[i];
    real zi = zred[i];
    real lam1 = vlam[i];
    MAYBE_UNUSED real gxi = 0, gyi = 0, gzi = 0;

    int nvlsti = vlst->nlst[i];
    #pragma acc loop vector independent reduction(+:gxi,gyi,gzi)
    for (int kk = 0; kk < nvlsti; ++kk) {
      int offset = (kk + i * n) & (bufsize - 1);
      int k = vlst->lst[i * maxnlst + kk];
      int kt = jvdw[k];
      real xr = xi - xred[k];
      real yr = yi - yred[k];
      real zr = zi - zred[k];
      real vlambda = vlam[k];

      if (vcouple == vcouple_decouple) {
        vlambda = (lam1 == vlambda ? 1 : (lam1 < vlambda ? lam1 : vlambda));
      } else if (vcouple == vcouple_annihilate) {
        vlambda = (lam1 < vlambda ? lam1 : vlambda);
      }

      image(xr, yr, zr, box);
      real rik2 = xr * xr + yr * yr + zr * zr;
      if (rik2 <= off2) {
        real rik = REAL_SQRT(rik2);
        real rv = radmin[it * njvdw + kt];
        real eps = epsilon[it * njvdw + kt];

        MAYBE_UNUSED real e, de;
        if_constexpr(VDWTYP == evdw_t::hal) {
          ehal_pair<do_g>(rik,                        //
                          rv, eps, ghal, dhal,        //
                          1, vlambda, scexp, scalpha, //
                          e, de);
        }

        if (rik2 > cut2) {
          real taper, dtaper;
          switch_taper5<do_g>(rik, cut, off, taper, dtaper);
          if_constexpr(do_g) de = e * dtaper + de * taper;
          if_constexpr(do_e) e = e * taper;
        }

        // Increment the energy, gradient, and virial.

        if_constexpr(do_e) {
          atomic_add_value(e, ev, offset);

          if_constexpr(do_a) {
            if (e != 0) {
              atomic_add_value(1, nev, offset);
            }
          }
        }

        if_constexpr(do_g) {
          de *= REAL_RECIP(rik);
          real dedx = de * xr;
          real dedy = de * yr;
          real dedz = de * zr;

          gxi += dedx;
          gyi += dedy;
          gzi += dedz;
          atomic_add_value(-dedx, gxred, k);
          atomic_add_value(-dedy, gyred, k);
          atomic_add_value(-dedz, gzred, k);

          if_constexpr(do_v) {
            real vxx = xr * dedx;
            real vyx = yr * dedx;
            real vzx = zr * dedx;
            real vyy = yr * dedy;
            real vzy = zr * dedy;
            real vzz = zr * dedz;

            atomic_add_value(vxx, vyx, vzx, vyy, vzy, vzz, vir_ev, offset);
          } // end if (do_v)
        }   // end if (do_g)
      }
    } // end for (int kk)

    if_constexpr(do_g) {
      atomic_add_value(gxi, gxred, i);
      atomic_add_value(gyi, gyred, i);
      atomic_add_value(gzi, gzred, i);
    }
  } // end for (int i)

  #pragma acc parallel\
              deviceptr(DEVICE_PTRS_,vdw_excluded_,vdw_excluded_scale_)
  #pragma acc loop independent
  for (int ii = 0; ii < nvdw_excluded_; ++ii) {
    int offset = ii & (bufsize - 1);

    int i = vdw_excluded_[ii][0];
    int k = vdw_excluded_[ii][1];
    real vscale = vdw_excluded_scale_[ii];

    int it = jvdw[i];
    real xi = xred[i];
    real yi = yred[i];
    real zi = zred[i];
    real lam1 = vlam[i];

    int kt = jvdw[k];
    real xr = xi - xred[k];
    real yr = yi - yred[k];
    real zr = zi - zred[k];
    real vlambda = vlam[k];

    if (vcouple == vcouple_decouple) {
      vlambda = (lam1 == vlambda ? 1 : (lam1 < vlambda ? lam1 : vlambda));
    } else if (vcouple == vcouple_annihilate) {
      vlambda = (lam1 < vlambda ? lam1 : vlambda);
    }

    image(xr, yr, zr, box);
    real rik2 = xr * xr + yr * yr + zr * zr;
    if (rik2 <= off2) {
      real rik = REAL_SQRT(rik2);
      real rv = radmin[it * njvdw + kt];
      real eps = epsilon[it * njvdw + kt];

      MAYBE_UNUSED real e, de;
      if_constexpr(VDWTYP == evdw_t::hal) {
        ehal_pair<do_g>(rik,                             //
                        rv, eps, ghal, dhal,             //
                        vscale, vlambda, scexp, scalpha, //
                        e, de);
      }

      if (rik2 > cut2) {
        real taper, dtaper;
        switch_taper5<do_g>(rik, cut, off, taper, dtaper);
        if_constexpr(do_g) de = e * dtaper + de * taper;
        if_constexpr(do_e) e = e * taper;
      }
      if_constexpr(do_e) {
        atomic_add_value(e, ev, offset);

        if_constexpr(do_a) {
          if (e != 0) {
            atomic_add_value(-1, nev, offset);
          }
        }
      }

      if_constexpr(do_g) {
        de *= REAL_RECIP(rik);
        real dedx = de * xr;
        real dedy = de * yr;
        real dedz = de * zr;

        atomic_add_value(dedx, gxred, i);
        atomic_add_value(dedy, gyred, i);
        atomic_add_value(dedz, gzred, i);
        atomic_add_value(-dedx, gxred, k);
        atomic_add_value(-dedy, gyred, k);
        atomic_add_value(-dedz, gzred, k);

        if_constexpr(do_v) {
          real vxx = xr * dedx;
          real vyx = yr * dedx;
          real vzx = zr * dedx;
          real vyy = yr * dedy;
          real vzy = zr * dedy;
          real vzz = zr * dedz;

          atomic_add_value(vxx, vyx, vzx, vyy, vzy, vzz, vir_ev, offset);
        } // end if (do_v)
      }   // end if (do_g)
    }
  } // end for (int ii)

  if_constexpr(do_g) evdw_resolve_gradient();
}

#define TINKER_EVDW_IMPL_(typ)                                                 \
  void evdw_##typ##_acc_impl_(int vers) {                                      \
    evdw_reduce_xyz();                                                         \
    if (vers == calc::v0)                                                      \
      evdw_tmpl<calc::v0, evdw_t::typ>();                                      \
    else if (vers == calc::v1)                                                 \
      evdw_tmpl<calc::v1, evdw_t::typ>();                                      \
    else if (vers == calc::v3)                                                 \
      evdw_tmpl<calc::v3, evdw_t::typ>();                                      \
    else if (vers == calc::v4)                                                 \
      evdw_tmpl<calc::v4, evdw_t::typ>();                                      \
    else if (vers == calc::v5)                                                 \
      evdw_tmpl<calc::v5, evdw_t::typ>();                                      \
    else if (vers == calc::v6)                                                 \
      evdw_tmpl<calc::v6, evdw_t::typ>();                                      \
  }
TINKER_EVDW_IMPL_(lj);
TINKER_EVDW_IMPL_(buck);
TINKER_EVDW_IMPL_(mm3hb);
TINKER_EVDW_IMPL_(hal);
TINKER_EVDW_IMPL_(gauss);
#undef TINKER_EVDW_IMPL_

#undef DEVICE_PTRS_

TINKER_NAMESPACE_END
