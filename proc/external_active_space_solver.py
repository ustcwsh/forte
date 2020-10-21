#!/usr/bin/env python
# -*- coding: utf-8 -*-

import json
import psi4
import forte
import numpy as np

class NumpyEncoder(json.JSONEncoder):
    def default(self, obj):
        if isinstance(obj, np.ndarray):
            return obj.tolist()
        return json.JSONEncoder.default(self, obj)

def write_wavefunction(ref_wfn):
    Ca = ref_wfn.Ca().to_array()

    with open('coeff.json', 'w+') as f:
        json.dump({'Ca': Ca}, f, cls=NumpyEncoder)

def read_wavefunction(ref_wfn):
    with open('coeff.json') as f:
        data = json.load(f)
        #print(C_list)
        C_read = data["Ca"]
#        C_list = np.asarray(C_read)

        C_list = []
        for i in range(len(C_read)):
            if not C_read[i]:
                C_list.append(None)
            else:
                C_list.append(np.asarray(C_read[i]))
        
        print(C_list)
        C_mat = psi4.core.Matrix.from_array(C_list)
        C_mat.print_out()
        ref_wfn.Ca().copy(C_mat)
        ref_wfn.Cb().copy(C_mat)

def write_external_active_space_file(as_ints, state_map, json_file="forte_ints.json"):
    for state,nroots in state_map.items():
        file = {}

        nmo = as_ints.nmo()

        file['state_symmetry'] = {"data" : state.irrep(), "description" : "Symmetry of the state"}

        file['na'] = {"data" : state.na(), "description" : "number of alpha electrons"}

        file['nb'] = {"data" : state.nb(), "description" : "number of beta electrons"}

        file['nso'] = {"data" : 2 * nmo, "description" : "number of spin orbitals"}

        file['symmetry'] = {"data" : [i for i in as_ints.mo_symmetry() for j in range(2)],
                            "description" :"symmetry of each spin orbital (Cotton ordering)"}

        file['spin'] = {"data" : [j for i in range(nmo) for j in range(2)],
                            "description" :"spin of each spin orbital (0 = alpha, 1 = beta)"}

        scalar_energy = as_ints.frozen_core_energy() + as_ints.scalar_energy() + as_ints.nuclear_repulsion_energy()
        file['scalar_energy'] = {"data" : scalar_energy, "description" :"scalar energy (sum of nuclear repulsion, frozen core, and scalar contributions"}

        oei_a = [(i * 2,j * 2,as_ints.oei_a(i,j)) for i in range(nmo) for j in range(nmo)]
        oei_b = [(i * 2 + 1,j * 2 + 1,as_ints.oei_b(i,j)) for i in range(nmo) for j in range(nmo)]

        file['oei'] = {"data" : oei_a + oei_b,
                        "description" : "one-electron integrals as a list of tuples (i,j,<i|h|j>)"}

        tei = []
        for i in range(nmo):
            for j in range(nmo):
                for k in range(nmo):
                    for l in range(nmo):
                        tei.append((i * 2,j * 2,k * 2,l * 2,as_ints.tei_aa(i,j,k,l))) # aaaa
                        tei.append((i * 2,j * 2 + 1,k * 2,l * 2 + 1,+as_ints.tei_ab(i,j,k,l))) # abab
                        tei.append((i * 2,j * 2 + 1,l * 2 + 1,k * 2,-as_ints.tei_ab(i,j,k,l))) # abba
                        tei.append((j * 2 + 1,i * 2,k * 2,l * 2 + 1,-as_ints.tei_ab(i,j,k,l))) # baab
                        tei.append((j * 2 + 1,i * 2,l * 2 + 1,k * 2,+as_ints.tei_ab(i,j,k,l))) # baba
                        tei.append((i * 2 + 1,j * 2 + 1,k * 2+ 1,l * 2 + 1,as_ints.tei_bb(i,j,k,l))) # bbbb

        file['tei'] = {"data" : tei,
                        "description" : "antisymmetrized two-electron integrals as a list of tuples (i,j,k,l,<ij||kl>)"}

        with open(json_file, 'w+') as f:
            json.dump(file, f, sort_keys=True, indent=2)

#        make_hamiltonian(as_ints,state)

