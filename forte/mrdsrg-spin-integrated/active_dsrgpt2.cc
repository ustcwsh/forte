/*
 * @BEGIN LICENSE
 *
 * Forte: an open-source plugin to Psi4 (https://github.com/psi4/psi4)
 * that implements a variety of quantum chemistry methods for strongly
 * correlated electrons.
 *
 * Copyright (c) 2012-2021 by its authors (see COPYING, COPYING.LESSER, AUTHORS).
 *
 * The copyrights for code used from other parties are included in
 * the corresponding files.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 *
 * @END LICENSE
 */

#ifdef ENABLE_UNTESTED_CODE

#include <algorithm>
#include <iomanip>
#include <map>
#include <fstream>
#include <cstdio>
#include <memory>

#include "psi4/libmints/pointgrp.h"
#include "psi4/libmints/dipole.h"
#include "psi4/libmints/petitelist.h"
#include "psi4/libmints/dimension.h"
#include "helpers/timer.h"
#include "psi4/physconst.h"

#include "integrals/active_space_integrals.h"
#include "sci/fci_mo.h"
#include "orbital-helpers/semi_canonicalize.h"
#include "master_mrdsrg.h"
#include "active_dsrgpt2.h"
#include "boost/format.hpp"
#include "helpers/printing.h"

using namespace psi;

