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
#include <core/CPatternSet.h>
#include <core/Constants.h>
#include <core/CoreTypes.h>

#include <model/CAnomalyDetectorModel.h>
#include <model/CDataGatherer.h>
#include <model/CDetectionRule.h>
#include <model/CResourceMonitor.h>
#include <model/CRuleCondition.h>
#include <model/CSearchKey.h>
#include <model/ModelTypes.h>
#include <model/SModelParams.h>

#include <maths/common/CNormalMeanPrecConjugate.h>
#include <maths/common/MathsTypes.h>
#include <maths/time_series/CTimeSeriesDecomposition.h>
#include <maths/time_series/CTimeSeriesModel.h>

#include <test/CRandomNumbers.h>

#include "Mocks.h"
#include "ModelTestHelpers.h"

#include <boost/test/tools/interface.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/test/unit_test_suite.hpp>

#include <memory>
#include <string>
#include <vector>

BOOST_AUTO_TEST_SUITE(CDetectionRuleTest)

using namespace ml;
using namespace model;

namespace {

using TFeatureVec = std::vector<model_t::EFeature>;
using TMockModelPtr = std::unique_ptr<CMockModel>;

const std::string EMPTY_STRING;

TMockModelPtr initializeModel(CResourceMonitor& resourceMonitor) {
    constexpr core_t::TTime bucketLength{600};
    SModelParams const params{bucketLength};
    CSearchKey const key;
    model_t::TFeatureVec features;
    // Initialize mock model
    CAnomalyDetectorModel::TDataGathererPtr gatherer;

    features.assign(1, model_t::E_IndividualSumByBucketAndPerson);

    gatherer = CDataGathererBuilder(analysisCategory(features[0]), features, params, key, 0)
                   .personFieldName("p")
                   .buildSharedPtr();
    std::string const person("p1");
    bool addedPerson{false};
    gatherer->addPerson(person, resourceMonitor, addedPerson);

    const CMockModel::TFeatureInfluenceCalculatorCPtrPrVecVec influenceCalculators; //we don't care about influence
    auto model = std::make_unique<CMockModel>(params, gatherer, influenceCalculators);

    maths::time_series::CTimeSeriesDecomposition const trend;
    maths::common::CNormalMeanPrecConjugate const prior{
        maths::common::CNormalMeanPrecConjugate::nonInformativePrior(maths_t::E_ContinuousData)};
    maths::common::CModelParams const timeSeriesModelParams{
        bucketLength, 1.0, 0.001, 0.2, 6 * core::constants::HOUR, 24 * core::constants::HOUR};
    auto timeSeriesModel = std::make_unique<maths::time_series::CUnivariateTimeSeriesModel>(
        timeSeriesModelParams, 0, trend, prior);
    CMockModel::TMathsModelUPtrVec models;
    models.emplace_back(std::move(timeSeriesModel));
    model->mockTimeSeriesModels(std::move(models));
    return model;
}
}

class CTestFixture {
protected:
    CResourceMonitor m_ResourceMonitor;
};

