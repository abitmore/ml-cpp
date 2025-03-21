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

#include <core/CJsonStatePersistInserter.h>
#include <core/CJsonStateRestoreTraverser.h>
#include <core/CLogger.h>

#include <maths/common/CBasicStatistics.h>
#include <maths/common/CBasicStatisticsCovariances.h>
#include <maths/common/CBasicStatisticsPersist.h>
#include <maths/common/CIntegration.h>
#include <maths/common/CLeastSquaresOnlineRegression.h>
#include <maths/common/CLeastSquaresOnlineRegressionDetail.h>

#include <test/BoostTestCloseAbsolute.h>
#include <test/CRandomNumbers.h>

#include <boost/test/unit_test.hpp>

#include <array>

BOOST_AUTO_TEST_SUITE(CLeastSquaresOnlineRegressionTest)

using namespace ml;

using TDoubleVec = std::vector<double>;
using TMeanAccumulator = maths::common::CBasicStatistics::SSampleMean<double>::TAccumulator;

namespace {

template<typename T>
T sum(const T& params, const T& delta) {
    T result;
    for (std::size_t i = 0; i < params.size(); ++i) {
        result[i] = params[i] + delta[i];
    }
    return result;
}

template<typename T>
double squareResidual(const T& params, const TDoubleVec& x, const TDoubleVec& y) {
    double result = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        double yi = 0.0;
        double xi = 1.0;
        for (std::size_t j = 0; j < params.size(); ++j, xi *= x[i]) {
            yi += params[j] * xi;
        }
        result += (y[i] - yi) * (y[i] - yi);
    }
    return result;
}

template<std::size_t N>
class CRegressionPrediction {
public:
    CRegressionPrediction(const maths::common::CLeastSquaresOnlineRegression<N, double>& regression)
        : m_Regression(regression) {}

    bool operator()(double x, double& result) const {
        result = m_Regression.predict(x);
        return true;
    }

private:
    maths::common::CLeastSquaresOnlineRegression<N, double> m_Regression;
};
}

using TDoubleArray2 = std::array<double, 2>;
using TDoubleArray3 = std::array<double, 3>;
using TDoubleArray4 = std::array<double, 4>;

BOOST_AUTO_TEST_CASE(testInvariants) {
    // Test at (local) minimum of quadratic residuals.

    test::CRandomNumbers rng;

    std::size_t n = 50;

    double intercept = 0.0;
    double slope = 2.0;
    double curvature = 0.2;

    for (std::size_t t = 0; t < 100; ++t) {
        maths::common::CLeastSquaresOnlineRegression<2, double> ls;

        TDoubleVec increments;
        rng.generateUniformSamples(1.0, 2.0, n, increments);
        TDoubleVec errors;
        rng.generateNormalSamples(0.0, 2.0, n, errors);

        TDoubleVec xs;
        TDoubleVec ys;
        double x = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            x += increments[i];
            double y = curvature * x * x + slope * x + intercept + errors[i];
            ls.add(x, y);
            xs.push_back(x);
            ys.push_back(y);
        }

        TDoubleArray3 params;
        BOOST_TEST_REQUIRE(ls.parameters(params));

        double residual = squareResidual(params, xs, ys);

        if (t % 10 == 0) {
            LOG_DEBUG(<< "params   = " << params);
            LOG_DEBUG(<< "residual = " << residual);
        }

        TDoubleVec delta;
        rng.generateUniformSamples(-1e-4, 1e-4, 15, delta);
        for (std::size_t j = 0; j < delta.size(); j += 3) {
            TDoubleArray3 deltaj;
            deltaj[0] = delta[j];
            deltaj[1] = delta[j + 1];
            deltaj[2] = delta[j + 2];

            double residualj = squareResidual(sum(params, deltaj), xs, ys);

            if (t % 10 == 0) {
                LOG_DEBUG(<< "  delta residual " << residualj);
            }

            BOOST_TEST_REQUIRE(residualj > residual);
        }
    }
}

