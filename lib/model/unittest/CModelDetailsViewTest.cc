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

#include <core/CLogger.h>
#include <core/Constants.h>

#include <maths/common/CNormalMeanPrecConjugate.h>

#include <maths/time_series/CTimeSeriesDecomposition.h>
#include <maths/time_series/CTimeSeriesModel.h>

#include <model/CDataGatherer.h>
#include <model/CModelPlotData.h>
#include <model/CResourceMonitor.h>
#include <model/CSearchKey.h>

#include "Mocks.h"

#include <boost/test/unit_test.hpp>

#include <memory>
#include <vector>

BOOST_TEST_DONT_PRINT_LOG_VALUE(ml::model::CModelPlotData::TFeatureStrByFieldDataUMapUMapCItr);

BOOST_AUTO_TEST_SUITE(CModelDetailsViewTest)

using namespace ml;

namespace {

const std::string EMPTY_STRING;

} // unnamed

class CTestFixture {
protected:
    model::CResourceMonitor m_ResourceMonitor;
};

BOOST_FIXTURE_TEST_CASE(testModelPlot, CTestFixture) {
    using TDoubleVec = std::vector<double>;
    using TStrVec = std::vector<std::string>;
    using TMockModelPtr = std::unique_ptr<model::CMockModel>;

    core_t::TTime bucketLength{600};
    model::CSearchKey key;
    model::SModelParams params{bucketLength};
    model_t::TFeatureVec features;

    model::CAnomalyDetectorModel::TDataGathererPtr gatherer;
    TMockModelPtr model;

    auto setupTest = [&]() {
        gatherer = std::make_shared<model::CDataGatherer>(
            model_t::analysisCategory(features[0]), model_t::E_None, params,
            EMPTY_STRING, EMPTY_STRING, "p", EMPTY_STRING, EMPTY_STRING,
            TStrVec{}, key, features, 0, 0);
        std::string person11{"p11"};
        std::string person12{"p12"};
        std::string person21{"p21"};
        std::string person22{"p22"};
        bool addedPerson{false};
        gatherer->addPerson(person11, m_ResourceMonitor, addedPerson);
        gatherer->addPerson(person12, m_ResourceMonitor, addedPerson);
        gatherer->addPerson(person21, m_ResourceMonitor, addedPerson);
        gatherer->addPerson(person22, m_ResourceMonitor, addedPerson);

        model.reset(new model::CMockModel{params, gatherer, {/*we don't care about influence*/}});

        maths::time_series::CTimeSeriesDecomposition trend;
        maths::common::CNormalMeanPrecConjugate prior{
            maths::common::CNormalMeanPrecConjugate::nonInformativePrior(maths_t::E_ContinuousData)};
        maths::common::CModelParams timeSeriesModelParams{
            bucketLength, 1.0, 0.001, 0.2, 6 * core::constants::HOUR, 24 * core::constants::HOUR};
        maths::time_series::CUnivariateTimeSeriesModel timeSeriesModel{
            timeSeriesModelParams, 0, trend, prior};
        model::CMockModel::TMathsModelUPtrVec models;
        models.emplace_back(timeSeriesModel.clone(0));
        models.emplace_back(timeSeriesModel.clone(1));
        models.emplace_back(timeSeriesModel.clone(2));
        models.emplace_back(timeSeriesModel.clone(3));
        model->mockTimeSeriesModels(std::move(models));
    };

    LOG_DEBUG(<< "Individual sum");
    {
        features.assign(1, model_t::E_IndividualSumByBucketAndPerson);
        setupTest();

        TDoubleVec values{2.0, 3.0, 0.0, 0.0};
        std::size_t pid{0};
        for (auto value : values) {
            model->mockAddBucketValue(model_t::E_IndividualSumByBucketAndPerson,
                                      pid++, 0, 0, {value});
        }

        model::CModelPlotData plotData;
        model->details()->modelPlot(0, 90.0, {}, plotData);
        BOOST_TEST_REQUIRE(plotData.begin() != plotData.end());
        for (const auto& featureByFieldData : plotData) {
            BOOST_REQUIRE_EQUAL(values.size(), featureByFieldData.second.size());
            for (const auto& byFieldData : featureByFieldData.second) {
                BOOST_TEST_REQUIRE(gatherer->personId(byFieldData.first, pid));
                BOOST_REQUIRE_EQUAL(1, byFieldData.second.s_ValuesPerOverField.size());
                for (const auto& currentBucketValue : byFieldData.second.s_ValuesPerOverField) {
                    BOOST_REQUIRE_EQUAL(values[pid], currentBucketValue.second);
                }
            }
        }
    }

    LOG_DEBUG(<< "Individual count");
    {
        features.assign(1, model_t::E_IndividualCountByBucketAndPerson);
        setupTest();

        TDoubleVec values{0.0, 1.0, 3.0};
        std::size_t pid{0};
        for (auto value : values) {
            model->mockAddBucketValue(model_t::E_IndividualCountByBucketAndPerson,
                                      pid++, 0, 0, {value});
        }

        model::CModelPlotData plotData;
        model->details()->modelPlot(0, 90.0, {}, plotData);
        BOOST_TEST_REQUIRE(plotData.begin() != plotData.end());
        for (const auto& featureByFieldData : plotData) {
            BOOST_REQUIRE_EQUAL(values.size(), featureByFieldData.second.size());
            for (const auto& byFieldData : featureByFieldData.second) {
                BOOST_TEST_REQUIRE(gatherer->personId(byFieldData.first, pid));
                BOOST_REQUIRE_EQUAL(1, byFieldData.second.s_ValuesPerOverField.size());
                for (const auto& currentBucketValue : byFieldData.second.s_ValuesPerOverField) {
                    BOOST_REQUIRE_EQUAL(values[pid], currentBucketValue.second);
                }
            }
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
