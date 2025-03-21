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

#include <maths/common/CStatisticalTests.h>

#include <model/CDetectorEqualizer.h>

#include <test/CRandomNumbers.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(CDetectorEqualizerTest)

using namespace ml;

namespace {
using TDoubleVec = std::vector<double>;
using TMeanAccumulator = maths::common::CBasicStatistics::SSampleMean<double>::TAccumulator;
const double THRESHOLD = std::log(0.05);
}

BOOST_AUTO_TEST_CASE(testCorrect) {
    // Test that the distribution of scores are more similar after correcting.

    TDoubleVec scales{1.0, 2.1, 3.2};

    model::CDetectorEqualizer equalizer;

    test::CRandomNumbers rng;

    for (std::size_t i = 0; i < scales.size(); ++i) {
        TDoubleVec logp;
        rng.generateGammaSamples(1.0, scales[i], 1000, logp);

        for (double pj : logp) {
            if (-pj <= THRESHOLD) {
                double p = std::exp(-pj);
                equalizer.add(static_cast<int>(i), p);
            }
        }
    }

    TDoubleVec raw[3];
    TDoubleVec corrected[3];
    for (std::size_t i = 0; i < scales.size(); ++i) {
        TDoubleVec logp;
        rng.generateGammaSamples(1.0, scales[i], 1000, logp);

        for (double logpj : logp) {
            if (-logpj <= THRESHOLD) {
                double p = std::exp(-logpj);
                raw[i].push_back(p);
                corrected[i].push_back(equalizer.correct(static_cast<int>(i), p));
            }
        }
    }

    TMeanAccumulator similarityIncrease;
    for (std::size_t i = 1, k = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < i; ++j, ++k) {
            double increase =
                maths::common::CStatisticalTests::twoSampleKS(corrected[i], corrected[j]) /
                maths::common::CStatisticalTests::twoSampleKS(raw[i], raw[j]);
            similarityIncrease.add(std::log(increase));
            LOG_DEBUG(<< "similarity increase = " << increase);
            BOOST_TEST_REQUIRE(increase > 3.0);
        }
    }
    LOG_DEBUG(<< "mean similarity increase = "
              << std::exp(maths::common::CBasicStatistics::mean(similarityIncrease)));
    BOOST_TEST_REQUIRE(
        std::exp(maths::common::CBasicStatistics::mean(similarityIncrease)) > 40.0);
}

BOOST_AUTO_TEST_CASE(testAge) {
    // Test that propagation doesn't introduce a bias into the corrections.

    TDoubleVec scales{1.0, 2.1, 3.2};

    model::CDetectorEqualizer equalizer;
    model::CDetectorEqualizer equalizerAged;

    test::CRandomNumbers rng;

    for (std::size_t i = 0; i < scales.size(); ++i) {
        TDoubleVec logp;
        rng.generateGammaSamples(1.0, scales[i], 1000, logp);

        for (double logpj : logp) {
            if (-logpj <= THRESHOLD) {
                double p = std::exp(-logpj);
                equalizer.add(static_cast<int>(i), p);
                equalizerAged.add(static_cast<int>(i), p);
                equalizerAged.age(0.995);
            }
        }
    }

    for (int i = 0; i < 3; ++i) {
        TMeanAccumulator meanBias;
        TMeanAccumulator meanError;
        double logp = THRESHOLD;
        for (std::size_t j = 0; j < 150; ++j, logp += std::log(0.9)) {
            double p = std::exp(logp);
            double pc = equalizer.correct(i, p);
            double pca = equalizerAged.correct(i, p);
            double error = std::fabs((std::log(pca) - std::log(pc)) / std::log(pc));
            meanError.add(error);
            meanBias.add((std::log(pca) - std::log(pc)) / std::log(pc));
            BOOST_TEST_REQUIRE(error < 0.2);
        }
        LOG_DEBUG(<< "mean bias  = " << maths::common::CBasicStatistics::mean(meanBias));
        LOG_DEBUG(<< "mean error = " << maths::common::CBasicStatistics::mean(meanError));
        BOOST_TEST_REQUIRE(std::fabs(maths::common::CBasicStatistics::mean(meanBias)) < 0.053);
        BOOST_TEST_REQUIRE(maths::common::CBasicStatistics::mean(meanError) < 0.053);
    }
}

BOOST_AUTO_TEST_CASE(testPersist) {
    TDoubleVec scales{1.0, 2.1, 3.2};

    model::CDetectorEqualizer origEqualizer;

    test::CRandomNumbers rng;

    TDoubleVec logp;
    rng.generateGammaSamples(1.0, 3.1, 1000, logp);

    for (std::size_t i = 0; i < scales.size(); ++i) {
        rng.generateGammaSamples(1.0, scales[i], 1000, logp);

        for (double logpj : logp) {
            if (-logpj <= THRESHOLD) {
                double p = std::exp(-logpj);
                origEqualizer.add(static_cast<int>(i), p);
            }
        }
    }

    std::ostringstream origJson;
    core::CJsonStatePersistInserter::persist(
        origJson, [&origEqualizer](core::CJsonStatePersistInserter& inserter) {
            origEqualizer.acceptPersistInserter(inserter);
        });

    LOG_DEBUG(<< "equalizer JSON representation:\n" << origJson.str());

    model::CDetectorEqualizer restoredEqualizer;
    {
        // The traverser expects the state json in a embedded document
        std::istringstream is("{\"topLevel\" : " + origJson.str() + "}");
        core::CJsonStateRestoreTraverser traverser(is);
        BOOST_TEST_REQUIRE(traverser.traverseSubLevel([&](auto& traverser_) {
            return restoredEqualizer.acceptRestoreTraverser(traverser_);
        }));
    }

    // Checksums should agree.
    BOOST_REQUIRE_EQUAL(origEqualizer.checksum(), restoredEqualizer.checksum());

    // The persist and restore should be idempotent.
    std::ostringstream newJson;
    core::CJsonStatePersistInserter::persist(
        newJson, [&restoredEqualizer](core::CJsonStatePersistInserter& inserter) {
            restoredEqualizer.acceptPersistInserter(inserter);
        });
    BOOST_REQUIRE_EQUAL(origJson.str(), newJson.str());
}

BOOST_AUTO_TEST_SUITE_END()