BOOST_FIXTURE_TEST_CASE(testApplyGivenScope, CTestFixture) {
    constexpr core_t::TTime bucketLength = 100;
    constexpr core_t::TTime startTime = 100;
    const SModelParams params(bucketLength);
    const CAnomalyDetectorModel::TFeatureInfluenceCalculatorCPtrPrVecVec influenceCalculators;

    TFeatureVec features;
    features.push_back(model_t::E_PopulationMeanByPersonAndAttribute);
    std::string const partitionFieldName("partition");
    std::string const partitionFieldValue("par_1");
    std::string const personFieldName("over");
    std::string const attributeFieldName("by");
    CSearchKey const key(0, function_t::E_PopulationMetricMean, false, model_t::E_XF_None,
                         "", attributeFieldName, personFieldName, partitionFieldName);
    auto gathererPtr = CDataGathererBuilder(model_t::E_PopulationMetric,
                                            features, params, key, startTime)
                           .partitionFieldValue(partitionFieldValue)
                           .personFieldName(personFieldName)
                           .attributeFieldName(attributeFieldName)
                           .buildSharedPtr();

    std::string const person1("p1");
    bool added = false;
    gathererPtr->addPerson(person1, m_ResourceMonitor, added);
    std::string const person2("p2");
    gathererPtr->addPerson(person2, m_ResourceMonitor, added);
    std::string const attr11("a1_1");
    std::string const attr12("a1_2");
    std::string const attr21("a2_1");
    std::string const attr22("a2_2");
    gathererPtr->addAttribute(attr11, m_ResourceMonitor, added);
    gathererPtr->addAttribute(attr12, m_ResourceMonitor, added);
    gathererPtr->addAttribute(attr21, m_ResourceMonitor, added);
    gathererPtr->addAttribute(attr22, m_ResourceMonitor, added);

    CMockModel model(params, gathererPtr, influenceCalculators);
    model.mockPopulation(true);
    CAnomalyDetectorModel::TDouble1Vec const actual(1, 4.99);
    model.mockAddBucketValue(model_t::E_PopulationMeanByPersonAndAttribute, 0, 0, 100, actual);
    model.mockAddBucketValue(model_t::E_PopulationMeanByPersonAndAttribute, 0, 1, 100, actual);
    model.mockAddBucketValue(model_t::E_PopulationMeanByPersonAndAttribute, 1, 2, 100, actual);
    model.mockAddBucketValue(model_t::E_PopulationMeanByPersonAndAttribute, 1, 3, 100, actual);

    for (auto filterType : {CRuleScope::E_Include, CRuleScope::E_Exclude}) {
        std::string const filterJson(R"(["a1_1","a2_2"])");
        core::CPatternSet valueFilter;
        valueFilter.initFromJson(filterJson);

        CDetectionRule rule;
        if (filterType == CRuleScope::E_Include) {
            rule.includeScope(attributeFieldName, valueFilter);
        } else {
            rule.excludeScope(attributeFieldName, valueFilter);
        }

        bool isInclude = filterType == CRuleScope::E_Include;
        model_t::CResultType const resultType(model_t::CResultType::E_Final);

        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 0, 0, 100) == isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 0, 1, 100) != isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 1, 2, 100) != isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 1, 3, 100) == isInclude);
    }

    for (auto filterType : {CRuleScope::E_Include, CRuleScope::E_Exclude}) {
        std::string const filterJson(R"(["a1*"])");
        core::CPatternSet valueFilter;
        valueFilter.initFromJson(filterJson);

        CDetectionRule rule;
        if (filterType == CRuleScope::E_Include) {
            rule.includeScope(attributeFieldName, valueFilter);
        } else {
            rule.excludeScope(attributeFieldName, valueFilter);
        }

        bool isInclude = filterType == CRuleScope::E_Include;
        model_t::CResultType const resultType(model_t::CResultType::E_Final);

        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 0, 0, 100) == isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 0, 1, 100) == isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 1, 2, 100) != isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 1, 3, 100) != isInclude);
    }

    for (auto filterType : {CRuleScope::E_Include, CRuleScope::E_Exclude}) {
        std::string const filterJson(R"(["*2"])");
        core::CPatternSet valueFilter;
        valueFilter.initFromJson(filterJson);

        CDetectionRule rule;
        if (filterType == CRuleScope::E_Include) {
            rule.includeScope(attributeFieldName, valueFilter);
        } else {
            rule.excludeScope(attributeFieldName, valueFilter);
        }

        bool isInclude = filterType == CRuleScope::E_Include;
        model_t::CResultType const resultType(model_t::CResultType::E_Final);

        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 0, 0, 100) != isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 0, 1, 100) == isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 1, 2, 100) != isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 1, 3, 100) == isInclude);
    }

    for (auto filterType : {CRuleScope::E_Include, CRuleScope::E_Exclude}) {
        std::string const filterJson(R"(["*1*"])");
        core::CPatternSet valueFilter;
        valueFilter.initFromJson(filterJson);

        CDetectionRule rule;
        if (filterType == CRuleScope::E_Include) {
            rule.includeScope(attributeFieldName, valueFilter);
        } else {
            rule.excludeScope(attributeFieldName, valueFilter);
        }

        bool isInclude = filterType == CRuleScope::E_Include;
        model_t::CResultType const resultType(model_t::CResultType::E_Final);

        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 0, 0, 100) == isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 0, 1, 100) == isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 1, 2, 100) == isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 1, 3, 100) != isInclude);
    }

    for (auto filterType : {CRuleScope::E_Include, CRuleScope::E_Exclude}) {
        std::string const filterJson(R"(["p2"])");
        core::CPatternSet valueFilter;
        valueFilter.initFromJson(filterJson);

        CDetectionRule rule;
        if (filterType == CRuleScope::E_Include) {
            rule.includeScope(personFieldName, valueFilter);
        } else {
            rule.excludeScope(personFieldName, valueFilter);
        }

        bool isInclude = filterType == CRuleScope::E_Include;
        model_t::CResultType const resultType(model_t::CResultType::E_Final);

        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 0, 0, 100) != isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 0, 1, 100) != isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 1, 2, 100) == isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 1, 3, 100) == isInclude);
    }

    for (auto filterType : {CRuleScope::E_Include, CRuleScope::E_Exclude}) {
        std::string const filterJson(R"(["par_1"])");
        core::CPatternSet valueFilter;
        valueFilter.initFromJson(filterJson);

        CDetectionRule rule;
        if (filterType == CRuleScope::E_Include) {
            rule.includeScope(partitionFieldName, valueFilter);
        } else {
            rule.excludeScope(partitionFieldName, valueFilter);
        }

        bool isInclude = filterType == CRuleScope::E_Include;
        model_t::CResultType const resultType(model_t::CResultType::E_Final);

        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 0, 0, 100) == isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 0, 1, 100) == isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 1, 2, 100) == isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 1, 3, 100) == isInclude);
    }

    for (auto filterType : {CRuleScope::E_Include, CRuleScope::E_Exclude}) {
        std::string const filterJson(R"(["par_2"])");
        core::CPatternSet valueFilter;
        valueFilter.initFromJson(filterJson);

        CDetectionRule rule;
        if (filterType == CRuleScope::E_Include) {
            rule.includeScope(partitionFieldName, valueFilter);
        } else {
            rule.excludeScope(partitionFieldName, valueFilter);
        }

        bool isInclude = filterType == CRuleScope::E_Include;
        model_t::CResultType const resultType(model_t::CResultType::E_Final);

        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 0, 0, 100) != isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 0, 1, 100) != isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 1, 2, 100) != isInclude);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_PopulationMeanByPersonAndAttribute,
                                      resultType, 1, 3, 100) != isInclude);
    }
}

