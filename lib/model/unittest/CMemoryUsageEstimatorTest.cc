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

#include <model/CMemoryUsageEstimator.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(CMemoryUsageEstimatorTest)

using namespace ml;
using namespace model;

namespace {

void addValue(CMemoryUsageEstimator& estimator,
              std::size_t memory,
              std::size_t people,
              std::size_t attributes,
              std::size_t correlations = 0) {
    CMemoryUsageEstimator::TSizeArray predictors;
    predictors[CMemoryUsageEstimator::E_People] = people;
    predictors[CMemoryUsageEstimator::E_Attributes] = attributes;
    predictors[CMemoryUsageEstimator::E_Correlations] = correlations;
    estimator.addValue(predictors, memory);
}

CMemoryUsageEstimator::TOptionalSize estimate(CMemoryUsageEstimator& estimator,
                                              std::size_t people,
                                              std::size_t attributes,
                                              std::size_t correlations = 0) {
    CMemoryUsageEstimator::TSizeArray predictors;
    predictors[CMemoryUsageEstimator::E_People] = people;
    predictors[CMemoryUsageEstimator::E_Attributes] = attributes;
    predictors[CMemoryUsageEstimator::E_Correlations] = correlations;
    return estimator.estimate(predictors);
}
}

BOOST_AUTO_TEST_CASE(testEstimateLinear) {
    CMemoryUsageEstimator estimator;

    // Pscale = 54
    // Ascale = 556

    // Test that several values have to be added before estimation starts
    auto mem = estimate(estimator, 1, 1);
    BOOST_TEST_REQUIRE(!mem);

    addValue(estimator, 610, 1, 1);
    mem = estimate(estimator, 2, 1);
    BOOST_TEST_REQUIRE(!mem);

    addValue(estimator, 664, 2, 1);
    mem = estimate(estimator, 3, 1);
    BOOST_TEST_REQUIRE(!mem);

    addValue(estimator, 718, 3, 1);
    mem = estimate(estimator, 4, 1);
    BOOST_TEST_REQUIRE(mem.has_value());
    BOOST_REQUIRE_EQUAL(772, *mem);

    addValue(estimator, 826, 5, 1);
    addValue(estimator, 880, 6, 1);
    addValue(estimator, 934, 7, 1);
    addValue(estimator, 988, 8, 1);
    mem = estimate(estimator, 9, 1);
    BOOST_TEST_REQUIRE(mem.has_value());
    BOOST_REQUIRE_EQUAL(1042, *mem);

    // Test that after 10 estimates we need to add some more real values
    for (std::size_t i = 0; i < 10; i++) {
        mem = estimate(estimator, 4, 1);
    }
    BOOST_TEST_REQUIRE(!mem);

    // Test that adding values for Attributes scales independently of People
    addValue(estimator, 1274, 3, 2);
    addValue(estimator, 2386, 3, 4);
    mem = estimate(estimator, 4, 4);
    BOOST_REQUIRE_EQUAL(2440, *mem);
    mem = estimate(estimator, 5, 4);
    BOOST_REQUIRE_EQUAL(2494, *mem);
    mem = estimate(estimator, 6, 5);
    BOOST_REQUIRE_EQUAL(3104, *mem);

    // This is outside the variance range of the supplied values
    mem = estimate(estimator, 60, 30);
    BOOST_TEST_REQUIRE(!mem);
}

