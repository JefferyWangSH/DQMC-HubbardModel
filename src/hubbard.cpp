#include "hubbard.h"
#include "BMatrixMult.hpp"
#include "StableGreens.hpp"

/** TODO:
 *   1. cyclic sweeping (done)
 *   2. chemical potential mu (done)
 *   3. stable multiplication of ill-conditioned matrices (done)
 *   4. check wrap error of green's function (done)
 *   6. detQMC class (done)
 *   7. checkerboard decomposition (missing)
 *   8. time-displaced dynamical measurements: green_t0/0t (done)
 *   9. attractive interaction U < 0 (done)
 *   10. reweighing for doped case (done)
 *   11. ...
 */


Hubbard::Hubbard(int ll, int lt, double beta, double t, double Uint, double mu, int nwrap, bool is_checkerboard)
{
    this->ll = ll;
    this->ls = ll * ll;
    this->lt = lt;

    this->beta = beta;
    this->dtau = beta / lt;
    this->Uint = Uint;
    this->alpha = acosh(exp(0.5 * dtau * abs(Uint)));
    this->u_is_attractive = (Uint < 0.0);
    this->is_checkerboard = is_checkerboard;

    this->t = t;
    this->mu = mu;

    this->nwrap = nwrap;
    this->current_tau = 0;

    // resize matrices and svdStacks
    s.resize(ls, lt);
    green_tt_up.resize(ls, ls);
    green_tt_dn.resize(ls, ls);
    green_t0_up.resize(ls, ls);
    green_t0_dn.resize(ls, ls);
    green_0t_up.resize(ls, ls);
    green_0t_dn.resize(ls, ls);

    stackLeftU = new SvdStack(ls, lt);
    stackLeftD = new SvdStack(ls, lt);
    stackRightU = new SvdStack(ls, lt);
    stackRightD = new SvdStack(ls, lt);

    vec_green_tt_up.reserve(lt);
    vec_green_tt_dn.reserve(lt);
    vec_green_t0_up.reserve(lt);
    vec_green_t0_dn.reserve(lt);
    vec_green_0t_up.reserve(lt);
    vec_green_0t_dn.reserve(lt);

    for (int l = 0; l < lt; ++l) {
        vec_green_tt_up.emplace_back(ls, ls);
        vec_green_tt_dn.emplace_back(ls, ls);
        vec_green_t0_up.emplace_back(ls, ls);
        vec_green_t0_dn.emplace_back(ls, ls);
        vec_green_0t_up.emplace_back(ls, ls);
        vec_green_0t_dn.emplace_back(ls, ls);
    }

    // set field config to random
    init_field_to_random();

    // initialize checkerboard
    this->checkerboard.init_from_model(*this);
    this->is_checkerboard = this->checkerboard.is_checker_board();

    // initialize udv stacks for sweep use, stabilize every nwrap slices
    init_stacks(this->nwrap);

    // determine sign of current configuration
    config_sign = (green_tt_up.determinant() * green_tt_dn.determinant() >= 0)? +1.0 : -1.0;
}

void Hubbard::init_field_to_random() {
    // set field configuration to random
    assert(s.rows() == ls && s.cols() == lt);

    std::bernoulli_distribution dist(0.5);
    for(int i = 0; i < ls; ++i) {
        for(int l = 0; l < lt; ++l) {
            s(i, l) = dist(random_seed_generate)? +1.0:-1.0;
        }
    }
}

void Hubbard::metropolis_update(int l) {
    /*
     * Update the aux boson field 's' at space-time position (i,l) for all i with Metropolis
     * probability, and - if the update is accepted - perform a
     * in-place update of the green's function.
     * Record the updated green's function at the life end of function.
     */
    assert(current_tau == l);
    assert(l >= 0 && l <= lt);

    const int tau = (l == 0)? lt-1 : l-1;
    for (int i = 0; i < ls; ++i) {
        // radio of flipping aux field s(i,l)
        double p;
        if (!u_is_attractive) {
            p = (1 + (1 - green_tt_up(i, i)) * (exp(-2 * alpha * s(i, tau)) - 1))
                       * (1 + (1 - green_tt_dn(i, i)) * (exp(+2 * alpha * s(i, tau)) - 1));
        }
        else {
            p = exp(2 * alpha * s(i, tau))  * (1 + (1 - green_tt_up(i, i)) * (exp(-2 * alpha * s(i, tau)) - 1))
                       * (1 + (1 - green_tt_dn(i, i)) * (exp(-2 * alpha * s(i, tau)) - 1));
        }

        if(std::bernoulli_distribution(std::min(1.0, abs(p)))(random_seed_generate)) {
            /** reference:
             *  Quantum Monte Carlo Methods (Algorithms for Lattice Models) Determinant method
             *  Here we use the sparseness of matrix \delta */
            // update greens function (which is wrapped such that the update is at time slice 0 of g)
            // with a number of arithmetic operations proportional to N^2
            double factorU = (exp(-2 * alpha * s(i, tau)) - 1) / (1 + (1 - green_tt_up(i, i)) * (exp(-2 * alpha * s(i, tau)) - 1));
            green_tt_up -= factorU * green_tt_up.col(i) * (Eigen::VectorXd::Unit(ls, i).transpose() - green_tt_up.row(i));

            double factorD = (!u_is_attractive)? (exp(+2 * alpha * s(i, tau)) - 1) / (1 + (1 - green_tt_dn(i, i)) * (exp(+2 * alpha * s(i, tau)) - 1)) : factorU;
            green_tt_dn -= factorD * green_tt_dn.col(i) * (Eigen::VectorXd::Unit(ls, i).transpose() - green_tt_dn.row(i));

            // flip aux field
            s(i, tau) = -s(i, tau);

            // keep track of sign problem
            config_sign = (p >= 0)? +config_sign : -config_sign;
        }
    }

    // record equal-time green function of current time
    vec_green_tt_up[tau] = green_tt_up;
    vec_green_tt_dn[tau] = green_tt_dn;
}