BOOST_AUTO_TEST_CASE(testFit) {
    test::CRandomNumbers rng;

    std::size_t n = 50;

    {
        double intercept = 0.0;
        double slope = 2.0;

        TMeanAccumulator interceptError;
        TMeanAccumulator slopeError;

        for (std::size_t t = 0; t < 100; ++t) {
            maths::common::CLeastSquaresOnlineRegression<1> ls;

            TDoubleVec increments;
            rng.generateUniformSamples(1.0, 5.0, n, increments);
            TDoubleVec errors;
            rng.generateNormalSamples(0.0, 2.0, n, errors);

            double x = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                double y = slope * x + intercept + errors[i];
                ls.add(x, y);
                x += increments[i];
            }

            TDoubleArray2 params;
            BOOST_TEST_REQUIRE(ls.parameters(params));

            if (t % 10 == 0) {
                LOG_DEBUG(<< "params = " << params);
            }

            BOOST_REQUIRE_CLOSE_ABSOLUTE(intercept, params[0], 1.3);
            BOOST_REQUIRE_CLOSE_ABSOLUTE(slope, params[1], 0.015);
            interceptError.add(std::fabs(params[0] - intercept));
            slopeError.add(std::fabs(params[1] - slope));
        }

        LOG_DEBUG(<< "intercept error = " << interceptError);
        LOG_DEBUG(<< "slope error = " << slopeError);
        BOOST_TEST_REQUIRE(maths::common::CBasicStatistics::mean(interceptError) < 0.35);
        BOOST_TEST_REQUIRE(maths::common::CBasicStatistics::mean(slopeError) < 0.04);
    }

    // Test a variety of the randomly generated polynomial fits.

    {
        for (std::size_t t = 0; t < 10; ++t) {
            maths::common::CLeastSquaresOnlineRegression<2, double> ls;

            TDoubleVec curve;
            rng.generateUniformSamples(0.0, 2.0, 3, curve);

            TDoubleVec increments;
            rng.generateUniformSamples(1.0, 2.0, n, increments);

            double x = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                double y = curve[2] * x * x + curve[1] * x + curve[0];
                ls.add(x, y);
                x += increments[i];
            }

            TDoubleArray3 params;
            ls.parameters(params);

            LOG_DEBUG(<< "curve  = " << curve);
            LOG_DEBUG(<< "params = " << params);
            for (std::size_t i = 0; i < curve.size(); ++i) {
                BOOST_REQUIRE_CLOSE_ABSOLUTE(curve[i], params[i], 0.03 * curve[i]);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(testShiftAbscissa) {
    // Test shifting the abscissa is equivalent to updating
    // with shifted X-values.

    {
        double intercept = 5.0;
        double slope = 2.0;

        maths::common::CLeastSquaresOnlineRegression<1> ls;
        maths::common::CLeastSquaresOnlineRegression<1> lss;

        for (std::size_t i = 0; i < 100; ++i) {
            double x = static_cast<double>(i);
            ls.add(x, slope * x + intercept);
            lss.add((x - 50.0), slope * x + intercept);
        }

        TDoubleArray2 params1;
        ls.parameters(params1);

        ls.shiftAbscissa(-50.0);
        TDoubleArray2 params2;
        ls.parameters(params2);

        TDoubleArray2 paramss;
        lss.parameters(paramss);

        LOG_DEBUG(<< "params 1 = " << params1);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(intercept, params1[0], 1e-3 * intercept);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(slope, params1[1], 1e-3 * slope);

        LOG_DEBUG(<< "params 2 = " << params2);
        LOG_DEBUG(<< "params s = " << paramss);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(paramss[0], params2[0], 1e-3 * paramss[0]);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(paramss[1], params2[1], 1e-3 * paramss[1]);
    }

    {
        double intercept = 5.0;
        double slope = 2.0;
        double curvature = 0.1;

        maths::common::CLeastSquaresOnlineRegression<2, double> ls;
        maths::common::CLeastSquaresOnlineRegression<2, double> lss;

        for (std::size_t i = 0; i < 100; ++i) {
            double x = static_cast<double>(i);
            ls.add(x, curvature * x * x + slope * x + intercept);
            lss.add(x - 50.0, curvature * x * x + slope * x + intercept);
        }

        TDoubleArray3 params1;
        ls.parameters(params1);

        ls.shiftAbscissa(-50.0);
        TDoubleArray3 params2;
        ls.parameters(params2);

        TDoubleArray3 paramss;
        lss.parameters(paramss);

        LOG_DEBUG(<< params1);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(intercept, params1[0], 2e-3 * intercept);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(slope, params1[1], 2e-3 * slope);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(curvature, params1[2], 2e-3 * curvature);

        LOG_DEBUG(<< params2);
        LOG_DEBUG(<< paramss);
        LOG_DEBUG(<< "params 2 = " << params2);
        LOG_DEBUG(<< "params s = " << paramss);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(paramss[0], params2[0], 1e-3 * paramss[0]);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(paramss[1], params2[1], 1e-3 * paramss[1]);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(paramss[2], params2[2], 1e-3 * paramss[2]);
    }
}

BOOST_AUTO_TEST_CASE(testShiftOrdinate) {
    // Test that translating the regression by a some delta
    // produces the desired translation and no change to any
    // of the derivatives.

    maths::common::CLeastSquaresOnlineRegression<3, double> regression;
    for (double x = 0.0; x < 100.0; x += 1.0) {
        regression.add(x, 0.01 * x * x * x - 0.2 * x * x + 1.0 * x + 10.0);
    }

    TDoubleArray4 params1;
    regression.parameters(params1);

    regression.shiftOrdinate(1000.0);

    TDoubleArray4 params2;
    regression.parameters(params2);

    LOG_DEBUG(<< "parameters 1 = " << params1);
    LOG_DEBUG(<< "parameters 2 = " << params2);

    BOOST_REQUIRE_CLOSE_ABSOLUTE(1000.0 + params1[0], params2[0],
                                 1e-6 * std::fabs(params1[0]));
    BOOST_REQUIRE_CLOSE_ABSOLUTE(params1[1], params2[1], 1e-6 * std::fabs(params1[1]));
    BOOST_REQUIRE_CLOSE_ABSOLUTE(params1[2], params2[2], 1e-6 * std::fabs(params1[2]));
    BOOST_REQUIRE_CLOSE_ABSOLUTE(params1[3], params2[3], 1e-6 * std::fabs(params1[3]));
}

BOOST_AUTO_TEST_CASE(testShiftGradient) {
    // Test that translating the regression by a some delta
    // produces the desired translation and no change to any
    // of the derivatives.

    maths::common::CLeastSquaresOnlineRegression<3, double> regression;
    for (double x = 0.0; x < 100.0; x += 1.0) {
        regression.add(x, 0.01 * x * x * x - 0.2 * x * x + 1.0 * x + 10.0);
    }

    TDoubleArray4 params1;
    regression.parameters(params1);

    regression.shiftGradient(10.0);

    TDoubleArray4 params2;
    regression.parameters(params2);

    LOG_DEBUG(<< "parameters 1 = " << params1);
    LOG_DEBUG(<< "parameters 2 = " << params2);

    BOOST_REQUIRE_CLOSE_ABSOLUTE(params1[0], params2[0], 1e-6 * std::fabs(params1[0]));
    BOOST_REQUIRE_CLOSE_ABSOLUTE(10.0 + params1[1], params2[1],
                                 1e-6 * std::fabs(params1[1]));
    BOOST_REQUIRE_CLOSE_ABSOLUTE(params1[2], params2[2], 1e-6 * std::fabs(params1[2]));
    BOOST_REQUIRE_CLOSE_ABSOLUTE(params1[3], params2[3], 1e-6 * std::fabs(params1[3]));
}

BOOST_AUTO_TEST_CASE(testLinearScale) {
    // Test that linearly scaling a regression linearly
    // scales all the parameters.

    maths::common::CLeastSquaresOnlineRegression<3, double> regression;
    for (double x = 0.0; x < 100.0; x += 1.0) {
        regression.add(x, 0.01 * x * x * x - 0.2 * x * x + 1.0 * x + 10.0);
    }

    TDoubleArray4 params1;
    regression.parameters(params1);

    regression.linearScale(0.1);

    TDoubleArray4 params2;
    regression.parameters(params2);

    LOG_DEBUG(<< "parameters 1 = " << params1);
    LOG_DEBUG(<< "parameters 2 = " << params2);

    for (std::size_t i = 0; i < 4; ++i) {
        BOOST_REQUIRE_CLOSE_ABSOLUTE(0.1 * params1[i], params2[i], 1e-6);
    }

    regression.linearScale(100.0);

    regression.parameters(params2);

    LOG_DEBUG(<< "parameters 1 = " << params1);
    LOG_DEBUG(<< "parameters 2 = " << params2);

    for (std::size_t i = 0; i < 4; ++i) {
        BOOST_REQUIRE_CLOSE_ABSOLUTE(10.0 * params1[i], params2[i], 1e-6);
    }
}

BOOST_AUTO_TEST_CASE(testAge) {
    // Test that the regression is mean reverting.

    double intercept = 5.0;
    double slope = 2.0;
    double curvature = 0.2;

    {
        maths::common::CLeastSquaresOnlineRegression<1> ls;

        for (std::size_t i = 0; i <= 100; ++i) {
            double x = static_cast<double>(i);
            ls.add(x, slope * x + intercept, 5.0);
        }

        TDoubleArray2 params;
        TDoubleArray2 lastParams;

        ls.parameters(params);
        LOG_DEBUG(<< "params(0) = " << params);

        lastParams = params;
        ls.age(exp(-0.01), 1.0);
        ls.parameters(params);
        LOG_DEBUG(<< "params(0.01) = " << params);
        BOOST_TEST_REQUIRE(params[0] > lastParams[0]);
        BOOST_TEST_REQUIRE(params[0] < 105.0);
        BOOST_TEST_REQUIRE(params[1] < lastParams[0]);
        BOOST_TEST_REQUIRE(params[1] > 0.0);

        lastParams = params;
        ls.age(exp(-0.49), 1.0);
        ls.parameters(params);
        LOG_DEBUG(<< "params(0.5) = " << params);
        BOOST_TEST_REQUIRE(params[0] > lastParams[0]);
        BOOST_TEST_REQUIRE(params[0] < 105.0);
        BOOST_TEST_REQUIRE(params[1] < lastParams[0]);
        BOOST_TEST_REQUIRE(params[1] > 0.0);

        lastParams = params;
        ls.age(exp(-0.5), 1.0);
        ls.parameters(params);
        LOG_DEBUG(<< "params(1.0) = " << params);
        BOOST_TEST_REQUIRE(params[0] > lastParams[0]);
        BOOST_TEST_REQUIRE(params[0] < 105.0);
        BOOST_TEST_REQUIRE(params[1] < lastParams[0]);
        BOOST_TEST_REQUIRE(params[1] > 0.0);

        lastParams = params;
        ls.age(exp(-4.0), 1.0);
        ls.parameters(params, ls.MAX_CONDITION);
        LOG_DEBUG(<< "params(5.0) = " << params);
        BOOST_TEST_REQUIRE(params[0] > lastParams[0]);
        BOOST_TEST_REQUIRE(params[0] < 105.0);
        BOOST_TEST_REQUIRE(params[1] < lastParams[0]);
        BOOST_TEST_REQUIRE(params[1] > 0.0);
    }

    {
        maths::common::CLeastSquaresOnlineRegression<2, double> ls;

        for (std::size_t i = 0; i <= 100; ++i) {
            double x = static_cast<double>(i);
            ls.add(x, curvature * x * x + slope * x + intercept, 5.0);
        }

        TDoubleArray3 params;
        TDoubleArray3 lastParams;

        ls.parameters(params, ls.MAX_CONDITION);
        LOG_DEBUG(<< "params(0) = " << params);

        lastParams = params;
        ls.age(exp(-0.01), 1.0);
        ls.parameters(params);
        LOG_DEBUG(<< "params(0.01) = " << params);
        BOOST_TEST_REQUIRE(params[0] > lastParams[0]);
        BOOST_TEST_REQUIRE(params[0] < 775.0);
        BOOST_TEST_REQUIRE(params[1] < lastParams[0]);
        BOOST_TEST_REQUIRE(params[1] > 0.0);
        BOOST_TEST_REQUIRE(params[2] < lastParams[0]);
        BOOST_TEST_REQUIRE(params[2] > 0.0);

        lastParams = params;
        ls.age(exp(-0.49), 1.0);
        ls.parameters(params);
        LOG_DEBUG(<< "params(0.5) = " << params);
        BOOST_TEST_REQUIRE(params[0] > lastParams[0]);
        BOOST_TEST_REQUIRE(params[0] < 775.0);
        BOOST_TEST_REQUIRE(params[1] < lastParams[0]);
        BOOST_TEST_REQUIRE(params[1] > 0.0);
        BOOST_TEST_REQUIRE(params[2] < lastParams[0]);
        BOOST_TEST_REQUIRE(params[2] > 0.0);

        lastParams = params;
        ls.age(exp(-0.5), 1.0);
        ls.parameters(params);
        LOG_DEBUG(<< "params(1.0) = " << params);
        BOOST_TEST_REQUIRE(params[0] > lastParams[0]);
        BOOST_TEST_REQUIRE(params[0] < 775.0);
        BOOST_TEST_REQUIRE(params[1] < lastParams[0]);
        BOOST_TEST_REQUIRE(params[1] > 0.0);
        BOOST_TEST_REQUIRE(params[2] < lastParams[0]);
        BOOST_TEST_REQUIRE(params[2] > 0.0);

        lastParams = params;
        ls.age(exp(-4.0), 1.0);
        ls.parameters(params);
        LOG_DEBUG(<< "params(5.0) = " << params);
        BOOST_TEST_REQUIRE(params[0] > lastParams[0]);
        BOOST_TEST_REQUIRE(params[0] < 775.0);
        BOOST_TEST_REQUIRE(params[1] < lastParams[0]);
        BOOST_TEST_REQUIRE(params[1] > 0.0);
        BOOST_TEST_REQUIRE(params[2] < lastParams[0]);
        BOOST_TEST_REQUIRE(params[2] > 0.0);
    }
}

BOOST_AUTO_TEST_CASE(testR2) {

    using TMeanVarAccumulator =
        maths::common::CBasicStatistics::SSampleMeanVar<double>::TAccumulator;

    double pi = 3.14159265358979;

    TMeanAccumulator m;
    maths::common::CLeastSquaresOnlineRegression<1, double, true> ls1;
    maths::common::CLeastSquaresOnlineRegression<2, double, true> ls2;

    TMeanVarAccumulator yMoments;
    for (std::size_t i = 0; i <= 400; ++i) {
        double x = 0.002 * pi * static_cast<double>(i);
        double y = std::sin(x);
        ls1.add(x, y);
        ls2.add(x, y);
        yMoments.add(y);
    }

    double r1;
    double r2;
    ls1.r2(r1);
    ls2.r2(r2);

    TMeanVarAccumulator yMinusP1Moments;
    TMeanVarAccumulator yMinusP2Moments;
    for (std::size_t i = 0; i <= 400; ++i) {
        double x = 0.002 * pi * static_cast<double>(i);
        double y = std::sin(x);
        yMinusP1Moments.add(y - ls1.predict(x));
        yMinusP2Moments.add(y - ls2.predict(x));
    }

    double r1Expected{1.0 - maths::common::CBasicStatistics::variance(yMinusP1Moments) /
                                maths::common::CBasicStatistics::variance(yMoments)};
    double r2Expected{1.0 - maths::common::CBasicStatistics::variance(yMinusP2Moments) /
                                maths::common::CBasicStatistics::variance(yMoments)};

    LOG_DEBUG(<< "actual   r1 = " << r1 << ", r2 = " << r2);
    LOG_DEBUG(<< "expected r1 = " << r1Expected << ", r2 = " << r2Expected);
    BOOST_REQUIRE_CLOSE(r1Expected, r1, 0.001);
    BOOST_REQUIRE_CLOSE(r2Expected, r2, 0.001);
}

BOOST_AUTO_TEST_CASE(testPrediction) {
    // Check we get successive better predictions of a power
    // series function, i.e. x -> sin(x), using higher order
    // approximations.

    double pi = 3.14159265358979;

    TMeanAccumulator m;
    maths::common::CLeastSquaresOnlineRegression<1> ls1;
    maths::common::CLeastSquaresOnlineRegression<2> ls2;
    maths::common::CLeastSquaresOnlineRegression<3> ls3;

    TMeanAccumulator em;
    TMeanAccumulator e2;
    TMeanAccumulator e3;
    TMeanAccumulator e4;

    double x0 = 0.0;
    for (std::size_t i = 0; i <= 400; ++i) {
        double x = 0.005 * pi * static_cast<double>(i);
        double y = std::sin(x);

        m.add(y);
        m.age(0.95);

        ls1.add(x - x0, y);
        ls1.age(0.95);

        ls2.add(x - x0, y);
        ls2.age(0.95);

        ls3.add(x - x0, y);
        ls3.age(0.95);

        if (x > x0 + 2.0) {
            ls1.shiftAbscissa(-2.0);
            ls2.shiftAbscissa(-2.0);
            ls3.shiftAbscissa(-2.0);
            x0 += 2.0;
        }

        TDoubleArray2 params2;
        ls1.parameters(params2);
        double y2 = params2[1] * (x - x0) + params2[0];

        TDoubleArray3 params3;
        ls2.parameters(params3);
        double y3 = params3[2] * (x - x0) * (x - x0) + params3[1] * (x - x0) + params3[0];

        TDoubleArray4 params4;
        ls3.parameters(params4);
        double y4 = params4[3] * (x - x0) * (x - x0) * (x - x0) +
                    params4[2] * (x - x0) * (x - x0) + params4[1] * (x - x0) +
                    params4[0];

        if (i % 10 == 0) {
            LOG_DEBUG(<< "y = " << y
                      << ", m = " << maths::common::CBasicStatistics::mean(m)
                      << ", y2 = " << y2 << ", y3 = " << y3 << ", y4 = " << y4);
        }

        em.add((y - maths::common::CBasicStatistics::mean(m)) *
               (y - maths::common::CBasicStatistics::mean(m)));
        e2.add((y - y2) * (y - y2));
        e3.add((y - y3) * (y - y3));
        e4.add((y - y4) * (y - y4));
    }

    LOG_DEBUG(<< "em = " << maths::common::CBasicStatistics::mean(em)
              << ", e2 = " << maths::common::CBasicStatistics::mean(e2)
              << ", e3 = " << maths::common::CBasicStatistics::mean(e3)
              << ", e4 = " << maths::common::CBasicStatistics::mean(e4));
    BOOST_TEST_REQUIRE(maths::common::CBasicStatistics::mean(e2) <
                       0.27 * maths::common::CBasicStatistics::mean(em));
    BOOST_TEST_REQUIRE(maths::common::CBasicStatistics::mean(e3) <
                       0.08 * maths::common::CBasicStatistics::mean(em));
    BOOST_TEST_REQUIRE(maths::common::CBasicStatistics::mean(e4) <
                       0.025 * maths::common::CBasicStatistics::mean(em));
}

BOOST_AUTO_TEST_CASE(testCombination) {
    // Test that we can combine regressions on two subsets of
    // the points to get the same result as the regression on
    // the full collection of points.

    double intercept = 1.0;
    double slope = 1.0;
    double curvature = -0.2;

    std::size_t n = 50;

    test::CRandomNumbers rng;

    TDoubleVec errors;
    rng.generateNormalSamples(0.0, 2.0, n, errors);

    maths::common::CLeastSquaresOnlineRegression<2> lsA;
    maths::common::CLeastSquaresOnlineRegression<2> lsB;
    maths::common::CLeastSquaresOnlineRegression<2> ls;

    for (std::size_t i = 0; i < (2 * n) / 3; ++i) {
        double x = static_cast<double>(i);
        double y = curvature * x * x + slope * x + intercept + errors[i];
        lsA.add(x, y);
        ls.add(x, y);
    }
    for (std::size_t i = (2 * n) / 3; i < n; ++i) {
        double x = static_cast<double>(i);
        double y = curvature * x * x + slope * x + intercept + errors[i];
        lsB.add(x, y);
        ls.add(x, y);
    }

    maths::common::CLeastSquaresOnlineRegression<2> lsAPlusB = lsA + lsB;

    TDoubleArray3 paramsA;
    lsA.parameters(paramsA);
    TDoubleArray3 paramsB;
    lsB.parameters(paramsB);
    TDoubleArray3 params;
    ls.parameters(params);
    TDoubleArray3 paramsAPlusB;
    lsAPlusB.parameters(paramsAPlusB);

    LOG_DEBUG(<< "params A     = " << paramsA);
    LOG_DEBUG(<< "params B     = " << paramsB);
    LOG_DEBUG(<< "params       = " << params);
    LOG_DEBUG(<< "params A + B = " << paramsAPlusB);

    for (std::size_t i = 0; i < params.size(); ++i) {
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params[i], paramsAPlusB[i],
                                     5e-3 * std::fabs(params[i]));
    }
}

BOOST_AUTO_TEST_CASE(testSingular) {
    // Test that we get the highest order polynomial regression
    // available for the points added at any time. In particular,
    // one needs at least n + 1 points to be able to determine
    // the parameters of a order n polynomial.

    {
        maths::common::CLeastSquaresOnlineRegression<2> regression;
        regression.add(0.0, 1.0);

        TDoubleArray3 params;
        regression.parameters(params);
        LOG_DEBUG(<< "params = " << params);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params[0], 1.0, 1e-6);
        BOOST_REQUIRE_EQUAL(params[1], 0.0);
        BOOST_REQUIRE_EQUAL(params[2], 0.0);

        regression.add(1.0, 2.0);

        regression.parameters(params);
        LOG_DEBUG(<< "params = " << params);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params[0], 1.0, 1e-6);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params[1], 1.0, 1e-6);
        BOOST_REQUIRE_EQUAL(params[2], 0.0);

        regression.add(2.0, 3.0);

        LOG_DEBUG(<< regression.print());
        regression.parameters(params);
        LOG_DEBUG(<< "params = " << params);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params[0], 1.0, 5e-6);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params[1], 1.0, 5e-6);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params[2], 0.0, 5e-6);
    }
    {
        maths::common::CLeastSquaresOnlineRegression<2> regression;
        regression.add(0.0, 1.0);

        TDoubleArray3 params;
        regression.parameters(params);
        LOG_DEBUG(<< "params = " << params);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params[0], 1.0, 1e-6);
        BOOST_REQUIRE_EQUAL(params[1], 0.0);
        BOOST_REQUIRE_EQUAL(params[2], 0.0);

        regression.add(1.0, 2.0);

        regression.parameters(params);
        LOG_DEBUG(<< "params = " << params);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params[0], 1.0, 1e-6);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params[1], 1.0, 1e-6);
        BOOST_REQUIRE_EQUAL(params[2], 0.0);

        regression.add(2.0, 5.0);

        LOG_DEBUG(<< regression.print());
        regression.parameters(params);
        LOG_DEBUG(<< "params = " << params);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params[0], 1.0, 5e-6);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params[1], 0.0, 5e-6);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params[2], 1.0, 5e-6);
    }
    {
        maths::common::CLeastSquaresOnlineRegression<1, double> regression1;
        maths::common::CLeastSquaresOnlineRegression<2, double> regression2;
        maths::common::CLeastSquaresOnlineRegression<3, double> regression3;

        regression1.add(0.0, 1.5 + 2.0 * 0.0 + 1.1 * 0.0 * 0.0 + 3.3 * 0.0 * 0.0 * 0.0);
        regression2.add(0.0, 1.5 + 2.0 * 0.0 + 1.1 * 0.0 * 0.0 + 3.3 * 0.0 * 0.0 * 0.0);
        regression3.add(0.0, 1.5 + 2.0 * 0.0 + 1.1 * 0.0 * 0.0 + 3.3 * 0.0 * 0.0 * 0.0);

        TDoubleArray4 params3;
        regression3.parameters(params3);
        LOG_DEBUG(<< "params3 = " << params3);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params3[0], 1.5, 5e-6);
        BOOST_REQUIRE_EQUAL(params3[1], 0.0);
        BOOST_REQUIRE_EQUAL(params3[2], 0.0);
        BOOST_REQUIRE_EQUAL(params3[3], 0.0);

        regression1.add(0.5, 1.5 + 2.0 * 0.5 + 1.1 * 0.5 * 0.5 + 3.3 * 0.5 * 0.5 * 0.5);
        regression2.add(0.5, 1.5 + 2.0 * 0.5 + 1.1 * 0.5 * 0.5 + 3.3 * 0.5 * 0.5 * 0.5);
        regression3.add(0.5, 1.5 + 2.0 * 0.5 + 1.1 * 0.5 * 0.5 + 3.3 * 0.5 * 0.5 * 0.5);

        TDoubleArray2 params1;
        regression1.parameters(params1);
        LOG_DEBUG(<< "params1 = " << params3);
        regression3.parameters(params3);
        LOG_DEBUG(<< "params3 = " << params3);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params3[0], 1.5, 5e-6);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params3[1], params1[1], 1e-6);
        BOOST_REQUIRE_EQUAL(params3[2], 0.0);
        BOOST_REQUIRE_EQUAL(params3[3], 0.0);

        regression2.add(1.0, 1.5 + 2.0 * 1.0 + 1.1 * 1.0 * 1.0 + 3.3 * 1.0 * 1.0 * 1.0);
        regression3.add(1.0, 1.5 + 2.0 * 1.0 + 1.1 * 1.0 * 1.0 + 3.3 * 1.0 * 1.0 * 1.0);

        TDoubleArray3 params2;
        regression2.parameters(params2);
        LOG_DEBUG(<< "params2 = " << params2);
        regression3.parameters(params3);
        LOG_DEBUG(<< "params3 = " << params3);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params3[0], 1.5, 5e-6);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params3[1], params2[1], 1e-6);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params3[2], params2[2], 1e-6);
        BOOST_REQUIRE_EQUAL(params3[3], 0.0);

        regression3.add(1.5, 1.5 + 2.0 * 1.5 + 1.1 * 1.5 * 1.5 + 3.3 * 1.5 * 1.5 * 1.5);

        LOG_DEBUG(<< regression3.print());
        regression3.parameters(params3);
        LOG_DEBUG(<< "params3 = " << params3);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params3[0], 1.5, 5e-5);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params3[1], 2.0, 5e-5);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params3[2], 1.1, 5e-5);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(params3[3], 3.3, 5e-5);
    }
}