BOOST_AUTO_TEST_CASE(testEstimateNonlinear) {
    {
        // intercept = 356
        // Pscale = 54
        // Ascale = 200
        // + noise

        CMemoryUsageEstimator estimator;
        addValue(estimator, 602, 1, 1);
        addValue(estimator, 668, 2, 1);
        addValue(estimator, 716, 3, 1);
        addValue(estimator, 830, 5, 1);
        addValue(estimator, 874, 6, 1);
        addValue(estimator, 938, 7, 1);
        addValue(estimator, 1020, 8, 1);

        auto mem = estimate(estimator, 9, 1);
        BOOST_REQUIRE_EQUAL(1080, *mem);

        addValue(estimator, 1188, 8, 2);
        mem = estimate(estimator, 9, 3);
        BOOST_REQUIRE_EQUAL(1443, *mem);
    }

    {
        // quadratic

        int pScale = 100;
        int aScale = 50;
        int cScale = 30;

        CMemoryUsageEstimator estimator;
        addValue(estimator, pScale * 10 * 10 + aScale * 9 * 9 + cScale * 15 * 15,
                 10, 9, 15);
        addValue(estimator, pScale * 11 * 11 + aScale * 11 * 11 + cScale * 20 * 20,
                 11, 11, 20);
        addValue(estimator, pScale * 12 * 12 + aScale * 13 * 13 + cScale * 25 * 25,
                 12, 13, 25);
        addValue(estimator, pScale * 13 * 13 + aScale * 15 * 15 + cScale * 26 * 26,
                 13, 15, 26);
        addValue(estimator, pScale * 17 * 17 + aScale * 19 * 19 + cScale * 27 * 27,
                 17, 19, 27);
        addValue(estimator, pScale * 20 * 20 + aScale * 19 * 19 + cScale * 30 * 30,
                 20, 19, 30);
        addValue(estimator, pScale * 20 * 20 + aScale * 25 * 25 + cScale * 40 * 40,
                 20, 25, 40);

        auto mem = estimate(estimator, 25, 35, 45);
        std::size_t actual = pScale * 25 * 25 + aScale * 35 * 35 + cScale * 45 * 45;
        LOG_DEBUG(<< "actual = " << actual << ", estimated = " << *mem);
        BOOST_TEST_REQUIRE(
            static_cast<double>(actual - *mem) / static_cast<double>(actual) < 0.15);
    }
}

BOOST_AUTO_TEST_CASE(testPersist) {
    CMemoryUsageEstimator origEstimator;
    {
        std::ostringstream origJson;
        core::CJsonStatePersistInserter::persist(
            origJson, std::bind_front(&CMemoryUsageEstimator::acceptPersistInserter,
                                      &origEstimator));
        LOG_DEBUG(<< "origJson = " << origJson.str());

        // Restore the JSON into a new data gatherer
        // The traverser expects the state json in a embedded document
        std::istringstream origJsonStrm("{\"topLevel\" : " + origJson.str() + "}");
        core::CJsonStateRestoreTraverser traverser(origJsonStrm);

        CMemoryUsageEstimator restoredEstimator;
        BOOST_TEST_REQUIRE(traverser.traverseSubLevel(std::bind_front(
            &CMemoryUsageEstimator::acceptRestoreTraverser, &restoredEstimator)));

        // The JSON representation of the new data gatherer should be the same
        // as the original.
        std::ostringstream newJson;
        core::CJsonStatePersistInserter::persist(
            newJson, std::bind_front(&CMemoryUsageEstimator::acceptPersistInserter,
                                     &restoredEstimator));
        BOOST_REQUIRE_EQUAL(origJson.str(), newJson.str());
    }
    {
        int pScale = 10000;
        int aScale = 5;
        int cScale = 3;
        addValue(origEstimator, pScale * 10 + aScale * 9 + cScale * 15, 10, 9, 15);
        addValue(origEstimator, pScale * 11 + aScale * 11 + cScale * 20, 11, 11, 20);
        addValue(origEstimator, pScale * 12 + aScale * 13 + cScale * 25, 12, 13, 25);
        addValue(origEstimator, pScale * 13 + aScale * 15 + cScale * 26, 13, 15, 26);
        addValue(origEstimator, pScale * 17 + aScale * 19 + cScale * 27, 17, 19, 27);
        addValue(origEstimator, pScale * 20 + aScale * 19 + cScale * 30, 20, 19, 30);

        std::ostringstream origJson;
        core::CJsonStatePersistInserter::persist(
            origJson, std::bind_front(&CMemoryUsageEstimator::acceptPersistInserter,
                                      &origEstimator));
        LOG_DEBUG(<< "origJson = " << origJson.str());

        // Restore the JSON into a new data gatherer
        // The traverser expects the state json in a embedded document
        std::istringstream origJsonStrm("{\"topLevel\" : " + origJson.str() + "}");
        core::CJsonStateRestoreTraverser traverser(origJsonStrm);

        CMemoryUsageEstimator restoredEstimator;
        BOOST_TEST_REQUIRE(traverser.traverseSubLevel(
            std::bind(&CMemoryUsageEstimator::acceptRestoreTraverser,
                      &restoredEstimator, std::placeholders::_1)));

        // The JSON representation of the new data gatherer should be the same
        // as the original.
        std::ostringstream newJson;
        core::CJsonStatePersistInserter::persist(
            newJson, std::bind_front(&CMemoryUsageEstimator::acceptPersistInserter,
                                     &restoredEstimator));
        BOOST_REQUIRE_EQUAL(origJson.str(), newJson.str());
    }
}

BOOST_AUTO_TEST_SUITE_END()