void Hubbard::wrap_north(int l) {
    /*
     * Propagate the green's function from the current time slice l
     * upward to the time slice l+1:
     * G(l+1) = B_{l+1} G(l) B_{l+1}^{-1}
     * for both spin-1/2 state. Change green's functions in place.
     */
    assert(l >= 0 && l <= lt);

    const int tau = (l == lt)? 1 : l + 1;
    mult_B_from_left(green_tt_up, tau, +1);
    mult_invB_from_right(green_tt_up, tau, +1);
    mult_B_from_left(green_tt_dn, tau, -1);
    mult_invB_from_right(green_tt_dn, tau, -1);
}

void Hubbard::wrap_south(int l) {
    /*
     * Propagate the green's function from the current time slice l
     * downward to the time slice l-1:
     * G(l-1) = B_{l}^{-1} G(l) B_{l}
     * for both spin-1/2 state. Change green's functions in place.
     */
    assert(l >= 0 && l <= lt);

    const int tau = (l == 0)? lt : l;
    mult_B_from_right(green_tt_up, tau, +1);
    mult_invB_from_left(green_tt_up, tau, +1);
    mult_B_from_right(green_tt_dn, tau, -1);
    mult_invB_from_left(green_tt_dn, tau, -1);
}

void Hubbard::init_stacks(int is_stable) {
    /*
     *  initialize udv stacks for sweep use
     *  sweep process will start from 0 to beta, so we initialize stackRight here.
     *  stabilize the process every is_stable steps
     */
    assert(stackLeftU->empty() && stackLeftD->empty());
    assert(stackRightU->empty() && stackRightD->empty());

    Eigen::MatrixXd tmpU = Eigen::MatrixXd::Identity(ls, ls);
    Eigen::MatrixXd tmpD = Eigen::MatrixXd::Identity(ls, ls);

    // initial udv stacks for sweeping use
    for (int l = lt; l >= 1; --l) {
        mult_transB_from_left(tmpU, l, +1);
        mult_transB_from_left(tmpD, l, -1);

        // stabilize every is_stable steps with svd decomposition
        if ((l - 1) % is_stable == 0) {
            stackRightU->push(tmpU);
            stackRightD->push(tmpD);
            tmpU = Eigen::MatrixXd::Identity(ls, ls);
            tmpD = Eigen::MatrixXd::Identity(ls, ls);
        }
    }

    // initialize green function at l = 0
    compute_Green_eqtime(stackLeftU, stackRightU, green_tt_up);
    compute_Green_eqtime(stackLeftD, stackRightD, green_tt_dn);
}