BOOST_FIXTURE_TEST_CASE(testApplyGivenNumericalActualCondition, CTestFixture) {
    constexpr core_t::TTime bucketLength = 100;
    constexpr core_t::TTime startTime = 100;
    CSearchKey const key;
    SModelParams const params(bucketLength);
    const CAnomalyDetectorModel::TFeatureInfluenceCalculatorCPtrPrVecVec influenceCalculators;

    TFeatureVec const features{model_t::E_IndividualMeanByPerson};
    auto gathererPtr = CDataGathererBuilder(model_t::E_Metric, features, params, key, startTime)
                           .buildSharedPtr();

    std::string const person1("p1");
    bool addedPerson = false;
    gathererPtr->addPerson(person1, m_ResourceMonitor, addedPerson);

    CMockModel model(params, gathererPtr, influenceCalculators);
    CAnomalyDetectorModel::TDouble1Vec const actual100{4.99};
    CAnomalyDetectorModel::TDouble1Vec const actual200{5.00};
    CAnomalyDetectorModel::TDouble1Vec const actual300{5.01};
    model.mockAddBucketValue(model_t::E_IndividualMeanByPerson, 0, 0, 100, actual100);
    model.mockAddBucketValue(model_t::E_IndividualMeanByPerson, 0, 0, 200, actual200);
    model.mockAddBucketValue(model_t::E_IndividualMeanByPerson, 0, 0, 300, actual300);

    auto testRule = [&](CRuleCondition::ERuleConditionOperator op, double value,
                        bool expected100, bool expected200, bool expected300) {
        CRuleCondition condition;
        condition.appliesTo(CRuleCondition::E_Actual);
        condition.op(op);
        condition.value(value);
        CDetectionRule rule;
        rule.addCondition(condition);

        model_t::CResultType const resultType(model_t::CResultType::E_Final);

        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_IndividualMeanByPerson,
                                      resultType, 0, 0, 100) == expected100);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_IndividualMeanByPerson,
                                      resultType, 0, 0, 200) == expected200);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_IndividualMeanByPerson,
                                      resultType, 0, 0, 300) == expected300);
    };

    testRule(CRuleCondition::E_LT, 5.0, true, false, false);
    testRule(CRuleCondition::E_LTE, 5.0, true, true, false);
    testRule(CRuleCondition::E_GT, 5.0, false, false, true);
    testRule(CRuleCondition::E_GTE, 5.0, false, true, true);
}

BOOST_FIXTURE_TEST_CASE(testApplyGivenNumericalTypicalCondition, CTestFixture) {
    constexpr core_t::TTime bucketLength = 100;
    constexpr core_t::TTime startTime = 100;
    const CSearchKey key;
    const SModelParams params(bucketLength);
    const CAnomalyDetectorModel::TFeatureInfluenceCalculatorCPtrPrVecVec influenceCalculators;

    TFeatureVec const features{model_t::E_IndividualMeanByPerson};
    auto gathererPtr = CDataGathererBuilder(model_t::E_Metric, features, params, key, startTime)
                           .buildSharedPtr();

    const std::string person1("p1");
    bool addedPerson = false;
    gathererPtr->addPerson(person1, m_ResourceMonitor, addedPerson);

    CMockModel model(params, gathererPtr, influenceCalculators);
    const CAnomalyDetectorModel::TDouble1Vec actual100{4.99};
    const CAnomalyDetectorModel::TDouble1Vec actual200{5.00};
    const CAnomalyDetectorModel::TDouble1Vec actual300{5.01};
    model.mockAddBucketValue(model_t::E_IndividualMeanByPerson, 0, 0, 100, actual100);
    model.mockAddBucketValue(model_t::E_IndividualMeanByPerson, 0, 0, 200, actual200);
    model.mockAddBucketValue(model_t::E_IndividualMeanByPerson, 0, 0, 300, actual300);
    const CAnomalyDetectorModel::TDouble1Vec typical100{44.99};
    const CAnomalyDetectorModel::TDouble1Vec typical200{45.00};
    const CAnomalyDetectorModel::TDouble1Vec typical300{45.01};
    model.mockAddBucketBaselineMean(model_t::E_IndividualMeanByPerson, 0, 0, 100, typical100);
    model.mockAddBucketBaselineMean(model_t::E_IndividualMeanByPerson, 0, 0, 200, typical200);
    model.mockAddBucketBaselineMean(model_t::E_IndividualMeanByPerson, 0, 0, 300, typical300);

    auto testRule = [&](CRuleCondition::ERuleConditionOperator op, double value,
                        bool expected100, bool expected200, bool expected300) {
        CRuleCondition condition;
        condition.appliesTo(CRuleCondition::E_Typical);
        condition.op(op);
        condition.value(value);
        CDetectionRule rule;
        rule.addCondition(condition);

        const model_t::CResultType resultType(model_t::CResultType::E_Final);

        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_IndividualMeanByPerson,
                                      resultType, 0, 0, 100) == expected100);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_IndividualMeanByPerson,
                                      resultType, 0, 0, 200) == expected200);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_IndividualMeanByPerson,
                                      resultType, 0, 0, 300) == expected300);
    };

    testRule(CRuleCondition::E_LT, 45.0, true, false, false);
    testRule(CRuleCondition::E_GT, 45.0, false, false, true);
}