def make_hamiltonian(as_ints,state):
    import itertools

    dets = []

    na = state.na()
    nb = state.nb()

    orbs = [i for i in range(as_ints.nmo())]

    # generate all the alpha strings
    for astr in itertools.combinations(orbs, na):
        # generate all the beta strings
        for bstr in itertools.combinations(orbs, nb):
            d = forte.Determinant()
            for i in astr: d.set_alfa_bit(i, True)
            for i in bstr: d.set_beta_bit(i, True)
            dets.append(d)

    print(f'==> List of FCI determinants <==')
    for d in dets:
        print(f'{d.str(4)}')

    import numpy as np

    ndets = len(dets)
    H = np.ndarray((ndets,ndets))
    for I, detI in enumerate(dets):
        for J, detJ in enumerate(dets):
            H[I][J] = as_ints.slater_rules(detI,detJ)

    print(H)
    evals, evecs = np.linalg.eigh(H)


    print(f'FCI Energy = {evals[0] + as_ints.scalar_energy() + as_ints.nuclear_repulsion_energy()}')


def write_external_rdm_file(active_space_solver, state_weights_map, max_rdm_level):
    rdm = active_space_solver.compute_average_rdms(state_weights_map, max_rdm_level)

    g1a = rdm.g1a()
    g1b = rdm.g1b()

    g2aa = rdm.g2aa()
    g2ab = rdm.g2ab()
    g2bb = rdm.g2bb()

    nact = g1a.shape[0]

    gamma1_a = [(i * 2,j * 2,g1a[i][j]) for i in range(nact) for j in range(nact)]
    gamma1_b = [(i * 2 + 1,j * 2 + 1,g1b[i][j]) for i in range(nact) for j in range(nact)]

    file = {}

    state_energies_map = active_space_solver.state_energies_map()
    for state,energies in state_energies_map.items():
        file['energy'] = {"data" : energies[0], "description" : "energy"}

    file['gamma1'] = {"data" : gamma1_a + gamma1_b,
                      "description" : "one-body density matrix as a list of tuples (i,j,<i^ j>)"}

    gamma2 = []
    for i in range(nact):
        for j in range(nact):
            for k in range(nact):
                for l in range(nact):
                    gamma2.append((i * 2,j * 2,k * 2,l * 2,g2aa[i,j,k,l])) # aaaa
                    gamma2.append((i * 2,j * 2 + 1,k * 2,l * 2 + 1,+g2ab[i,j,k,l])) # abab
                    gamma2.append((i * 2,j * 2 + 1,l * 2 + 1,k * 2,-g2ab[i,j,k,l])) # abba
                    gamma2.append((j * 2 + 1,i * 2,k * 2,l * 2 + 1,-g2ab[i,j,k,l])) # baab
                    gamma2.append((j * 2 + 1,i * 2,l * 2 + 1,k * 2,+g2ab[i,j,k,l])) # baba
                    gamma2.append((i * 2 + 1,j * 2 + 1,k * 2+ 1,l * 2 + 1,g2bb[i,j,k,l])) # bbbb

    file['gamma2'] = {"data" : gamma2,
                      "description" : "two-body density matrix as a list of tuples (i,j,k,l,<i^ j^ l k>)"}

    if max_rdm_level == 3:
        g3aaa = rdm.g3aaa()
        g3aab = rdm.g3aab()
        g3abb = rdm.g3abb()
        g3bbb = rdm.g3bbb()
        gamma3 = []
        for i in range(nact):
            for j in range(nact):
                for k in range(nact):
                    for l in range(nact):
                        for m in range(nact):
                            for n in range(nact):
                                # aaa case
                                gamma3.append((i * 2, j * 2, k * 2, l * 2, m * 2, n * 2, g3aaa[i,j,k,l,m,n]))

                                # aab case
                                gamma3.append((i * 2, j * 2, k * 2 + 1, l * 2, m * 2, n * 2 + 1, g3aab[i,j,k,l,m,n]))
                                gamma3.append((i * 2, j * 2, k * 2 + 1, l * 2, n * 2 + 1, m * 2, -g3aab[i,j,k,l,m,n]))
                                gamma3.append((i * 2, j * 2, k * 2 + 1, n * 2 + 1, l * 2, m * 2, g3aab[i,j,k,l,m,n]))

                                gamma3.append((i * 2, k * 2 + 1, j * 2, l * 2, m * 2, n * 2 + 1, -g3aab[i,j,k,l,m,n]))
                                gamma3.append((i * 2, k * 2 + 1, j * 2, l * 2, n * 2 + 1, m * 2, g3aab[i,j,k,l,m,n]))
                                gamma3.append((i * 2, k * 2 + 1, j * 2, n * 2 + 1, l * 2, m * 2, -g3aab[i,j,k,l,m,n]))

                                gamma3.append((k * 2 + 1, i * 2, j * 2, l * 2, m * 2, n * 2 + 1, g3aab[i,j,k,l,m,n]))
                                gamma3.append((k * 2 + 1, i * 2, j * 2, l * 2, n * 2 + 1, m * 2, -g3aab[i,j,k,l,m,n]))
                                gamma3.append((k * 2 + 1, i * 2, j * 2, n * 2 + 1, l * 2, m * 2, g3aab[i,j,k,l,m,n]))

                                # abb case
                                gamma3.append((i * 2, j * 2 + 1, k * 2 + 1, l * 2, m * 2 + 1, n * 2 + 1, g3abb[i,j,k,l,m,n]))
                                gamma3.append((i * 2, j * 2 + 1, k * 2 + 1, m * 2 + 1, l * 2, n * 2 + 1, -g3abb[i,j,k,l,m,n]))
                                gamma3.append((i * 2, j * 2 + 1, k * 2 + 1, m * 2 + 1, n * 2 + 1, l * 2, g3abb[i,j,k,l,m,n]))

                                gamma3.append((j * 2 + 1, i * 2, k * 2 + 1, l * 2, m * 2 + 1, n * 2 + 1, -g3abb[i,j,k,l,m,n]))
                                gamma3.append((j * 2 + 1, i * 2, k * 2 + 1, m * 2 + 1, l * 2, n * 2 + 1, g3abb[i,j,k,l,m,n]))
                                gamma3.append((j * 2 + 1, i * 2, k * 2 + 1, m * 2 + 1, n * 2 + 1, l * 2, -g3abb[i,j,k,l,m,n]))

                                gamma3.append((j * 2 + 1, k * 2 + 1, i * 2, l * 2, m * 2 + 1, n * 2 + 1, g3abb[i,j,k,l,m,n]))
                                gamma3.append((j * 2 + 1, k * 2 + 1, i * 2, m * 2 + 1, l * 2, n * 2 + 1, -g3abb[i,j,k,l,m,n]))
                                gamma3.append((j * 2 + 1, k * 2 + 1, i * 2, m * 2 + 1, n * 2 + 1, l * 2, g3abb[i,j,k,l,m,n]))

                                # bbb case
                                gamma3.append((i * 2 + 1, j * 2 + 1, k * 2 + 1, l * 2 + 1, m * 2 + 1, n * 2 + 1, g3bbb[i,j,k,l,m,n]))

        file['gamma3'] = {"data" : gamma3,
                          "description" : "three-body density matrix as a list of tuples (i,j,k,l,m,n <i^ j^ k^ n m l>)"}

    with open('rdms.json','w+') as f:
        json.dump(file,f, sort_keys=True, indent=2)