void Hubbard::sweep_0_to_beta(int is_stable) {
    /*
     *  Update the space-time lattice of aux fields.
     *  For l = 1,2...,lt  flip fields and propagate green's functions
     *  Stabilize every is_stable time slices
     */
    current_tau++;

    int nlen = (lt % is_stable == 0)? lt/is_stable : lt/is_stable  +1;
    assert(current_tau == 1);
    assert(stackLeftU->empty() && stackLeftD->empty());
    assert(stackRightU->len == nlen && stackRightD->len == nlen);

    // temporary matrices
    Eigen::MatrixXd tmpU = Eigen::MatrixXd::Identity(ls, ls);
    Eigen::MatrixXd tmpD = Eigen::MatrixXd::Identity(ls, ls);

    // sweep up from 0 to beta
    for (int l = 1; l <= lt; ++l) {
        // wrap green function to current time slice l
        wrap_north(l - 1);

        // update aux field and record new greens
        metropolis_update(l);

        mult_B_from_left(tmpU, l, +1);
        mult_B_from_left(tmpD, l, -1);

        if (l % is_stable == 0 || l == lt) {
            // wrap greens function
            stackRightU->pop();
            stackRightD->pop();
            stackLeftU->push(tmpU);
            stackLeftD->push(tmpD);

            Eigen::MatrixXd tmp_green_tt_up = Eigen::MatrixXd::Zero(ls, ls);
            Eigen::MatrixXd tmp_green_tt_dn = Eigen::MatrixXd::Zero(ls, ls);
            double tmp_wrap_error_tt_up = 0.0;
            double tmp_wrap_error_tt_dn = 0.0;

            // compute fresh greens every is_stable steps: g = (1 + stackLeft * stackRight^T)^-1
            // stackLeft = B(l-1) *...* B(0)
            // stackRight = B(l)^T *...* B(L-1)^T
            compute_Green_eqtime(stackLeftU, stackRightU, tmp_green_tt_up);
            compute_Green_eqtime(stackLeftD, stackRightD, tmp_green_tt_dn);

            // calculate wrap error
            matrix_compare_error(tmp_green_tt_up, green_tt_up, tmp_wrap_error_tt_up);
            matrix_compare_error(tmp_green_tt_dn, green_tt_dn, tmp_wrap_error_tt_dn);
            max_wrap_error_equal = std::max(max_wrap_error_equal, std::max(tmp_wrap_error_tt_up, tmp_wrap_error_tt_dn));

            green_tt_up = tmp_green_tt_up;
            green_tt_dn = tmp_green_tt_dn;

            tmpU = Eigen::MatrixXd::Identity(ls, ls);
            tmpD = Eigen::MatrixXd::Identity(ls, ls);
        }

        // in the end stop at l = lt + 1
        current_tau++;
    }

    // end with fresh green functions
    vec_green_tt_up[lt-1] = green_tt_up;
    vec_green_tt_dn[lt-1] = green_tt_dn;
}

void Hubbard::sweep_beta_to_0(int is_stable) {
    /*
     *  Update the space-time lattice of aux fields.
     *  For l=lt,lt-1,...,1  flip fields and propagate green's functions
     *  Stabilize every is_stable time slices
     */
    current_tau--;

    int nlen = (lt % is_stable == 0)? lt/is_stable : lt/is_stable + 1;
    assert(current_tau == lt);
    assert(stackRightU->empty() && stackRightD->empty());
    assert(stackLeftU->len == nlen && stackLeftD->len == nlen);

    // temporary matrices
    Eigen::MatrixXd tmpU = Eigen::MatrixXd::Identity(ls, ls);
    Eigen::MatrixXd tmpD = Eigen::MatrixXd::Identity(ls, ls);

    // sweep down from beta to 0
    for (int l = lt; l >= 1; --l) {
        if (l % is_stable == 0 && l != lt) {
            // update udv stacks
            stackLeftU->pop();
            stackLeftD->pop();
            stackRightU->push(tmpU);
            stackRightD->push(tmpD);

            Eigen::MatrixXd tmp_green_tt_up = Eigen::MatrixXd::Zero(ls, ls);
            Eigen::MatrixXd tmp_green_tt_dn = Eigen::MatrixXd::Zero(ls, ls);
            double tmp_wrap_error_tt_up = 0.0;
            double tmp_wrap_error_tt_dn = 0.0;

            compute_Green_eqtime(stackLeftU, stackRightU, tmp_green_tt_up);
            compute_Green_eqtime(stackLeftD, stackRightD, tmp_green_tt_dn);

            // calculate wrap error
            matrix_compare_error(tmp_green_tt_up, green_tt_up, tmp_wrap_error_tt_up);
            matrix_compare_error(tmp_green_tt_dn, green_tt_dn, tmp_wrap_error_tt_dn);
            max_wrap_error_equal = std::max(max_wrap_error_equal, std::max(tmp_wrap_error_tt_up, tmp_wrap_error_tt_dn));

            green_tt_up = tmp_green_tt_up;
            green_tt_dn = tmp_green_tt_dn;

            tmpU = Eigen::MatrixXd::Identity(ls, ls);
            tmpD = Eigen::MatrixXd::Identity(ls, ls);
        }

        // update aux field and record new greens
        metropolis_update(l);

        mult_transB_from_left(tmpU, l, +1);
        mult_transB_from_left(tmpD, l, -1);

        wrap_south(l);

        current_tau--;
    }

    // at l = 0
    stackLeftU->pop();
    stackLeftD->pop();
    stackRightU->push(tmpU);
    stackRightD->push(tmpD);

    compute_Green_eqtime(stackLeftU, stackRightU, green_tt_up);
    compute_Green_eqtime(stackLeftD, stackRightD, green_tt_dn);

    // end with fresh green functions
    vec_green_tt_up[lt-1] = green_tt_up;
    vec_green_tt_dn[lt-1] = green_tt_dn;
}