BOOST_FIXTURE_TEST_CASE(testApplyGivenNumericalDiffAbsCondition, CTestFixture) {
    constexpr core_t::TTime bucketLength = 100;
    constexpr core_t::TTime startTime = 100;
    const CSearchKey key;
    const SModelParams params(bucketLength);
    const CAnomalyDetectorModel::TFeatureInfluenceCalculatorCPtrPrVecVec influenceCalculators;

    TFeatureVec const features{model_t::E_IndividualMeanByPerson};
    auto gathererPtr = CDataGathererBuilder(model_t::E_Metric, features, params, key, startTime)
                           .buildSharedPtr();

    const std::string person1("p1");
    bool addedPerson = false;
    gathererPtr->addPerson(person1, m_ResourceMonitor, addedPerson);

    CMockModel model(params, gathererPtr, influenceCalculators);
    const std::vector<CAnomalyDetectorModel::TDouble1Vec> actuals{
        {8.9}, {9.0}, {9.1}, {10.9}, {11.0}, {11.1}};
    const std::vector<CAnomalyDetectorModel::TDouble1Vec> typicals(6, {10.0});

    for (size_t i = 0; i < actuals.size(); ++i) {
        model.mockAddBucketValue(model_t::E_IndividualMeanByPerson, 0, 0,
                                 100 * (i + 1), actuals[i]);
        model.mockAddBucketBaselineMean(model_t::E_IndividualMeanByPerson, 0, 0,
                                        100 * (i + 1), typicals[i]);
    }

    auto testRule = [&](CRuleCondition::ERuleConditionOperator op, double value,
                        const std::vector<bool>& expected) {
        CRuleCondition condition;
        condition.appliesTo(CRuleCondition::E_DiffFromTypical);
        condition.op(op);
        condition.value(value);
        CDetectionRule rule;
        rule.addCondition(condition);

        const model_t::CResultType resultType(model_t::CResultType::E_Final);

        for (size_t i = 0; i < expected.size(); ++i) {
            BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                          model_t::E_IndividualMeanByPerson, resultType,
                                          0, 0, 100 * (i + 1)) == expected[i]);
        }
    };

    testRule(CRuleCondition::E_LT, 1.0, {false, false, true, true, false, false});
    testRule(CRuleCondition::E_GT, 1.0, {true, false, false, false, false, true});
}

BOOST_FIXTURE_TEST_CASE(testApplyGivenNoActualValueAvailable, CTestFixture) {
    constexpr core_t::TTime bucketLength = 100;
    constexpr core_t::TTime startTime = 100;
    CSearchKey const key;
    SModelParams const params(bucketLength);
    const CAnomalyDetectorModel::TFeatureInfluenceCalculatorCPtrPrVecVec influenceCalculators;

    TFeatureVec features;
    features.push_back(model_t::E_IndividualMeanByPerson);
    CAnomalyDetectorModel::TDataGathererPtr const gathererPtr =
        CDataGathererBuilder(model_t::E_Metric, features, params, key, startTime)
            .buildSharedPtr();

    std::string const person1("p1");
    bool addedPerson = false;
    gathererPtr->addPerson(person1, m_ResourceMonitor, addedPerson);

    CMockModel model(params, gathererPtr, influenceCalculators);
    CAnomalyDetectorModel::TDouble1Vec const actual100(1, 4.99);
    CAnomalyDetectorModel::TDouble1Vec const actual200(1, 5.00);
    CAnomalyDetectorModel::TDouble1Vec const actual300(1, 5.01);
    model.mockAddBucketValue(model_t::E_IndividualMeanByPerson, 0, 0, 100, actual100);
    model.mockAddBucketValue(model_t::E_IndividualMeanByPerson, 0, 0, 200, actual200);
    model.mockAddBucketValue(model_t::E_IndividualMeanByPerson, 0, 0, 300, actual300);

    CRuleCondition condition;
    condition.appliesTo(CRuleCondition::E_Actual);
    condition.op(CRuleCondition::E_LT);
    condition.value(5.0);
    CDetectionRule rule;
    rule.addCondition(condition);

    model_t::CResultType const resultType(model_t::CResultType::E_Final);

    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model, model_t::E_IndividualMeanByPerson,
                                  resultType, 0, 0, 400) == false);
}

