#include "eqtimeMeasure.h"
#include "hubbard.h"

void measure::eqtimeMeasure::resize(const int &nbin) {
    this->nbin = nbin;
}

void measure::eqtimeMeasure::initial() {
    obs_bin_eqtime["DoubleOccu"].reserve(nbin);
    obs_bin_eqtime["KineticEnergy"].reserve(nbin);
    obs_bin_eqtime["StructFactor"].reserve(nbin);
    obs_bin_eqtime["MomentumDist"].reserve(nbin);
    obs_bin_eqtime["localSpinCorr"].reserve(nbin);
    obs_bin_eqtime["AverageSign"].reserve(nbin);
}

void measure::eqtimeMeasure::clear() {
    n_equal_time = 0;
    DoubleOccu = 0.0;
    KineticEnergy = 0.0;
    StructFactor = 0.0;
    MomentumDist = 0.0;
    localSpinCorr = 0.0;
    AverageSign = 0.0;
}

void measure::eqtimeMeasure::meas_Double_Occu(const Hubbard &hubbard, const int &t) {
    assert( t >= 0 && t < hubbard.lt);
    const Eigen::MatrixXd gu = hubbard.vec_green_tt_up[t];
    const Eigen::MatrixXd gd = hubbard.vec_green_tt_dn[t];

    for (int i = 0; i < hubbard.ls; ++i) {
        const double doubleoccu = (1 - gu(i,i)) * (1 - gd(i,i));
        DoubleOccu += hubbard.config_sign * doubleoccu;
    }
}

void measure::eqtimeMeasure::meas_Kinetic_Energy(const Hubbard &hubbard, const int &t) {
    assert( t >= 0 && t < hubbard.lt);
    const int ll = hubbard.ll;
    const Eigen::MatrixXd gu = hubbard.vec_green_tt_up[t];
    const Eigen::MatrixXd gd = hubbard.vec_green_tt_dn[t];

    for (int x = 0; x < ll; ++x) {
        for (int y = 0; y < ll; ++y) {
            const double kinetic = 2 * hubbard.t * (gu(x + ll*y, ((x+1)%ll) + ll*y) + gu(x + ll*y, x + ll*((y+1)%ll)))
                                   + 2 * hubbard.t * (gd(x + ll*y, ((x+1)%ll) + ll*y) + gd(x + ll*y, x + ll*((y+1)%ll)));
            KineticEnergy += hubbard.config_sign * kinetic;
        }
    }
}

void measure::eqtimeMeasure::meas_Momentum_Dist(const Hubbard &hubbard, const int &t, const Eigen::VectorXd &p) {
    assert( t >= 0 && t < hubbard.lt);
    const int ll = hubbard.ll;
    const Eigen::MatrixXd gu = hubbard.vec_green_tt_up[t];
    const Eigen::MatrixXd gd = hubbard.vec_green_tt_dn[t];
    double tmpfourier = 0.0;

    for (int xi = 0; xi < ll; ++xi) {
        for (int yi = 0; yi < ll; ++yi) {
            for (int xj = 0; xj < ll; ++xj) {
                for (int yj = 0; yj < ll; ++yj) {
                    const int i = xi + ll * yi;
                    const int j = xj + ll * yj;
                    const Eigen::VectorXd r = (Eigen::VectorXd(2) << (xi-xj), (yi-yj)).finished();
                    tmpfourier += cos(-r.dot(p)) * (gu(j, i) + gd(j, i));
                }
            }
        }
    }
    MomentumDist += hubbard.config_sign * (1 - 0.5 * tmpfourier / hubbard.ls);
}

void measure::eqtimeMeasure::meas_local_Spin_Corr(const Hubbard &hubbard, const int &t) {
    assert( t >= 0 && t < hubbard.lt);
    const int ls = hubbard.ls;
    const Eigen::MatrixXd gu = hubbard.vec_green_tt_up[t];
    const Eigen::MatrixXd gd = hubbard.vec_green_tt_dn[t];
    double  onsitecorr = 0.0;

    for (int i = 0; i < ls; ++i) {
        onsitecorr += gu(i, i) + gd(i, i) - 2 * gu(i, i) * gd(i, i);
    }
    localSpinCorr += hubbard.config_sign * onsitecorr / ls;
}