BOOST_AUTO_TEST_CASE(testScale) {
    // Test that scale reduces the count in the regression statistic

    maths::common::CLeastSquaresOnlineRegression<1, double> regression;

    for (std::size_t i = 0; i < 20; ++i) {
        double x = static_cast<double>(i);
        regression.add(x, 5.0 + 0.3 * x);
    }

    LOG_DEBUG(<< "statistic = " << regression.statistic());
    TDoubleArray2 params1;
    regression.parameters(params1);
    BOOST_REQUIRE_EQUAL(maths::common::CBasicStatistics::count(regression.statistic()), 20.0);

    maths::common::CLeastSquaresOnlineRegression<1, double> regression2 =
        regression.scaled(0.5);
    LOG_DEBUG(<< "statistic = " << regression2.statistic());
    TDoubleArray2 params2;
    regression2.parameters(params2);
    BOOST_REQUIRE_EQUAL(core::CContainerPrinter::print(params1),
                        core::CContainerPrinter::print(params2));
    BOOST_REQUIRE_EQUAL(
        maths::common::CBasicStatistics::count(regression2.statistic()), 10.0);

    maths::common::CLeastSquaresOnlineRegression<1, double> regression3 =
        regression2.scaled(0.5);
    LOG_DEBUG(<< "statistic = " << regression3.statistic());
    TDoubleArray2 params3;
    regression3.parameters(params3);
    BOOST_REQUIRE_EQUAL(core::CContainerPrinter::print(params1),
                        core::CContainerPrinter::print(params3));
    BOOST_REQUIRE_EQUAL(maths::common::CBasicStatistics::count(regression3.statistic()), 5.0);
}