BOOST_FIXTURE_TEST_CASE(testApplyGivenDifferentSeriesAndIndividualModel, CTestFixture) {
    constexpr core_t::TTime bucketLength = 100;
    constexpr core_t::TTime startTime = 100;
    CSearchKey const key;
    SModelParams const params(bucketLength);
    const CAnomalyDetectorModel::TFeatureInfluenceCalculatorCPtrPrVecVec influenceCalculators;

    TFeatureVec features;
    features.push_back(model_t::E_IndividualMeanByPerson);
    std::string const personFieldName("series");
    CAnomalyDetectorModel::TDataGathererPtr const gathererPtr =
        CDataGathererBuilder(model_t::E_Metric, features, params, key, startTime)
            .personFieldName(personFieldName)
            .buildSharedPtr();

    std::string const person1("p1");
    bool addedPerson = false;
    gathererPtr->addPerson(person1, m_ResourceMonitor, addedPerson);
    std::string const person2("p2");
    gathererPtr->addPerson(person2, m_ResourceMonitor, addedPerson);

    CMockModel model(params, gathererPtr, influenceCalculators);
    CAnomalyDetectorModel::TDouble1Vec const p1Actual(1, 4.99);
    CAnomalyDetectorModel::TDouble1Vec const p2Actual(1, 4.99);
    model.mockAddBucketValue(model_t::E_IndividualMeanByPerson, 0, 0, 100, p1Actual);
    model.mockAddBucketValue(model_t::E_IndividualMeanByPerson, 1, 0, 100, p2Actual);

    CDetectionRule rule;

    std::string const filterJson(R"(["p1"])");
    core::CPatternSet valueFilter;
    valueFilter.initFromJson(filterJson);
    rule.includeScope(personFieldName, valueFilter);

    CRuleCondition condition;
    condition.appliesTo(CRuleCondition::E_Actual);
    condition.op(CRuleCondition::E_LT);
    condition.value(5.0);

    rule.addCondition(condition);

    model_t::CResultType const resultType(model_t::CResultType::E_Final);

    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model, model_t::E_IndividualMeanByPerson,
                                  resultType, 0, 0, 100));
    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model, model_t::E_IndividualMeanByPerson,
                                  resultType, 1, 0, 100) == false);
}

BOOST_FIXTURE_TEST_CASE(testApplyGivenDifferentSeriesAndPopulationModel, CTestFixture) {
    constexpr core_t::TTime bucketLength = 100;
    constexpr core_t::TTime startTime = 100;
    CSearchKey const key;
    SModelParams const params(bucketLength);
    const CAnomalyDetectorModel::TFeatureInfluenceCalculatorCPtrPrVecVec influenceCalculators;

    TFeatureVec features;
    features.push_back(model_t::E_PopulationMeanByPersonAndAttribute);
    std::string const personFieldName("over");
    std::string const attributeFieldName("by");
    auto gathererPtr = CDataGathererBuilder(model_t::E_PopulationMetric,
                                            features, params, key, startTime)
                           .personFieldName(personFieldName)
                           .attributeFieldName(attributeFieldName)
                           .buildSharedPtr();
    std::string const person1("p1");
    bool added = false;
    gathererPtr->addPerson(person1, m_ResourceMonitor, added);
    std::string const person2("p2");
    gathererPtr->addPerson(person2, m_ResourceMonitor, added);
    std::string const attr11("a1_1");
    std::string const attr12("a1_2");
    std::string const attr21("a2_1");
    std::string const attr22("a2_2");
    gathererPtr->addAttribute(attr11, m_ResourceMonitor, added);
    gathererPtr->addAttribute(attr12, m_ResourceMonitor, added);
    gathererPtr->addAttribute(attr21, m_ResourceMonitor, added);
    gathererPtr->addAttribute(attr22, m_ResourceMonitor, added);

    CMockModel model(params, gathererPtr, influenceCalculators);
    model.mockPopulation(true);
    CAnomalyDetectorModel::TDouble1Vec const actual(1, 4.99);
    model.mockAddBucketValue(model_t::E_PopulationMeanByPersonAndAttribute, 0, 0, 100, actual);
    model.mockAddBucketValue(model_t::E_PopulationMeanByPersonAndAttribute, 0, 1, 100, actual);
    model.mockAddBucketValue(model_t::E_PopulationMeanByPersonAndAttribute, 1, 2, 100, actual);
    model.mockAddBucketValue(model_t::E_PopulationMeanByPersonAndAttribute, 1, 3, 100, actual);

    CDetectionRule rule;

    std::string const filterJson("[\"" + attr12 + "\"]");
    core::CPatternSet valueFilter;
    valueFilter.initFromJson(filterJson);
    rule.includeScope(attributeFieldName, valueFilter);

    CRuleCondition condition;
    condition.appliesTo(CRuleCondition::E_Actual);
    condition.op(CRuleCondition::E_LT);
    condition.value(5.0);
    rule.addCondition(condition);

    model_t::CResultType const resultType(model_t::CResultType::E_Final);

    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                  model_t::E_PopulationMeanByPersonAndAttribute,
                                  resultType, 0, 0, 100) == false);
    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                  model_t::E_PopulationMeanByPersonAndAttribute,
                                  resultType, 0, 1, 100));
    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                  model_t::E_PopulationMeanByPersonAndAttribute,
                                  resultType, 1, 2, 100) == false);
    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                  model_t::E_PopulationMeanByPersonAndAttribute,
                                  resultType, 1, 3, 100) == false);
}