namespace forte {

ACTIVE_DSRGPT2::ACTIVE_DSRGPT2(std::shared_ptr<SCFInfo> scf_info,
                               std::shared_ptr<ForteOptions> options,
                               std::shared_ptr<ForteIntegrals> ints,
                               std::shared_ptr<MOSpaceInfo> mo_space_info)
    : ints_(ints), mo_space_info_(mo_space_info), scf_info_(scf_info), foptions_(options),
      total_nroots_(0) {

    std::string description = "Wrapper for Multiple SS-DSRG-MRPT2 Computations";
    print_method_banner({"ACTIVE-DSRG-MRPT2", description, "Chenyang Li"});

    outfile->Printf("\n  Note: Orbitals are NOT optimized throughout the process.");
    outfile->Printf("\n  Reference selection criterium (CAS/CIS/CISD) will NOT change.");
    outfile->Printf("\n  Each state uses its OWN semicanonical orbitals.");
    outfile->Printf("\n  Ground state is assumed to be a singlet.");
    outfile->Printf("\n  Otherwise, please run separate DSRG-MRPT2 jobs.");

    startup();
}

ACTIVE_DSRGPT2::~ACTIVE_DSRGPT2() {}

void ACTIVE_DSRGPT2::startup() {
    if ((foptions_->psi_options())["NROOTPI"].size() == 0) {
        throw psi::PSIEXCEPTION("Please specify NROOTPI for ACTIVE-DSRGPT2 jobs.");
    } else {
        std::shared_ptr<psi::Molecule> molecule = psi::Process::environment.molecule();
        multiplicity_ = molecule->multiplicity();
        if (foptions_->has_changed("MULTIPLICITY")) {
            multiplicity_ = foptions_->get_int("MULTIPLICITY");
        }

        ref_type_ = foptions_->get_str("FCIMO_ACTV_TYPE");
        if (ref_type_ == "COMPLETE") {
            ref_type_ = "CAS";
        }

        int nirrep = mo_space_info_->nirrep();
        dominant_dets_ = std::vector<vector<Determinant>>(nirrep, std::vector<Determinant>());
        ref_wfns_ = std::vector<psi::SharedMatrix>(nirrep, psi::SharedMatrix());

        // determined absolute orbitals indices in C1 symmetry
        psi::Dimension nmopi = mo_space_info_->dimension("ALL");
        psi::Dimension frzcpi = mo_space_info_->dimension("FROZEN_DOCC");
        psi::Dimension corepi = mo_space_info_->dimension("RESTRICTED_DOCC");
        psi::Dimension actvpi = mo_space_info_->dimension("ACTIVE");
        psi::Dimension virtpi = mo_space_info_->dimension("RESTRICTED_UOCC");

        coreIdxC1_ = std::vector<vector<size_t>>(nirrep, std::vector<size_t>());
        actvIdxC1_ = std::vector<vector<size_t>>(nirrep, std::vector<size_t>());
        virtIdxC1_ = std::vector<vector<size_t>>(nirrep, std::vector<size_t>());
        std::vector<std::tuple<double, int, int>> order;
        for (int h = 0; h < nirrep; ++h) {
            for (int i = 0; i < nmopi[h]; ++i) {
                order.push_back(
                    std::tuple<double, int, int>(scf_info_->epsilon_a()->get(h, i), i, h));
            }
        }
        std::sort(order.begin(), order.end(), std::less<std::tuple<double, int, int>>());

        for (size_t idx = 0; idx < order.size(); ++idx) {
            int i = std::get<1>(order[idx]);
            int h = std::get<2>(order[idx]);

            int core_min = frzcpi[h];
            int core_max = core_min + corepi[h];
            int actv_max = core_max + actvpi[h];
            int virt_max = actv_max + virtpi[h];

            if (i >= core_min && i < core_max) {
                coreIdxC1_[h].push_back(static_cast<size_t>(idx));
            } else if (i >= core_max && i < actv_max) {
                actvIdxC1_[h].push_back(static_cast<size_t>(idx));
            } else if (i >= actv_max && i < virt_max) {
                virtIdxC1_[h].push_back(static_cast<size_t>(idx));
            }
        }

        CharacterTable ct = psi::Process::environment.molecule()->point_group()->char_table();
        std::string cisd_noHF;
        if (ref_type_ == "CISD") {
            t1_percentage_ = std::vector<vector<double>>(nirrep, std::vector<double>());
            if (foptions_->get_bool("FCIMO_CISD_NOHF")) {
                cisd_noHF = "TURE";
            } else {
                cisd_noHF = "FALSE";
            }
        }

        for (int h = 0; h < nirrep; ++h) {
            nrootpi_.push_back((foptions_->psi_options())["NROOTPI"][h].to_integer());
            irrep_symbol_.push_back(std::string(ct.gamma(h).symbol()));
            total_nroots_ += nrootpi_[h];
        }

        // print request
        print_h2("Input Summary");
        std::vector<std::pair<std::string, std::string>> calculation_info_string{
            {"total roots requested (include S0)", std::to_string(total_nroots_)},
            {"multiplicity", std::to_string(multiplicity_)},
            {"reference space type", ref_type_}};
        if (ref_type_ == "CISD") {
            calculation_info_string.push_back({"separate HF in CISD", cisd_noHF});
        }
        std::string ipea = foptions_->get_str("FCIMO_IPEA");
        if (ipea != "NONE") {
            calculation_info_string.push_back({"IPEA type", ipea});
        }
        bool internals = (foptions_->get_str("INTERNAL_AMP") != "NONE");
        calculation_info_string.push_back(
            {"DSRG-MRPT2 internal amplitudes", foptions_->get_str("INTERNAL_AMP")});
        if (internals) {
            calculation_info_string.push_back(
                {"DSRG-MRPT2 internal type", foptions_->get_str("INTERNAL_AMP_SELECT")});
        }
        for (const auto& str_dim : calculation_info_string) {
            outfile->Printf("\n    %-40s %15s", str_dim.first.c_str(), str_dim.second.c_str());
        }

        print_h2("Roots Summary");
        int total_width = 4 + 6 + 6 * nirrep;
        outfile->Printf("\n      %s", std::string(6, ' ').c_str());
        for (int h = 0; h < nirrep; ++h)
            outfile->Printf(" %5s", irrep_symbol_[h].c_str());
        outfile->Printf("\n    %s", std::string(total_width, '-').c_str());
        outfile->Printf("\n      NROOTS");
        for (int h = 0; h < nirrep; ++h) {
            outfile->Printf("%6d", nrootpi_[h]);
        }
        outfile->Printf("\n    %s", std::string(total_width, '-').c_str());
    }
}

double ACTIVE_DSRGPT2::compute_energy() {
    if (total_nroots_ == 0) {
        outfile->Printf("\n  NROOTPI is zero. Did nothing.");
        return 0.0;
    }

    int nirrep = mo_space_info_->nirrep();
    CharacterTable ct = psi::Process::environment.molecule()->point_group()->char_table();
    std::vector<std::string> multi_label{"Singlet", "Doublet", "Triplet", "Quartet", "Quintet",
                                         "Sextet",  "Septet",  "Octet",   "Nonet",   "Decaet"};

    // final energies
    ref_energies_ = std::vector<vector<double>>(nirrep, std::vector<double>());
    pt2_energies_ = std::vector<vector<double>>(nirrep, std::vector<double>());

    // save T1 and T2 blocks that are useful to compute PT2 oscillator strength
    std::vector<std::string> T1blocks{"aa", "AA", "av", "AV", "ca", "CA"};
    std::vector<std::string> T2blocks{"aaaa", "cava", "caaa", "aava", "AAAA",
                                      "CAVA", "CAAA", "AAVA", "aAaA", "cAvA",
                                      "aCaV", "cAaA", "aCaA", "aAvA", "aAaV"};

    // save a copy of the original orbitals
    psi::SharedMatrix Ca0(ints_->Ca()->clone());
    psi::SharedMatrix Cb0(ints_->Cb()->clone());

    // compute MO dipole integrals assume equivalent alpha beta orbitals
    modipole_ints_.clear();
    modipole_ints_ = ints_->mo_dipole_ints();

    // FCI_MO object
    fci_mo_ = std::make_shared<FCI_MO>(scf_info_, foptions_, ints_, mo_space_info_);

    // max cumulant level
    int max_cu_level = foptions_->get_str("THREEPDC") == "ZERO" ? 2 : 3;

    // compute orbital extent
    orb_extents_ = flatten_fci_orbextents(fci_mo_->orb_extents());

    // semicanonicalzer
    auto semi = std::make_shared<SemiCanonical>(mo_space_info_, ints_, foptions_, true);
    if (ref_type_ == "CIS" || ref_type_ == "CISD") {
        semi->set_actv_dims(fci_mo_->actv_docc(), fci_mo_->actv_virt());
    }

    // some preps for oscillator strength
    std::vector<std::pair<psi::SharedVector, double>> eigen0;
    ref_wfns_.clear();

    // real computation
    for (int h = 0; h < nirrep; ++h) {
        if (nrootpi_[h] == 0) {
            if (h == 0) {
                outfile->Printf("\n  Please change the nroot of %s to 1 for the ground state.",
                                irrep_symbol_[0].c_str());
                throw psi::PSIEXCEPTION("Please change NROOTPI to account for the ground state.");
            } else {
                continue;
            }
        }

        // print title
        size_t name_size = multi_label[multiplicity_ - 1].size() + irrep_symbol_[h].size();
        outfile->Printf("\n\n  %s", std::string(name_size + 21, '=').c_str());
        outfile->Printf("\n  Current Job: %s %s states", multi_label[multiplicity_ - 1].c_str(),
                        irrep_symbol_[h].c_str());
        outfile->Printf("\n  %s\n", std::string(name_size + 21, '=').c_str());

        // basic setting under this irrep
        int nroot = nrootpi_[h];
        fci_mo_->set_root_sym(h);

        // set the ground state to singlet A1 when multiplicity is not 1
        if (multiplicity_ != 1 && h == 0) {
            outfile->Printf("\n  Set ground state to singlet %s.", irrep_symbol_[0].c_str());

            // compute reference energy
            set_fcimo_params(1, 0, 1); // nroots, root, multiplicity
            double Eref = fci_mo_->compute_ss_energies()[0];
            ref_energies_[0].push_back(Eref);

            dominant_dets_[h].push_back(fci_mo_->dominant_dets()[0]);
            if (ref_type_ == "CISD") {
                t1_percentage_[h].push_back(fci_mo_->compute_T1_percentage()[0]);
            }

            // compute cumulants
            std::vector<std::pair<size_t, size_t>> root;
            root.push_back(std::make_pair(0, 0));
            RDMs rdms = fci_mo_->rdms(root, 3)[0];

            // semicanonicalize integrals and cumulants
            semi->semicanonicalize(rdms, max_cu_level);

            // compute DSRG-MRPT2 energy
            std::shared_ptr<MASTER_DSRG> dsrg;
            double Ept2 = compute_dsrg_mrpt2_energy(dsrg, rdms);
            pt2_energies_[0].push_back(Ept2);

            // minus nroot (just for irrep 0) by 1
            nroot -= 1;

            // transform integrals to the original basis
            print_h2("Transform Integrals to the Original Basis");
            transform_integrals(Ca0, Cb0);
        }

        // make sure we use the original integrals because of CI coefficients
        if (h != 0) {
            print_h2("Transform Integrals to the Original Basis");
            transform_integrals(Ca0, Cb0);
        }

        // compute reference energy for a given symmetry
        set_fcimo_params(nroot, 0, multiplicity_);
        fci_mo_->compute_ss_energies()[0];

        // loop over nroot and save a copy of the orbital rotation matrix
        // (from original to corresponding semicanonical basis)

        print_h2("Prepare Orbital Rotation Matrices");
        std::vector<psi::SharedMatrix> Uas, Ubs;
        std::vector<ambit::Tensor> Uas_t, Ubs_t;

        for (int i = 0; i < nroot; ++i) {
            outfile->Printf("\n\n  Computing semicanonical orbitals for root %d.", i);
            fci_mo_->set_root(i);
            std::vector<std::pair<size_t, size_t>> root;
            root.push_back(std::make_pair(i, i));
            RDMs rdms = fci_mo_->rdms(root, 3)[0];
            semi->semicanonicalize(rdms, 1, true, false);

            Uas.emplace_back(semi->Ua()->clone());
            Ubs.emplace_back(semi->Ub()->clone());
            Uas_t.emplace_back(semi->Ua_t().clone());
            Ubs_t.emplace_back(semi->Ub_t().clone());
        }

        // fill in dominant_dets_
        std::vector<Determinant> dominant_dets = fci_mo_->dominant_dets();
        for (int i = 0; i < nroot; ++i) {
            dominant_dets_[h].push_back(dominant_dets[i]);
        }

        // fill in %T1
        if (ref_type_ == "CISD") {
            std::vector<double> t1 = fci_mo_->compute_T1_percentage();
            for (int i = 0; i < nroot; ++i) {
                t1_percentage_[h].push_back(t1[i]);
            }
        }

        // compute reference oscillator strength (only for singlet)
        if (multiplicity_ == 1) {
            std::vector<std::pair<psi::SharedVector, double>> eigen = fci_mo_->eigen();
            int eigen_size = eigen.size();
            if (eigen_size != nroot) {
                outfile->Printf("\n  FCI_MO error from ACTIVE_DSRGPT2: Inconsistent nroot "
                                "to eigen_size.");
                throw psi::PSIEXCEPTION("Inconsistent nroot to eigen_size.");
            }

            outfile->Printf("\n\n  Computing V%s reference oscillator strength 0%s -> n%s ... ",
                            ref_type_.c_str(), irrep_symbol_[0].c_str(), irrep_symbol_[h].c_str());

            int dim = (eigen[0].first)->dim();
            psi::SharedMatrix evecs(new psi::Matrix("evecs", dim, eigen_size));
            for (int i = 0; i < eigen_size; ++i) {
                evecs->set_column(0, i, (eigen[i]).first);
            }

            // save a copy of the ref. wfn. in the original basis
            ref_wfns_[h] = evecs;

            std::vector<Determinant> p_space1 = fci_mo_->p_space();
            if (h == 0) {
                eigen0 = eigen;
                p_space_g_ = fci_mo_->p_space();
            }
            compute_osc_ref(0, h, p_space_g_, p_space1, eigen0, eigen);
            outfile->Printf("Done.");
        }

        // loop over nroot to compute ss-dsrg-mrpt2 energies
        for (int i = 0; i < nroot; ++i) {
            int i_real = i;
            if (multiplicity_ != 1 && h == 0) {
                i_real = i + 1;
            }
            std::string current = "Working on Root " + std::to_string(i_real);
            print_h2(current);

            // save reference energies
            double Eref = ((fci_mo_->eigen())[i]).second;
            ref_energies_[h].push_back(Eref);

            // compute cumulants
            fci_mo_->set_root(i);

            // can move this out of loop
            std::vector<std::pair<size_t, size_t>> rootvec;
            rootvec.push_back(std::make_pair(i, i));
            RDMs rdms = fci_mo_->rdms(rootvec, max_cu_level)[0];

            // manually rotate the RDMs and integrals
            semi->transform_rdms(Uas_t[i], Ubs_t[i], rdms, max_cu_level);
            print_h2("Integral Transformation to Semicanonical Basis");
            psi::SharedMatrix Ca = ints_->Ca();
            psi::SharedMatrix Cb = ints_->Cb();
            Ca->gemm(false, false, 1.0, Ca0, Uas[i], 0.0);
            Cb->gemm(false, false, 1.0, Cb0, Ubs[i], 0.0);
            ints_->update_orbitals(Ca, Cb);

            // obtain the name of transition type
            std::string trans_name = transition_type(0, 0, i_real, h);

            // decide whether to compute oscillator strength or not
            bool do_osc = false;
            if (f_ref_.find(trans_name) != f_ref_.end()) {
                if (f_ref_[trans_name].t > 1.0e-6) {
                    do_osc = true;
                }
            }
            bool gs = (h == 0) && (i_real == 0);

            // compute DSRG-MRPT2 energy
            std::shared_ptr<MASTER_DSRG> dsrg;
            double Ept2 = compute_dsrg_mrpt2_energy(dsrg, rdms);
            pt2_energies_[h].push_back(Ept2);

            // Declare useful amplitudes outside dsrg-mrpt2 to avoid storage of multiple
            // 3-density, since orbital space labels are identical for different states
            // (although it is set in dsrg-mrpt2 for each state)
            double Tde = 0.0;
            ambit::BlockedTensor T1, T2;

            if (gs || do_osc) {
                // obtain de-normal-orderd T1 and T2 amplitudes
                T1 = dsrg->get_T1deGNO(Tde);
                T2 = dsrg->get_T2(T2blocks);

                // rotate T1 and T2 from semicanonical to original basis
                rotate_amp(Uas[i]->transpose(), Ubs[i]->transpose(), T1, T2);

                if (gs) {
                    Tde_g_ = Tde;
                    T1_g_ = ambit::BlockedTensor::build(ambit::CoreTensor, "T1_g", T1blocks);
                    T2_g_ = ambit::BlockedTensor::build(ambit::CoreTensor, "T2_g", T2blocks);
                    T1_g_["ia"] = T1["ia"];
                    T1_g_["IA"] = T1["IA"];
                    T2_g_["ijab"] = T2["ijab"];
                    T2_g_["iJaB"] = T2["iJaB"];
                    T2_g_["IJAB"] = T2["IJAB"];
                }
            }

            // if the reference oscillator strength is nonzero
            if (do_osc) {
                local_timer osc_pt2;
                outfile->Printf("\n\n  Computing V%s-DSRG-PT2 oscillator strength ...",
                                ref_type_.c_str());
                compute_osc_pt2(h, i_real, Tde, T1, T2);
                //                        compute_osc_pt2_dets(h, i_real, Tde, T1, T2);
                //                        compute_osc_pt2_overlap(h, i_real, Tde, T1, T2);
                outfile->Printf(" Done. Timing %15.6f s", osc_pt2.get());
            }
        }
    }

    // print results
    if (multiplicity_ == 1) {
        print_osc();
    }
    print_summary();

    // pass to psi::Process::environment
    for (int h = 0, counter = 0; h < nirrep; ++h) {
        for (int i = 0; i < nrootpi_[h]; ++i) {
            std::string str_counter = std::to_string(counter);
            psi::Process::environment.globals["ENERGY ROOT " + str_counter] = pt2_energies_[h][i];
            psi::Process::environment.globals["CURRENT ENERGY"] = pt2_energies_[h][i];
            ++counter;
        }
    }

    // return the last energy
    return psi::Process::environment.globals["CURRENT ENERGY"];
}

void ACTIVE_DSRGPT2::set_fcimo_params(int nroots, int root, int multiplicity) {
    fci_mo_->set_multiplicity(multiplicity);
    fci_mo_->set_nroots(nroots);
    fci_mo_->set_root(root);
}

double ACTIVE_DSRGPT2::compute_dsrg_mrpt2_energy(std::shared_ptr<MASTER_DSRG>& dsrg, RDMs& rdms) {
    IntegralType int_type = ints_->integral_type();
    if (int_type == Conventional) {
        dsrg = std::make_shared<DSRG_MRPT2>(rdms, scf_info_, foptions_, ints_, mo_space_info_);
    } else if (int_type == Cholesky || int_type == DF || int_type == DiskDF) {
        dsrg =
            std::make_shared<THREE_DSRG_MRPT2>(rdms, scf_info_, foptions_, ints_, mo_space_info_);
    } else {
        throw psi::PSIEXCEPTION("Unknown integral type for DSRG.");
    }
    dsrg->set_actv_occ(fci_mo_->actv_occ());
    dsrg->set_actv_uocc(fci_mo_->actv_uocc());

    return dsrg->compute_energy();
}

void ACTIVE_DSRGPT2::rotate_amp(psi::SharedMatrix Ua, psi::SharedMatrix Ub,
                                ambit::BlockedTensor& T1, ambit::BlockedTensor& T2) {
    ambit::BlockedTensor U =
        ambit::BlockedTensor::build(ambit::CoreTensor, "Uorb", spin_cases({"gg"}));

    std::map<char, std::vector<std::pair<size_t, size_t>>> space_to_relmo;
    space_to_relmo['c'] = mo_space_info_->relative_mo("RESTRICTED_DOCC");
    space_to_relmo['a'] = mo_space_info_->relative_mo("ACTIVE");
    space_to_relmo['v'] = mo_space_info_->relative_mo("RESTRICTED_UOCC");

    for (const std::string& block : {"cc", "aa", "vv", "CC", "AA", "VV"}) {
        char space = tolower(block[0]);
        auto Uspin = islower(block[0]) ? Ua : Ub;

        U.block(block).iterate([&](const std::vector<size_t>& i, double& value) {
            std::pair<size_t, size_t> p0 = space_to_relmo[space][i[0]];
            std::pair<size_t, size_t> p1 = space_to_relmo[space][i[1]];
            size_t h0 = p0.first, h1 = p1.first;
            size_t i0 = p0.second, i1 = p1.second;

            if (h0 == h1) {
                value = Uspin->get(h0, i0, i1);
            }
        });
    }

    // rotate amplitudes
    BlockedTensor temp = ambit::BlockedTensor::build(CoreTensor, "Temp T2", T2.block_labels());
    temp["klab"] = U["ik"] * U["jl"] * T2["ijab"];
    temp["kLaB"] = U["ik"] * U["JL"] * T2["iJaB"];
    temp["KLAB"] = U["IK"] * U["JL"] * T2["IJAB"];
    T2["ijcd"] = temp["ijab"] * U["bd"] * U["ac"];
    T2["iJcD"] = temp["iJaB"] * U["BD"] * U["ac"];
    T2["IJCD"] = temp["IJAB"] * U["BD"] * U["AC"];

    temp = ambit::BlockedTensor::build(CoreTensor, "Temp T1", T1.block_labels());
    temp["jb"] = U["ij"] * T1["ia"] * U["ab"];
    temp["JB"] = U["IJ"] * T1["IA"] * U["AB"];
    T1["ia"] = temp["ia"];
    T1["IA"] = temp["IA"];
}

void ACTIVE_DSRGPT2::compute_osc_ref(
    const int& irrep0, const int& irrep1, const std::vector<Determinant>& p_space0,
    const std::vector<Determinant>& p_space1,
    const std::vector<std::pair<psi::SharedVector, double>>& eigen0,
    const std::vector<std::pair<psi::SharedVector, double>>& eigen1) {
    // some basic test
    size_t ndet0 = p_space0.size();
    size_t ndet1 = p_space1.size();
    if (ndet0 != static_cast<size_t>(eigen0[0].first->dim())) {
        std::string error = "Error from compute_ref_osc: size of p_space does not match the "
                            "dimension of eigen vector.";
        outfile->Printf("\n  %s", error.c_str());
        throw psi::PSIEXCEPTION(error);
    }

    // determine if p_space0 and p_space1 are the same (even ordering)
    bool same = false;
    same = (p_space0 == p_space1) && (irrep0 == irrep1);

    // combined space of determinants
    size_t ndet = ndet0;
    std::vector<Determinant> p_space(p_space0);
    if (!same) {
        ndet += ndet1;
        p_space.insert(p_space.end(), p_space1.begin(), p_space1.end());
    }

    // combined eigen values and vectors
    size_t nroot0 = eigen0.size();
    size_t nroot1 = eigen1.size();
    size_t nroot = nroot0;
    std::vector<double> evals(nroot, 0.0);
    psi::SharedMatrix evecs(new psi::Matrix("combined evecs", ndet, nroot));

    if (same) {
        for (size_t n = 0; n < nroot0; ++n) {
            evals[n] = eigen0[n].second;
            evecs->set_column(0, n, eigen0[n].first);
        }
    } else {
        nroot += nroot1;
        evals = std::vector<double>(nroot, 0.0);
        evecs = std::make_shared<psi::Matrix>("combined evecs", ndet, nroot);

        for (size_t n = 0; n < nroot0; ++n) {
            evals[n] = eigen0[n].second;

            psi::SharedVector evec0 = eigen0[n].first;
            psi::SharedVector evec(new Vector("combined evec0 " + std::to_string(n), ndet));
            for (size_t i = 0; i < ndet0; ++i) {
                evec->set(i, evec0->get(i));
            }
            evecs->set_column(0, n, evec);
        }

        for (size_t n = 0; n < nroot1; ++n) {
            evals[n + nroot0] = eigen1[n].second;

            psi::SharedVector evec1 = eigen1[n].first;
            psi::SharedVector evec(new Vector("combined evec1 " + std::to_string(n), ndet));
            for (size_t i = 0; i < ndet1; ++i) {
                evec->set(i + ndet0, evec1->get(i));
            }
            evecs->set_column(0, n + nroot0, evec);
        }
    }

    // compute oscillator strength for S0(sym0) -> Sn
    int start = 1, offset = 0;
    if (nroot != nroot0) {
        // different irrep
        start = nroot0;
        offset = nroot0;
    }

    for (size_t n = start; n < nroot; ++n) {
        Vector4 transD = compute_td_ref_root(fci_mo_->fci_ints(), p_space, evecs, 0, n);
        double Eexcited = evals[n] - evals[0];

        Vector4 osc;
        osc.x = 2.0 / 3.0 * Eexcited * transD.x * transD.x;
        osc.y = 2.0 / 3.0 * Eexcited * transD.y * transD.y;
        osc.z = 2.0 / 3.0 * Eexcited * transD.z * transD.z;
        osc.t = osc.x + osc.y + osc.z;

        std::string name = transition_type(0, irrep0, n - offset, irrep1);
        tdipole_ref_[name] = transD;
        f_ref_[name] = osc;
    }
}

std::string ACTIVE_DSRGPT2::transition_type(const int& n0, const int& irrep0, const int& n1,
                                            const int& irrep1) {
    CharacterTable ct = psi::Process::environment.molecule()->point_group()->char_table();
    std::string symbol = ct.symbol();
    int width = 2;
    if (symbol == "cs" || symbol == "d2h") {
        width = 3;
    } else if (symbol == "c1") {
        width = 1;
    }

    std::string S0_symbol = ct.gamma(irrep0).symbol();
    std::string Sn_symbol = ct.gamma(irrep1).symbol();

    std::stringstream name_ss;
    name_ss << std::setw(2) << n0 << " " << std::setw(width) << S0_symbol << " -> " << std::setw(2)
            << n1 << " " << std::setw(width) << Sn_symbol;
    return name_ss.str();
}

Vector4 ACTIVE_DSRGPT2::compute_td_ref_root(std::shared_ptr<ActiveSpaceIntegrals> fci_ints,
                                            const std::vector<Determinant>& p_space,
                                            psi::SharedMatrix evecs, const int& root0,
                                            const int& root1) {
    int nirrep = mo_space_info_->nirrep();
    psi::Dimension nmopi = mo_space_info_->dimension("ALL");
    psi::Dimension actvpi = mo_space_info_->dimension("ACTIVE");
    size_t nactv = actvpi.sum();
    size_t nmo = nmopi.sum();

    // obtain MO transition density
    CI_RDMS ci_rdms(fci_ints, p_space, evecs, root0, root1);
    std::vector<double> opdm_a(nactv * nactv, 0.0);
    std::vector<double> opdm_b(nactv * nactv, 0.0);
    ci_rdms.compute_1rdm(opdm_a, opdm_b);

    // prepare MO transition density (spin summed)
    psi::SharedMatrix MOtransD(new psi::Matrix("MO TransD", nmo, nmo));

    auto offset_irrep = [](const int& h, const psi::Dimension& npi) -> size_t {
        int h_local = h;
        size_t offset = 0;
        while ((--h_local) >= 0) {
            offset += npi[h_local];
        }
        return offset;
    };

    for (int h0 = 0; h0 < nirrep; ++h0) {
        size_t offset_rdm_h0 = offset_irrep(h0, actvpi);

        for (int h1 = 0; h1 < nirrep; ++h1) {
            size_t offset_rdm_h1 = offset_irrep(h1, actvpi);

            for (int u = 0; u < actvpi[h0]; ++u) {
                size_t u_rdm = u + offset_rdm_h0;
                size_t u_all = actvIdxC1_[h0][u];

                for (int v = 0; v < actvpi[h1]; ++v) {
                    size_t v_rdm = v + offset_rdm_h1;
                    size_t v_all = actvIdxC1_[h1][v];

                    size_t idx = u_rdm * nactv + v_rdm;
                    MOtransD->set(u_all, v_all, opdm_a[idx] + opdm_b[idx]);
                }
            }
        }
    }

    // compute transition dipole
    Vector4 transD;
    transD.x = MOtransD->vector_dot(modipole_ints_[0]);
    transD.y = MOtransD->vector_dot(modipole_ints_[1]);
    transD.z = MOtransD->vector_dot(modipole_ints_[2]);
    transD.t = sqrt(transD.x * transD.x + transD.y * transD.y + transD.z * transD.z);

    return transD;
}

void ACTIVE_DSRGPT2::compute_osc_pt2(const int& irrep, const int& root, const double& Tde_x,
                                     ambit::BlockedTensor& T1_x, ambit::BlockedTensor& T2_x) {
    // compute rdms transition density
    // step 1: combine p_space and eigenvectors if needed
    int n = root;
    std::vector<Determinant> p_space(p_space_g_);
    psi::SharedMatrix evecs = ref_wfns_[0];

    if (irrep != 0) {
        n += ref_wfns_[0]->ncol();
        std::vector<Determinant> p_space1 = fci_mo_->p_space();
        p_space.insert(p_space.end(), p_space1.begin(), p_space1.end());
        evecs = combine_evecs(0, irrep);
    }

    // step 2: use CI_RDMS to compute transition density
    CI_RDMS ci_rdms(fci_mo_->fci_ints(), p_space, evecs, 0, n);

    ambit::BlockedTensor TD1, TD2, TD3;
    TD1 = ambit::BlockedTensor::build(ambit::CoreTensor, "TD1", spin_cases({"aa"}));
    TD2 = ambit::BlockedTensor::build(ambit::CoreTensor, "TD2", spin_cases({"aaaa"}));
    TD3 = ambit::BlockedTensor::build(ambit::CoreTensor, "TD3", spin_cases({"aaaaaa"}));

    ci_rdms.compute_1rdm(TD1.block("aa").data(), TD1.block("aa").data());
    ci_rdms.compute_2rdm(TD2.block("aaaa").data(), TD2.block("aAaA").data(),
                         TD2.block("AAAA").data());
    ci_rdms.compute_3rdm(TD3.block("aaaaaa").data(), TD3.block("aaAaaA").data(),
                         TD3.block("aAAaAA").data(), TD3.block("AAAAAA").data());

    // compute first-order effective transition density
    // step 1: initialization
    ambit::BlockedTensor TDeff = ambit::BlockedTensor::build(ambit::CoreTensor, "TDeff",
                                                             spin_cases({"hp", "vc", "va", "ac"}));
    ambit::BlockedTensor temp =
        ambit::BlockedTensor::build(ambit::CoreTensor, "TDeff temp", spin_cases({"hp"}));

    // step 2: compute TDeff from <ref_x| (A_x)^+ * mu |ref_g>
    double fc_x = compute_TDeff(T1_x, T2_x, TD1, TD2, TD3, temp, true);
    TDeff["ai"] = temp["ia"];
    TDeff["AI"] = temp["IA"];

    // step 3: compute TDeff from <ref_x| mu * A_g |ref_g>
    double fc_g = compute_TDeff(T1_g_, T2_g_, TD1, TD2, TD3, TDeff, false);

    // put TDeff into psi::SharedMatrix format
    // step 1: setup orbital maps
    std::map<char, std::vector<std::pair<size_t, size_t>>> space_rel_idx;
    space_rel_idx['c'] = mo_space_info_->relative_mo("RESTRICTED_DOCC");
    space_rel_idx['a'] = mo_space_info_->relative_mo("ACTIVE");
    space_rel_idx['v'] = mo_space_info_->relative_mo("RESTRICTED_UOCC");

    std::map<char, std::vector<std::vector<size_t>>> space_C1_idx;
    space_C1_idx['c'] = coreIdxC1_;
    space_C1_idx['a'] = actvIdxC1_;
    space_C1_idx['v'] = virtIdxC1_;

    std::map<char, psi::Dimension> space_offsets;
    space_offsets['c'] = mo_space_info_->dimension("FROZEN_DOCC");
    space_offsets['a'] = space_offsets['c'] + mo_space_info_->dimension("RESTRICTED_DOCC");
    space_offsets['v'] = space_offsets['a'] + mo_space_info_->dimension("ACTIVE");

    // step 2: copy data to psi::SharedMatrix
    size_t nmo = modipole_ints_[0]->nrow();
    psi::SharedMatrix MOtransD(new psi::Matrix("MO TransD", nmo, nmo));
    for (const std::string& block : TDeff.block_labels()) {
        char c0 = tolower(block[0]);
        char c1 = tolower(block[1]);

        std::vector<std::pair<size_t, size_t>> relIdx0 = space_rel_idx[c0];
        std::vector<std::pair<size_t, size_t>> relIdx1 = space_rel_idx[c1];

        TDeff.block(block).citerate([&](const std::vector<size_t>& i, const double& value) {
            size_t h0 = relIdx0[i[0]].first;
            size_t h1 = relIdx1[i[1]].first;

            size_t offset0 = space_offsets[c0][h0];
            size_t offset1 = space_offsets[c1][h1];

            size_t ri0 = relIdx0[i[0]].second - offset0;
            size_t ri1 = relIdx1[i[1]].second - offset1;

            size_t n0 = space_C1_idx[c0][h0][ri0];
            size_t n1 = space_C1_idx[c1][h1][ri1];

            MOtransD->add(n0, n1, value);
        });
    }
    //    MOtransD->print();

    // contract with MO dipole integrals
    Vector4 transD;
    transD.x = MOtransD->vector_dot(modipole_ints_[0]);
    transD.y = MOtransD->vector_dot(modipole_ints_[1]);
    transD.z = MOtransD->vector_dot(modipole_ints_[2]);
    transD.t = 0.0;

    // add diagonal core contribution sum_{m} mu^{m}_{m} * tc, where tc is a scalar from T * TD
    std::vector<double> mud_core(3, 0.0);
    for (int dir = 0; dir < 3; ++dir) {
        double mu = 0.0;
        for (const auto& p : space_rel_idx['c']) {
            size_t h = p.first;
            size_t m = p.second - space_offsets['c'][h];
            size_t idx = space_C1_idx['c'][h][m];
            mu += modipole_ints_[dir]->get(idx, idx);
        }
        mu *= fc_g + fc_x;

        mud_core[dir] = mu;
    }
    transD.x += mud_core[0];
    transD.y += mud_core[1];
    transD.z += mud_core[2];

    // add zeroth-order transition density
    std::string name = transition_type(0, 0, root, irrep);
    double scale = 1.0 + Tde_g_ + Tde_x;
    transD.x += tdipole_ref_[name].x * scale;
    transD.y += tdipole_ref_[name].y * scale;
    transD.z += tdipole_ref_[name].z * scale;

    // save DSRG-PT2 transition density
    transD.t = sqrt(transD.x * transD.x + transD.y * transD.y + transD.z * transD.z);
    tdipole_pt2_[name] = transD;

    // compute oscillator strength
    double Eexcited = pt2_energies_[irrep][root] - pt2_energies_[0][0];
    Vector4 osc;
    osc.x = 2.0 / 3.0 * Eexcited * transD.x * transD.x;
    osc.y = 2.0 / 3.0 * Eexcited * transD.y * transD.y;
    osc.z = 2.0 / 3.0 * Eexcited * transD.z * transD.z;
    osc.t = osc.x + osc.y + osc.z;
    f_pt2_[name] = osc;
}

double ACTIVE_DSRGPT2::compute_TDeff(ambit::BlockedTensor& T1, ambit::BlockedTensor& T2,
                                     ambit::BlockedTensor& TD1, ambit::BlockedTensor& TD2,
                                     ambit::BlockedTensor& TD3, ambit::BlockedTensor& TDeff,
                                     const bool& transpose) {
    // initialization
    double scalar = 0.0;
    bool internal_amp = foptions_->get_str("INTERNAL_AMP") != "NONE";

    std::string uv = "uv", UV = "UV";
    std::string uvxy = "uvxy", uVxY = "uVxY", vUyX = "vUyX", UVXY = "UVXY";
    std::string uvwxyz = "uvwxyz", uvWxyZ = "uvWxyZ", uVWxYZ = "uVWxYZ";
    std::string vwUyzX = "vwUyzX", vUWyXZ = "vUWyXZ", UVWXYZ = "UVWXYZ";

    if (transpose) {
        uv = "vu";
        UV = "VU";

        uvxy = "xyuv";
        uVxY = "xYuV";
        vUyX = "yXvU";
        UVXY = "XYUV";

        uvwxyz = "xyzuvw";
        uvWxyZ = "xyZuvW";
        uVWxYZ = "xYZuVW";
        vwUyzX = "yzXvwU";
        vUWyXZ = "yXZvUW";
        UVWXYZ = "XYZUVW";
    }

    if (internal_amp) {
        scalar += T1["vu"] * TD1[uv];
        scalar += T1["VU"] * TD1[UV];

        scalar -= T1["uv"] * TD1[uv];
        scalar -= T1["UV"] * TD1[UV];

        scalar += 0.25 * T2["xyuv"] * TD2[uvxy];
        scalar += 0.25 * T2["XYUV"] * TD2[UVXY];
        scalar += T2["xYuV"] * TD2[uVxY];

        scalar -= 0.25 * T2["uvxy"] * TD2[uvxy];
        scalar -= 0.25 * T2["UVXY"] * TD2[UVXY];
        scalar -= T2["uVxY"] * TD2[uVxY];

        TDeff["ux"] += T1["vx"] * TD1[uv];
        TDeff["UX"] += T1["VX"] * TD1[UV];

        TDeff["ux"] -= T1["xv"] * TD1[uv];
        TDeff["UX"] -= T1["XV"] * TD1[UV];

        TDeff["ux"] += T1["yv"] * TD2[uvxy];
        TDeff["ux"] += T1["YV"] * TD2[uVxY];
        TDeff["UX"] += T1["yv"] * TD2[vUyX];
        TDeff["UX"] += T1["YV"] * TD2[UVXY];

        TDeff["ux"] -= T1["vy"] * TD2[uvxy];
        TDeff["ux"] -= T1["VY"] * TD2[uVxY];
        TDeff["UX"] -= T1["vy"] * TD2[vUyX];
        TDeff["UX"] -= T1["VY"] * TD2[UVXY];

        TDeff["uz"] += 0.5 * T2["xyzv"] * TD2[uvxy];
        TDeff["uz"] += T2["xYzV"] * TD2[uVxY];
        TDeff["UZ"] += T2["yXvZ"] * TD2[vUyX];
        TDeff["UZ"] += 0.5 * T2["XYZV"] * TD2[UVXY];

        TDeff["uz"] -= 0.5 * T2["zvxy"] * TD2[uvxy];
        TDeff["uz"] -= T2["zVxY"] * TD2[uVxY];
        TDeff["UZ"] -= T2["vZyX"] * TD2[vUyX];
        TDeff["UZ"] -= 0.5 * T2["ZVXY"] * TD2[UVXY];

        TDeff["ux"] += 0.25 * T2["yzvw"] * TD3[uvwxyz];
        TDeff["ux"] += T2["yZvW"] * TD3[uvWxyZ];
        TDeff["ux"] += 0.25 * T2["YZVW"] * TD3[uVWxYZ];
        TDeff["UX"] += 0.25 * T2["yzvw"] * TD3[vwUyzX];
        TDeff["UX"] += T2["yZvW"] * TD3[vUWyXZ];
        TDeff["UX"] += 0.25 * T2["YZVW"] * TD3[UVWXYZ];

        TDeff["ux"] -= 0.25 * T2["vwyz"] * TD3[uvwxyz];
        TDeff["ux"] -= T2["vWyZ"] * TD3[uvWxyZ];
        TDeff["ux"] -= 0.25 * T2["VWYZ"] * TD3[uVWxYZ];
        TDeff["UX"] -= 0.25 * T2["vwyz"] * TD3[vwUyzX];
        TDeff["UX"] -= T2["vWyZ"] * TD3[vUWyXZ];
        TDeff["UX"] -= 0.25 * T2["VWYZ"] * TD3[UVWXYZ];
    }

    TDeff["ue"] += T1["ve"] * TD1[uv];
    TDeff["UE"] += T1["VE"] * TD1[UV];

    TDeff["mv"] -= T1["mu"] * TD1[uv];
    TDeff["MV"] -= T1["MU"] * TD1[UV];

    TDeff["ma"] += T2["mvau"] * TD1[uv];
    TDeff["ma"] += T2["mVaU"] * TD1[UV];
    TDeff["MA"] += T2["vMuA"] * TD1[uv];
    TDeff["MA"] += T2["MVAU"] * TD1[UV];

    TDeff["ue"] += 0.5 * T2["xyev"] * TD2[uvxy];
    TDeff["ue"] += T2["xYeV"] * TD2[uVxY];
    TDeff["UE"] += T2["yXvE"] * TD2[vUyX];
    TDeff["UE"] += 0.5 * T2["XYEV"] * TD2[UVXY];

    TDeff["mx"] -= 0.5 * T2["myuv"] * TD2[uvxy];
    TDeff["mx"] -= T2["mYuV"] * TD2[uVxY];
    TDeff["MX"] -= T2["yMvU"] * TD2[vUyX];
    TDeff["MX"] -= 0.5 * T2["MYUV"] * TD2[UVXY];

    return scalar;
}

psi::SharedMatrix ACTIVE_DSRGPT2::combine_evecs(const int& h0, const int& h1) {
    psi::SharedMatrix evecs0 = ref_wfns_[h0];
    psi::SharedMatrix evecs1 = ref_wfns_[h1];

    int nroot0 = evecs0->ncol();
    int nroot1 = evecs1->ncol();
    int nroot = nroot0 + nroot1;

    size_t ndet0 = evecs0->nrow();
    size_t ndet1 = evecs1->nrow();
    size_t ndet = ndet0 + ndet1;

    psi::SharedMatrix evecs(new psi::Matrix("combined evecs", ndet, nroot));

    for (int n = 0; n < nroot0; ++n) {
        psi::SharedVector evec0 = evecs0->get_column(0, n);
        psi::SharedVector evec(new Vector("combined evec0 " + std::to_string(n), ndet));
        for (size_t i = 0; i < ndet0; ++i) {
            evec->set(i, evec0->get(i));
        }
        evecs->set_column(0, n, evec);
    }

    for (int n = 0; n < nroot1; ++n) {
        psi::SharedVector evec1 = evecs1->get_column(0, n);
        psi::SharedVector evec(new Vector("combined evec1 " + std::to_string(n), ndet));
        for (size_t i = 0; i < ndet1; ++i) {
            evec->set(i + ndet0, evec1->get(i));
        }
        evecs->set_column(0, n + nroot0, evec);
    }

    return evecs;
}

void ACTIVE_DSRGPT2::transform_integrals(psi::SharedMatrix Ca0, psi::SharedMatrix Cb0) {
    // copy to the wave function
    psi::SharedMatrix Ca = ints_->Ca();
    psi::SharedMatrix Cb = ints_->Cb();
    Ca->copy(Ca0);
    Cb->copy(Cb0);

    // transform integrals
    outfile->Printf("\n\n");
    std::vector<size_t> idx_a = mo_space_info_->corr_absolute_mo("ACTIVE");
    ints_->update_orbitals(Ca, Cb);
    ambit::Tensor tei_active_aa = ints_->aptei_aa_block(idx_a, idx_a, idx_a, idx_a);
    ambit::Tensor tei_active_ab = ints_->aptei_ab_block(idx_a, idx_a, idx_a, idx_a);
    ambit::Tensor tei_active_bb = ints_->aptei_bb_block(idx_a, idx_a, idx_a, idx_a);
    auto fci_ints =
        std::make_shared<ActiveSpaceIntegrals>(ints_, mo_space_info_->corr_absolute_mo("ACTIVE"),
                                               mo_space_info_->corr_absolute_mo("RESTRICTED_DOCC"));
    fci_ints->set_active_integrals(tei_active_aa, tei_active_ab, tei_active_bb);
    fci_ints->compute_restricted_one_body_operator();
    fci_mo_->set_fci_int(fci_ints);
}

void ACTIVE_DSRGPT2::print_osc() {

    auto loop_print = [&](std::map<std::string, Vector4> vecpair) -> std::string {
        std::stringstream ss_out;
        for (const auto& p : vecpair) {
            const Vector4& d = p.second;
            ss_out << std::endl
                   << "  " << p.first << ":  X: " << format_double(d.x, 7, 4)
                   << "  Y: " << format_double(d.y, 7, 4) << "  Z: " << format_double(d.z, 7, 4)
                   << "  Total: " << format_double(d.t, 7, 4);
        }
        return ss_out.str();
    };

    std::stringstream out;

    auto out_print = [&](std::string title, std::string values) {
        print_h2(title);
        outfile->Printf("%s", values.c_str());
        out << title << std::endl << values << std::endl << std::endl;
    };

    // print reference transition dipole
    std::string title = "V" + ref_type_ + " Transition Dipole Moment (a.u.)";
    std::string values = loop_print(tdipole_ref_);
    out_print(title, values);

    // print reference oscillator strength
    title = "V" + ref_type_ + " Oscillator Strength (a.u.)";
    values = loop_print(f_ref_);
    out_print(title, values);

    // print DSRG-PT2 transition dipole
    title = "V" + ref_type_ + "-DSRG-PT2 Transition Dipole Moment (a.u.)";
    values = loop_print(tdipole_pt2_);
    out_print(title, values);

    // print DSRG-PT2 oscillator strength
    title = "V" + ref_type_ + "-DSRG-PT2 Oscillator Strength (a.u.)";
    values = loop_print(f_pt2_);
    out_print(title, values);

    // write to file (overwrite existing file)
    std::ofstream out_osc;
    out_osc.open("result_osc.txt");
    out_osc << out.rdbuf();
    out_osc.close();
}

void ACTIVE_DSRGPT2::print_summary() {
    int nirrep = mo_space_info_->nirrep();

    // print raw data
    std::string title = "  ==> ACTIVE-DSRG-MRPT2 Summary <==";
    std::stringstream out;
    out << title << std::endl;

    std::string ref_name = (ref_type_ == "CAS") ? "CAS" : "V" + ref_type_;
    std::string pt2_name = ref_name + "-DSRG-PT2";
    out << std::endl
        << "    2S+1  Sym.  ROOT  " << std::setw(18) << ref_name << "  " << std::setw(18)
        << pt2_name;

    if (ref_type_ == "CISD") {
        int total_width = 4 + 4 + 4 + 18 + 18 + 6 + 2 * 5;
        out << "  " << std::setw(6) << "% T1";
        out << std::endl << "    " << std::string(total_width, '-');

        for (int h = 0; h < nirrep; ++h) {
            if (nrootpi_[h] != 0) {
                std::string sym = irrep_symbol_[h];

                for (int i = 0; i < nrootpi_[h]; ++i) {
                    int multi = multiplicity_;
                    if (h == 0 && multiplicity_ != 1 && i == 0) {
                        multi = 1;
                    }
                    out << std::endl
                        << "    " << std::setw(4) << multi << "  " << std::setw(4) << sym << "  "
                        << std::setw(4) << i << "  " << format_double(ref_energies_[h][i], 18, 10)
                        << "  " << format_double(pt2_energies_[h][i], 18, 10) << "  "
                        << format_double(t1_percentage_[h][i], 6, 2);
                }
                out << std::endl << "    " << std::string(total_width, '-');
            }
        }
    } else {
        int total_width = 4 + 4 + 4 + 18 + 18 + 2 * 4;
        out << std::endl << "    " << std::string(total_width, '-');

        for (int h = 0; h < nirrep; ++h) {
            if (nrootpi_[h] != 0) {
                std::string sym = irrep_symbol_[h];

                for (int i = 0; i < nrootpi_[h]; ++i) {
                    int multi = multiplicity_;
                    if (h == 0 && multiplicity_ != 1 && i == 0) {
                        multi = 1;
                    }
                    out << std::endl
                        << "    " << std::setw(4) << multi << "  " << std::setw(4) << sym << "  "
                        << std::setw(4) << i << "  " << format_double(ref_energies_[h][i], 18, 10)
                        << "  " << format_double(pt2_energies_[h][i], 18, 10);
                }
                out << std::endl << "    " << std::string(total_width, '-');
            }
        }
    }

    // print excitation energies in eV
    if (total_nroots_ > 1) {
        title = "  ==> Relative Energy WRT Totally Symmetric Ground State (eV) <==";
        out << std::endl << std::endl << title << std::endl;

        double ev = pc_hartree2ev;
        if (ref_type_ == "CAS") {
            int width = 4 + 4 + 4 + 8 + 8 + 2 * 4;
            out << std::endl
                << "    2S+1  Sym.  ROOT  " << std::setw(8) << ref_name << "  " << std::setw(8)
                << "DSRG-PT2";
            out << std::endl << "    " << std::string(width, '-');

            for (int h = 0; h < nirrep; ++h) {
                if (nrootpi_[h] != 0) {
                    std::string sym = irrep_symbol_[h];

                    for (int i = 0; i < nrootpi_[h]; ++i) {
                        if (h == 0 && i == 0) {
                            continue;
                        }

                        double Eref = ev * (ref_energies_[h][i] - ref_energies_[0][0]);
                        double Ept2 = ev * (pt2_energies_[h][i] - pt2_energies_[0][0]);

                        out << std::endl
                            << "    " << std::setw(4) << multiplicity_ << "  " << std::setw(4)
                            << sym << "  " << std::setw(4) << i << "  " << format_double(Eref, 8, 4)
                            << "  " << format_double(Ept2, 8, 4);
                    }
                    if (h != 0 || nrootpi_[0] != 1)
                        out << std::endl << "    " << std::string(width, '-');
                }
            }
        } else {
            int width = 4 + 4 + 4 + 8 + 8 + 40 + 2 * 5;
            out << std::endl
                << "    2S+1  Sym.  ROOT  " << std::setw(8) << ref_name << "  DSRG-PT2  "
                << std::setw(40) << "Excitation Type";
            out << std::endl << "    " << std::string(width, '-');

            for (int h = 0; h < nirrep; ++h) {
                if (nrootpi_[h] != 0) {
                    std::string sym = irrep_symbol_[h];

                    for (int i = 0; i < nrootpi_[h]; ++i) {
                        if (h == 0 & i == 0) {
                            continue;
                        }

                        double Eref = ev * (ref_energies_[h][i] - ref_energies_[0][0]);
                        double Ept2 = ev * (pt2_energies_[h][i] - pt2_energies_[0][0]);

                        std::string ex_type =
                            compute_ex_type(dominant_dets_[h][i], dominant_dets_[0][0]);

                        out << std::endl
                            << "    " << std::setw(4) << multiplicity_ << "  " << std::setw(4)
                            << sym << "  " << std::setw(4) << i << "  " << format_double(Eref, 8, 4)
                            << "  " << format_double(Ept2, 8, 4) << "  " << std::setw(40)
                            << ex_type;
                    }
                    if (h != 0 || nrootpi_[0] != 1)
                        out << std::endl << "    " << std::string(width, '-');
                }
            }
            out << std::endl << "    Notes on excitation type:";
            out << std::endl << "    General format: mAH -> nAP (<r^2>) (S/D)";
            out << std::endl << "      mAH:   Mulliken symbol of m-th Active Hole orbital";
            out << std::endl << "      nAP:   Mulliken symbol of n-th Active Particle orbital";
            out << std::endl << "      <r^2>: orbital extent of the nAP orbital in a.u.";
            out << std::endl << "      S/D:   single/double excitation";
            out << std::endl
                << "    NOTE: m and n are ZERO based ACTIVE indices (NO core orbitals)!";
        }
    }
    outfile->Printf("\n\n\n%s", out.str().c_str());

    // write to file (overwrite)
    std::ofstream out_Eex;
    out_Eex.open("result_ex.txt");
    out_Eex << out.rdbuf();
    out_Eex.close();
}

std::string ACTIVE_DSRGPT2::compute_ex_type(const Determinant& det, const Determinant& ref_det) {
    psi::Dimension active = mo_space_info_->dimension("ACTIVE");
    size_t nact = mo_space_info_->size("ACTIVE");
    int nirrep = mo_space_info_->nirrep();
    std::vector<std::string> sym_active;
    for (int h = 0; h < nirrep; ++h) {
        for (int i = 0; i < active[h]; ++i) {
            sym_active.push_back(std::to_string(i) + irrep_symbol_[h]);
        }
    }

    // compare alpha occ
    std::vector<int> occA_ref(ref_det.get_alfa_occ(nact));
    std::vector<int> occA_det(det.get_alfa_occ(nact));
    std::vector<int> commonA;
    std::set_intersection(occA_ref.begin(), occA_ref.end(), occA_det.begin(), occA_det.end(),
                          back_inserter(commonA));
    occA_ref.erase(std::set_difference(occA_ref.begin(), occA_ref.end(), commonA.begin(),
                                       commonA.end(), occA_ref.begin()),
                   occA_ref.end());
    occA_det.erase(std::set_difference(occA_det.begin(), occA_det.end(), commonA.begin(),
                                       commonA.end(), occA_det.begin()),
                   occA_det.end());

    // compare beta occ
    std::vector<int> occB_ref(ref_det.get_beta_occ(nact));
    std::vector<int> occB_det(det.get_beta_occ(nact));
    std::vector<int> commonB;
    std::set_intersection(occB_ref.begin(), occB_ref.end(), occB_det.begin(), occB_det.end(),
                          back_inserter(commonB));
    occB_ref.erase(std::set_difference(occB_ref.begin(), occB_ref.end(), commonB.begin(),
                                       commonB.end(), occB_ref.begin()),
                   occB_ref.end());
    occB_det.erase(std::set_difference(occB_det.begin(), occB_det.end(), commonB.begin(),
                                       commonB.end(), occB_det.begin()),
                   occB_det.end());

    // output string
    std::string output;
    size_t A = occA_ref.size();
    size_t B = occB_ref.size();

    // same as reference
    if (A + B == 0) {
        output = "same as reference (?)";
    }

    // CIS
    if (A + B == 1) {
        int idx_ref, idx_det;
        if (A == 1) {
            idx_ref = occA_ref[0];
            idx_det = occA_det[0];
        } else {
            idx_ref = occB_ref[0];
            idx_det = occB_det[0];
        }
        double orbex_det = orb_extents_[idx_det];
        std::string r2_str =
            (orbex_det > 1.0e6 ? " (Diffuse) " : str(boost::format(" (%7.2f) ") % orbex_det));

        output = sym_active[idx_ref] + " -> " + sym_active[idx_det] + r2_str + "(S)";
    }

    // CISD
    if (A + B == 2) {
        if (A == 1 && B == 1) { // both single excitations
            int i_ref = occA_ref[0], j_ref = occB_ref[0];
            int i_det = occA_det[0], j_det = occB_det[0];
            if (i_ref == j_ref && i_det == j_det) {
                double orbex_det = orb_extents_[i_det];
                std::string r2_str =
                    (orbex_det > 1.0e6 ? " (Diffuse) "
                                       : str(boost::format(" (%7.2f) ") % orbex_det));
                output = sym_active[i_ref] + " -> " + sym_active[i_det] + r2_str + "(D)";
            } else {
                double orbex_i_det = orb_extents_[i_det];
                double orbex_j_det = orb_extents_[j_det];
                std::string r2_str_i =
                    (orbex_i_det > 1.0e6 ? " (Diffuse) "
                                         : str(boost::format(" (%7.2f)") % orbex_i_det));
                std::string r2_str_j =
                    (orbex_j_det > 1.0e6 ? " (Diffuse) "
                                         : str(boost::format(" (%7.2f)") % orbex_j_det));

                output = sym_active[i_ref] + "," + sym_active[j_ref] + " -> " + sym_active[i_det] +
                         r2_str_i + "," + sym_active[j_det] + r2_str_j;
            }
        } else {
            int i_ref, j_ref, i_det, j_det;
            if (A == 2) { // alpha double excitations
                i_ref = occA_ref[0], j_ref = occA_ref[1];
                i_det = occA_det[0], j_det = occA_det[1];
            } else { // beta double excitations
                i_ref = occB_ref[0], j_ref = occB_ref[1];
                i_det = occB_det[0], j_det = occB_det[1];
            }

            double orbex_i_det = orb_extents_[i_det];
            double orbex_j_det = orb_extents_[j_det];
            std::string r2_str_i =
                (orbex_i_det > 1.0e6 ? " (Diffuse) "
                                     : str(boost::format(" (%7.2f)") % orbex_i_det));
            std::string r2_str_j =
                (orbex_j_det > 1.0e6 ? " (Diffuse) "
                                     : str(boost::format(" (%7.2f)") % orbex_j_det));

            output = sym_active[i_ref] + "," + sym_active[j_ref] + " -> " + sym_active[i_det] +
                     r2_str_i + "," + sym_active[j_det] + r2_str_j;
        }
    }

    return output;
}

std::vector<double> ACTIVE_DSRGPT2::flatten_fci_orbextents(
    const std::vector<std::vector<std::vector<double>>>& fci_orb_extents) {
    std::vector<double> out;

    size_t nirrep = fci_orb_extents.size();
    for (size_t h = 0; h < nirrep; ++h) {
        size_t nmo = fci_orb_extents[h].size();
        for (size_t i = 0; i < nmo; ++i) {
            double r2 =
                fci_orb_extents[h][i][0] + fci_orb_extents[h][i][1] + fci_orb_extents[h][i][2];
            out.push_back(r2);
        }
    }

    return out;
}

std::string ACTIVE_DSRGPT2::format_double(const double& value, const int& width,
                                          const int& precision, const bool& scientific) {
    std::stringstream out;
    out.precision(precision);
    if (scientific) {
        out << std::fixed << std::scientific << std::setw(width) << value;
    } else {
        out << std::fixed << std::setw(width) << value;
    }
    return out.str();
}

std::map<Determinant, double>
ACTIVE_DSRGPT2::p_space_actv_to_nmo(const std::vector<Determinant>& p_space,
                                    psi::SharedVector wfn) {
    //    Determinant::reset_ints();
    std::map<Determinant, double> detsmap;

    size_t nact = mo_space_info_->size("ACTIVE");
    std::vector<size_t> core_mos = mo_space_info_->corr_absolute_mo("RESTRICTED_DOCC");
    std::vector<size_t> actv_mos = mo_space_info_->corr_absolute_mo("ACTIVE");

    for (size_t I = 0, sp = p_space.size(); I < sp; ++I) {
        double ci = wfn->get(I);
        if (fabs(ci) < 1.0e-12) {
            continue;
        }

        // find out occupation of determinant (active only)
        Determinant det_actv(p_space[I]);
        std::vector<int> occ_alfa(det_actv.get_alfa_occ(nact));
        std::vector<int> occ_beta(det_actv.get_beta_occ(nact));
        //        det_actv.print();

        // create a empty big determinant
        //        Determinant det(ncmo); <- xsize
        Determinant det;
        // fill in core orbitals
        double sign = 1.0;
        for (size_t m = 0, sc = core_mos.size(); m < sc; ++m) {
            sign *= det.create_alfa_bit(static_cast<int>(core_mos[m]));
            sign *= det.create_beta_bit(static_cast<int>(core_mos[m]));
        }

        // fill in active orbitals
        for (const auto& u : occ_alfa) {
            sign *= det.create_alfa_bit(actv_mos[u]);
        }
        for (const int& u : occ_beta) {
            sign *= det.create_beta_bit(actv_mos[u]);
        }

        // push back
        detsmap[det] = sign * ci;

        //        det.print();
        //        outfile->Printf("  %18.12f", detsmap[det]);
    }

    //    Determinant::set_ints(fci_mo_->fci_ints_);
    return detsmap;
}

std::map<Determinant, double>
ACTIVE_DSRGPT2::excited_wfn_1st(const std::map<Determinant, double>& ref, ambit::BlockedTensor& T1,
                                ambit::BlockedTensor& T2) {
    //    Determinant::reset_ints();

    std::map<Determinant, double> out;

    for (const auto& detCIpair : ref) {
        double ci = detCIpair.second;
        Determinant det(detCIpair.first);

        // singles
        T1.citerate([&](const std::vector<size_t>& i, const std::vector<SpinType>& spin,
                        const double& value) {
            // h: i[0], p: i[1]
            if (fabs(value) > 1.0e-12) {
                if (spin[0] == AlphaSpin) {
                    double sign = 1.0;
                    Determinant E(det);
                    sign *= E.destroy_alfa_bit(i[0]);
                    sign *= E.create_alfa_bit(i[1]);
                    if (sign != 0) {
                        if (out.find(E) != out.end()) {
                            out[E] += ci * value * sign;
                        } else {
                            out[E] = ci * value * sign;
                        }
                    }

                    sign = 1.0;
                    E = det;
                    sign *= E.destroy_alfa_bit(i[1]);
                    sign *= E.create_alfa_bit(i[0]);
                    if (sign != 0) {
                        if (out.find(E) != out.end()) {
                            out[E] -= ci * value * sign;
                        } else {
                            out[E] = -1.0 * ci * value * sign;
                        }
                    }
                } else {
                    double sign = 1.0;
                    Determinant E(det);
                    sign *= E.destroy_beta_bit(i[0]);
                    sign *= E.create_beta_bit(i[1]);
                    if (sign != 0) {
                        if (out.find(E) != out.end()) {
                            out[E] += ci * value * sign;
                        } else {
                            out[E] = ci * value * sign;
                        }
                    }

                    sign = 1.0;
                    E = det;
                    sign *= E.destroy_beta_bit(i[1]);
                    sign *= E.create_beta_bit(i[0]);
                    if (sign != 0) {
                        if (out.find(E) != out.end()) {
                            out[E] -= ci * value * sign;
                        } else {
                            out[E] = -1.0 * ci * value * sign;
                        }
                    }
                }
            }
        });

        // doubles
        T2.citerate([&](const std::vector<size_t>& i, const std::vector<SpinType>& spin,
                        const double& value) {
            // h: i[0], i[1]; p: i[2], i[3]
            if (fabs(value) > 1.0e-12) {
                if (spin[0] == AlphaSpin && spin[1] == AlphaSpin) {
                    // a^+ b^+ j i
                    double sign = 1.0;
                    Determinant E(det);
                    sign *= E.destroy_alfa_bit(i[0]);
                    sign *= E.destroy_alfa_bit(i[1]);
                    sign *= E.create_alfa_bit(i[3]);
                    sign *= E.create_alfa_bit(i[2]);
                    if (sign != 0) {
                        if (out.find(E) != out.end()) {
                            out[E] += 0.25 * ci * value * sign;
                        } else {
                            out[E] = 0.25 * ci * value * sign;
                        }
                    }

                    // i^+ j^+ b a
                    sign = 1.0;
                    E = det;
                    sign *= E.destroy_alfa_bit(i[2]);
                    sign *= E.destroy_alfa_bit(i[3]);
                    sign *= E.create_alfa_bit(i[1]);
                    sign *= E.create_alfa_bit(i[0]);
                    if (sign != 0) {
                        if (out.find(E) != out.end()) {
                            out[E] -= 0.25 * ci * value * sign;
                        } else {
                            out[E] = -0.25 * ci * value * sign;
                        }
                    }
                } else if (spin[0] == AlphaSpin && spin[1] == BetaSpin) {
                    // a^+ B^+ J i
                    double sign = 1.0;
                    Determinant E(det);
                    sign *= E.destroy_alfa_bit(i[0]);
                    sign *= E.destroy_beta_bit(i[1]);
                    sign *= E.create_beta_bit(i[3]);
                    sign *= E.create_alfa_bit(i[2]);
                    if (sign != 0) {
                        if (out.find(E) != out.end()) {
                            out[E] += ci * value * sign;
                        } else {
                            out[E] = ci * value * sign;
                        }
                    }

                    // i^+ J^+ B a
                    sign = 1.0;
                    E = det;
                    sign *= E.destroy_alfa_bit(i[2]);
                    sign *= E.destroy_beta_bit(i[3]);
                    sign *= E.create_beta_bit(i[1]);
                    sign *= E.create_alfa_bit(i[0]);
                    if (sign != 0) {
                        if (out.find(E) != out.end()) {
                            out[E] -= ci * value * sign;
                        } else {
                            out[E] = -1.0 * ci * value * sign;
                        }
                    }
                } else if (spin[0] == BetaSpin && spin[1] == BetaSpin) {
                    // A^+ B^+ J I
                    double sign = 1.0;
                    Determinant E(det);
                    sign *= E.destroy_beta_bit(i[0]);
                    sign *= E.destroy_beta_bit(i[1]);
                    sign *= E.create_beta_bit(i[3]);
                    sign *= E.create_beta_bit(i[2]);
                    if (sign != 0) {
                        if (out.find(E) != out.end()) {
                            out[E] += 0.25 * ci * value * sign;
                        } else {
                            out[E] = 0.25 * ci * value * sign;
                        }
                    }

                    // I^+ J^+ B A
                    sign = 1.0;
                    E = det;
                    sign *= E.destroy_beta_bit(i[2]);
                    sign *= E.destroy_beta_bit(i[3]);
                    sign *= E.create_beta_bit(i[1]);
                    sign *= E.create_beta_bit(i[0]);
                    if (sign != 0) {
                        if (out.find(E) != out.end()) {
                            out[E] -= 0.25 * ci * value * sign;
                        } else {
                            out[E] = -0.25 * ci * value * sign;
                        }
                    }
                }
            }
        });
    }

    //    outfile->Printf("\n excited 1st order wave function");
    //    for (const auto& x : out) {
    //        x.first.print();
    //        outfile->Printf("  %18.12f", x.second);
    //    }
    //    Determinant::set_ints(fci_mo_->fci_ints_);
    return out;
}

void ACTIVE_DSRGPT2::compute_osc_pt2_dets(const int& irrep, const int& root, const double& Tde_x,
                                          ambit::BlockedTensor& T1_x, ambit::BlockedTensor& T2_x) {
    /**
     * IMPORTANT NOTE:
     *   1) All blocks of T should be stored
     *   2) Number of basis function should not exceed 128
     */

    // form determinants for ground and excited states
    std::map<Determinant, double> wfn0_g =
        p_space_actv_to_nmo(p_space_g_, ref_wfns_[0]->get_column(0, 0));
    std::map<Determinant, double> wfn0_x =
        p_space_actv_to_nmo(fci_mo_->p_space(), ref_wfns_[irrep]->get_column(0, root));

    // compute transition density matrix of <Psi_x 1st| p^+ q |Psi_g 0th>

    // step 1: compute first order wavefunction of the excited state
    std::map<Determinant, double> wfn_1st = excited_wfn_1st(wfn0_x, T1_x, T2_x);
    //    std::map<Determinant, double> wfn_1st;

    // step 2: combine determinant space
    std::vector<Determinant> p_space;
    for (const auto& detCIpair : wfn0_g) {
        p_space.emplace_back(detCIpair.first);
    }
    size_t offset = p_space.size();
    for (const auto& detCIpair : wfn_1st) {
        p_space.emplace_back(detCIpair.first);
    }

    // step 3: combine eigen vectors
    size_t np = p_space.size();
    psi::SharedMatrix evecs(new psi::Matrix("combined evecs", np, 2));
    for (size_t i = 0; i < offset; ++i) {
        evecs->set(i, 0, wfn0_g[p_space[i]]);
    }
    for (size_t i = offset; i < np; ++i) {
        evecs->set(i, 1, wfn_1st[p_space[i]]);
    }

    // step 4: compute one transition density using CIRDMS
    CI_RDMS rdms(fci_mo_->fci_ints(), p_space, evecs, 0, 1);
    size_t ncmo = mo_space_info_->size("CORRELATED");
    size_t ncmo2 = ncmo * ncmo;
    std::vector<double> tdm_a(ncmo2, 0.0);
    std::vector<double> tdm_b(ncmo2, 0.0);
    rdms.compute_1rdm(tdm_a, tdm_b);

    // compute transition density matrix of <Psi_x 0th| p^+ q |Psi_g 1st>

    // step 1: compute first order wavefunction of the ground state
    //    outfile->Printf("\n Compute first order wavefunction of the ground state");
    wfn_1st = excited_wfn_1st(wfn0_g, T1_g_, T2_g_);

    // step 2: combine determinant space
    p_space.clear();
    for (const auto& detCIpair : wfn0_x) {
        p_space.emplace_back(detCIpair.first);
    }
    offset = p_space.size();
    for (const auto& detCIpair : wfn_1st) {
        p_space.emplace_back(detCIpair.first);
    }

    // step 3: combine eigen vectors
    np = p_space.size();
    evecs = std::make_shared<psi::Matrix>("combined evecs", np, 2);
    //    psi::SharedMatrix evecs(new psi::Matrix("combined evecs", np, 2));
    for (size_t i = 0; i < offset; ++i) {
        evecs->set(i, 0, wfn0_x[p_space[i]]);
    }
    for (size_t i = offset; i < np; ++i) {
        evecs->set(i, 1, wfn_1st[p_space[i]]);
    }

    // step 4: compute one transition density using CIRDMS
    CI_RDMS rdms1(fci_mo_->fci_ints(), p_space, evecs, 1, 0);
    std::vector<double> todm_a(ncmo2, 0.0);
    std::vector<double> todm_b(ncmo2, 0.0);
    rdms1.compute_1rdm(todm_a, todm_b);

    // add to previous results tdm_a and tdm_b
    std::transform(tdm_a.begin(), tdm_a.end(), todm_a.begin(), tdm_a.begin(), std::plus<double>());
    std::transform(tdm_b.begin(), tdm_b.end(), todm_b.begin(), tdm_b.begin(), std::plus<double>());

    //    // compute transition density matrix of <Psi_x 1th| p^+ q |Psi_g 1st>

    //    // step 1: compute first-order wavefunction of the excited state
    //    std::map<Determinant, double> wfn1_x = excited_wfn_1st(wfn0_x, T1_x, T2_x);

    //    // step 2: combine determinant space
    //    p_space.clear();
    //    for (const auto& detCIpair : wfn1_x) {
    //        p_space.emplace_back(detCIpair.first);
    //    }
    //    offset = p_space.size();
    //    for (const auto& detCIpair : wfn_1st) {
    //        p_space.emplace_back(detCIpair.first);
    //    }

    //    // step 3: combine eigen vectors
    //    np = p_space.size();
    //    evecs = std::make_shared<psi::Matrix>("combined evecs", np, 2));
    //    for (size_t i = 0; i < offset; ++i) {
    //        evecs->set(i, 0, wfn1_x[p_space[i]]);
    //    }
    //    for (size_t i = offset; i < np; ++i) {
    //        evecs->set(i, 1, wfn_1st[p_space[i]]);
    //    }

    //    // step 4: compute one transition density using CIRDMS
    //    CI_RDMS rdms2(fci_mo_->fci_ints_, p_space, evecs, 1, 0);
    //    rdms2.compute_1rdm(todm_a, todm_b);

    //    // step 5: add todm to tdm
    //    std::transform(tdm_a.begin(), tdm_a.end(), todm_a.begin(), tdm_a.begin(),
    //    std::plus<double>());
    //    std::transform(tdm_b.begin(), tdm_b.end(), todm_b.begin(), tdm_b.begin(),
    //    std::plus<double>());

    // translate tdm to C1 Pitzer ordering
    psi::Dimension nmopi = mo_space_info_->dimension("ALL");
    std::vector<std::tuple<double, int, int>> order;
    int nirrep = mo_space_info_->nirrep();
    for (int h = 0; h < nirrep; ++h) {
        for (int i = 0; i < nmopi[h]; ++i) {
            order.push_back(std::tuple<double, int, int>(scf_info_->epsilon_a()->get(h, i), i, h));
        }
    }
    std::sort(order.begin(), order.end(), std::less<std::tuple<double, int, int>>());

    psi::Dimension frzcpi = mo_space_info_->dimension("FROZEN_DOCC");
    psi::Dimension ncmopi = mo_space_info_->dimension("CORRELATED");
    std::vector<size_t> indices(ncmo, 0);
    for (size_t idx = 0, si = order.size(); idx < si; ++idx) {
        int i = std::get<1>(order[idx]);
        int h = std::get<2>(order[idx]);

        if (i < frzcpi[h]) {
            continue;
        } else {
            size_t offset = -frzcpi[h];
            while ((--h) >= 0) {
                offset += ncmopi[h];
            }

            indices[i + offset] = idx;
        }
    }

    size_t nmo = modipole_ints_[0]->nrow();
    psi::SharedMatrix MOtransD(new psi::Matrix("MO TransD", nmo, nmo));
    for (size_t i = 0; i < ncmo; ++i) {
        size_t ni = indices[i];
        for (size_t j = 0; j < ncmo; ++j) {
            size_t nj = indices[j];
            size_t idx = i * ncmo + j;
            MOtransD->set(ni, nj, tdm_a[idx] + tdm_b[idx]);
        }
    }
    //    MOtransD->print();

    // contract with mo dipole integrals
    Vector4 transD;
    transD.x = MOtransD->vector_dot(modipole_ints_[0]);
    transD.y = MOtransD->vector_dot(modipole_ints_[1]);
    transD.z = MOtransD->vector_dot(modipole_ints_[2]);

    // add zeroth-order transition density
    std::string name = transition_type(0, 0, root, irrep);
    double scale = 1.0 + Tde_g_ + Tde_x;
    transD.x += tdipole_ref_[name].x * scale;
    transD.y += tdipole_ref_[name].y * scale;
    transD.z += tdipole_ref_[name].z * scale;
    transD.t = sqrt(transD.x * transD.x + transD.y * transD.y + transD.z * transD.z);

    // print DSRG-PT2 transition density
    outfile->Printf("\nTrans. Dipole %s: X: %7.4f, Y: %7.4f, Z: %7.4f", name.c_str(), transD.x,
                    transD.y, transD.z);
}

std::map<Determinant, double> ACTIVE_DSRGPT2::excited_ref(const std::map<Determinant, double>& ref,
                                                          const int& p, const int& q) {
    size_t nact = mo_space_info_->size("ACTIVE");

    std::map<Determinant, double> out;

    for (const auto& detCIpair : ref) {
        double ci = detCIpair.second;
        Determinant det(detCIpair.first);

        std::vector<int> o_a = det.get_alfa_occ(nact);
        std::vector<int> o_b = det.get_beta_occ(nact);
        std::vector<int> v_a = det.get_alfa_vir(nact);
        std::vector<int> v_b = det.get_beta_vir(nact);

        if (p == q) {
            // alpha
            if (std::find(o_a.begin(), o_a.end(), q) != o_a.end()) {
                if (out.find(det) != out.end()) {
                    out[det] += ci;
                } else {
                    out[det] = ci;
                }
            }

            // beta
            if (std::find(o_b.begin(), o_b.end(), q) != o_b.end()) {
                if (out.find(det) != out.end()) {
                    out[det] += ci;
                } else {
                    out[det] = ci;
                }
            }

        } else {
            // alpha
            if (std::find(o_a.begin(), o_a.end(), q) != o_a.end() &&
                std::find(v_a.begin(), v_a.end(), p) != v_a.end()) {
                Determinant E(det);
                double sign = E.single_excitation_a(q, p);
                if (out.find(E) != out.end()) {
                    out[E] += ci * sign;
                } else {
                    out[E] = ci * sign;
                }
            }

            // beta
            if (std::find(o_b.begin(), o_b.end(), q) != o_b.end() &&
                std::find(v_b.begin(), v_b.end(), p) != v_b.end()) {
                Determinant E(det);
                double sign = E.single_excitation_b(q, p);
                if (out.find(E) != out.end()) {
                    out[E] += ci * sign;
                } else {
                    out[E] = ci * sign;
                }
            }
        }
    }
    return out;
}

void ACTIVE_DSRGPT2::compute_osc_pt2_overlap(const int& irrep, const int& root,
                                             ambit::BlockedTensor& T1_x,
                                             ambit::BlockedTensor& T2_x) {
    // form determinants for ground and excited states
    std::map<Determinant, double> wfn0_g =
        p_space_actv_to_nmo(p_space_g_, ref_wfns_[0]->get_column(0, 0));
    std::map<Determinant, double> wfn0_x =
        p_space_actv_to_nmo(fci_mo_->p_space(), ref_wfns_[irrep]->get_column(0, root));

    // compute first order wavefunctions for the ground and excited states
    std::map<Determinant, double> wfn1_g = excited_wfn_1st(wfn0_g, T1_g_, T2_g_);
    std::map<Determinant, double> wfn1_x = excited_wfn_1st(wfn0_x, T1_x, T2_x);

    // figure out C1 Pitzer ordering
    psi::Dimension nmopi = mo_space_info_->dimension("ALL");
    std::vector<std::tuple<double, int, int>> order;
    int nirrep = mo_space_info_->nirrep();
    for (int h = 0; h < nirrep; ++h) {
        for (int i = 0; i < nmopi[h]; ++i) {
            order.push_back(std::tuple<double, int, int>(scf_info_->epsilon_a()->get(h, i), i, h));
        }
    }
    std::sort(order.begin(), order.end(), std::less<std::tuple<double, int, int>>());

    size_t ncmo = mo_space_info_->size("CORRELATED");
    psi::Dimension frzcpi = mo_space_info_->dimension("FROZEN_DOCC");
    psi::Dimension ncmopi = mo_space_info_->dimension("CORRELATED");
    std::vector<size_t> indices(ncmo, 0);
    for (size_t idx = 0, si = order.size(); idx < si; ++idx) {
        int i = std::get<1>(order[idx]);
        int h = std::get<2>(order[idx]);

        if (i < frzcpi[h]) {
            continue;
        } else {
            size_t offset = -frzcpi[h];
            while ((--h) >= 0) {
                offset += ncmopi[h];
            }

            indices[i + offset] = idx;
        }
    }

    size_t nmo = modipole_ints_[0]->nrow();
    psi::SharedMatrix MOtransD(new psi::Matrix("MO TransD", nmo, nmo));
    for (size_t i = 0; i < ncmo; ++i) {
        size_t ni = indices[i];
        for (size_t j = 0; j < ncmo; ++j) {
            size_t nj = indices[j];

            // compute p^+ q |Psi_g 0th>
            std::map<Determinant, double> g0_pq = excited_ref(wfn0_g, i, j);

            // compute overlap <Psi_x 1th| p^+ q |Psi_g 0th>
            double value = compute_overlap(g0_pq, wfn1_x);

            // compute <Psi_x 0th| p^+ q
            std::map<Determinant, double> x0_qp = excited_ref(wfn0_x, j, i);

            // compute overlap <Psi_x 0th| p^+ q |Psi_g 1st>
            value += compute_overlap(x0_qp, wfn1_g);

            // set value
            MOtransD->set(ni, nj, value);
        }
    }
    MOtransD->print();

    // compute transition density matrix of <Psi_x 1st| p^+ q |Psi_g 0th>
}

double ACTIVE_DSRGPT2::compute_overlap(std::map<Determinant, double> wfn1,
                                       std::map<Determinant, double> wfn2) {
    double value = 0.0;

    for (const auto& p1 : wfn1) {
        Determinant det1(p1.first);
        if (wfn2.find(det1) != wfn2.end()) {
            value += p1.second * wfn2[det1];
        }
    }

    return value;
}
} // namespace forte

#endif