void measure::eqtimeMeasure::meas_Struct_Factor(const Hubbard &hubbard, const int &t, const Eigen::VectorXd &p) {
    assert( t >= 0 && t < hubbard.lt);
    const int ll = hubbard.ll;
    const int ls = hubbard.ls;
    const Eigen::MatrixXd gu = hubbard.vec_green_tt_up[t];
    const Eigen::MatrixXd gd = hubbard.vec_green_tt_dn[t];

    /**  gu(i,j) = < c_i c^+_j >
     *  guc(i,j) = < c^+_i c_j > */
    Eigen::MatrixXd guc = Eigen::MatrixXd::Identity(ls, ls);
    Eigen::MatrixXd gdc = Eigen::MatrixXd::Identity(ls, ls);

    // get guc and gdc
    for (int i = 0; i < ls; ++i) {
        for (int j = 0; j < ls; ++j) {
            guc(j, i) = - gu(i, j);
            gdc(j, i) = - gd(i, j);
        }
        guc(i, i)++;
        gdc(i, i)++;
    }

    // loop for site i, j
    for (int xi = 0; xi < ll; ++xi) {
        for (int yi = 0; yi < ll; ++yi) {
            for (int xj = 0; xj < ll; ++xj) {
                for (int yj = 0; yj < ll; ++yj) {
                    const int i = xi + ll * yi;
                    const int j = xj + ll * yj;
                    const Eigen::VectorXd r = (Eigen::VectorXd(2) << (xi-xj), (yi-yj)).finished();
                    const double factor = cos(-r.dot(p));
                    /** factor 4 comes from spin 1/2 */
                    const double structfactor = factor / 4 * (
                            + guc(i, i) * guc(j, j) + guc(i, j) * gu(i, j)
                            + gdc(i, i) * gdc(j, j) + gdc(i, j) * gd(i, j)
                            - gdc(i, i) * guc(j, j) - guc(i, i) * gdc(j, j)
                            );
                    StructFactor += hubbard.config_sign * structfactor;
                }
            }
        }
    }
}

void measure::eqtimeMeasure::measure_equal_time(const Hubbard &hubbard) {
    for (int t = 0; t < hubbard.lt; ++t) {
        meas_Double_Occu(hubbard, t);
        meas_Kinetic_Energy(hubbard, t);
        meas_Struct_Factor(hubbard, t, q);
        meas_Momentum_Dist(hubbard, t, q);
        meas_local_Spin_Corr(hubbard, t);
    }
    AverageSign += hubbard.config_sign;
    n_equal_time++;
}

void measure::eqtimeMeasure::normalizeStats(const Hubbard &hubbard) {
    AverageSign /= n_equal_time;
    DoubleOccu /= hubbard.ls * hubbard.lt * n_equal_time * AverageSign;
    KineticEnergy /= hubbard.ls * hubbard.lt * n_equal_time * AverageSign;
    StructFactor /= hubbard.ls * hubbard.ls * hubbard.lt * n_equal_time * AverageSign;
    MomentumDist /= hubbard.lt * n_equal_time * AverageSign;
    localSpinCorr /= hubbard.lt * n_equal_time * AverageSign;
}

void measure::eqtimeMeasure::write_Stats_to_bins(int bin) {
    obs_bin_eqtime["DoubleOccu"][bin] = DoubleOccu;
    obs_bin_eqtime["KineticEnergy"][bin] = KineticEnergy;
    obs_bin_eqtime["StructFactor"][bin] = StructFactor;
    obs_bin_eqtime["MomentumDist"][bin] = MomentumDist;
    obs_bin_eqtime["localSpinCorr"][bin] = localSpinCorr;
    obs_bin_eqtime["AverageSign"][bin] = AverageSign;
}

void measure::eqtimeMeasure::analyse_equal_time_Stats(const std::string &obs) {
    assert(obs_bin_eqtime.count(obs) == 1);

    // clear data of previous statistics
    obs_mean_eqtime[obs] = 0;
    obs_err_eqtime[obs] = 0;

    for (int bin = 0; bin < nbin; ++bin) {
        obs_mean_eqtime[obs] += obs_bin_eqtime[obs][bin];
        obs_err_eqtime[obs] += pow(obs_bin_eqtime[obs][bin], 2);
    }

    obs_mean_eqtime[obs] /= nbin;
    obs_err_eqtime[obs] /= nbin;
    obs_err_eqtime[obs] = pow(obs_err_eqtime[obs] - pow(obs_mean_eqtime[obs], 2), 0.5) / pow(nbin - 1, 0.5);
}

void measure::eqtimeMeasure::analyseStats() {
    analyse_equal_time_Stats("DoubleOccu");
    analyse_equal_time_Stats("KineticEnergy");
    analyse_equal_time_Stats("StructFactor");
    analyse_equal_time_Stats("MomentumDist");
    analyse_equal_time_Stats("localSpinCorr");
    analyse_equal_time_Stats("AverageSign");
}