BOOST_AUTO_TEST_CASE(testMean) {
    // Test that the mean agrees with the numeric integration
    // of the regression.

    test::CRandomNumbers rng;
    for (std::size_t i = 0; i < 5; ++i) {
        TDoubleVec coeffs;
        rng.generateUniformSamples(-1.0, 1.0, 4, coeffs);
        maths::common::CLeastSquaresOnlineRegression<3, double> regression;
        for (double x = 0.0; x < 10.0; x += 1.0) {
            regression.add(x, 0.2 * coeffs[0] * x * x * x + 0.4 * coeffs[1] * x * x +
                                  coeffs[2] * x + 2.0 * coeffs[3]);
        }

        double expected;
        maths::common::CIntegration::gaussLegendre<maths::common::CIntegration::OrderThree>(
            CRegressionPrediction<3>(regression), 10.0, 15.0, expected);
        expected /= 5.0;
        double actual = regression.mean(10.0, 15.0);
        LOG_DEBUG(<< "expected = " << expected);
        LOG_DEBUG(<< "actual   = " << actual);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(expected, actual, 1e-6);

        // Test interval spanning 0.0.
        maths::common::CIntegration::gaussLegendre<maths::common::CIntegration::OrderThree>(
            CRegressionPrediction<3>(regression), -3.0, 0.0, expected);
        expected /= 3.0;
        actual = regression.mean(-3.0, 0.0);
        LOG_DEBUG(<< "expected = " << expected);
        LOG_DEBUG(<< "actual   = " << actual);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(expected, actual, 1e-6);

        // Test zero length interval.
        maths::common::CIntegration::gaussLegendre<maths::common::CIntegration::OrderThree>(
            CRegressionPrediction<3>(regression), -3.0, -3.0 + 1e-7, expected);
        expected /= 1e-7;
        actual = regression.mean(-3.0, -3.0);
        LOG_DEBUG(<< "expected = " << expected);
        LOG_DEBUG(<< "actual   = " << actual);
        BOOST_REQUIRE_CLOSE_ABSOLUTE(expected, actual, 1e-6);
    }
}