BOOST_FIXTURE_TEST_CASE(testApplyGivenMultipleConditions, CTestFixture) {
    constexpr core_t::TTime bucketLength = 100;
    constexpr core_t::TTime startTime = 100;
    const CSearchKey key;
    const SModelParams params(bucketLength);
    const CAnomalyDetectorModel::TFeatureInfluenceCalculatorCPtrPrVecVec influenceCalculators;

    TFeatureVec const features{model_t::E_IndividualMeanByPerson};
    const std::string personFieldName("series");
    auto gathererPtr = CDataGathererBuilder(model_t::E_Metric, features, params, key, startTime)
                           .personFieldName(personFieldName)
                           .buildSharedPtr();

    const std::string person1("p1");
    bool addedPerson = false;
    gathererPtr->addPerson(person1, m_ResourceMonitor, addedPerson);

    CMockModel model(params, gathererPtr, influenceCalculators);
    const CAnomalyDetectorModel::TDouble1Vec p1Actual{10.0};
    model.mockAddBucketValue(model_t::E_IndividualMeanByPerson, 0, 0, 100, p1Actual);

    auto testRule = [&](const CRuleCondition& condition1,
                        const CRuleCondition& condition2, bool expected) {
        CDetectionRule rule;
        rule.addCondition(condition1);
        rule.addCondition(condition2);

        const model_t::CResultType resultType(model_t::CResultType::E_Final);
        BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model,
                                      model_t::E_IndividualMeanByPerson,
                                      resultType, 0, 0, 100) == expected);
    };

    CRuleCondition condition1;
    condition1.appliesTo(CRuleCondition::E_Actual);
    condition1.op(CRuleCondition::E_LT);
    condition1.value(9.0);

    CRuleCondition condition2;
    condition2.appliesTo(CRuleCondition::E_Actual);
    condition2.op(CRuleCondition::E_LT);
    condition2.value(9.5);

    testRule(condition1, condition2, false);

    condition1.value(11.0);
    condition2.value(9.5);
    testRule(condition1, condition2, false);

    condition1.value(9.0);
    condition2.value(10.5);
    testRule(condition1, condition2, false);

    condition1.value(12.0);
    condition2.value(10.5);
    testRule(condition1, condition2, true);
}

BOOST_FIXTURE_TEST_CASE(testApplyGivenTimeCondition, CTestFixture) {
    constexpr core_t::TTime bucketLength = 100;
    constexpr core_t::TTime startTime = 100;
    SModelParams const params(bucketLength);
    const CAnomalyDetectorModel::TFeatureInfluenceCalculatorCPtrPrVecVec influenceCalculators;

    TFeatureVec features;
    features.push_back(model_t::E_IndividualMeanByPerson);
    std::string const partitionFieldName("partition");
    std::string const personFieldName("series");
    CSearchKey const key(0, function_t::E_IndividualMetricMean, false, model_t::E_XF_None,
                         "", personFieldName, EMPTY_STRING, partitionFieldName);
    auto gathererPtr = CDataGathererBuilder(model_t::E_Metric, features, params, key, startTime)
                           .personFieldName(personFieldName)
                           .buildSharedPtr();
    CMockModel const model(params, gathererPtr, influenceCalculators);
    CRuleCondition conditionGte;
    conditionGte.appliesTo(CRuleCondition::E_Time);
    conditionGte.op(CRuleCondition::E_GTE);
    conditionGte.value(100);
    CRuleCondition conditionLt;
    conditionLt.appliesTo(CRuleCondition::E_Time);
    conditionLt.op(CRuleCondition::E_LT);
    conditionLt.value(200);

    CDetectionRule rule;
    rule.addCondition(conditionGte);
    rule.addCondition(conditionLt);

    model_t::CResultType const resultType(model_t::CResultType::E_Final);

    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model, model_t::E_IndividualMeanByPerson,
                                  resultType, 0, 0, 99) == false);
    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model, model_t::E_IndividualMeanByPerson,
                                  resultType, 0, 0, 100));
    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model, model_t::E_IndividualMeanByPerson,
                                  resultType, 0, 0, 150));
    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model, model_t::E_IndividualMeanByPerson,
                                  resultType, 0, 0, 200) == false);
}