void Hubbard::sweep_0_to_beta_displaced(int is_stable) {
    /*
     *  Calculate time-displaced green function, while the aux field remains unchanged.
     *  For l = 1,2...,lt, recompute SvdStacks every is_stable time slices.
     *  Data is to be stored in vec_green_t0/0t_up and vec_green_t0/0t_dn.
     */
    current_tau++;

    int nlen = (lt % is_stable == 0)? lt/is_stable : lt/is_stable + 1;
    assert(current_tau == 1);
    assert(stackLeftU->empty() && stackLeftD->empty());
    assert(stackRightU->len == nlen && stackRightD->len == nlen);

    // initialize: at l = 0, gt0 = g00, g0t = g00 - 1
    green_t0_up = green_tt_up;
    green_t0_dn = green_tt_dn;
    green_0t_up = green_tt_up - Eigen::MatrixXd::Identity(ls, ls);
    green_0t_dn = green_tt_dn - Eigen::MatrixXd::Identity(ls, ls);

    // temporary matrices
    Eigen::MatrixXd tmpU = Eigen::MatrixXd::Identity(ls, ls);
    Eigen::MatrixXd tmpD = Eigen::MatrixXd::Identity(ls, ls);

    // sweep up from 0 to beta
    for (int l = 1; l <= lt; ++l) {
        
        // calculate and record time-displaced green functions at different time slices
        mult_B_from_left(green_t0_up, l, +1);
        mult_B_from_left(green_t0_dn, l, -1);
        vec_green_t0_up[l-1] = green_t0_up;
        vec_green_t0_dn[l-1] = green_t0_dn;

        mult_invB_from_right(green_0t_up, l, +1);
        mult_invB_from_right(green_0t_dn, l, -1);
        vec_green_0t_up[l-1] = green_0t_up;
        vec_green_0t_dn[l-1] = green_0t_dn;

        mult_B_from_left(tmpU, l, +1);
        mult_B_from_left(tmpD, l, -1);

        if (l % is_stable == 0 || l == lt) {
            // wrap greens function
            stackRightU->pop();
            stackRightD->pop();
            stackLeftU->push(tmpU);
            stackLeftD->push(tmpD);

            Eigen::MatrixXd tmp_green_t0_up = Eigen::MatrixXd::Zero(ls, ls);
            Eigen::MatrixXd tmp_green_t0_dn = Eigen::MatrixXd::Zero(ls, ls);
            Eigen::MatrixXd tmp_green_0t_up = Eigen::MatrixXd::Zero(ls, ls);
            Eigen::MatrixXd tmp_green_0t_dn = Eigen::MatrixXd::Zero(ls, ls);
            double tmp_wrap_error_t0_up = 0.0;
            double tmp_wrap_error_t0_dn = 0.0;
            double tmp_wrap_error_0t_up = 0.0;
            double tmp_wrap_error_0t_dn = 0.0;

            // compute fresh greens every is_stable steps
            // stackLeft = B(l-1) *...* B(0)
            // stackRight = B(l)^T *...* B(L-1)^T
            compute_Green_displaced(stackLeftU, stackRightU, tmp_green_t0_up, tmp_green_0t_up);
            compute_Green_displaced(stackLeftD, stackRightD, tmp_green_t0_dn, tmp_green_0t_dn);

            // calculate wrap error
            matrix_compare_error(tmp_green_t0_up, green_t0_up, tmp_wrap_error_t0_up);
            matrix_compare_error(tmp_green_t0_dn, green_t0_dn, tmp_wrap_error_t0_dn);
            max_wrap_error_displaced = std::max(max_wrap_error_displaced, std::max(tmp_wrap_error_t0_up, tmp_wrap_error_t0_dn));

            matrix_compare_error(tmp_green_0t_up, green_0t_up, tmp_wrap_error_0t_up);
            matrix_compare_error(tmp_green_t0_dn, green_t0_dn, tmp_wrap_error_t0_dn);
            max_wrap_error_displaced = std::max(max_wrap_error_displaced, std::max(tmp_wrap_error_0t_up, tmp_wrap_error_0t_dn));

            green_t0_up = tmp_green_t0_up;
            green_t0_dn = tmp_green_t0_dn;
            green_0t_up = tmp_green_0t_up;
            green_0t_dn = tmp_green_0t_dn;

            vec_green_t0_up[l-1] = green_t0_up;
            vec_green_t0_dn[l-1] = green_t0_dn;
            vec_green_0t_up[l-1] = green_0t_up;
            vec_green_0t_dn[l-1] = green_0t_dn;

            tmpU = Eigen::MatrixXd::Identity(ls, ls);
            tmpD = Eigen::MatrixXd::Identity(ls, ls);
        }

        // in the end stop at l = lt + 1
        current_tau++;
    }
}