BOOST_AUTO_TEST_CASE(testCovariances) {
    // Test the covariance matrix of the regression parameters
    // agree with the observed sample covariances of independent
    // fits to a matched model.

    using TVector2 = maths::common::CVectorNx1<double, 2>;
    using TMatrix2 = maths::common::CSymmetricMatrixNxN<double, 2>;
    using TVector3 = maths::common::CVectorNx1<double, 3>;
    using TMatrix3 = maths::common::CSymmetricMatrixNxN<double, 3>;

    test::CRandomNumbers rng;

    LOG_DEBUG(<< "linear");
    {
        double n = 75.0;
        double variance = 16.0;

        maths::common::CBasicStatistics::SSampleCovariances<TVector2> covariances(2);
        for (std::size_t i = 0; i < 500; ++i) {
            TDoubleVec noise;
            rng.generateNormalSamples(0.0, variance, static_cast<std::size_t>(n), noise);
            maths::common::CLeastSquaresOnlineRegression<1, double> regression;
            for (double x = 0.0; x < n; x += 1.0) {
                regression.add(x, 1.5 * x + noise[static_cast<std::size_t>(x)]);
            }
            TDoubleArray2 params;
            regression.parameters(params);
            covariances.add(TVector2(params));
        }
        TMatrix2 expected = maths::common::CBasicStatistics::covariances(covariances);

        maths::common::CLeastSquaresOnlineRegression<1, double> regression;
        for (double x = 0.0; x < n; x += 1.0) {
            regression.add(x, 1.5 * x);
        }
        TMatrix2 actual;
        regression.covariances(variance, actual);

        LOG_DEBUG(<< "expected = " << expected);
        LOG_DEBUG(<< "actual   = " << actual);
        BOOST_TEST_REQUIRE((actual - expected).frobenius() / expected.frobenius() < 0.05);
    }

    LOG_DEBUG(<< "quadratic");
    {
        double n = 75.0;
        double variance = 16.0;

        maths::common::CBasicStatistics::SSampleCovariances<TVector3> covariances(3);
        for (std::size_t i = 0; i < 500; ++i) {
            TDoubleVec noise;
            rng.generateNormalSamples(0.0, variance, static_cast<std::size_t>(n), noise);
            maths::common::CLeastSquaresOnlineRegression<2, double> regression;
            for (double x = 0.0; x < n; x += 1.0) {
                regression.add(x, 0.25 * x * x + 1.5 * x +
                                      noise[static_cast<std::size_t>(x)]);
            }
            TDoubleArray3 params;
            regression.parameters(params);
            covariances.add(TVector3(params));
        }
        TMatrix3 expected = maths::common::CBasicStatistics::covariances(covariances);

        maths::common::CLeastSquaresOnlineRegression<2, double> regression;
        for (double x = 0.0; x < n; x += 1.0) {
            regression.add(x, 0.25 * x * x + 1.5 * x);
        }
        TMatrix3 actual;
        regression.covariances(variance, actual);

        LOG_DEBUG(<< "expected = " << expected);
        LOG_DEBUG(<< "actual   = " << actual);
        BOOST_TEST_REQUIRE((actual - expected).frobenius() / expected.frobenius() < 0.095);
    }
}