BOOST_FIXTURE_TEST_CASE(testRuleActions, CTestFixture) {
    constexpr core_t::TTime bucketLength = 100;
    constexpr core_t::TTime startTime = 100;
    SModelParams const params(bucketLength);
    const CAnomalyDetectorModel::TFeatureInfluenceCalculatorCPtrPrVecVec influenceCalculators;

    TFeatureVec features;
    features.push_back(model_t::E_IndividualMeanByPerson);
    std::string const partitionFieldName("partition");
    std::string const personFieldName("series");
    CSearchKey const key(0, function_t::E_IndividualMetricMean, false, model_t::E_XF_None,
                         "", personFieldName, EMPTY_STRING, partitionFieldName);
    auto gathererPtr = CDataGathererBuilder(model_t::E_Metric, features, params, key, startTime)
                           .personFieldName(personFieldName)
                           .buildSharedPtr();
    CMockModel const model(params, gathererPtr, influenceCalculators);
    CRuleCondition conditionGte;
    conditionGte.appliesTo(CRuleCondition::E_Time);
    conditionGte.op(CRuleCondition::E_GTE);
    conditionGte.value(100);

    CDetectionRule rule;
    rule.addCondition(conditionGte);

    model_t::CResultType const resultType(model_t::CResultType::E_Final);

    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model, model_t::E_IndividualMeanByPerson,
                                  resultType, 0, 0, 100));
    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipModelUpdate, model,
                                  model_t::E_IndividualMeanByPerson, resultType,
                                  0, 0, 100) == false);

    rule.action(CDetectionRule::E_SkipModelUpdate);
    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model, model_t::E_IndividualMeanByPerson,
                                  resultType, 0, 0, 100) == false);
    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipModelUpdate, model,
                                  model_t::E_IndividualMeanByPerson, resultType,
                                  0, 0, 100));

    rule.action(3);
    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipResult, model, model_t::E_IndividualMeanByPerson,
                                  resultType, 0, 0, 100));
    BOOST_TEST_REQUIRE(rule.apply(CDetectionRule::E_SkipModelUpdate, model,
                                  model_t::E_IndividualMeanByPerson, resultType,
                                  0, 0, 100));
}

BOOST_FIXTURE_TEST_CASE(testRuleTimeShiftShouldShiftTimeSeriesModelState, CTestFixture) {

    test::CRandomNumbers rng;
    test::CRandomNumbers::TDoubleVec timeShifts;
    rng.generateUniformSamples(-3600, 3600, 10, timeShifts);

    for (const auto timeShift : timeShifts) {
        core_t::TTime timeShiftInSecs{static_cast<core_t::TTime>(timeShift)};
        TMockModelPtr model{initializeModel(m_ResourceMonitor)};
        // Capture state before the rule is applied
        const auto& trendModel =
            static_cast<const maths::time_series::CTimeSeriesDecomposition&>(
                static_cast<const maths::time_series::CUnivariateTimeSeriesModel*>(
                    model->model(0))
                    ->trendModel());
        const core_t::TTime lastValueTime = trendModel.lastValueTime();
        const auto& annotations = model->annotations();
        const std::size_t numAnnotationsBeforeShift = annotations.size();

        constexpr core_t::TTime timestamp{100};
        CRuleCondition conditionGte;
        conditionGte.appliesTo(CRuleCondition::E_Time);
        conditionGte.op(CRuleCondition::E_GTE);
        conditionGte.value(static_cast<double>(timestamp));

        // When time shift rule is applied
        CDetectionRule rule;
        rule.addCondition(conditionGte);
        rule.addTimeShift(timeShiftInSecs);
        rule.executeCallback(*model, timestamp);

        // the time series model should have been shifted by specified amount.
        BOOST_TEST_REQUIRE(trendModel.lastValueTime() == lastValueTime + timeShiftInSecs);
        BOOST_TEST_REQUIRE(trendModel.timeShift() == timeShiftInSecs);

        // and an annotation should have been added to the model
        BOOST_TEST_REQUIRE(annotations.size() == numAnnotationsBeforeShift + 1);
    }
}

BOOST_FIXTURE_TEST_CASE(testRuleTimeShiftShouldNotApplyTwice, CTestFixture) {
    // Test that if a rule has already been applied, it should not be applied again.
    constexpr core_t::TTime timeShift{3600};

    const TMockModelPtr model{initializeModel(m_ResourceMonitor)};
    const auto& trendModel = static_cast<const maths::time_series::CTimeSeriesDecomposition&>(
        static_cast<const maths::time_series::CUnivariateTimeSeriesModel*>(model->model(0))
            ->trendModel());

    core_t::TTime timestamp{100};
    CRuleCondition conditionGte;
    conditionGte.appliesTo(CRuleCondition::E_Time);
    conditionGte.op(CRuleCondition::E_GTE);
    conditionGte.value(static_cast<double>(timestamp));

    // When time shift rule is applied twice
    CDetectionRule rule;
    rule.addCondition(conditionGte);
    rule.addTimeShift(timeShift);
    rule.executeCallback(*model, timestamp);

    core_t::TTime lastValueTimeAfterFirstShift = trendModel.lastValueTime();
    core_t::TTime timeShiftAfterFirstShift = trendModel.timeShift();

    // the values after the second time should be the same as the values after the first time shift.
    timestamp += timeShift; // simulate the time has moved forward by the time shift
    rule.executeCallback(*model, timestamp);
    BOOST_TEST_REQUIRE(trendModel.lastValueTime() == lastValueTimeAfterFirstShift);
    BOOST_TEST_REQUIRE(trendModel.timeShift() == timeShiftAfterFirstShift);
}

