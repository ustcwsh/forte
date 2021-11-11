#ifndef _dynamic_correlation_solver_h_
#define _dynamic_correlation_solver_h_

#include <memory>

#include "integrals/integrals.h"
#include "integrals/active_space_integrals.h"
#include "base_classes/rdms.h"
#include "base_classes/forte_options.h"
#include "base_classes/coupling_coefficients.h"

namespace forte {

class SCFInfo;

class DynamicCorrelationSolver {
  public:
    /**
     * Constructor
     * @param ref_wfn The reference wavefunction object
     * @param options The main options object
     * @param ints A pointer to an allocated integral object
     * @param mo_space_info The MOSpaceInfo object
     */
    DynamicCorrelationSolver(RDMs rdms, std::shared_ptr<SCFInfo> scf_info,
                             std::shared_ptr<ForteOptions> options,
                             std::shared_ptr<ForteIntegrals> ints,
                             std::shared_ptr<MOSpaceInfo> mo_space_info);
    /// Compute energy
    virtual double compute_energy() = 0;

    /// Compute dressed Hamiltonian
    virtual std::shared_ptr<ActiveSpaceIntegrals> compute_Heff_actv() = 0;

    /// Destructor
    virtual ~DynamicCorrelationSolver() = default;

    /// Set whether to read amplitudes or not manually
    void set_read_amps_cwd(bool read) { read_amps_cwd_ = read; }

    /// Clean up amplitudes checkpoint files
    void clean_checkpoints();

    /// Set coupling coefficients
    /// TODO: remove this when implemented more efficient way of computing CI response
    virtual void set_coupling_coefficients(const CICouplingCoefficients& cc) {
        coupling_coefficients_ = cc;
    }

    /// Set CI coefficients
    /// TODO: remove this when implemented more efficient way of computing CI response
    virtual void set_ci_vectors(const std::vector<ambit::Tensor>& ci_vectors) {
        ci_vectors_ = ci_vectors;
    }

  protected:
    /// The molecular integrals
    std::shared_ptr<ForteIntegrals> ints_;

    /// The MO space info
    std::shared_ptr<MOSpaceInfo> mo_space_info_;

    /// The RDMs and cumulants of the reference wave function
    RDMs rdms_;

    /// The SCFInfo
    std::shared_ptr<SCFInfo> scf_info_;

    /// The ForteOptions
    std::shared_ptr<ForteOptions> foptions_;

    /// The coupling coefficients
    CICouplingCoefficients coupling_coefficients_;

    /// The CI coefficients
    std::vector<ambit::Tensor> ci_vectors_;

    /// Common settings
    void startup();

    /// Nuclear repulsion energy
    double Enuc_;

    /// Frozen core energy
    double Efrzc_;

    /// Printing level
    int print_;

    /// The integral type
    std::string ints_type_;
    /// If ERI density fitted or Cholesky decomposed
    bool eri_df_;

    // ==> DIIS control <==

    /// Cycle number to start DIIS
    int diis_start_;
    /// Minimum number of DIIS vectors
    int diis_min_vec_;
    /// Maximum number of DIIS vectors
    int diis_max_vec_;
    /// Frequency of extrapolating the current DIIS vectors
    int diis_freq_;

    // ==> amplitudes file names <==

    /// Checkpoint file for T1 amplitudes
    std::string t1_file_chk_;
    /// Checkpoint file for T2 amplitudes
    std::string t2_file_chk_;

    /// File name for T1 amplitudes to be saved in current directory
    std::string t1_file_cwd_;
    /// File name for T2 amplitudes to be saved in current directory
    std::string t2_file_cwd_;

    /// Dump amplitudes to current directory
    bool dump_amps_cwd_ = false;
    /// Read amplitudes from current directory
    bool read_amps_cwd_ = false;

    /// Dump the converged amplitudes to disk
    /// Iterative methods should override this function
    virtual void dump_amps_to_disk() {}
};

std::shared_ptr<DynamicCorrelationSolver>
make_dynamic_correlation_solver(const std::string& type, std::shared_ptr<ForteOptions> options,
                                std::shared_ptr<ForteIntegrals> ints,
                                std::shared_ptr<MOSpaceInfo> mo_space_info);

} // namespace forte

#endif // DYNAMIC_CORRELATION_SOLVER_H