BOOST_AUTO_TEST_CASE(testParameters) {
    maths::common::CLeastSquaresOnlineRegression<3, double> regression;

    for (std::size_t i = 0; i < 20; ++i) {
        double x = static_cast<double>(i);
        regression.add(x, 5.0 + 0.3 * x + 0.5 * x * x - 0.03 * x * x * x);
    }

    for (std::size_t i = 20; i < 25; ++i) {
        TDoubleArray4 params1 = regression.parameters(static_cast<double>(i - 19));

        maths::common::CLeastSquaresOnlineRegression<3, double> regression2(regression);
        regression2.shiftAbscissa(19.0 - static_cast<double>(i));
        TDoubleArray4 params2;
        regression2.parameters(params2);

        LOG_DEBUG(<< "params 1 = " << params1);
        LOG_DEBUG(<< "params 2 = " << params2);
        BOOST_REQUIRE_EQUAL(core::CContainerPrinter::print(params2),
                            core::CContainerPrinter::print(params1));
    }
}

BOOST_AUTO_TEST_CASE(testPersist) {
    // Test that persistence is idempotent.

    maths::common::CLeastSquaresOnlineRegression<2, double> origRegression;

    for (std::size_t i = 0; i < 20; ++i) {
        double x = static_cast<double>(i);
        origRegression.add(x, 5.0 + 0.3 * x + 0.5 * x * x);
    }

    std::ostringstream origJson;
    core::CJsonStatePersistInserter::persist(
        origJson, std::bind_front(&maths::common::CLeastSquaresOnlineRegression<2, double>::acceptPersistInserter,
                                  &origRegression));

    LOG_DEBUG(<< "Regression JSON representation:\n" << origJson.str());

    // Restore the JSON into a new regression.
    std::istringstream origJsonStrm{"{\"topLevel\" : " + origJson.str() + "}"};
    core::CJsonStateRestoreTraverser traverser(origJsonStrm);

    maths::common::CLeastSquaresOnlineRegression<2, double> restoredRegression;
    BOOST_TEST_REQUIRE(traverser.traverseSubLevel(std::bind_front(
        &maths::common::CLeastSquaresOnlineRegression<2, double>::acceptRestoreTraverser,
        &restoredRegression)));

    BOOST_REQUIRE_EQUAL(origRegression.checksum(), restoredRegression.checksum());

    std::ostringstream restoredJson;
    core::CJsonStatePersistInserter::persist(
        restoredJson,
        std::bind_front(&maths::common::CLeastSquaresOnlineRegression<2, double>::acceptPersistInserter,
                        &restoredRegression));
    BOOST_REQUIRE_EQUAL(origJson.str(), restoredJson.str());
}

BOOST_AUTO_TEST_SUITE_END()