#import functools

#dets = []
#orbs = range(nact)

## get the symmetry of each active orbital
#act_sym = mo_space_info.symmetry('ACTIVE')

#nact_ael = sum(nact_aelpi)
#nact_bel = sum(nact_belpi)
#print(f'Number of alpha electrons: {nact_ael}')
#print(f'Number of beta electrons:  {nact_bel}')





## or we could use the more fancy looping below that avoid computing half of the matrix elements
## for I, detI in enumerate(dets):
##     H[I][I] = as_ints.slater_rules(detI,detI) # diagonal term
##     for J, detJ in enumerate(dets[:I]):
##         HIJ = as_ints.slater_rules(detI,detJ) # off-diagonal term (only upper half)
##         H[I][J] = H[J][I] = HIJ

#print(H)
#evals, evecs = np.linalg.eigh(H)

#psi4_fci = -74.846380133240530
#print(f'FCI Energy = {evals[0] + as_ints.scalar_energy() + as_ints.nuclear_repulsion_energy()}')
#print(f'FCI Energy Error = {evals[0] + as_ints.scalar_energy() + as_ints.nuclear_repulsion_energy()- psi4_fci}')

#index_hf = dets.index(ref)
#print(f'Index of the HF determinant in the FCI vector {index_hf}')



#file = {
#    "hamiltonian" : {"data" : [(0,0,0.0)],
#                     "description" : "matrix elements of the Hamiltonian as a list of tuples (I,J,<I|H|J>)"},
#    "determinants" : {"data" : [(1,1,0,0),(0,0,1,1)],
#                     "description" : "the basis of determinants (I) in occupation number representation (I = i_1 i_2 ... i_nso)"},
#}

def read_external_active_space_file(as_ints, state_map):
    return False