BOOST_FIXTURE_TEST_CASE(testTwoTimeShiftRuleShouldShiftTwice, CTestFixture) {
    // Test that if two rules are applied, the time series model should be shifted twice.
    constexpr core_t::TTime timeShift1{3600};
    constexpr core_t::TTime timeShift2{7200};

    const TMockModelPtr model{initializeModel(m_ResourceMonitor)};
    const auto& trendModel = static_cast<const maths::time_series::CTimeSeriesDecomposition&>(
        static_cast<const maths::time_series::CUnivariateTimeSeriesModel*>(model->model(0))
            ->trendModel());

    core_t::TTime timestamp{100};
    CRuleCondition conditionGte;
    conditionGte.appliesTo(CRuleCondition::E_Time);
    conditionGte.op(CRuleCondition::E_GTE);
    conditionGte.value(static_cast<double>(timestamp));

    // When time shift rule is applied twice
    CDetectionRule rule1;
    rule1.addCondition(conditionGte);
    rule1.addTimeShift(timeShift1);
    rule1.executeCallback(*model, timestamp);

    const core_t::TTime lastValueTimeAfterFirstShift = trendModel.lastValueTime();

    CDetectionRule rule2;
    rule2.addCondition(conditionGte);
    rule2.addTimeShift(timeShift2);
    rule2.executeCallback(*model, timestamp);

    // the values after the second time should be the sum of two rules.
    timestamp += timeShift1; // simulate the time has moved forward by the time shift
    rule2.executeCallback(*model, timestamp);
    BOOST_TEST_REQUIRE(trendModel.lastValueTime() == lastValueTimeAfterFirstShift + timeShift2);
    BOOST_TEST_REQUIRE(trendModel.timeShift() == timeShift1 + timeShift2);
}

BOOST_FIXTURE_TEST_CASE(testChecksum, CTestFixture) {
    // Create two identical rules
    CDetectionRule rule1;
    CDetectionRule rule2;

    // Compute checksums
    std::uint64_t checksum1 = rule1.checksum();
    std::uint64_t checksum2 = rule2.checksum();

    // Verify that identical rules have the same checksum
    BOOST_REQUIRE_EQUAL(checksum1, checksum2);

    // Test actions
    // Modify the action of rule2
    rule2.action(CDetectionRule::E_SkipModelUpdate);

    // Verify that different actions result in different checksums
    checksum1 = rule1.checksum();
    checksum2 = rule2.checksum();
    BOOST_REQUIRE_NE(checksum1, checksum2);

    // Test conditions
    // Reset rule2 to be identical to rule1
    rule2 = rule1;

    // Add a condition to rule2
    CRuleCondition condition;
    condition.appliesTo(CRuleCondition::E_Actual);
    condition.op(CRuleCondition::E_GT);
    condition.value(100.0);
    rule2.addCondition(condition);

    // Verify that adding a condition changes the checksum
    checksum1 = rule1.checksum();
    checksum2 = rule2.checksum();
    BOOST_REQUIRE_NE(checksum1, checksum2);

    // Add the same condition to rule1
    rule1.addCondition(condition);

    // Verify that identical conditions result in the same checksum
    checksum1 = rule1.checksum();
    checksum2 = rule2.checksum();
    BOOST_REQUIRE_EQUAL(checksum1, checksum2);

    // Modify the condition in rule2
    condition.value(200.0);
    rule2.clearConditions();
    rule2.addCondition(condition);

    // Verify that different condition values result in different checksums
    checksum1 = rule1.checksum();
    checksum2 = rule2.checksum();
    BOOST_REQUIRE_NE(checksum1, checksum2);

    // Test Scope
    rule2 = rule1;

    // Modify the scope of rule2
    const std::string fieldName = "user";
    core::CPatternSet valueFilter;
    valueFilter.initFromPatternList({"admin"});
    rule2.includeScope(fieldName, valueFilter);

    // Verify that different scopes result in different checksums
    checksum1 = rule1.checksum();
    checksum2 = rule2.checksum();
    BOOST_REQUIRE_NE(checksum1, checksum2);

    // Add the same scope to rule1
    rule1.includeScope(fieldName, valueFilter);

    // Verify that identical scopes result in the same checksum
    checksum1 = rule1.checksum();
    checksum2 = rule2.checksum();
    BOOST_REQUIRE_EQUAL(checksum1, checksum2);

    // Test Time Shift
    // Modify the time shift in rule2
    rule2.addTimeShift(3600);

    // Verify that different time shifts result in different checksums
    checksum1 = rule1.checksum();
    checksum2 = rule2.checksum();
    BOOST_REQUIRE_NE(checksum1, checksum2);

    // Add the same time shift to rule1
    rule1.addTimeShift(3600);

    // Verify that identical time shifts result in the same checksum
    checksum1 = rule1.checksum();
    checksum2 = rule2.checksum();
    BOOST_REQUIRE_EQUAL(checksum1, checksum2);
}

BOOST_AUTO_TEST_SUITE_END()
