/*
 * Copyright Elasticsearch B.V. and/or licensed to Elasticsearch B.V. under one
 * or more contributor license agreements. Licensed under the Elastic License
 * 2.0 and the following additional limitation. Functionality enabled by the
 * files subject to the Elastic License 2.0 may only be used in production when
 * invoked by an Elasticsearch process with a license key installed that permits
 * use of machine learning features. You may not use this file except in
 * compliance with the Elastic License 2.0 and the foregoing additional
 * limitation.
 */

#include <maths/common/CGradientDescent.h>

#include <core/CLogger.h>

#include <maths/common/CBasicStatistics.h>
#include <maths/common/CLinearAlgebraShims.h>
#include <maths/common/CLinearAlgebraTools.h>

namespace ml {
namespace maths {
namespace common {
CGradientDescent::CGradientDescent(double learnRate, double momentum)
    : m_LearnRate(learnRate), m_Momentum(momentum) {
}

void CGradientDescent::learnRate(double learnRate) {
    m_LearnRate = learnRate;
}

void CGradientDescent::momentum(double momentum) {
    m_Momentum = momentum;
}

bool CGradientDescent::run(std::size_t n,
                           const TVector& x0,
                           const CFunction& f,
                           const CGradient& gf,
                           TVector& xBest,
                           TDoubleVec& fi) {
    fi.clear();
    fi.reserve(n);

    m_PreviousStep = las::zero(x0);
    TVector x(x0);
    TVector gfx(las::zero(x));

    CBasicStatistics::COrderStatisticsStack<double, 1> min;
    CBasicStatistics::SSampleMean<double>::TAccumulator scale;

    for (std::size_t i = 0; i < n; ++i) {
        double fx;
        if (!f(x, fx)) {
            LOG_ERROR(<< "Bailing on iteration " << i);
            return false;
        }

        if (min.add(fx)) {
            xBest = x;
        }
        fi.push_back(fx);

        if (!gf(x, gfx)) {
            LOG_ERROR(<< "Bailing on iteration " << i);
            return false;
        }
        double norm = las::norm(gfx);
        scale.add(norm);
        gfx *= (-m_LearnRate / CBasicStatistics::mean(scale));

        m_PreviousStep *= m_Momentum;
        m_PreviousStep += gfx;
        LOG_TRACE(<< "gradient fx = " << gfx << ", step = " << m_PreviousStep);

        x += m_PreviousStep;
    }

    LOG_TRACE(<< "fi = " << fi);
    return true;
}

CGradientDescent::CEmpiricalCentralGradient::CEmpiricalCentralGradient(const CFunction& f,
                                                                       double eps)
    : m_Eps(eps), m_F(f) {
}

bool CGradientDescent::CEmpiricalCentralGradient::operator()(const TVector& x,
                                                             TVector& result) const {
    if (las::dimension(x) != las::dimension(result)) {
        LOG_ERROR(<< "Dimension mismatch");
        return false;
    }

    xShiftEps = x;
    for (std::size_t i = 0; i < las::dimension(x); ++i) {
        xShiftEps(i) -= m_Eps;
        double fMinusEps;
        if (!m_F(xShiftEps, fMinusEps)) {
            LOG_ERROR(<< "Failed to evaluate function at x - eps");
            return false;
        }
        xShiftEps(i) += 2.0 * m_Eps;
        double fPlusEps;
        if (!m_F(xShiftEps, fPlusEps)) {
            LOG_ERROR(<< "Failed to evaluate function at x + eps");
            return false;
        }
        xShiftEps(i) -= m_Eps;
        result(i) = (fPlusEps - fMinusEps) / (2.0 * m_Eps);
    }

    return true;
}
}
}
}
