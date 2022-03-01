/**
 * Solve the z-vector equations.
 */
#include "psi4/libpsi4util/PsiOutStream.h"
#include "../dsrg_mrpt2.h"
#include "helpers/timer.h"
#include "psi4/libpsi4util/process.h"
#include "psi4/libmints/vector.h"

#include <numeric>
#include <vector>
#include <cmath>
#include <iostream>
#include <iomanip>

using namespace ambit;
using namespace psi;

namespace forte {

int max_iter = 500;
double err = 1e-9;

void DSRG_MRPT2::set_zvec_moinfo() {
    int dim_vc = nvirt * ncore, dim_ca = ncore * na, dim_va = nvirt * na,
        dim_aa = na * (na - 1) / 2, dim_ci = ndets;
    dim = dim_vc + dim_ca + dim_va + dim_aa + dim_ci;
    preidx = {{"vc", 0},
              {"VC", 0},
              {"ca", dim_vc},
              {"CA", dim_vc},
              {"va", dim_vc + dim_ca},
              {"VA", dim_vc + dim_ca},
              {"aa", dim_vc + dim_ca + dim_va},
              {"AA", dim_vc + dim_ca + dim_va},
              {"ci", dim_vc + dim_ca + dim_va + dim_aa}};

    block_dim = {{"vc", ncore}, {"VC", ncore}, {"ca", na}, {"CA", na},
                 {"va", na},    {"VA", na},    {"aa", 0},  {"AA", 0}};
}

void DSRG_MRPT2::set_z() {
    Z = BTF_->build(CoreTensor, "Z Matrix", spin_cases({"gg"}));
    outfile->Printf("\n    Initializing Diagonal Entries of the OPDM Z ..... ");
    set_z_cc();
    set_z_vv();
    set_z_aa_diag();
    outfile->Printf("Done");
    solve_linear_iter();
}

void DSRG_MRPT2::set_w() {
    outfile->Printf("\n    Solving Entries of the EWDM W.................... ");
    W = BTF_->build(CoreTensor, "Energy weighted density matrix(Lagrangian)", spin_cases({"gg"}));

    // Form Gamma_tilde
    for (const auto& pair : as_solver_->state_energies_map()) {
        const auto& state = pair.first;
        auto g1r = BTF_->build(tensor_type_, "1GRDM_ket", spin_cases({"aa"}));
        auto g2r = BTF_->build(tensor_type_, "2GRDM_ket", spin_cases({"aaaa"}));
        auto vec_ptr = x_ci.data();

        as_solver_->generalized_rdms(state, 0, vec_ptr, Gamma1_tilde, false, 1);
        as_solver_->generalized_rdms(state, 0, vec_ptr, g1r, true, 1);
        as_solver_->generalized_rdms(state, 0, vec_ptr, Gamma2_tilde, false, 2);
        as_solver_->generalized_rdms(state, 0, vec_ptr, g2r, true, 2);

        Gamma1_tilde["uv"] += g1r["uv"];
        Gamma1_tilde["UV"] += g1r["UV"];
        Gamma2_tilde["uvxy"] += g2r["uvxy"];
        Gamma2_tilde["UVXY"] += g2r["UVXY"];
        Gamma2_tilde["uVxY"] += g2r["uVxY"];
    }

    BlockedTensor temp = BTF_->build(CoreTensor, "temporal tensor", {"hhpp","hHpP"});
    // NOTICE: w for {virtual-general}
    if (CORRELATION_TERM) {
        W["pe"] += 0.5 * sigma3_xi3["ie"] * F["ip"];
        if (eri_df_) {
            W["pe"] += 2.0 * Tau1["ijeb"] * B["gpi"] * B["gbj"];
            W["pe"] += 2.0 * Tau1["iJeB"] * B["gpi"] * B["gBJ"];

            temp["kled"] += Kappa["kled"] * Eeps2_p["kled"];
            temp["kLeD"] += Kappa["kLeD"] * Eeps2_p["kLeD"];
            W["pe"] += 2.0 * temp["kled"] * B["gpk"] * B["gdl"];
            W["pe"] += 2.0 * temp["kLeD"] * B["gpk"] * B["gDL"];
        } else {
            W["pe"] +=       Tau1["ijeb"] * V["pbij"];
            W["pe"] += 2.0 * Tau1["iJeB"] * V["pBiJ"];

            temp["kled"] += Kappa["kled"] * Eeps2_p["kled"];
            temp["kLeD"] += Kappa["kLeD"] * Eeps2_p["kLeD"];
            W["pe"] +=       temp["kled"] * V["pdkl"];
            W["pe"] += 2.0 * temp["kLeD"] * V["pDkL"];
        }
        temp.zero();
    }
    W["pe"] += Z["e,m1"] * F["m1,p"];
    W["pe"] += Z["eu"] * H["vp"] * Gamma1_["uv"];
    W["pe"] += Z["eu"] * V_pmqm["vp"] * Gamma1_["uv"];
    W["pe"] += Z["e,f1"] * F["f1,p"];
    if (eri_df_) {
        W["pe"] += Z["eu"] * Gamma2_["uvxy"] * B["gxp"] * B["gyv"];
        W["pe"] += Z["eu"] * Gamma2_["uVxY"] * B["gxp"] * B["gYV"];
    } else { 
        W["ie"] += 0.5 * Z["eu"] * Gamma2_["uvxy"] * V["xyiv"];
        W["fe"] += 0.5 * Z["eu"] * Gamma2_["uvxy"] * V["fvxy"];
        W["ie"] +=       Z["eu"] * Gamma2_["uVxY"] * V["xYiV"];
        W["fe"] +=       Z["eu"] * Gamma2_["uVxY"] * V["fVxY"];
    }
    W["ei"] = W["ie"];

    // NOTICE: w for {core-hole}
    if (CORRELATION_TERM) {
        W["jm"] += 0.5 * sigma3_xi3["ma"] * F["ja"];
        if (eri_df_) {
            W["jm"] += 2.0 * sigma3_xi3["ia"] * B["gai"] * B["gmj"];
            W["jm"] -= 0.5 * sigma3_xi3["ia"] * B["gaj"] * B["gmi"];
            W["jm"] -= 0.5 * sigma3_xi3["ia"] * B["gam"] * B["gji"];

            W["im"] += 2.0 * Tau1["mjab"] * B["gai"] * B["gbj"];
            W["im"] += 2.0 * Tau1["mJaB"] * B["gai"] * B["gBJ"];

            temp["mlcd"] += Kappa["mlcd"] * Eeps2_p["mlcd"];
            temp["mLcD"] += Kappa["mLcD"] * Eeps2_p["mLcD"];
            W["im"] += 2.0 * temp["mlcd"] * B["gci"] * B["gdl"];
            W["im"] += 2.0 * temp["mLcD"] * B["gci"] * B["gDL"];
        } else {
            W["jm"] += 0.5 * sigma3_xi3["ia"] * V["amij"];
            W["jm"] += 0.5 * sigma3_xi3["IA"] * V["mAjI"];
            W["jm"] += 0.5 * sigma3_xi3["ia"] * V["ajim"];
            W["jm"] += 0.5 * sigma3_xi3["IA"] * V["jAmI"];

            W["im"] +=       Tau1["mjab"] * V["abij"];
            W["im"] += 2.0 * Tau1["mJaB"] * V["aBiJ"];

            temp["mlcd"] += Kappa["mlcd"] * Eeps2_p["mlcd"];
            temp["mLcD"] += Kappa["mLcD"] * Eeps2_p["mLcD"];
            W["im"] +=       temp["mlcd"] * V["cdil"];
            W["im"] += 2.0 * temp["mLcD"] * V["cDiL"];
        }
        temp.zero();
    }
    W["im"] += Z["e1,m"] * F["i,e1"];
    W["im"] += Z["m,n1"] * F["n1,i"];
    W["im"] += Z["mu"] * F["ui"];
    W["im"] -= Z["mu"] * H["vi"] * Gamma1_["uv"];
    W["im"] -= Z["mu"] * V_pmqm["vi"] * Gamma1_["uv"];
    if (eri_df_) {
        W["im"] += 4.0 * Z["e1,m1"] * B["g,e1,m1"] * B["gim"];
        W["im"] -= Z["e1,m1"] * B["g,e1,m"] * B["g,i,m1"];
        W["im"] -= Z["e1,m1"] * B["g,e1,i"] * B["g,m,m1"];
        W["im"] -= Z["mu"] * Gamma2_["uvxy"] * B["gxi"] * B["gyv"];
        W["im"] -= Z["mu"] * Gamma2_["uVxY"] * B["gxi"] * B["gYV"];
        W["im"] += 4.0 * Z["n1,u"] * B["g,u,n1"] * B["gim"];
        W["im"] -= Z["n1,u"] * B["gum"] * B["g,i,n1"];
        W["im"] -= Z["n1,u"] * B["gui"] * B["g,m,n1"];
        W["im"] -= 4.0 * Z["n1,u"] * Gamma1_["uv"] * B["g,v,n1"] * B["gim"];
        W["im"] += Z["n1,u"] * Gamma1_["uv"] * B["gvm"] * B["g,i,n1"];
        W["im"] += Z["n1,u"] * Gamma1_["uv"] * B["gvi"] * B["g,m,n1"];
        W["im"] += 4.0 * Z["e1,u"] * Gamma1_["uv"] * B["g,e1,v"] * B["gim"];
        W["im"] -= Z["e1,u"] * Gamma1_["uv"] * B["g,e1,m"] * B["giv"];
        W["im"] -= Z["e1,u"] * Gamma1_["uv"] * B["g,e1,i"] * B["gmv"];
        W["im"] += 2.0 * Z["m1,n1"] * B["g,n1,m1"] * B["gim"];
        W["im"] -= Z["m1,n1"] * B["g,n1,m"] * B["g,i,m1"];

        W["im"] += 2.0 * Z["uv"] * B["gvu"] * B["gim"];
        W["im"] -= Z["uv"] * B["gvm"] * B["giu"];
        W["im"] += 2.0 * Z["e1,f"] * B["g,f,e1"] * B["gim"];
        W["im"] -= Z["e1,f"] * B["gfm"] * B["g,i,e1"];
        W["nm"] +=       Gamma1_tilde["uv"] * B["guv"] * B["gmn"];
        W["nm"] -= 0.5 * Gamma1_tilde["uv"] * B["gun"] * B["gmv"];
        W["xm"] +=       Gamma1_tilde["uv"] * B["gvu"] * B["gxm"];
        W["xm"] -= 0.5 * Gamma1_tilde["uv"] * B["gvm"] * B["gxu"];
    } else {
        W["im"] += Z["e1,m1"] * V["e1,i,m1,m"];
        W["im"] += Z["E1,M1"] * V["i,E1,m,M1"];
        W["im"] += Z["e1,m1"] * V["e1,m,m1,i"];
        W["im"] += Z["E1,M1"] * V["m,E1,i,M1"];
        W["im"] -= 0.5 * Z["mu"] * V["xyiv"] * Gamma2_["uvxy"];
        W["im"] -=       Z["mu"] * V["xYiV"] * Gamma2_["uVxY"];

        W["im"] += Z["n1,u"] * V["u,i,n1,m"];
        W["im"] += Z["N1,U"] * V["i,U,m,N1"];
        W["im"] += Z["n1,u"] * V["u,m,n1,i"];
        W["im"] += Z["N1,U"] * V["m,U,i,N1"];
        W["im"] -= Z["n1,u"] * Gamma1_["uv"] * V["v,i,n1,m"];
        W["im"] -= Z["N1,U"] * Gamma1_["UV"] * V["i,V,m,N1"];
        W["im"] -= Z["n1,u"] * Gamma1_["uv"] * V["v,m,n1,i"];
        W["im"] -= Z["N1,U"] * Gamma1_["UV"] * V["m,V,i,N1"];
        W["im"] += Z["e1,u"] * Gamma1_["uv"] * V["e1,i,v,m"];
        W["im"] += Z["E1,U"] * Gamma1_["UV"] * V["i,E1,m,V"];
        W["im"] += Z["e1,u"] * Gamma1_["uv"] * V["e1,m,v,i"];
        W["im"] += Z["E1,U"] * Gamma1_["UV"] * V["m,E1,i,V"];
        W["im"] += Z["m1,n1"] * V["n1,i,m1,m"];
        W["im"] += Z["M1,N1"] * V["i,N1,m,M1"];
        W["im"] += Z["uv"] * V["vium"];
        W["im"] += Z["UV"] * V["iVmU"];
        W["im"] += Z["e1,f"] * V["f,i,e1,m"];
        W["im"] += Z["E1,F"] * V["i,F,m,E1"];
        W["nm"] += 0.5 * Gamma1_tilde["uv"] * V["umvn"];
        W["nm"] += 0.5 * Gamma1_tilde["UV"] * V["mUnV"];
        W["xm"] += 0.5 * Gamma1_tilde["uv"] * V["vxum"];
        W["xm"] += 0.5 * Gamma1_tilde["UV"] * V["xVmU"];
    }
    W["mu"] = W["um"];

    // NOTICE: w for {active-active}
    if (CORRELATION_TERM) {
        W["zw"] += 0.5 * sigma3_xi3["wa"] * F["za"];
        W["zw"] += 0.5 * sigma3_xi3["iw"] * F["iz"];
        if (eri_df_) {
            W["zw"] += sigma3_xi3["ia"] * Gamma1_["wv"] * B["gai"] * B["gzv"];
            W["zw"] -= 0.5 * sigma3_xi3["ia"] * Gamma1_["wv"] * B["gav"] * B["gzi"];
            W["zw"] += sigma3_xi3["ia"] * Gamma1_["uw"] * B["gai"] * B["guz"];
            W["zw"] -= 0.5 * sigma3_xi3["ia"] * Gamma1_["uw"] * B["gaz"] * B["gui"];
            W["zw"] += 2.0 * Tau1["ijwb"] * B["gzi"] * B["gbj"];
            W["zw"] += 2.0 * Tau1["iJwB"] * B["gzi"] * B["gBJ"];
            W["zw"] += 2.0 * Tau1["wjab"] * B["gaz"] * B["gbj"];
            W["zw"] += 2.0 * Tau1["wJaB"] * B["gaz"] * B["gBJ"];

            temp["klwd"] += Kappa["klwd"] * Eeps2_p["klwd"];
            temp["kLwD"] += Kappa["kLwD"] * Eeps2_p["kLwD"];
            W["zw"] += 2.0 * temp["klwd"] * B["gzk"] * B["gdl"];
            W["zw"] += 2.0 * temp["kLwD"] * B["gzk"] * B["gDL"];
            temp.zero();
            temp["wlcd"] += Kappa["wlcd"] * Eeps2_p["wlcd"];
            temp["wLcD"] += Kappa["wLcD"] * Eeps2_p["wLcD"];
            W["zw"] += 2.0 * temp["wlcd"] * B["gcz"] * B["gdl"];
            W["zw"] += 2.0 * temp["wLcD"] * B["gcz"] * B["gDL"];
        } else {
            W["zw"] += 0.5 * sigma3_xi3["ia"] * Gamma1_["wv"] * V["aziv"];
            W["zw"] += 0.5 * sigma3_xi3["IA"] * Gamma1_["wv"] * V["zAvI"];
            W["zw"] += 0.5 * sigma3_xi3["ia"] * Gamma1_["uw"] * V["auiz"];
            W["zw"] += 0.5 * sigma3_xi3["IA"] * Gamma1_["uw"] * V["uAzI"];

            W["zw"] += Tau1["ijwb"] * V["zbij"];
            W["zw"] += 2.0 * Tau1["iJwB"] * V["zBiJ"];

            temp["klwd"] += Kappa["klwd"] * Eeps2_p["klwd"];
            temp["kLwD"] += Kappa["kLwD"] * Eeps2_p["kLwD"];
            W["zw"] += temp["klwd"] * V["zdkl"];
            W["zw"] += 2.0 * temp["kLwD"] * V["zDkL"];
            temp.zero();

            W["zw"] += Tau1["wjab"] * V["abzj"];
            W["zw"] += 2.0 * Tau1["wJaB"] * V["aBzJ"];

            temp["wlcd"] += Kappa["wlcd"] * Eeps2_p["wlcd"];
            temp["wLcD"] += Kappa["wLcD"] * Eeps2_p["wLcD"];
            W["zw"] += temp["wlcd"] * V["cdzl"];
            W["zw"] += 2.0 * temp["wLcD"] * V["cDzL"];
        }
        temp.zero();
    }
    W["zw"] += Z["wv"] * F["vz"];
    W["zw"] += Z["n1,w"] * F["z,n1"];
    W["zw"] += 0.50 * H["vz"] * Gamma1_tilde["wv"];
    W["zw"] -= Z["n1,u"] * H["z,n1"] * Gamma1_["uw"];
    W["zw"] -= Z["n1,u"] * V_pmqm["z,n1"] * Gamma1_["uw"];
    W["zw"] += Z["e1,u"] * H["z,e1"] * Gamma1_["uw"];
    W["zw"] += Z["e1,u"] * V_pmqm["z,e1"] * Gamma1_["uw"];
    W["zw"] += 0.50 * V_pmqm["uz"] * Gamma1_tilde["uw"];
    if (eri_df_) {
        W["zw"] += 4.0 * Z["e1,m1"] * Gamma1_["uw"] * B["g,e1,m1"] * B["guz"];
        W["zw"] -= Z["e1,m1"] * Gamma1_["uw"] * B["g,e1,z"] * B["g,u,m1"];
        W["zw"] -= Z["e1,m1"] * Gamma1_["uw"] * B["g,e1,u"] * B["g,z,m1"];
        W["zw"] += 4.0 * Z["n1,u"] * Gamma1_["wv"] * B["g,u,n1"] * B["gvz"];
        W["zw"] -= Z["n1,u"] * Gamma1_["wv"] * B["g,u,z"] * B["g,v,n1"];
        W["zw"] -= Z["n1,u"] * Gamma1_["wv"] * B["guv"] * B["g,z,n1"];
        W["zw"] -= Z["n1,u"] * Gamma2_["uwxy"] * B["g,x,n1"] * B["gyz"];
        W["zw"] -= Z["N1,U"] * Gamma2_["w,U,y,X"] * B["gyz"] * B["g,X,N1"];
        W["zw"] -= Z["n1,u"] * Gamma2_["u,v,w,y"] * B["g,z,n1"] * B["gyv"];
        W["zw"] += Z["n1,u"] * Gamma2_["u,v,w,y"] * B["gzv"] * B["g,y,n1"];
        W["zw"] -= Z["n1,u"] * Gamma2_["u,V,w,Y"] * B["g,z,n1"] * B["gYV"];
        W["zw"] -= Z["N1,U"] * Gamma2_["v,U,w,Y"] * B["gzv"] * B["g,Y,N1"];
        W["zw"] += Z["e1,u"] * Gamma2_["u,w,x,y"] * B["g,e1,x"] * B["gzy"];
        W["zw"] += Z["E1,U"] * Gamma2_["w,U,y,X"] * B["gzy"] * B["g,E1,X"];
        W["zw"] += Z["e1,u"] * Gamma2_["u,v,w,y"] * B["g,e1,z"] * B["gvy"];
        W["zw"] -= Z["e1,u"] * Gamma2_["u,v,w,y"] * B["g,e1,y"] * B["gvz"];
        W["zw"] += Z["e1,u"] * Gamma2_["u,V,w,Y"] * B["g,e1,z"] * B["gVY"];
        W["zw"] += Z["E1,U"] * Gamma2_["v,U,w,Y"] * B["gvz"] * B["g,E1,Y"];
        W["zw"] += 2.0 * Z["m1,n1"] * Gamma1_["wv"] * B["g,n1,m1"] * B["gvz"];
        W["zw"] -= Z["m1,n1"] * Gamma1_["wv"] * B["g,n1,z"] * B["g,v,m1"];
        W["zw"] += 2.0 * Z["e1,f1"] * Gamma1_["wv"] * B["g,f1,e1"] * B["gvz"];
        W["zw"] -= Z["e1,f1"] * Gamma1_["wv"] * B["g,f1,z"] * B["g,v,e1"];
        W["zw"] += 2.0 * Z["u1,a1"] * Gamma1_["wv"] * B["g,a1,u1"] * B["gvz"];
        W["zw"] -= Z["u1,a1"] * Gamma1_["wv"] * B["g,a1,z"] * B["g,v,u1"];
        W["zw"] += 0.5 * Gamma2_tilde["wvxy"] * B["gzx"] * B["gvy"];
        W["zw"] += 0.5 * Gamma2_tilde["wVxY"] * B["gzx"] * B["gVY"];
    } else {
        W["zw"] += Z["e1,m1"] * V["e1,u,m1,z"] * Gamma1_["uw"];
        W["zw"] += Z["E1,M1"] * V["u,E1,z,M1"] * Gamma1_["uw"];
        W["zw"] += Z["e1,m1"] * V["e1,z,m1,u"] * Gamma1_["uw"];
        W["zw"] += Z["E1,M1"] * V["z,E1,u,M1"] * Gamma1_["uw"];
        W["zw"] += Z["n1,u"] * V["u,v,n1,z"] * Gamma1_["wv"];
        W["zw"] += Z["N1,U"] * V["v,U,z,N1"] * Gamma1_["wv"];
        W["zw"] += Z["n1,u"] * V["u,z,n1,v"] * Gamma1_["wv"];
        W["zw"] += Z["N1,U"] * V["z,U,v,N1"] * Gamma1_["wv"];
        W["zw"] -= 0.5 * Z["n1,u"] * V["x,y,n1,z"] * Gamma2_["u,w,x,y"];
        W["zw"] -= Z["N1,U"] * V["y,X,z,N1"] * Gamma2_["w,U,y,X"];
        W["zw"] -= Z["n1,u"] * V["z,y,n1,v"] * Gamma2_["u,v,w,y"];
        W["zw"] -= Z["n1,u"] * V["z,Y,n1,V"] * Gamma2_["u,V,w,Y"];
        W["zw"] -= Z["N1,U"] * V["z,Y,v,N1"] * Gamma2_["v,U,w,Y"];
        W["zw"] += 0.5 * Z["e1,u"] * V["e1,z,x,y"] * Gamma2_["u,w,x,y"];
        W["zw"] += Z["E1,U"] * V["z,E1,y,X"] * Gamma2_["w,U,y,X"];
        W["zw"] += Z["e1,u"] * V["e1,v,z,y"] * Gamma2_["u,v,w,y"];
        W["zw"] += Z["e1,u"] * V["e1,V,z,Y"] * Gamma2_["u,V,w,Y"];
        W["zw"] += Z["E1,U"] * V["v,E1,z,Y"] * Gamma2_["v,U,w,Y"];

        W["zw"] += Z["m1,n1"] * V["n1,v,m1,z"] * Gamma1_["wv"];
        W["zw"] += Z["M1,N1"] * V["v,N1,z,M1"] * Gamma1_["wv"];
        W["zw"] += Z["e1,f1"] * V["f1,v,e1,z"] * Gamma1_["wv"];
        W["zw"] += Z["E1,F1"] * V["v,F1,z,E1"] * Gamma1_["wv"];

        W["zw"] += Z["u1,a1"] * V["a1,v,u1,z"] * Gamma1_["wv"];
        W["zw"] += Z["U1,A1"] * V["v,A1,z,U1"] * Gamma1_["wv"];
        W["zw"] += 0.25 * V["zvxy"] * Gamma2_tilde["wvxy"];
        W["zw"] += 0.50 * V["zVxY"] * Gamma2_tilde["wVxY"];
    }

    // CASSCF reference
    BlockedTensor temp1 = BTF_->build(CoreTensor, "temporal tensor 1", {"ag"});
    W["mp"] += F["mp"];
    temp1["vp"] = H["vp"];
    temp1["vp"] += V_pmqm["vp"];
    W["up"] += temp1["vp"] * Gamma1_["uv"];
    if (eri_df_) {
        W["ui"] += Gamma2_["uvxy"] * B["gxi"] * B["gyv"];
        W["ue"] += Gamma2_["uvxy"] * B["gex"] * B["gvy"];
        W["ui"] += Gamma2_["uVxY"] * B["gxi"] * B["gYV"];
        W["ue"] += Gamma2_["uVxY"] * B["gex"] * B["gVY"];
    } else {    
        W["ui"] += 0.5 * Gamma2_["uvxy"] * V["xyiv"];
        W["ue"] += 0.5 * Gamma2_["uvxy"] * V["evxy"];
        W["ui"] +=       Gamma2_["uVxY"] * V["xYiV"];
        W["ue"] +=       Gamma2_["uVxY"] * V["eVxY"];
    }

    // Copy alpha-alpha to beta-beta
    (W.block("CC")).iterate([&](const std::vector<size_t>& i, double& value) {
        value = W.block("cc").data()[i[0] * ncore + i[1]];
    });
    (W.block("AA")).iterate([&](const std::vector<size_t>& i, double& value) {
        value = W.block("aa").data()[i[0] * na + i[1]];
    });
    (W.block("VV")).iterate([&](const std::vector<size_t>& i, double& value) {
        value = W.block("vv").data()[i[0] * nvirt + i[1]];
    });
    (W.block("CV")).iterate([&](const std::vector<size_t>& i, double& value) {
        value = W.block("cv").data()[i[0] * nvirt + i[1]];
    });
    (W.block("VC")).iterate([&](const std::vector<size_t>& i, double& value) {
        value = W.block("vc").data()[i[0] * ncore + i[1]];
    });
    (W.block("CA")).iterate([&](const std::vector<size_t>& i, double& value) {
        value = W.block("ca").data()[i[0] * na + i[1]];
    });
    (W.block("AC")).iterate([&](const std::vector<size_t>& i, double& value) {
        value = W.block("ac").data()[i[0] * ncore + i[1]];
    });
    (W.block("AV")).iterate([&](const std::vector<size_t>& i, double& value) {
        value = W.block("av").data()[i[0] * nvirt + i[1]];
    });
    (W.block("VA")).iterate([&](const std::vector<size_t>& i, double& value) {
        value = W.block("va").data()[i[0] * na + i[1]];
    });
    outfile->Printf("Done");
}

void DSRG_MRPT2::set_z_cc() {
    BlockedTensor val1 = BTF_->build(CoreTensor, "val1", {"c"});
    BlockedTensor temp = BTF_->build(CoreTensor, "temporal tensor", spin_cases({"hhpp"}));
    BlockedTensor temp_1 = BTF_->build(CoreTensor, "temporal tensor_1", spin_cases({"hhpp"}));

    // core-core diagonal entries
    if (CORRELATION_TERM) {
        val1["m"] -= sigma1_xi1_xi2["ma"] * F["ma"];
        val1["m"] -= DelGam1["xu"] * T2_["muax"] * sigma1_xi1_xi2["ma"];
        val1["m"] -= DelGam1["XU"] * T2_["mUaX"] * sigma1_xi1_xi2["ma"];
        val1["m"] -= 2.0 * T2OverDelta["mjab"] * Tau2["mjab"];
        val1["m"] -= 4.0 * T2OverDelta["mJaB"] * Tau2["mJaB"];

        if (eri_df_) {
            temp["mjab"] += 2.0 * Eeps2["mjab"] * B["gam"] * B["gbj"];
            temp["mJaB"] += Eeps2["mJaB"] * B["gam"] * B["gBJ"];
            val1["m"] += 4.0 * s_ * Tau2["mjab"] * temp["mjab"];
            val1["m"] += 8.0 * s_ * Tau2["mJaB"] * temp["mJaB"];
            temp.zero();

            temp["mlcd"] += 2.0 * Eeps2["mlcd"] * B["gcm"] * B["gdl"];
            temp["mLcD"] += Eeps2["mLcD"] * B["gcm"] * B["gDL"];
            temp_1["mlcd"] += Kappa["mlcd"] * Delta2["mlcd"];
            temp_1["mLcD"] += Kappa["mLcD"] * Delta2["mLcD"];
            val1["m"] -= 4.0 * s_ * temp["mlcd"] * temp_1["mlcd"];
            val1["m"] -= 8.0 * s_ * temp["mLcD"] * temp_1["mLcD"];
            temp.zero();
            temp_1.zero();
        } else {
            temp["mjab"] += V["abmj"] * Eeps2["mjab"];
            temp["mJaB"] += V["aBmJ"] * Eeps2["mJaB"];
            val1["m"] += 4.0 * s_ * Tau2["mjab"] * temp["mjab"];
            val1["m"] += 8.0 * s_ * Tau2["mJaB"] * temp["mJaB"];
            temp.zero();

            temp["mlcd"] += V["cdml"] * Eeps2["mlcd"];
            temp["mLcD"] += V["cDmL"] * Eeps2["mLcD"];
            temp_1["mlcd"] += Kappa["mlcd"] * Delta2["mlcd"];
            temp_1["mLcD"] += Kappa["mLcD"] * Delta2["mLcD"];
            val1["m"] -= 4.0 * s_ * temp["mlcd"] * temp_1["mlcd"];
            val1["m"] -= 8.0 * s_ * temp["mLcD"] * temp_1["mLcD"];
            temp.zero();
            temp_1.zero();
        }
    }
    BlockedTensor zmn = BTF_->build(CoreTensor, "z{mn} normal", {"cc"});
    // core-core block entries within normal conditions
    if (CORRELATION_TERM) {
        zmn["mn"] += 0.5 * sigma3_xi3["na"] * F["ma"];
        zmn["mn"] -= 0.5 * sigma3_xi3["ma"] * F["na"];

        if (eri_df_) {
            zmn["mn"] += 2.0 * Tau1["njab"] * B["gam"] * B["gbj"];
            zmn["mn"] += 2.0 * Tau1["nJaB"] * B["gam"] * B["gBJ"];
            zmn["mn"] -= 2.0 * Tau1["mjab"] * B["gan"] * B["gbj"];
            zmn["mn"] -= 2.0 * Tau1["mJaB"] * B["gan"] * B["gBJ"];

            temp["nlcd"] += Kappa["nlcd"] * Eeps2_p["nlcd"];
            temp["nLcD"] += Kappa["nLcD"] * Eeps2_p["nLcD"];
            zmn["mn"] += 2.0 * temp["nlcd"] * B["gcm"] * B["gdl"];
            zmn["mn"] += 2.0 * temp["nLcD"] * B["gcm"] * B["gDL"];
            temp.zero();
            temp["mlcd"] += Kappa["mlcd"] * Eeps2_p["mlcd"];
            temp["mLcD"] += Kappa["mLcD"] * Eeps2_p["mLcD"];
            zmn["mn"] -= 2.0 * temp["mlcd"] * B["gcn"] * B["gdl"];
            zmn["mn"] -= 2.0 * temp["mLcD"] * B["gcn"] * B["gDL"];
            temp.zero();
        } else {
            zmn["mn"] +=       Tau1["njab"] * V["abmj"];
            zmn["mn"] += 2.0 * Tau1["nJaB"] * V["aBmJ"];
            zmn["mn"] -=       Tau1["mjab"] * V["abnj"];
            zmn["mn"] -= 2.0 * Tau1["mJaB"] * V["aBnJ"];

            temp["nlcd"] += Kappa["nlcd"] * Eeps2_p["nlcd"];
            temp["nLcD"] += Kappa["nLcD"] * Eeps2_p["nLcD"];
            zmn["mn"] +=       temp["nlcd"] * V["cdml"];
            zmn["mn"] += 2.0 * temp["nLcD"] * V["cDmL"];
            temp.zero();
            temp["mlcd"] += Kappa["mlcd"] * Eeps2_p["mlcd"];
            temp["mLcD"] += Kappa["mLcD"] * Eeps2_p["mLcD"];
            zmn["mn"] -=       temp["mlcd"] * V["cdnl"];
            zmn["mn"] -= 2.0 * temp["mLcD"] * V["cDnL"];
            temp.zero();
        }
    }

    for (const std::string& block : {"cc", "CC"}) {
        (Z.block(block)).iterate([&](const std::vector<size_t>& i, double& value) {
            if (i[0] == i[1]) {
                value = val1.block("c").data()[i[0]];
            } else {
                auto dmt = Delta1.block("cc").data()[i[1] * ncore + i[0]];
                if (std::fabs(dmt) > 1e-12) {
                    value = zmn.block("cc").data()[i[0] * ncore + i[1]] / dmt;
                }
            }
        });
    }
}

void DSRG_MRPT2::set_z_vv() {
    BlockedTensor val2 = BTF_->build(CoreTensor, "val2", {"v"});
    BlockedTensor temp = BTF_->build(CoreTensor, "temporal tensor", spin_cases({"hhpp"}));
    BlockedTensor temp_1 = BTF_->build(CoreTensor, "temporal tensor_1", spin_cases({"hhpp"}));

    // virtual-virtual diagonal entries
    if (CORRELATION_TERM) {
        val2["e"] += sigma1_xi1_xi2["ie"] * F["ie"];
        val2["e"] += DelGam1["xu"] * T2_["iuex"] * sigma1_xi1_xi2["ie"];
        val2["e"] += DelGam1["XU"] * T2_["iUeX"] * sigma1_xi1_xi2["ie"];
        val2["e"] += 2.0 * T2OverDelta["ijeb"] * Tau2["ijeb"];
        val2["e"] += 4.0 * T2OverDelta["iJeB"] * Tau2["iJeB"];
        if (eri_df_) {
            temp["ijeb"] += 2.0 * Eeps2["ijeb"] * B["gei"] * B["gbj"];
            temp["iJeB"] += Eeps2["iJeB"] * B["gei"] * B["gBJ"];
            val2["e"] -= 4.0 * s_ * Tau2["ijeb"] * temp["ijeb"];
            val2["e"] -= 8.0 * s_ * Tau2["iJeB"] * temp["iJeB"];
            temp.zero();
            temp["kled"] += 2.0 * Eeps2["kled"] * B["gek"] * B["gdl"];
            temp["kLeD"] += Eeps2["kLeD"] * B["gek"] * B["gDL"];
            temp_1["kled"] += Kappa["kled"] * Delta2["kled"];
            temp_1["kLeD"] += Kappa["kLeD"] * Delta2["kLeD"];
            val2["e"] += 4.0 * s_ * temp["kled"] * temp_1["kled"];
            val2["e"] += 8.0 * s_ * temp["kLeD"] * temp_1["kLeD"];
            temp.zero();
            temp_1.zero();
        } else {
            temp["ijeb"] += V["ebij"] * Eeps2["ijeb"];
            temp["iJeB"] += V["eBiJ"] * Eeps2["iJeB"];
            val2["e"] -= 4.0 * s_ * Tau2["ijeb"] * temp["ijeb"];
            val2["e"] -= 8.0 * s_ * Tau2["iJeB"] * temp["iJeB"];
            temp.zero();

            temp["kled"] += V["edkl"] * Eeps2["kled"];
            temp["kLeD"] += V["eDkL"] * Eeps2["kLeD"];
            temp_1["kled"] += Kappa["kled"] * Delta2["kled"];
            temp_1["kLeD"] += Kappa["kLeD"] * Delta2["kLeD"];
            val2["e"] += 4.0 * s_ * temp["kled"] * temp_1["kled"];
            val2["e"] += 8.0 * s_ * temp["kLeD"] * temp_1["kLeD"];
            temp.zero();
            temp_1.zero();
        }
    }

    BlockedTensor zef = BTF_->build(CoreTensor, "z{ef} normal", {"vv"});
    // virtual-virtual block entries within normal conditions
    if (CORRELATION_TERM) {
        zef["ef"] += 0.5 * sigma3_xi3["if"] * F["ie"];
        zef["ef"] -= 0.5 * sigma3_xi3["ie"] * F["if"];
        if (eri_df_) {
            zef["ef"] += 2.0 * Tau1["ijfb"] * B["gei"] * B["gbj"];
            zef["ef"] += 2.0 * Tau1["iJfB"] * B["gei"] * B["gBJ"];
            zef["ef"] -= 2.0 * Tau1["ijeb"] * B["gfi"] * B["gbj"];
            zef["ef"] -= 2.0 * Tau1["iJeB"] * B["gfi"] * B["gBJ"];

            temp["klfd"] += Kappa["klfd"] * Eeps2_p["klfd"];
            temp["kLfD"] += Kappa["kLfD"] * Eeps2_p["kLfD"];
            zef["ef"] += 2.0 * temp["klfd"] * B["gek"] * B["gdl"];
            zef["ef"] += 2.0 * temp["kLfD"] * B["gek"] * B["gDL"];
            temp.zero();
            temp["kled"] += Kappa["kled"] * Eeps2_p["kled"];
            temp["kLeD"] += Kappa["kLeD"] * Eeps2_p["kLeD"];
            zef["ef"] -= 2.0 * temp["kled"] * B["gfk"] * B["gdl"];
            zef["ef"] -= 2.0 * temp["kLeD"] * B["gfk"] * B["gDL"];
            temp.zero();
        } else {
            zef["ef"] +=       Tau1["ijfb"] * V["ebij"];
            zef["ef"] += 2.0 * Tau1["iJfB"] * V["eBiJ"];

            temp["klfd"] += Kappa["klfd"] * Eeps2_p["klfd"];
            temp["kLfD"] += Kappa["kLfD"] * Eeps2_p["kLfD"];
            zef["ef"] +=       temp["klfd"] * V["edkl"];
            zef["ef"] += 2.0 * temp["kLfD"] * V["eDkL"];
            temp.zero();

            zef["ef"] -=       Tau1["ijeb"] * V["fbij"];
            zef["ef"] -= 2.0 * Tau1["iJeB"] * V["fBiJ"];

            temp["kled"] += Kappa["kled"] * Eeps2_p["kled"];
            temp["kLeD"] += Kappa["kLeD"] * Eeps2_p["kLeD"];
            zef["ef"] -=       temp["kled"] * V["fdkl"];
            zef["ef"] -= 2.0 * temp["kLeD"] * V["fDkL"];
            temp.zero();
        }
    }

    for (const std::string& block : {"vv", "VV"}) {
        (Z.block(block)).iterate([&](const std::vector<size_t>& i, double& value) {
            if (i[0] == i[1]) {
                value = val2.block("v").data()[i[0]];
            } else {
                auto dmt = Delta1.block("vv").data()[i[1] * nvirt + i[0]];
                if (std::fabs(dmt) > 1e-12) {
                    value = zef.block("vv").data()[i[0] * nvirt + i[1]] / dmt;
                }
            }
        });
    }
}

void DSRG_MRPT2::set_z_aa_diag() {
    BlockedTensor val3 = BTF_->build(CoreTensor, "val3", {"a"});
    BlockedTensor temp = BTF_->build(CoreTensor, "temporal tensor", spin_cases({"hhpp"}));
    BlockedTensor temp_1 = BTF_->build(CoreTensor, "temporal tensor_1", spin_cases({"hhpp"}));

    // active-active diagonal entries
    if (CORRELATION_TERM) {
        val3["w"] -=  sigma1_xi1_xi2["wa"] * F["wa"];
        val3["w"] -=  DelGam1["xu"] * T2_["wuax"] * sigma1_xi1_xi2["wa"];
        val3["w"] -=  DelGam1["XU"] * T2_["wUaX"] * sigma1_xi1_xi2["wa"];
        val3["w"] +=  sigma1_xi1_xi2["iw"] * F["iw"];
        val3["w"] +=  DelGam1["xu"] * T2_["iuwx"] * sigma1_xi1_xi2["iw"];
        val3["w"] +=  DelGam1["XU"] * T2_["iUwX"] * sigma1_xi1_xi2["iw"];

        val3["w"] += sigma2_xi3["ia"] * T2_["iuaw"] * Gamma1_["wu"];
        val3["w"] += sigma2_xi3["IA"] * T2_["uIwA"] * Gamma1_["wu"];
        val3["w"] -= sigma2_xi3["ia"] * T2_["iwax"] * Gamma1_["xw"];
        val3["w"] -= sigma2_xi3["IA"] * T2_["wIxA"] * Gamma1_["xw"];

        val3["u"] -= 2.0 * T2OverDelta["ujab"] * Tau2["ujab"];
        val3["u"] -= 4.0 * T2OverDelta["uJaB"] * Tau2["uJaB"];
        val3["u"] += 2.0 * T2OverDelta["ijub"] * Tau2["ijub"];
        val3["u"] += 4.0 * T2OverDelta["iJuB"] * Tau2["iJuB"];
        if (eri_df_) {
            temp["ujab"] += 2.0 * Eeps2["ujab"] * B["gau"] * B["gbj"];
            temp["uJaB"] += Eeps2["uJaB"] * B["gau"] * B["gBJ"];
            val3["u"] += 4.0 * s_ * Tau2["ujab"] * temp["ujab"];
            val3["u"] += 8.0 * s_ * Tau2["uJaB"] * temp["uJaB"];
            temp.zero();

            temp["ulcd"] += 2.0 * Eeps2["ulcd"] * B["gcu"] * B["gdl"];
            temp["uLcD"] += Eeps2["uLcD"] * B["gcu"] * B["gDL"];
            temp_1["ulcd"] += Kappa["ulcd"] * Delta2["ulcd"];
            temp_1["uLcD"] += Kappa["uLcD"] * Delta2["uLcD"];
            val3["u"] -= 4.0 * s_ * temp["ulcd"] * temp_1["ulcd"];
            val3["u"] -= 8.0 * s_ * temp["uLcD"] * temp_1["uLcD"];
            temp.zero();
            temp_1.zero();

            temp["ijub"] += 2.0 * Eeps2["ijub"] * B["gui"] * B["gbj"];
            temp["iJuB"] += Eeps2["iJuB"] * B["gui"] * B["gBJ"];
            val3["u"] -= 4.0 * s_ * Tau2["ijub"] * temp["ijub"];
            val3["u"] -= 8.0 * s_ * Tau2["iJuB"] * temp["iJuB"];
            temp.zero();

            temp["klud"] += 2.0 * Eeps2["klud"] * B["guk"] * B["gdl"];
            temp["kLuD"] += Eeps2["kLuD"] * B["guk"] * B["gDL"];
            temp_1["klud"] += Kappa["klud"] * Delta2["klud"];
            temp_1["kLuD"] += Kappa["kLuD"] * Delta2["kLuD"];
            val3["u"] += 4.0 * s_ * temp["klud"] * temp_1["klud"];
            val3["u"] += 8.0 * s_ * temp["kLuD"] * temp_1["kLuD"];
            temp.zero();
            temp_1.zero();
        } else {
            temp["ujab"] += V["abuj"] * Eeps2["ujab"];
            temp["uJaB"] += V["aBuJ"] * Eeps2["uJaB"];
            val3["u"] += 4.0 * s_ * Tau2["ujab"] * temp["ujab"];
            val3["u"] += 8.0 * s_ * Tau2["uJaB"] * temp["uJaB"];
            temp.zero();

            temp["ulcd"] += V["cdul"] * Eeps2["ulcd"];
            temp["uLcD"] += V["cDuL"] * Eeps2["uLcD"];
            temp_1["ulcd"] += Kappa["ulcd"] * Delta2["ulcd"];
            temp_1["uLcD"] += Kappa["uLcD"] * Delta2["uLcD"];
            val3["u"] -= 4.0 * s_ * temp["ulcd"] * temp_1["ulcd"];
            val3["u"] -= 8.0 * s_ * temp["uLcD"] * temp_1["uLcD"];
            temp.zero();
            temp_1.zero();

            temp["ijub"] += V["ubij"] * Eeps2["ijub"];
            temp["iJuB"] += V["uBiJ"] * Eeps2["iJuB"];
            val3["u"] -= 4.0 * s_ * Tau2["ijub"] * temp["ijub"];
            val3["u"] -= 8.0 * s_ * Tau2["iJuB"] * temp["iJuB"];
            temp.zero();

            temp["klud"] += V["udkl"] * Eeps2["klud"];
            temp["kLuD"] += V["uDkL"] * Eeps2["kLuD"];
            temp_1["klud"] += Kappa["klud"] * Delta2["klud"];
            temp_1["kLuD"] += Kappa["kLuD"] * Delta2["kLuD"];
            val3["u"] += 4.0 * s_ * temp["klud"] * temp_1["klud"];
            val3["u"] += 8.0 * s_ * temp["kLuD"] * temp_1["kLuD"];
            temp.zero();
            temp_1.zero();
        }   
    }
    for (const std::string& block : {"aa", "AA"}) {
        (Z.block(block)).iterate([&](const std::vector<size_t>& i, double& value) {
            if (i[0] == i[1]) {
                value = val3.block("a").data()[i[0]];
            }
        });
    }
}

double f_norm(std::vector<double> const& vec) {
    double accum = 0.0;
    for (int i = 0; i < vec.size(); ++i) {
        accum += vec[i] * vec[i];
    }
    return std::sqrt(accum);
}

double diff_f_norm(std::vector<double> const& vec1, std::vector<double> const& vec2) {
    double accum = 0.0;
    if (vec1.size() != vec2.size()) {
        exit(1);
    }
    for (int i = 0; i < vec1.size(); ++i) {
        double diff = vec1[i] - vec2[i];
        accum += diff * diff;
    }
    return std::sqrt(accum);
}

void DSRG_MRPT2::z_vector_contraction(std::vector<double> & qk_vec, std::vector<double> & y_vec) {
    BlockedTensor qk = BTF_->build(CoreTensor, "vector qk (orbital rotation) in GMRES", {"vc", "VC", "ca", "CA", "va", "VA", "aa", "AA"});
    auto qk_ci = ambit::Tensor::build(ambit::CoreTensor, "qk (ci) in GMRES", {ndets});

    for (const std::string& row : {"vc", "VC", "ca", "CA", "va", "VA", "aa", "AA"}) {
        int idx1 = block_dim[row];
        int pre1 = preidx[row];
        if (row != "aa" && row != "AA") {
            qk.block(row).iterate([&](const std::vector<size_t>& i, double& value) {
                int index = pre1 + i[0] * idx1 + i[1];
                value = qk_vec.at(index);
            });
        } else {
            qk.block(row).iterate([&](const std::vector<size_t>& i, double& value) {
                int i0 = i[0] > i[1] ? i[0] : i[1], i1 = i[0] > i[1] ? i[1] : i[0];
                if (i[0] != i[1]) {
                    int index = pre1 + i0 * (i0 - 1) / 2 + i1;
                    value = qk_vec.at(index);
                }
            });
        }
    }

    for (const std::string& row : {"ci"}) {
        int pre1 = preidx[row];
        (qk_ci).iterate([&](const std::vector<size_t>& i, double& value) {
            int index = pre1 + i[0];
            value = qk_vec.at(index);
        });
    }

    BlockedTensor y = BTF_->build(CoreTensor, "y (orbital rotation) in GMRES", {"vc", "ca", "va", "aa"});
    auto y_ci = ambit::Tensor::build(ambit::CoreTensor, "y (ci) in GMRES", {ndets});

    /// MO RESPONSE -- MO RESPONSE
    // VIRTUAL-CORE
    y["em"] += Delta1["me"] * qk["em"];
    y["em"] -= F["ue"] * qk["mu"];
    y["em"] += F["um"] * qk["eu"];
    if (eri_df_) {
        y["em"] -= 4.0 * qk["e1,m1"] * B["g,e1,m1"] * B["gem"];
        y["em"] +=       qk["e1,m1"] * B["g,e1,m"] * B["g,e,m1"];
        y["em"] +=       qk["e1,m1"] * B["g,m1,m"] * B["g,e,e1"];
        y["em"] -= 4.0 * qk["n1,u"] * B["g,u,n1"] * B["gem"];
        y["em"] +=       qk["n1,u"] * B["g,u,m"] * B["g,e,n1"];
        y["em"] +=       qk["n1,u"]  * B["g,n1,m"] * B["geu"];
        y["em"] -= 2.0 * qk["uv"] * B["gvu"] * B["gem"];
        y["em"] +=       qk["uv"] * B["gvm"] * B["geu"];
        y["em"] += 4.0 * Gamma1_["uv"] * qk["n1,u"] * B["g,v,n1"] * B["gem"];
        y["em"] -=       Gamma1_["uv"] * qk["n1,u"] * B["gvm"] * B["g,e,n1"];
        y["em"] -=       Gamma1_["uv"] * qk["n1,u"] * B["g,n1,m"] * B["gev"];
        y["em"] -= 4.0 * Gamma1_["uv"] * qk["e1,u"] * B["g,e1,v"] * B["gem"];
        y["em"] +=       Gamma1_["uv"] * qk["e1,u"] * B["g,e1,m"] * B["gev"];
        y["em"] +=       Gamma1_["uv"] * qk["e1,u"] * B["gvm"] * B["g,e,e1"];
    } else {
        y["em"] -= qk["e1,m1"] * V["e1,e,m1,m"];
        y["em"] -= qk["E1,M1"] * V["e,E1,m,M1"];
        y["em"] -= qk["n1,u"] * V["u,e,n1,m"];
        y["em"] -= qk["N1,U"] * V["e,U,m,N1"];
        y["em"] -= qk["uv"] * V["veum"];
        y["em"] -= qk["UV"] * V["eVmU"];
        y["em"] += Gamma1_["uv"] * qk["n1,u"] * V["v,e,n1,m"];
        y["em"] += Gamma1_["UV"] * qk["N1,U"] * V["e,V,m,N1"];
        y["em"] -= Gamma1_["uv"] * qk["e1,u"] * V["e1,e,v,m"];
        y["em"] -= Gamma1_["UV"] * qk["E1,U"] * V["e,E1,m,V"];
        y["em"] -= V["m1,e,e1,m"] * qk["e1,m1"];
        y["em"] -= V["e,M1,m,E1"] * qk["E1,M1"];
        y["em"] -= V["n1,e,u,m"] * qk["n1,u"];
        y["em"] -= V["e,N1,m,U"] * qk["N1,U"];
        y["em"] += Gamma1_["uv"] * V["n1,e,v,m"] * qk["n1,u"];
        y["em"] += Gamma1_["UV"] * V["e,N1,m,V"] * qk["N1,U"];
        y["em"] -= Gamma1_["uv"] * V["v,e,e1,m"] * qk["e1,u"];
        y["em"] -= Gamma1_["UV"] * V["e,V,m,E1"] * qk["E1,U"];
    }

    // CORE-ACTIVE
    y["mw"] += F["we"] * qk["em"];
    y["mw"] -= F["vm"] * qk["wv"];
    y["mw"] += F["uw"] * qk["mu"];
    y["mw"] -= F["m,n1"] * qk["n1,w"];
    y["mw"] -= H["vw"] * Gamma1_["uv"] * qk["mu"];
    y["mw"] -= V_pmqm["v,w"] * Gamma1_["uv"] * qk["mu"];
    y["mw"] += H["m,n1"] * Gamma1_["uw"] * qk["n1,u"];
    y["mw"] += V_pmqm["m,n1"] * Gamma1_["uw"] * qk["n1,u"];
    y["mw"] -= H["m,e1"] * Gamma1_["uw"] * qk["e1,u"];
    y["mw"] -= V_pmqm["e1,m"] * Gamma1_["uw"] * qk["e1,u"];
    if (eri_df_) {
        y["mw"] += 4.0 * qk["e1,m1"] * B["g,e1,m1"] * B["gwm"];
        y["mw"] -=       qk["e1,m1"] * B["g,e1,m"] * B["g,w,m1"];
        y["mw"] -=       qk["e1,m1"] * B["g,e1,w"] * B["g,m,m1"];
        y["mw"] += 4.0 * qk["n1,u"] * B["g,u,n1"] * B["gwm"];
        y["mw"] -=       qk["n1,u"] * B["gum"] * B["g,w,n1"];
        y["mw"] -=       qk["n1,u"] * B["guw"] * B["g,m,n1"];
        y["mw"] -= 4.0 * Gamma1_["uv"] * qk["n1,u"] * B["g,v,n1"] * B["gwm"];
        y["mw"] +=       Gamma1_["uv"] * qk["n1,u"] * B["gvm"] * B["g,w,n1"];
        y["mw"] +=       Gamma1_["uv"] * qk["n1,u"] * B["gvw"] * B["g,m,n1"];
        y["mw"] += 4.0 * Gamma1_["uv"] * qk["e1,u"] * B["g,e1,v"] * B["gwm"];
        y["mw"] -=       Gamma1_["uv"] * qk["e1,u"] * B["g,e1,m"] * B["gwv"];
        y["mw"] -=       Gamma1_["uv"] * qk["e1,u"] * B["g,e1,w"] * B["gmv"];
        y["mw"] += 2.0 * qk["uv"] * B["gvu"] * B["gwm"];
        y["mw"] -=       qk["uv"] * B["gvm"] * B["gwu"];
        y["mw"] -= 4.0 * Gamma1_["uw"] * qk["e1,m1"] * B["g,e1,m1"] * B["gum"];
        y["mw"] +=       Gamma1_["uw"] * qk["e1,m1"] * B["g,e1,m"] * B["g,u,m1"];
        y["mw"] +=       Gamma1_["uw"] * qk["e1,m1"] * B["g,e1,u"] * B["g,m,m1"];
        y["mw"] -= 4.0 * Gamma1_["wv"] * qk["n1,u"] * B["g,u,n1"] * B["gvm"];
        y["mw"] +=       Gamma1_["wv"] * qk["n1,u"] * B["guv"] * B["g,m,n1"];
        y["mw"] +=       Gamma1_["wv"] * qk["n1,u"] * B["gum"] * B["g,v,n1"];
        y["mw"] -= 2.0 * Gamma1_["wv"] * qk["u1,a1"] * B["g,a1,u1"] * B["gvm"];
        y["mw"] +=       Gamma1_["wv"] * qk["u1,a1"] * B["g,a1,m"] * B["g,v,u1"];
        y["mw"] -= Gamma2_["uvxy"] * qk["mu"] * B["gxw"] * B["gyv"];
        y["mw"] -= Gamma2_["uVxY"] * qk["mu"] * B["gxw"] * B["gYV"];
        y["mw"] += Gamma2_["u,w,x,y"] * qk["n1,u"] * B["g,x,n1"] * B["gym"];
        y["mw"] += Gamma2_["w,U,y,X"] * qk["N1,U"] * B["gym"] * B["g,X,N1"];
        y["mw"] -= Gamma2_["u,v,w,y"] * qk["e1,u"] * B["g,e1,m"] * B["gvy"];
        y["mw"] += Gamma2_["u,v,w,y"] * qk["e1,u"] * B["g,e1,y"] * B["gvm"];
        y["mw"] -= Gamma2_["u,V,w,Y"] * qk["e1,u"] * B["g,e1,m"] * B["gVY"];
        y["mw"] -= Gamma2_["v,U,w,Y"] * qk["E1,U"] * B["g,v,m"] * B["g,E1,Y"];
        y["mw"] += Gamma2_["u,v,w,y"] * qk["n1,u"] * B["g,m,n1"] * B["gyv"];
        y["mw"] -= Gamma2_["u,v,w,y"] * qk["n1,u"] * B["gmv"] * B["g,y,n1"];
        y["mw"] += Gamma2_["u,V,w,Y"] * qk["n1,u"] * B["g,m,n1"] * B["g,Y,V"];
        y["mw"] += Gamma2_["v,U,w,Y"] * qk["N1,U"] * B["gmv"] * B["g,Y,N1"];
        y["mw"] -= Gamma2_["u,w,x,y"] * qk["e1,u"] * B["g,e1,x"] * B["gmy"];
        y["mw"] -= Gamma2_["w,U,y,X"] * qk["E1,U"] * B["gmy"] * B["g,E1,X"];
    } else {
        y["mw"] += qk["e1,m1"] * V["e1,w,m1,m"];
        y["mw"] += qk["E1,M1"] * V["w,E1,m,M1"];
        y["mw"] -= 0.5 * Gamma2_["uvxy"] * qk["mu"] * V["xywv"];
        y["mw"] -= Gamma2_["uVxY"] * qk["mu"] * V["xYwV"];
        y["mw"] += qk["n1,u"] * V["u,w,n1,m"];
        y["mw"] += qk["N1,U"] * V["w,U,m,N1"];
        y["mw"] -= Gamma1_["uv"] * qk["n1,u"] * V["v,w,n1,m"];
        y["mw"] -= Gamma1_["UV"] * qk["N1,U"] * V["w,V,m,N1"];
        y["mw"] += Gamma1_["uv"] * qk["e1,u"] * V["e1,w,v,m"];
        y["mw"] += Gamma1_["UV"] * qk["E1,U"] * V["w,E1,m,V"];
        y["mw"] += qk["uv"] * V["vwum"];
        y["mw"] += qk["UV"] * V["wVmU"];
        y["mw"] -= Gamma1_["uw"] * qk["e1,m1"] * V["e1,u,m1,m"];
        y["mw"] -= Gamma1_["uw"] * qk["E1,M1"] * V["u,E1,m,M1"];
        y["mw"] -= Gamma1_["wv"] * qk["n1,u"] * V["u,v,n1,m"];
        y["mw"] -= Gamma1_["wv"] * qk["N1,U"] * V["v,U,m,N1"];
        y["mw"] += 0.5 * Gamma2_["u,w,x,y"] * qk["n1,u"] * V["x,y,n1,m"];
        y["mw"] +=       Gamma2_["w,U,y,X"] * qk["N1,U"] * V["y,X,m,N1"];
        y["mw"] -= Gamma2_["u,v,w,y"] * qk["e1,u"] * V["e1,v,m,y"];
        y["mw"] -= Gamma2_["u,V,w,Y"] * qk["e1,u"] * V["e1,V,m,Y"];
        y["mw"] -= Gamma2_["v,U,w,Y"] * qk["E1,U"] * V["v,E1,m,Y"];
        y["mw"] -= Gamma1_["wv"] * qk["u1,a1"] * V["a1,v,u1,m"];
        y["mw"] -= Gamma1_["wv"] * qk["U1,A1"] * V["v,A1,m,U1"];

        y["mw"] += V["e1,m,m1,w"] * qk["e1,m1"];
        y["mw"] += V["m,E1,w,M1"] * qk["E1,M1"];
        y["mw"] += V["u,m,n1,w"] * qk["n1,u"];
        y["mw"] += V["m,U,w,N1"] * qk["N1,U"];
        y["mw"] -= Gamma1_["uv"] * V["v,m,n1,w"] * qk["n1,u"];
        y["mw"] -= Gamma1_["UV"] * V["m,V,w,N1"] * qk["N1,U"];
        y["mw"] += Gamma1_["uv"] * V["e1,m,v,w"] * qk["e1,u"];
        y["mw"] += Gamma1_["UV"] * V["m,E1,w,V"] * qk["E1,U"];
        y["mw"] -= V["e1,m,m1,u"] * Gamma1_["uw"] * qk["e1,m1"];
        y["mw"] -= V["m,E1,u,M1"] * Gamma1_["uw"] * qk["E1,M1"];
        y["mw"] -= V["u,m,n1,v"] * Gamma1_["wv"] * qk["n1,u"];
        y["mw"] -= V["m,U,v,N1"] * Gamma1_["wv"] * qk["N1,U"];
        y["mw"] += V["m,y,n1,v"] * Gamma2_["u,v,w,y"] * qk["n1,u"];
        y["mw"] += V["m,Y,n1,V"] * Gamma2_["u,V,w,Y"] * qk["n1,u"];
        y["mw"] += V["m,Y,v,N1"] * Gamma2_["v,U,w,Y"] * qk["N1,U"];
        y["mw"] -= 0.5 * V["e1,m,x,y"] * Gamma2_["u,w,x,y"] * qk["e1,u"];
        y["mw"] -=       V["m,E1,y,X"] * Gamma2_["w,U,y,X"] * qk["E1,U"];
    }

    // VIRTUAL-ACTIVE
    y["ew"] += F["m1,w"] * qk["e,m1"];
    y["ew"] -= F["ve"] * qk["wv"];
    y["ew"] += H["vw"] * Gamma1_["uv"] * qk["eu"];
    y["ew"] += V_pmqm["v,w"] * Gamma1_["uv"] * qk["eu"];
    y["ew"] += H["e,n1"] * Gamma1_["uw"] * qk["n1,u"];
    y["ew"] += V_pmqm["e,n1"] * Gamma1_["uw"] * qk["n1,u"];
    y["ew"] -= H["e,e1"] * Gamma1_["uw"] * qk["e1,u"];
    y["ew"] -= V_pmqm["e,e1"] * Gamma1_["uw"] * qk["e1,u"];
    if (eri_df_) {
        y["ew"] -= 4.0 * Gamma1_["uw"] * qk["e1,m1"] * B["g,e1,m1"] * B["geu"];
        y["ew"] +=       Gamma1_["uw"] * qk["e1,m1"] * B["g,e1,u"] * B["g,e,m1"];
        y["ew"] +=       Gamma1_["uw"] * qk["e1,m1"] * B["g,e1,e"] * B["g,u,m1"];
        y["ew"] -= 4.0 * Gamma1_["wv"] * qk["n1,u"] * B["g,u,n1"] * B["gev"];
        y["ew"] +=       Gamma1_["wv"] * qk["n1,u"] * B["guv"] * B["g,e,n1"];
        y["ew"] +=       Gamma1_["wv"] * qk["n1,u"] * B["gue"] * B["g,v,n1"];
        y["ew"] -= 2.0 * Gamma1_["wv"] * qk["u1,a1"] * B["g,u1,a1"] * B["gev"];
        y["ew"] +=       Gamma1_["wv"] * qk["u1,a1"] * B["g,u1,v"] * B["g,e,a1"];
        y["ew"] += Gamma2_["uvxy"] * qk["eu"] * B["gxw"] * B["gyv"];
        y["ew"] += Gamma2_["uVxY"] * qk["eu"] * B["gxw"] * B["gYV"];
        y["ew"] += Gamma2_["u,v,w,y"] * qk["n1,u"] * B["g,e,n1"] * B["gyv"];
        y["ew"] -= Gamma2_["u,v,w,y"] * qk["n1,u"] * B["gev"] * B["g,y,n1"];
        y["ew"] += Gamma2_["u,V,w,Y"] * qk["n1,u"] * B["g,e,n1"] * B["gYV"];
        y["ew"] += Gamma2_["v,U,w,Y"] * qk["N1,U"] * B["g,e,v"] * B["g,Y,N1"];
        y["ew"] += Gamma2_["u,w,x,y"] * qk["n1,u"] * B["g,x,n1"] * B["gye"];
        y["ew"] += Gamma2_["w,U,y,X"] * qk["N1,U"] * B["gye"] * B["g,X,N1"];
        y["ew"] -= Gamma2_["u,w,x,y"] * qk["e1,u"] * B["g,e1,x"] * B["gey"];
        y["ew"] -= Gamma2_["w,U,y,X"] * qk["E1,U"] * B["gey"] * B["g,E1,X"];
        y["ew"] -= Gamma2_["u,v,w,y"] * qk["e1,u"] * B["g,e,e1"] * B["gyv"];
        y["ew"] += Gamma2_["u,v,w,y"] * qk["e1,u"] * B["gev"] * B["g,y,e1"];
        y["ew"] -= Gamma2_["u,V,w,Y"] * qk["e1,u"] * B["g,e,e1"] * B["gYV"];
        y["ew"] -= Gamma2_["v,U,w,Y"] * qk["E1,U"] * B["gev"] * B["g,Y,E1"];
    } else {
        y["ew"] += 0.5 * Gamma2_["uvxy"] * qk["eu"] * V["xywv"];
        y["ew"] += Gamma2_["uVxY"] * qk["eu"] * V["xYwV"];
        y["ew"] -= Gamma1_["uw"] * qk["e1,m1"] * V["e1,e,m1,u"];
        y["ew"] -= Gamma1_["uw"] * qk["E1,M1"] * V["e,E1,u,M1"]; 
        y["ew"] -= Gamma1_["wv"] * qk["n1,u"] * V["u,e,n1,v"];
        y["ew"] -= Gamma1_["wv"] * qk["N1,U"] * V["e,U,v,N1"];  
        y["ew"] += Gamma2_["u,v,w,y"] * qk["n1,u"] * V["e,y,n1,v"];
        y["ew"] += Gamma2_["u,V,w,Y"] * qk["n1,u"] * V["e,Y,n1,V"];
        y["ew"] += Gamma2_["v,U,w,Y"] * qk["N1,U"] * V["e,Y,v,N1"];
        y["ew"] -= Gamma1_["wv"] * qk["u1,a1"] * V["u1,e,a1,v"];
        y["ew"] -= Gamma1_["wv"] * qk["U1,A1"] * V["e,U1,v,A1"];
        y["ew"] -= 0.5 * Gamma2_["u,w,x,y"] * qk["e1,u"] * V["e1,e,x,y"];
        y["ew"] -= Gamma2_["w,U,y,X"] * qk["E1,U"] * V["e,E1,y,X"];

        y["ew"] -= V["e1,u,m1,e"] * Gamma1_["uw"] * qk["e1,m1"];
        y["ew"] -= V["u,E1,e,M1"] * Gamma1_["uw"] * qk["E1,M1"];
        y["ew"] -= V["u,v,n1,e"] * Gamma1_["wv"] * qk["n1,u"];
        y["ew"] -= V["v,U,e,N1"] * Gamma1_["wv"] * qk["N1,U"];
        y["ew"] += 0.5 * V["x,y,n1,e"] * Gamma2_["u,w,x,y"] * qk["n1,u"];
        y["ew"] +=       V["y,X,e,N1"] * Gamma2_["w,U,y,X"] * qk["N1,U"];
        y["ew"] -= V["e,y,e1,v"] * Gamma2_["u,v,w,y"] * qk["e1,u"];
        y["ew"] -= V["e,Y,e1,V"] * Gamma2_["u,V,w,Y"] * qk["e1,u"];
        y["ew"] -= V["e,Y,v,E1"] * Gamma2_["v,U,w,Y"] * qk["E1,U"];
    }

    // ACTIVE-ACTIVE
    BlockedTensor temp_y = BTF_->build(CoreTensor, "temporal matrix for y{aa} symmetrization", spin_cases({"aa"}));
    temp_y["wz"] -= F["w,n1"] * qk["n1,z"];
    temp_y["wz"] += H["w,n1"] * Gamma1_["uz"] * qk["n1,u"];
    temp_y["wz"] += V_pmqm["w,n1"] * Gamma1_["uz"] * qk["n1,u"];
    temp_y["wz"] -= H["w,e1"] * Gamma1_["uz"] * qk["e1,u"];
    temp_y["wz"] -= V_pmqm["e1,w"] * Gamma1_["uz"] * qk["e1,u"];
    if (eri_df_) {
        temp_y["wz"] -= 2.0 * Gamma1_["zv"] * qk["u1,a1"] * B["g,a1,u1"] * B["gvw"];
        temp_y["wz"] +=       Gamma1_["zv"] * qk["u1,a1"] * B["g,a1,w"] * B["g,v,u1"];
        temp_y["wz"] -= 4.0 * Gamma1_["uz"] * qk["e1,m1"] * B["g,e1,m1"] * B["guw"];
        temp_y["wz"] +=       Gamma1_["uz"] * qk["e1,m1"] * B["g,e1,w"] * B["g,u,m1"];
        temp_y["wz"] +=       Gamma1_["uz"] * qk["e1,m1"] * B["g,e1,u"] * B["g,w,m1"];
        temp_y["wz"] -= 4.0 * Gamma1_["zv"] * qk["n1,u"] * B["g,u,n1"] * B["gvw"];
        temp_y["wz"] +=       Gamma1_["zv"] * qk["n1,u"] * B["guw"] * B["g,v,n1"];
        temp_y["wz"] +=       Gamma1_["zv"] * qk["n1,u"] * B["guv"] * B["g,w,n1"];
        temp_y["wz"] += Gamma2_["u,z,x,y"] * qk["n1,u"] * B["g,x,n1"] * B["gyw"];
        temp_y["wz"] += Gamma2_["z,U,y,X"] * qk["N1,U"] * B["gyw"] * B["g,X,N1"];
        temp_y["wz"] += Gamma2_["u,v,z,y"] * qk["n1,u"] * B["g,w,n1"] * B["gyv"];
        temp_y["wz"] -= Gamma2_["u,v,z,y"] * qk["n1,u"] * B["gwv"] * B["g,y,n1"];
        temp_y["wz"] += Gamma2_["u,V,z,Y"] * qk["n1,u"] * B["g,w,n1"] * B["gYV"];
        temp_y["wz"] += Gamma2_["v,U,z,Y"] * qk["N1,U"] * B["gwv"] * B["g,Y,N1"];
        temp_y["wz"] -= Gamma2_["u,z,x,y"] * qk["e1,u"] * B["g,e1,x"] * B["gwy"];
        temp_y["wz"] -= Gamma2_["z,U,y,X"] * qk["E1,U"] * B["gwy"] * B["g,E1,X"];
        temp_y["wz"] -= Gamma2_["u,v,z,y"] * qk["e1,u"] * B["g,e1,w"] * B["gvy"];
        temp_y["wz"] += Gamma2_["u,v,z,y"] * qk["e1,u"] * B["g,e1,y"] * B["gvw"];
        temp_y["wz"] -= Gamma2_["u,V,z,Y"] * qk["e1,u"] * B["g,e1,w"] * B["gVY"];
        temp_y["wz"] -= Gamma2_["v,U,z,Y"] * qk["E1,U"] * B["gvw"] * B["g,E1,Y"];
    } else {
        temp_y["wz"] -= Gamma1_["uz"] * qk["e1,m1"] * V["e1,u,m1,w"];
        temp_y["wz"] -= Gamma1_["uz"] * qk["E1,M1"] * V["u,E1,w,M1"];
        temp_y["wz"] -= Gamma1_["uz"] * qk["e1,m1"] * V["e1,w,m1,u"];
        temp_y["wz"] -= Gamma1_["uz"] * qk["E1,M1"] * V["w,E1,u,M1"];
        temp_y["wz"] -= Gamma1_["zv"] * qk["n1,u"] * V["u,v,n1,w"];
        temp_y["wz"] -= Gamma1_["zv"] * qk["N1,U"] * V["v,U,w,N1"];
        temp_y["wz"] -= Gamma1_["zv"] * qk["n1,u"] * V["u,w,n1,v"];
        temp_y["wz"] -= Gamma1_["zv"] * qk["N1,U"] * V["w,U,v,N1"];
        temp_y["wz"] += 0.5 * Gamma2_["u,z,x,y"] * qk["n1,u"] * V["x,y,n1,w"];
        temp_y["wz"] += Gamma2_["z,U,y,X"] * qk["N1,U"] * V["y,X,w,N1"];
        temp_y["wz"] += Gamma2_["u,v,z,y"] * qk["n1,u"] * V["w,y,n1,v"];
        temp_y["wz"] += Gamma2_["u,V,z,Y"] * qk["n1,u"] * V["w,Y,n1,V"];
        temp_y["wz"] += Gamma2_["v,U,z,Y"] * qk["N1,U"] * V["w,Y,v,N1"];
        temp_y["wz"] -= 0.5 * Gamma2_["u,z,x,y"] * qk["e1,u"] * V["e1,w,x,y"];
        temp_y["wz"] -= Gamma2_["z,U,y,X"] * qk["E1,U"] * V["w,E1,y,X"];
        temp_y["wz"] -= Gamma2_["u,v,z,y"] * qk["e1,u"] * V["e1,v,w,y"];
        temp_y["wz"] -= Gamma2_["u,V,z,Y"] * qk["e1,u"] * V["e1,V,w,Y"];
        temp_y["wz"] -= Gamma2_["v,U,z,Y"] * qk["E1,U"] * V["v,E1,w,Y"];
        temp_y["wz"] -= Gamma1_["zv"] * qk["u1,a1"] * V["a1,v,u1,w"];
        temp_y["wz"] -= Gamma1_["zv"] * qk["U1,A1"] * V["v,A1,w,U1"];
    }
    y["wz"] += temp_y["wz"];
    y["zw"] -= temp_y["wz"];  
    y["wz"] += Delta1["zw"] * qk["wz"];   

    /// MO RESPONSE -- CI EQUATION
    // Form contraction between qk_ci and ci, cc1, cc2
    auto cc1_qkci = BTF_->build(CoreTensor, "cc1 * qk_ci", spin_cases({"aa"}));
    auto cc2_qkci = BTF_->build(CoreTensor, "cc2 * qk_ci", spin_cases({"aaaa"}));
    for (const auto& pair : as_solver_->state_energies_map()) {
        const auto& state = pair.first;
        auto g1r = BTF_->build(tensor_type_, "1GRDM_ket", spin_cases({"aa"}));
        auto g2r = BTF_->build(tensor_type_, "2GRDM_ket", spin_cases({"aaaa"}));
        auto vec_ptr = qk_ci.data();
        as_solver_->generalized_rdms(state, 0, vec_ptr, cc1_qkci, false, 1);
        as_solver_->generalized_rdms(state, 0, vec_ptr, g1r, true, 1);
        as_solver_->generalized_rdms(state, 0, vec_ptr, cc2_qkci, false, 2);
        as_solver_->generalized_rdms(state, 0, vec_ptr, g2r, true, 2);

        cc1_qkci["uv"] += g1r["uv"];
        cc1_qkci["UV"] += g1r["UV"];
        cc2_qkci["uvxy"] += g2r["uvxy"];
        cc2_qkci["UVXY"] += g2r["UVXY"];
        cc2_qkci["uVxY"] += g2r["uVxY"];
    }

    double ci_qk_dot;
    ci_qk_dot = ci("I") * qk_ci("I");

    temp_y = BTF_->build(CoreTensor, "temporal matrix for y{aa} symmetrization", spin_cases({"aa"}));
    temp_y["wz"] -= 0.50 * H["vw"] * cc1_qkci["zv"];
    temp_y["wz"] -= 0.50 * V_pmqm["uw"] * cc1_qkci["uz"];

    y["em"] -= ci_qk_dot * H["em"];
    y["em"] -= ci_qk_dot * V_pmqm["me"];
    y["mw"] -= 0.50 * H["vm"] * cc1_qkci["wv"];
    y["mw"] -= 0.50 * V_pmqm["um"] * cc1_qkci["uw"];
    y["mw"] += ci_qk_dot * H["wm"];
    y["mw"] += ci_qk_dot * V_pmqm["mw"];
    y["ew"] -= 0.50 * H["ve"] * cc1_qkci["wv"];
    y["ew"] -= 0.50 * V_pmqm["ue"] * cc1_qkci["uw"];
    if (eri_df_) {
        y["em"] -= cc1_qkci["uv"] * B["gvu"] * B["gem"];
        y["em"] += 0.5 * cc1_qkci["uv"] * B["gvm"] * B["geu"];
        y["mw"] -= 0.5 * cc2_qkci["wvxy"] * B["gxm"] * B["gyv"];
        y["mw"] -= 0.5 * cc2_qkci["wVxY"] * B["gxm"] * B["gYV"];
        y["mw"] += cc1_qkci["uv"] * B["gvu"] * B["gwm"];
        y["mw"] -= 0.5 * cc1_qkci["uv"] * B["gvm"] * B["gwu"];
        y["ew"] -= 0.5 * cc2_qkci["wvxy"] * B["gex"] * B["gvy"];
        y["ew"] -= 0.5 * cc2_qkci["wVxY"] * B["gex"] * B["gVY"];
        temp_y["wz"] -= 0.5 * cc2_qkci["zvxy"] * B["gwx"] * B["gvy"];
        temp_y["wz"] -= 0.5 * cc2_qkci["zVxY"] * B["gwx"] * B["gVY"];
    } else {
        y["em"] -= 0.50 * cc1_qkci["uv"] * V["veum"];
        y["em"] -= 0.50 * cc1_qkci["UV"] * V["eVmU"];
        y["mw"] -= 0.25 * cc2_qkci["wvxy"] * V["xymv"];
        y["mw"] -= 0.50 * cc2_qkci["wVxY"] * V["xYmV"];
        y["mw"] += 0.50 * cc1_qkci["uv"] * V["vwum"];
        y["mw"] += 0.50 * cc1_qkci["UV"] * V["wVmU"];
        y["ew"] -= 0.25 * cc2_qkci["wvxy"] * V["evxy"];
        y["ew"] -= 0.50 * cc2_qkci["wVxY"] * V["eVxY"];
        temp_y["wz"] -= 0.25 * cc2_qkci["zvxy"] * V["wvxy"];
        temp_y["wz"] -= 0.50 * cc2_qkci["zVxY"] * V["wVxY"];
    }
    y["wz"] += temp_y["wz"];
    y["zw"] -= temp_y["wz"];

    /// CI EQUATION -- MO RESPONSE
    y_ci("K") += 8 * ci("K") * H.block("ac")("vn") * Gamma1_.block("aa")("uv") * qk.block("ca")("nu");
    y_ci("K") += 8 * ci("K") * V_pmqm.block("ac")("vn") * Gamma1_.block("aa")("uv") * qk.block("ca")("nu");
    y_ci("K") -= 8 * ci("K") * H.block("av")("ve") * Gamma1_.block("aa")("uv") * qk.block("va")("eu");
    y_ci("K") -= 8 * ci("K") * V_pmqm.block("av")("ve") * Gamma1_.block("aa")("uv") * qk.block("va")("eu");
    if (eri_df_) {
        y_ci("K") -= 16 * ci("K") * Gamma1_.block("aa")("xy") * qk.block("vc")("em") * B.block("Lvc")("gem") * B.block("Laa")("gxy");
        y_ci("K") +=  8 * ci("K") * Gamma1_.block("aa")("xy") * qk.block("vc")("em") * B.block("Lva")("gey") * B.block("Lac")("gxm");
        y_ci("K") -= 16 * ci("K") * Gamma1_.block("aa")("xy") * qk.block("ca")("nu") * B.block("Lac")("gun") * B.block("Laa")("gyx");
        y_ci("K") +=  8 * ci("K") * Gamma1_.block("aa")("xy") * qk.block("ca")("nu") * B.block("Laa")("gux") * B.block("Lac")("gyn");
        y_ci("K") -=  8 * ci("K") * Gamma1_.block("aa")("xy") * qk.block("aa")("uv") * B.block("Laa")("guv") * B.block("Laa")("gyx");
        y_ci("K") +=  4 * ci("K") * Gamma1_.block("aa")("xy") * qk.block("aa")("uv") * B.block("Laa")("gux") * B.block("Laa")("gyv");
        y_ci("K") += 8 * ci("K") * Gamma2_.block("aaaa")("uvxy") * qk.block("ca")("nu") * B.block("Lac")("gxn") * B.block("Laa")("gyv");
        y_ci("K") += 4 * ci("K") * Gamma2_.block("aAaA")("uVxY") * qk.block("ca")("nu") * B.block("Lac")("gxn") * B.block("LAA")("gYV");
        y_ci("K") += 4 * ci("K") * Gamma2_.block("aAaA")("vUxY") * qk.block("CA")("NU") * B.block("Laa")("gxv") * B.block("LAC")("gYN");
        y_ci("K") -= 8 * ci("K") * Gamma2_.block("aaaa")("uvxy") * qk.block("va")("eu") * B.block("Lva")("gex") * B.block("Laa")("gvy");
        y_ci("K") -= 4 * ci("K") * Gamma2_.block("aAaA")("uVxY") * qk.block("va")("eu") * B.block("Lva")("gex") * B.block("LAA")("gVY");
        y_ci("K") -= 4 * ci("K") * Gamma2_.block("aAaA")("vUxY") * qk.block("VA")("EU") * B.block("Laa")("gvx") * B.block("LVA")("gEY");
    } else {
        y_ci("K") -= 8 * ci("K") * Gamma1_.block("aa")("xy") * qk.block("vc")("em") * V.block("vaca")("exmy");
        y_ci("K") -= 4 * ci("K") * Gamma1_.block("AA")("XY") * qk.block("vc")("em") * V.block("vAcA")("eXmY");
        y_ci("K") -= 4 * ci("K") * Gamma1_.block("aa")("xy") * qk.block("VC")("EM") * V.block("aVaC")("xEyM");

        y_ci("K") -= 8 * ci("K") * Gamma1_.block("aa")("xy") * qk.block("ca")("nu") * V.block("aaca")("uynx");
        y_ci("K") -= 4 * ci("K") * Gamma1_.block("AA")("XY") * qk.block("ca")("nu") * V.block("aAcA")("uYnX");
        y_ci("K") += 4 * ci("K") * Gamma2_.block("aaaa")("uvxy") * qk.block("ca")("nu") * V.block("aaca")("xynv");
        y_ci("K") += 4 * ci("K") * Gamma2_.block("aAaA")("uVxY") * qk.block("ca")("nu") * V.block("aAcA")("xYnV");

        y_ci("K") -= 4 * ci("K") * Gamma1_.block("aa")("xy") * qk.block("CA")("NU") * V.block("aAaC")("yUxN");
        y_ci("K") += 4 * ci("K") * Gamma2_.block("aAaA")("vUxY") * qk.block("CA")("NU") * V.block("aAaC")("xYvN");
        
        y_ci("K") -= 4 * ci("K") * Gamma2_.block("aaaa")("uvxy") * qk.block("va")("eu") * V.block("vaaa")("evxy");
        y_ci("K") -= 4 * ci("K") * Gamma2_.block("aAaA")("uVxY") * qk.block("va")("eu") * V.block("vAaA")("eVxY");
        y_ci("K") -= 4 * ci("K") * Gamma2_.block("aAaA")("vUxY") * qk.block("VA")("EU") * V.block("aVaA")("vExY");

        y_ci("K") -= 4 * ci("K") * Gamma1_.block("aa")("xy") * qk.block("aa")("uv") * V.block("aaaa")("uyvx");
        y_ci("K") -= 2 * ci("K") * Gamma1_.block("AA")("XY") * qk.block("aa")("uv") * V.block("aAaA")("uYvX");
        y_ci("K") -= 2 * ci("K") * Gamma1_.block("aa")("xy") * qk.block("AA")("UV") * V.block("aAaA")("yUxV");
    }

    /// CI EQUATION -- MO RESPONSE
    //  Call the generalized sigma function to complete the contraction
    for (const auto& pair: as_solver_->state_space_size_map()) {
        const auto& state = pair.first;
        std::map<std::string, double> block_factor1;
        std::map<std::string, double> block_factor2;
        block_factor1["aa"] = 1.0;
        block_factor1["AA"] = 1.0;
        block_factor2["aaaa"] = 1.0;
        block_factor2["aAaA"] = 1.0;
        block_factor2["AAAA"] = 1.0;

        auto sym_1 = BTF_->build(CoreTensor, "symmetrized 1-body tensor", spin_cases({"aa"}));
        auto sym_2 = BTF_->build(CoreTensor, "symmetrized 2-body tensor", spin_cases({"aaaa"}));
        {
            auto temp_1 = BTF_->build(CoreTensor, "1-body intermediate tensor", {"aa"});
            temp_1["uv"] -= 2 * H["vn"] * qk["nu"];
            temp_1["uv"] -= 2 * V_pmqm["vn"] * qk["nu"];
            temp_1["uv"] += 2 * H["ve"] * qk["eu"];
            temp_1["uv"] += 2 * V_pmqm["ve"] * qk["eu"];
            if (eri_df_) {
                temp_1["uv"] += 4 * qk["em"] * B["gem"] * B["guv"];
                temp_1["uv"] -= 2 * qk["em"] * B["gev"] * B["gum"];
                temp_1["xy"] += 4 * qk["nu"] * B["gun"] * B["gyx"];
                temp_1["xy"] -= 2 * qk["nu"] * B["gux"] * B["gyn"];
                temp_1["xy"] += 2 * qk["uv"] * B["guv"] * B["gyx"];
                temp_1["xy"] -=     qk["uv"] * B["gux"] * B["gyv"];
            } else {
                temp_1["xy"] += 2 * qk["NU"] * V["yUxN"];
                temp_1["uv"] += 2 * qk["em"] * V["eumv"];
                temp_1["uv"] += 2 * qk["EM"] * V["uEvM"];
                temp_1["xy"] += 2 * qk["nu"] * V["uynx"];
                temp_1["xy"] +=     qk["uv"] * V["uyvx"];
                temp_1["xy"] +=     qk["UV"] * V["yUxV"];
            }

            /// Symmetrization
            //  α α
            sym_1["uv"] += temp_1["uv"];
            sym_1["uv"] += temp_1["vu"];
            //  β β
            sym_1.block("AA")("pq") = sym_1.block("aa")("pq");
        }
        {
            auto temp_2 = BTF_->build(CoreTensor, "2-body intermediate tensor", {"aaaa","aAaA"});

            if (eri_df_) {
                temp_2["uvxy"] -= 2 * qk["nu"] * B["gxn"] * B["gyv"];
                temp_2["uVxY"] -= 2 * qk["nu"] * B["gxn"] * B["gYV"];
                temp_2["vUxY"] -= 2 * qk["NU"] * B["gxv"] * B["gYN"];
                temp_2["uvxy"] += 2 * qk["eu"] * B["gex"] * B["gvy"];
                temp_2["uVxY"] += 2 * qk["eu"] * B["gex"] * B["gVY"];
                temp_2["vUxY"] += 2 * qk["EU"] * B["gvx"] * B["gEY"];
            } else { 
                temp_2["uvxy"] -=     qk["nu"] * V["xynv"];
                temp_2["uVxY"] -= 2 * qk["nu"] * V["xYnV"];
                temp_2["vUxY"] -= 2 * qk["NU"] * V["xYvN"];

                temp_2["uvxy"] +=     qk["eu"] * V["evxy"];
                temp_2["uVxY"] += 2 * qk["eu"] * V["eVxY"];
                temp_2["vUxY"] += 2 * qk["EU"] * V["vExY"];
            }
            /// Symmetrization
            //  α α α α
            //  Antisymmetrization
            sym_2["xyuv"] += temp_2["uvxy"];
            sym_2["xyuv"] -= temp_2["uvyx"];
            sym_2["xyuv"] -= temp_2["vuxy"];
            sym_2["xyuv"] += temp_2["vuyx"];
            //  Antisymmetrization
            sym_2["uvxy"] += temp_2["uvxy"];
            sym_2["uvxy"] -= temp_2["uvyx"];
            sym_2["uvxy"] -= temp_2["vuxy"];
            sym_2["uvxy"] += temp_2["vuyx"];
            //  β β β β
            sym_2.block("AAAA")("pqrs") = sym_2.block("aaaa")("pqrs");
            /// Symmetrization
            //  α β α β
            sym_2["uVxY"] += temp_2["uVxY"];
            sym_2["uVxY"] += temp_2["xYuV"];
        }
        as_solver_->add_sigma_kbody(state, 0, sym_1, block_factor1, y_ci.data());
        as_solver_->add_sigma_kbody(state, 0, sym_2, block_factor2, y_ci.data());
    }

    /// CI EQUATION -- CI EQUATION
    y_ci("K") += 2.0 * H.block("cc")("mn") * I.block("cc")("mn") * qk_ci("K");
    y_ci("K") += V_pmqm["m,m1"] * I["m,m1"] * qk_ci("K");
    y_ci("K") -= (Eref_ - Enuc_ - Efrzc_) * qk_ci("K");
    // NOTICE : Efrzc_ is subtracted since it has been counted once in the generalized_sigma.
    y_ci("K") -= Efrzc_ * qk_ci("K");

    //  Call the generalized sigma function to complete the contraction
    //  sum_{J} <I| H |J> x_J where H is the active space Hamiltonian, which includes
    //      cc1a("IJuv") * fa("uv") * x("J")
    //      cc1b("IJuv") * fb("uv") * x("J")
    //      0.25 * cc2aa("IJuvxy") * vaa("uvxy") * x("J")
    //      cc2ab("IJuvxy") * vab("uvxy") * x("J")
    //      0.25 * cc2bb("IJuvxy") * vbb("uvxy") * x("J")
    //  where fa and fb are core Fock matrices
    for (const auto& pair: as_solver_->state_space_size_map()) {
        const auto& state = pair.first;
        psi::SharedVector svq(new psi::Vector(ndets));
        psi::SharedVector svy(new psi::Vector(ndets));

        for (int i = 0; i < ndets; ++i) {
            svq->set(i, qk_ci.data()[i]);
            svy->set(i, y_ci.data()[i]);
        }
        as_solver_->generalized_sigma(state, svq, svy);
        for (int i = 0; i < ndets; ++i) {
            y_ci.data()[i] += svy->get(i);
        }
    }

    /// Fill the y (y = A * qk) and pass it to the GMRES solver
    for (const std::string& row : {"vc", "ca", "va", "aa"}) {
        int idx1 = block_dim[row];
        int pre1 = preidx[row];
        if (row != "aa") {
            y.block(row).iterate([&](const std::vector<size_t>& i, double& value) {
                int index = pre1 + i[0] * idx1 + i[1];
                y_vec.at(index) = value;
            });
        } else {
            y.block(row).iterate([&](const std::vector<size_t>& i, double& value) {
                if (i[0] > i[1]) {
                    int index = pre1 + i[0] * (i[0] - 1) / 2 + i[1];
                    y_vec.at(index)  = value;
                }
            });
        }
    }

    for (const std::string& row : {"ci"}) {
        int pre1 = preidx[row];
        (y_ci).iterate([&](const std::vector<size_t>& i, double& value) {   
            y_vec.at(pre1 + i[0]) = value;
        });
    }
}

void DSRG_MRPT2::set_preconditioner(std::vector<double> & D) {
    BlockedTensor D_mo = BTF_->build(CoreTensor, "Preconditioner (orbital rotation) in GMRES", {"vc", "ca", "va", "aa"});
    BlockedTensor temp_d = BTF_->build(CoreTensor, "temporal tensor", {"vc", "ca", "va", "aa"});

    // VIRTUAL-CORE
    D_mo["em"] += Delta1["m,e"];

    // CORE-ACTIVE
    D_mo["mw"] += F["uw"] * one_vec["m"] * I["uw"];
    D_mo["mw"] -= H["vw"] * Gamma1_["wv"] * one_vec["m"];
    D_mo["mw"] -= V_pmqm["vw"] * Gamma1_["wv"] * one_vec["m"];
    D_mo["mw"] -= F["m,n1"] * one_vec["w"] * I["m,n1"];
    D_mo["mw"] += H["m,n1"] * Gamma1_["uw"] * I["wu"] * I["m,n1"];
    D_mo["mw"] += V_pmqm["m,n1"] * Gamma1_["uw"] * I["m,n1"] * I["wu"];

    // VIRTUAL-ACTIVE
    D_mo["ew"] += H["vw"] * Gamma1_["wv"] * one_vec["e"];
    D_mo["ew"] += V_pmqm["v,w"] * Gamma1_["wv"] * one_vec["e"];
    D_mo["ew"] -= H["e,e1"] * Gamma1_["uw"] * I["e,e1"] * I["uw"];
    D_mo["ew"] -= V_pmqm["e,e1"] * Gamma1_["uw"] * I["e,e1"] * I["uw"];

    // ACTIVE-ACTIVE
    D_mo["wz"] += Delta1["zw"] * one_vec["w"];
    if (eri_df_) {
        // VIRTUAL-CORE
        D_mo["em"] -= B["gme"] * B["gem"];
        D_mo["em"] += B["g,m1,m"] * B["g,e,e1"] * I["e1,e"] * I["m1,m"];

        // CORE-ACTIVE
        D_mo["mw"] += B["gwm"] * B["gmw"];
        D_mo["mw"] -= B["guw"] * B["g,m,n1"] * I["m,n1"] * I["wu"];
        D_mo["mw"] -= 2 * B["gwm"] * B["gmv"] * Gamma1_["wv"];
        D_mo["mw"] += B["gwv"] * B["g,m,n1"] * Gamma1_["wv"] * I["m,n1"];
        D_mo["mw"] -= B["gmv"] * B["gym"] * Gamma2_["u,v,w,y"] * I["wu"];
        D_mo["mw"] += B["g,m,n1"] * B["gyv"] * Gamma2_["u,v,w,y"] * I["m,n1"] * I["wu"];
        D_mo["mw"] += B["g,m,n1"] * B["gYV"] * Gamma2_["u,V,w,Y"] * I["m,n1"] * I["wu"];
        D_mo["mw"] -= B["gxw"] * B["gyv"] * Gamma2_["wvxy"] * one_vec["m"];
        D_mo["mw"] -= B["gxw"] * B["gYV"] * Gamma2_["wVxY"] * one_vec["m"]; 

        // VIRTUAL-ACTIVE
        D_mo["ew"] += B["gxw"] * B["gyv"] * Gamma2_["wvxy"] * one_vec["e"];
        D_mo["ew"] += B["gxw"] * B["gYV"] * Gamma2_["wVxY"] * one_vec["e"];
        D_mo["ew"] -= B["gex"] * B["gey"] * Gamma2_["u,w,x,y"] * I["uw"];
        D_mo["ew"] += B["gev"] * B["gye"] * Gamma2_["u,v,w,y"] * I["uw"];
        D_mo["ew"] -= B["g,e,e1"] * B["gyv"] * Gamma2_["u,v,w,y"] * I["e,e1"] * I["uw"];
        D_mo["ew"] -= B["g,e,e1"] * B["gYV"] * Gamma2_["u,V,w,Y"] * I["e,e1"] * I["uw"];

        // ACTIVE-ACTIVE
        D_mo["wz"] += B["gzw"] * B["gvz"] * Gamma1_["wv"];
        D_mo["wz"] -= B["g,a1,z"] * B["gvw"] * Gamma1_["wv"] * I["z,a1"];
    } else {
        // VIRTUAL-CORE
        D_mo["em"] -= V["m1,e,e1,m"] * I["e1,e"] * I["m1,m"];
        D_mo["em"] -= V["e1,e,m1,m"] * I["e1,e"] * I["m1,m"];

        // CORE-ACTIVE
        D_mo["mw"] += V["u,m,n1,w"] * I["m,n1"] * I["wu"];
        D_mo["mw"] -= Gamma1_["wv"] * V["v,m,n1,w"] * I["m,n1"];
        D_mo["mw"] -= V["w,m,n1,v"] * Gamma1_["wv"] * I["m,n1"];
        D_mo["mw"] += V["m,y,n1,v"] * Gamma2_["u,v,w,y"] * I["m,n1"] * I["wu"];
        D_mo["mw"] += V["m,Y,n1,V"] * Gamma2_["u,V,w,Y"] * I["m,n1"] * I["wu"];

        D_mo["mw"] -= V["w,v,n1,m"] * Gamma1_["wv"] * I["m,n1"];  
        D_mo["mw"] += 0.5 * V["x,y,n1,m"] * Gamma2_["u,w,x,y"] * I["m,n1"] * I["wu"];
        D_mo["mw"] -= 0.5 * V["xywv"] * Gamma2_["wvxy"] * one_vec["m"];
        D_mo["mw"] -= V["xYwV"] * Gamma2_["wVxY"] * one_vec["m"];
        D_mo["mw"] += V["u,w,n1,m"] * I["m,n1"] * I["wu"];  
        D_mo["mw"] -= Gamma1_["wv"] * V["v,w,n1,m"] * I["m,n1"];

        // VIRTUAL-ACTIVE
        D_mo["ew"] += 0.5 * V["xywv"] * Gamma2_["wvxy"] * one_vec["e"];
        D_mo["ew"] += V["xYwV"] * Gamma2_["wVxY"] * one_vec["e"];
        D_mo["ew"] -= 0.5 * V["e1,e,x,y"] * Gamma2_["u,w,x,y"] * I["e,e1"] * I["uw"];

        D_mo["ew"] -= V["e,y,e1,v"] * Gamma2_["u,v,w,y"] * I["e,e1"] * I["uw"];
        D_mo["ew"] -= V["e,Y,e1,V"] * Gamma2_["u,V,w,Y"] * I["e,e1"] * I["uw"];

        // ACTIVE-ACTIVE
        D_mo["wz"] -= V["z,v,u1,w"] * Gamma1_["zv"] * I["u1,w"];
        D_mo["wz"] += V["a1,v,w,z"] * Gamma1_["wv"] * I["z,a1"];
    }

    for (const std::string& row : {"vc", "ca", "va", "aa"}) {
        int idx1 = block_dim[row];
        int pre1 = preidx[row];
        if (row != "aa") {
            D_mo.block(row).iterate([&](const std::vector<size_t>& i, double& value) {
                if (std::fabs(value) > err) {
                    int index = pre1 + i[0] * idx1 + i[1];
                    D.at(index) = 1.0 / value;
                }
            });
        } else {
            D_mo.block(row).iterate([&](const std::vector<size_t>& i, double& value) {
                if (std::fabs(value) > err) { 
                    if (i[0] > i[1]) {
                        int index = pre1 + i[0] * (i[0] - 1) / 2 + i[1];
                        D.at(index)  = 1.0 / value;
                    }
                }
            });
        }
    }

    // Attention :  d_ci are the approximate preconditioner components for the CI part
    double d_ci = 0.0;
    d_ci += 2.0 * H["mn"] * I["mn"];
    d_ci += V_pmqm["m,m1"] * I["m,m1"];
    d_ci -= Eref_ - Enuc_ - Efrzc_;

    if (std::fabs(d_ci) > err) {
        double value = 1.0 / d_ci;
        int idx = preidx["ci"];
        for (int i = 0; i < ndets; ++i) {    
            D.at(idx + i) = value;
        }
    }
}

void DSRG_MRPT2::gmres_solver(std::vector<double> & x_new) {
    outfile->Printf("\n    Solving the linear system ....................... ");
    int iters;
    std::vector<double> x_old(dim);
    x_old = x_new;
    std::vector<double> r(dim);
    std::vector<double> q(max_iter * dim, 0.0);
    std::vector<double> h((max_iter + 1) * max_iter, 0.0);
    std::vector<double> bh(max_iter + 1, 0.0);
    // D is a Jacobi preconditioner
    std::vector<double> D(dim, 1.0);

    set_preconditioner(D);

    for (int i = 0; i < b.size(); ++i) {
        b[i] *= D[i];
    }

    z_vector_contraction(x_old, r);

    for (int i = 0; i < r.size(); ++i) {
        r[i] = b[i] - D[i] * r[i];
    }

    bh[0] = f_norm(r);

    for (int j = 0; j < dim; ++j) {
        // index here : i * dim + j, where i = 0
        q[j] = r[j] / bh[0];
    }

    std::vector<double> y_vec(dim, 0.0);

    for (int iter = 0; iter < max_iter; ++iter) {
        if (diff_f_norm(x_old, x_new) < err && iter > 2) {
            break;
        }
        iters = iter + 1;
        x_old = x_new;

        std::vector<double> qk_vec(dim);
        for (int i = 0; i < dim; ++i) {
            qk_vec.at(i) = q[iter * dim + i];
        }

        z_vector_contraction(qk_vec, y_vec);

        for (int i = 0; i < y_vec.size(); ++i) {
            y_vec[i] *= D[i];
        }

        for (int i = 0; i < iter+1; ++i) {
            h[i + iter * (max_iter + 1)] = C_DDOT(dim, &q[i * dim], 1, &y_vec[0], 1);
            for (int j = 0; j < dim; ++j) {
                y_vec[j] -= h[i + iter * (max_iter + 1)] * q[i * dim + j];
            }
        }

        h[(iter + 1) + iter * (max_iter + 1)] = f_norm(y_vec);
        bool condition = (std::fabs(h[(iter + 1) + iter * (max_iter + 1)]) < 1e-10) || (iter == max_iter - 1);

        std::vector<double> ck(max_iter + 1, 0.0);
        ck = bh;

        int lwork = 2 * max_iter;
        std::vector<double> work(lwork);

        std::vector<double> h_sub((iter + 2) * (iter + 1), 0.0);
        for (int i = 0; i < iter + 2; ++i) {
            for (int j = 0; j < iter + 1; ++j) {
                h_sub[i + j * (iter + 2)] = h[i + j * (max_iter + 1)];
            }
        }

        C_DGELS('n', iter+2, iter+1, 1, &h_sub[0], iter+2, &ck[0], iter+2, &work[0], lwork);

        if (!condition) {
            for (int j = 0; j < dim; ++j) {
                q[(iter + 1) * dim + j] = y_vec[j] / h[(iter + 1) + iter * (max_iter + 1)];
            }
            C_DGEMV('t', iter+2, dim, 1.0, &(q[0]), dim, &(ck[0]), 1, 0, &(x_new[0]), 1);
        }
        else if (iter == max_iter - 1){
            throw PSIEXCEPTION("GMRES solution is not converged, please change max iteration or error threshold in GMRES.");
        } else {
            C_DGEMV('t', max_iter, dim, 1.0, &(q[0]), dim, &(ck[0]), 1, 0, &(x_new[0]), 1);
            break;
        }
    }
    outfile->Printf("Done");
    outfile->Printf("\n        Z vector equation was solved in %d iterations", iters);
}

void DSRG_MRPT2::solve_linear_iter() {
    set_zvec_moinfo();
    set_b(dim, preidx, block_dim);
    std::vector<double> solution(dim, 0.0);
    gmres_solver(solution);

    // Conduct projection to get the correct solution
    double ci_xci_dot = C_DDOT(ndets, &solution[preidx["ci"]], 1, &ci.data()[0], 1);
    {
        int idx = preidx["ci"];
        auto ci_vec = ci.data();
        for (int i = idx; i < dim; ++i) {
            solution[i] -= ci_xci_dot * ci_vec[i - idx];
        }
    }

    // Write the solution of z-vector equations (stored in solution) into the Z matrix
    for (const std::string& block : {"vc", "ca", "va", "aa"}) {
        int pre = preidx[block], idx = block_dim[block];
        if (block != "aa") {
            (Z.block(block)).iterate([&](const std::vector<size_t>& i, double& value) {
                int index = pre + i[0] * idx + i[1];
                value = solution.at(index);
            });
        } else {
            (Z.block(block)).iterate([&](const std::vector<size_t>& i, double& value) {
                int i0 = i[0] > i[1] ? i[0] : i[1], i1 = i[0] > i[1] ? i[1] : i[0];
                if (i0 != i1) {
                    int index = pre + i0 * (i0 - 1) / 2 + i1;
                    value = solution.at(index);
                }
            });
        }
    }
    for (const std::string& block : {"ci"}) {
        int pre = preidx[block];
        (x_ci).iterate([&](const std::vector<size_t>& i, double& value) {
            int index = pre + i[0];
            value = solution.at(index);
        });
    }
    Z["me"] = Z["em"];
    Z["wm"] = Z["mw"];
    Z["we"] = Z["ew"];

    // Beta part
    // Caution: This is only valid when restricted orbitals are assumed 
    //          i.e. MO coefficients (alpha) equal MO coefficients (beta)
    for (const std::string& block : {"VC"}) {
        (Z.block(block)).iterate([&](const std::vector<size_t>& i, double& value) {
            value = Z.block("vc").data()[i[0] * ncore + i[1]];
        });
    }
    for (const std::string& block : {"CA"}) {
        (Z.block(block)).iterate([&](const std::vector<size_t>& i, double& value) {
            value = Z.block("ca").data()[i[0] * na + i[1]];
        });
    }
    for (const std::string& block : {"VA"}) {
        (Z.block(block)).iterate([&](const std::vector<size_t>& i, double& value) {
            value = Z.block("va").data()[i[0] * na + i[1]];
        });
    }
    for (const std::string& block : {"AA"}) {
        (Z.block(block)).iterate([&](const std::vector<size_t>& i, double& value) {
            value = Z.block("aa").data()[i[0] * na + i[1]];
        });
    }
    Z["ME"] = Z["EM"];
    Z["WM"] = Z["MW"];
    Z["WE"] = Z["EW"];
}

}// namespace forte