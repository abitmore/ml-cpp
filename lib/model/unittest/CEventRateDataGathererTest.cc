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
#include <core/CRegex.h>

#include <maths/common/COrderings.h>

#include <model/CDataGatherer.h>
#include <model/CEventData.h>
#include <model/CEventRateBucketGatherer.h>
#include <model/CResourceMonitor.h>
#include <model/CSearchKey.h>
#include <model/ModelTypes.h>
#include <model/SModelParams.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <utility>
#include <vector>

#include "ModelTestHelpers.h"

BOOST_AUTO_TEST_SUITE(CEventRateDataGathererTest)

using namespace ml;
using namespace model;

using TSizeVec = std::vector<std::size_t>;
using TFeatureVec = std::vector<model_t::EFeature>;
using TSizeUInt64Pr = std::pair<std::size_t, std::uint64_t>;
using TSizeUInt64PrVec = std::vector<TSizeUInt64Pr>;
using TStrVec = std::vector<std::string>;
using TStrVecCItr = TStrVec::const_iterator;
using TStrVecVec = std::vector<TStrVec>;
using TFeatureData = SEventRateFeatureData;
using TSizeFeatureDataPr = std::pair<std::size_t, TFeatureData>;
using TSizeFeatureDataPrVec = std::vector<TSizeFeatureDataPr>;
using TFeatureSizeFeatureDataPrVecPr = std::pair<model_t::EFeature, TSizeFeatureDataPrVec>;
using TFeatureSizeFeatureDataPrVecPrVec = std::vector<TFeatureSizeFeatureDataPrVecPr>;
using TSizeSizePr = std::pair<std::size_t, std::size_t>;
using TSizeSizePrFeatureDataPr = std::pair<TSizeSizePr, TFeatureData>;
using TSizeSizePrFeatureDataPrVec = std::vector<TSizeSizePrFeatureDataPr>;
using TFeatureSizeSizePrFeatureDataPrVecPr =
    std::pair<model_t::EFeature, TSizeSizePrFeatureDataPrVec>;
using TFeatureSizeSizePrFeatureDataPrVecPrVec = std::vector<TFeatureSizeSizePrFeatureDataPrVecPr>;
using TSizeSizePrOptionalStrPr = CBucketGatherer::TSizeSizePrOptionalStrPr;
using TSizeSizePrOptionalStrPrUInt64UMapVec = CBucketGatherer::TSizeSizePrOptionalStrPrUInt64UMapVec;
using TTimeVec = std::vector<core_t::TTime>;
using TStrCPtrVec = CBucketGatherer::TStrCPtrVec;

namespace {

const CSearchKey key;
const std::string EMPTY_STRING;

std::size_t addPerson(CDataGatherer& gatherer,
                      CResourceMonitor& resourceMonitor,
                      const std::string& p,
                      const std::string& v = EMPTY_STRING,
                      const std::size_t numInfluencers = 0) {
    CDataGatherer::TStrCPtrVec person;
    person.push_back(&p);
    std::string const i("i");
    for (std::size_t j = 0; j < numInfluencers; ++j) {
        person.push_back(&i);
    }
    if (!v.empty()) {
        person.push_back(&v);
    }
    CEventData result;
    gatherer.processFields(person, result, resourceMonitor);
    return *result.personId();
}

void addArrival(CDataGatherer& gatherer,
                CResourceMonitor& resourceMonitor,
                const core_t::TTime time,
                const std::string& person) {
    CDataGatherer::TStrCPtrVec fieldValues;
    fieldValues.push_back(&person);

    CEventData eventData;
    eventData.time(time);

    gatherer.addArrival(fieldValues, eventData, resourceMonitor);
}

void addArrival(CDataGatherer& gatherer,
                CResourceMonitor& resourceMonitor,
                const core_t::TTime time,
                const std::string& person,
                const std::string& attribute) {
    CDataGatherer::TStrCPtrVec fieldValues;
    fieldValues.push_back(&person);
    fieldValues.push_back(&attribute);

    CEventData eventData;
    eventData.time(time);

    gatherer.addArrival(fieldValues, eventData, resourceMonitor);
}

void addArrival(CDataGatherer& gatherer,
                CResourceMonitor& resourceMonitor,
                const core_t::TTime time,
                const std::string& person,
                const std::string& value,
                const std::string& influencer) {
    CDataGatherer::TStrCPtrVec fieldValues;
    fieldValues.push_back(&person);
    fieldValues.push_back(&influencer);
    fieldValues.push_back(&value);

    CEventData eventData;
    eventData.time(time);

    gatherer.addArrival(fieldValues, eventData, resourceMonitor);
}

void addArrival(CDataGatherer& gatherer,
                CResourceMonitor& resourceMonitor,
                const core_t::TTime time,
                const std::string& person,
                const TStrVec& influencers,
                const std::string& value) {
    CDataGatherer::TStrCPtrVec fieldValues;
    fieldValues.push_back(&person);

    for (const auto& influencer : influencers) {
        fieldValues.push_back(&influencer);
    }

    if (!value.empty()) {
        fieldValues.push_back(&value);
    }

    CEventData eventData;
    eventData.time(time);

    gatherer.addArrival(fieldValues, eventData, resourceMonitor);
}

void testInfluencerPerFeature(const model_t::EFeature feature,
                              const TTimeVec& data,
                              const TStrVecVec& influencers,
                              const TStrVec& expected,
                              const std::string& valueField,
                              CResourceMonitor& resourceMonitor) {
    LOG_DEBUG(<< " *** testing " << model_t::print(feature) << " ***");

    constexpr core_t::TTime startTime = 0;
    constexpr core_t::TTime bucketLength = 600;
    SModelParams const params(bucketLength);

    TFeatureVec features;
    features.push_back(feature);
    TStrVec influencerFieldNames;
    influencerFieldNames.emplace_back("IF1");
    CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                  params, key, startTime)
                                 .influenceFieldNames(influencerFieldNames)
                                 .valueFieldName(valueField)
                                 .build();

    BOOST_TEST_REQUIRE(!gatherer.isPopulation());
    BOOST_REQUIRE_EQUAL(0, addPerson(gatherer, resourceMonitor, "p", valueField, 1));

    BOOST_REQUIRE_EQUAL(1, gatherer.numberFeatures());
    for (std::size_t i = 0; i < features.size(); ++i) {
        BOOST_REQUIRE_EQUAL(features[i], gatherer.feature(i));
    }

    testGathererAttributes(gatherer, startTime, bucketLength);

    core_t::TTime time = startTime;
    for (std::size_t i = 0, j = 0; i < data.size(); ++i) {
        for (/**/; j < 5 && data[i] >= time + bucketLength;
             time += bucketLength, ++j, gatherer.timeNow(time)) {
            LOG_DEBUG(<< "Processing bucket [" << time << ", " << time + bucketLength << ")");

            TFeatureSizeFeatureDataPrVecPrVec featureData;
            gatherer.featureData(time, bucketLength, featureData);
            LOG_DEBUG(<< "featureData = " << featureData);
            BOOST_REQUIRE_EQUAL(1, featureData.size());

            BOOST_REQUIRE_EQUAL(feature, featureData[0].first);
            BOOST_REQUIRE_EQUAL(expected[j],
                                core::CContainerPrinter::print(featureData[0].second));

            testPersistence(params, gatherer, model_t::E_EventRate);
        }

        if (j < 5) {
            addArrival(gatherer, resourceMonitor, data[i], "p", influencers[i],
                       valueField.empty() ? EMPTY_STRING : "value");
        }
    }
}

void importCsvData(CDataGatherer& gatherer,
                   CResourceMonitor& resourceMonitor,
                   const std::string& filename,
                   const TSizeVec& fields) {
    auto ifs(std::make_shared<std::ifstream>(filename.c_str()));
    BOOST_TEST_REQUIRE(ifs->is_open());

    core::CRegex regex;
    BOOST_TEST_REQUIRE(regex.init(","));

    std::string line;
    // read the header
    BOOST_TEST_REQUIRE(std::getline(*ifs, line).good());

    while (std::getline(*ifs, line)) {
        LOG_TRACE(<< "Got string: " << line);
        core::CRegex::TStrVec tokens;
        regex.split(line, tokens);

        core_t::TTime time;
        BOOST_TEST_REQUIRE(core::CStringUtils::stringToType(tokens[0], time));

        CDataGatherer::TStrCPtrVec fieldValues;
        CEventData data;
        data.time(time);
        for (const auto field : fields) {
            fieldValues.push_back(&tokens[field]);
        }
        gatherer.addArrival(fieldValues, data, resourceMonitor);
    }
    ifs.reset();
}

struct STestTimes {
    core_t::TTime s_StartTime;
    core_t::TTime s_BucketLength;
};

struct STestData {
    std::vector<core_t::TTime> data1;
    std::vector<core_t::TTime> data2;
};

void testGathererMultipleSeries(const STestTimes& testTimes,
                                const STestData& testData,
                                const std::vector<std::string>& expectedPersonCounts,
                                const SModelParams& params,
                                core_t::TTime upperLimit,
                                CDataGatherer& gatherer,
                                CResourceMonitor& resourceMonitor) {

    const core_t::TTime startTime = testTimes.s_StartTime;
    const core_t::TTime bucketLength = testTimes.s_BucketLength;

    BOOST_REQUIRE_EQUAL(0, addPerson(gatherer, resourceMonitor, "p1"));
    BOOST_REQUIRE_EQUAL(1, addPerson(gatherer, resourceMonitor, "p2"));

    core_t::TTime time = startTime;

    std::size_t i1 = 0U;
    std::size_t i2 = 0U;
    std::size_t j = 0U;
    for (;;) {
        for (/**/; j < 5 && std::min(testData.data1[i1], testData.data2[i2]) >= time + upperLimit;
             time += bucketLength, ++j) {
            LOG_DEBUG(<< "Processing bucket [" << time << ", " << time + bucketLength << ")");

            TFeatureSizeFeatureDataPrVecPrVec featureData;
            gatherer.featureData(time, bucketLength, featureData);
            LOG_DEBUG(<< "featureData = " << featureData);
            BOOST_REQUIRE_EQUAL(1, featureData.size());
            BOOST_REQUIRE_EQUAL(model_t::E_IndividualCountByBucketAndPerson,
                                featureData[0].first);
            BOOST_REQUIRE_EQUAL(expectedPersonCounts[j],
                                core::CContainerPrinter::print(featureData[0].second));

            testPersistence(params, gatherer, model_t::E_EventRate);
        }

        if (j >= 5) {
            break;
        }

        if (testData.data1[i1] < testData.data2[i2]) {
            LOG_DEBUG(<< "Adding arrival for p1 at " << testData.data1[i1]);
            addArrival(gatherer, resourceMonitor, testData.data1[i1], "p1");
            ++i1;
        } else {
            LOG_DEBUG(<< "Adding arrival for p2 at " << testData.data2[i2]);
            addArrival(gatherer, resourceMonitor, testData.data2[i2], "p2");
            ++i2;
        }
    }

    TSizeVec peopleToRemove;
    peopleToRemove.push_back(1);
    gatherer.recyclePeople(peopleToRemove);

    BOOST_REQUIRE_EQUAL(1, gatherer.numberActivePeople());
    BOOST_REQUIRE_EQUAL(std::string("p1"), gatherer.personName(0));
    BOOST_REQUIRE_EQUAL(std::string("-"), gatherer.personName(1));
    std::size_t pid;
    BOOST_TEST_REQUIRE(gatherer.personId("p1", pid));
    BOOST_REQUIRE_EQUAL(0, pid);
    BOOST_TEST_REQUIRE(!gatherer.personId("p2", pid));

    TFeatureSizeFeatureDataPrVecPrVec featureData;
    gatherer.featureData(startTime + (4 * bucketLength), bucketLength, featureData);
    LOG_DEBUG(<< "featureData = " << featureData);
    BOOST_REQUIRE_EQUAL(1, featureData.size());
    BOOST_REQUIRE_EQUAL(model_t::E_IndividualCountByBucketAndPerson,
                        featureData[0].first);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 3)]"),
                        core::CContainerPrinter::print(featureData[0].second));
}

void testGathererMultipleSeries(const core_t::TTime startTime,
                                const core_t::TTime bucketLength,
                                CDataGatherer& gatherer,
                                CResourceMonitor& resourceMonitor) {
    TFeatureVec features;
    features.push_back(model_t::E_IndividualCountByBucketAndPerson);

    BOOST_REQUIRE_EQUAL(0, addPerson(gatherer, resourceMonitor, "p1"));
    BOOST_REQUIRE_EQUAL(1, addPerson(gatherer, resourceMonitor, "p2"));
    BOOST_REQUIRE_EQUAL(2, addPerson(gatherer, resourceMonitor, "p3"));
    BOOST_REQUIRE_EQUAL(3, addPerson(gatherer, resourceMonitor, "p4"));
    BOOST_REQUIRE_EQUAL(4, addPerson(gatherer, resourceMonitor, "p5"));

    for (std::size_t i = 0; i < 5; ++i) {
        addArrival(gatherer, resourceMonitor, startTime, gatherer.personName(i));
    }
    addArrival(gatherer, resourceMonitor, startTime + 1, gatherer.personName(2));
    addArrival(gatherer, resourceMonitor, startTime + 2, gatherer.personName(4));
    addArrival(gatherer, resourceMonitor, startTime + 3, gatherer.personName(4));

    const TSizeUInt64PrVec personCounts;

    TFeatureSizeFeatureDataPrVecPrVec featureData;
    gatherer.featureData(startTime, bucketLength, featureData);
    LOG_DEBUG(<< "featureData = " << featureData);
    BOOST_REQUIRE_EQUAL(1, featureData.size());
    BOOST_REQUIRE_EQUAL(model_t::E_IndividualCountByBucketAndPerson,
                        featureData[0].first);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1), (1, 1), (2, 2), (3, 1), (4, 3)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    TSizeVec peopleToRemove;
    peopleToRemove.push_back(0);
    peopleToRemove.push_back(1);
    peopleToRemove.push_back(3);
    gatherer.recyclePeople(peopleToRemove);

    BOOST_REQUIRE_EQUAL(2, gatherer.numberActivePeople());
    BOOST_REQUIRE_EQUAL(std::string("p3"), gatherer.personName(2));
    BOOST_REQUIRE_EQUAL(std::string("p5"), gatherer.personName(4));
    BOOST_REQUIRE_EQUAL(std::string("-"), gatherer.personName(0));
    BOOST_REQUIRE_EQUAL(std::string("-"), gatherer.personName(1));
    BOOST_REQUIRE_EQUAL(std::string("-"), gatherer.personName(3));
    std::size_t pid;
    BOOST_TEST_REQUIRE(gatherer.personId("p3", pid));
    BOOST_REQUIRE_EQUAL(2, pid);
    BOOST_TEST_REQUIRE(gatherer.personId("p5", pid));
    BOOST_REQUIRE_EQUAL(4, pid);

    gatherer.featureData(startTime, bucketLength, featureData);
    LOG_DEBUG(<< "featureData = " << featureData);
    BOOST_REQUIRE_EQUAL(1, featureData.size());
    BOOST_REQUIRE_EQUAL(model_t::E_IndividualCountByBucketAndPerson,
                        featureData[0].first);
    BOOST_REQUIRE_EQUAL(std::string("[(2, 2), (4, 3)]"),
                        core::CContainerPrinter::print(featureData[0].second));
}

} // namespace

class CTestFixture {
protected:
    CResourceMonitor m_ResourceMonitor;
};

BOOST_FIXTURE_TEST_CASE(testLatencyPersist, CTestFixture) {
    constexpr core_t::TTime bucketLength = 3600;
    constexpr core_t::TTime latency = 5 * bucketLength;
    constexpr core_t::TTime startTime = 1420192800;
    SModelParams params(bucketLength);
    params.configureLatency(latency, bucketLength);

    {
        // Create a gatherer, no influences
        TFeatureVec features;
        features.push_back(model_t::E_IndividualUniqueCountByBucketAndPerson);
        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .personFieldName("program")
                                     .valueFieldName("file")
                                     .build();
        TSizeVec fields;
        fields.push_back(2);
        fields.push_back(1);

        importCsvData(gatherer, m_ResourceMonitor,
                      "testfiles/files_users_programs.csv", fields);

        testPersistence(params, gatherer, model_t::E_EventRate);
    }
    {
        // Create a gatherer, with influences
        TFeatureVec features;
        TStrVec influencers;
        influencers.emplace_back("user");
        features.push_back(model_t::E_IndividualUniqueCountByBucketAndPerson);
        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .personFieldName("program")
                                     .valueFieldName("file")
                                     .build();
        TSizeVec fields;
        fields.push_back(2);
        fields.push_back(3);
        fields.push_back(1);

        importCsvData(gatherer, m_ResourceMonitor,
                      "testfiles/files_users_programs.csv", fields);

        testPersistence(params, gatherer, model_t::E_EventRate);
    }
    {
        // Create a gatherer, no influences
        TFeatureVec features;
        features.push_back(model_t::E_IndividualNonZeroCountByBucketAndPerson);
        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .personFieldName("program")
                                     .build();
        TSizeVec fields;
        fields.push_back(2);

        importCsvData(gatherer, m_ResourceMonitor,
                      "testfiles/files_users_programs.csv", fields);

        testPersistence(params, gatherer, model_t::E_EventRate);
    }
    {
        // Create a gatherer, with influences
        TFeatureVec features;
        TStrVec influencers;
        influencers.emplace_back("user");
        features.push_back(model_t::E_IndividualNonZeroCountByBucketAndPerson);
        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .personFieldName("program")
                                     .influenceFieldNames(influencers)
                                     .build();

        TSizeVec fields;
        fields.push_back(2);
        fields.push_back(3);

        importCsvData(gatherer, m_ResourceMonitor,
                      "testfiles/files_users_programs.csv", fields);

        testPersistence(params, gatherer, model_t::E_EventRate);
    }
}

BOOST_FIXTURE_TEST_CASE(testSingleSeries, CTestFixture) {

    // Test that the various statistics come back as we expect.

    constexpr core_t::TTime startTime = 0;
    constexpr core_t::TTime bucketLength = 600;
    SModelParams const params(bucketLength);

    constexpr std::array<core_t::TTime, 15> data = {
        1, 15, 180, 190, 400,
        550, // bucket 1
        600, 799,
        1199, // bucket 2
        1200,
        1250, // bucket 3
              // bucket 4
        2420, 2480,
        2490, // bucket 5
        10000 // sentinel
    };

    std::array const expectedPersonCounts{
        std::string("[(0, 6)]"), std::string("[(0, 3)]"), std::string("[(0, 2)]"),
        std::string("[(0, 0)]"), std::string("[(0, 3)]")};

    std::array const expectedPersonNonZeroCounts{
        std::string("[(0, 6)]"), std::string("[(0, 3)]"),
        std::string("[(0, 2)]"), std::string("[]"), std::string("[(0, 3)]")};

    std::array const expectedPersonIndicator{
        std::string("[(0, 1)]"), std::string("[(0, 1)]"),
        std::string("[(0, 1)]"), std::string("[]"), std::string("[(0, 1)]")};

    // Test the count by bucket and person and bad feature
    // (which should be ignored).
    {
        TFeatureVec features;
        features.push_back(model_t::E_IndividualCountByBucketAndPerson);
        features.push_back(model_t::E_IndividualMinByPerson);
        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .build();
        BOOST_TEST_REQUIRE(!gatherer.isPopulation());
        BOOST_REQUIRE_EQUAL(0, addPerson(gatherer, m_ResourceMonitor, "p"));

        BOOST_REQUIRE_EQUAL(1, gatherer.numberFeatures());
        for (std::size_t i = 0; i < 1; ++i) {
            BOOST_REQUIRE_EQUAL(features[i], gatherer.feature(i));
        }
        BOOST_TEST_REQUIRE(gatherer.hasFeature(model_t::E_IndividualCountByBucketAndPerson));
        BOOST_TEST_REQUIRE(!gatherer.hasFeature(model_t::E_IndividualMinByPerson));

        testGathererAttributes(gatherer, startTime, bucketLength);

        core_t::TTime time = startTime;
        for (std::size_t i = 0, j = 0; i < std::size(data); ++i) {
            for (/**/; j < 5 && data[i] >= time + bucketLength;
                 time += bucketLength, ++j, gatherer.timeNow(time)) {
                LOG_DEBUG(<< "Processing bucket [" << time << ", "
                          << time + bucketLength << ")");

                TFeatureSizeFeatureDataPrVecPrVec featureData;
                gatherer.featureData(time, bucketLength, featureData);
                LOG_DEBUG(<< "featureData = " << featureData);
                BOOST_REQUIRE_EQUAL(1, featureData.size());
                BOOST_REQUIRE_EQUAL(model_t::E_IndividualCountByBucketAndPerson,
                                    featureData[0].first);
                BOOST_REQUIRE_EQUAL(
                    expectedPersonCounts[j],
                    core::CContainerPrinter::print(featureData[0].second));

                testPersistence(params, gatherer, model_t::E_EventRate);
            }

            if (j < 5) {
                addArrival(gatherer, m_ResourceMonitor, data[i], "p");
            }
        }
    }

    // Test non-zero count and person bucket count.
    {
        TFeatureVec features;
        features.push_back(model_t::E_IndividualNonZeroCountByBucketAndPerson);
        features.push_back(model_t::E_IndividualTotalBucketCountByPerson);
        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .build();
        BOOST_REQUIRE_EQUAL(0, addPerson(gatherer, m_ResourceMonitor, "p"));

        core_t::TTime time = startTime;
        for (std::size_t i = 0, j = 0; i < std::size(data); ++i) {
            for (/**/; j < 5 && data[i] >= time + bucketLength;
                 time += bucketLength, ++j, gatherer.timeNow(time)) {
                LOG_DEBUG(<< "Processing bucket [" << time << ", "
                          << time + bucketLength << ")");

                TFeatureSizeFeatureDataPrVecPrVec featureData;
                gatherer.featureData(time, bucketLength, featureData);
                LOG_DEBUG(<< "featureData = " << featureData);
                BOOST_REQUIRE_EQUAL(2, featureData.size());
                BOOST_REQUIRE_EQUAL(model_t::E_IndividualNonZeroCountByBucketAndPerson,
                                    featureData[0].first);
                BOOST_REQUIRE_EQUAL(
                    expectedPersonNonZeroCounts[j],
                    core::CContainerPrinter::print(featureData[0].second));
                BOOST_REQUIRE_EQUAL(model_t::E_IndividualTotalBucketCountByPerson,
                                    featureData[1].first);
                BOOST_REQUIRE_EQUAL(
                    expectedPersonNonZeroCounts[j],
                    core::CContainerPrinter::print(featureData[1].second));

                testPersistence(params, gatherer, model_t::E_EventRate);
            }

            if (j < 5) {
                addArrival(gatherer, m_ResourceMonitor, data[i], "p");
            }
        }
    }

    // Test person indicator by bucket.
    {
        TFeatureVec features;
        features.push_back(model_t::E_IndividualIndicatorOfBucketPerson);
        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .build();
        BOOST_REQUIRE_EQUAL(0, addPerson(gatherer, m_ResourceMonitor, "p"));

        core_t::TTime time = startTime;
        for (std::size_t i = 0, j = 0; i < std::size(data); ++i) {
            for (/**/; j < 5 && data[i] >= time + bucketLength;
                 time += bucketLength, ++j, gatherer.timeNow(time)) {
                LOG_DEBUG(<< "Processing bucket [" << time << ", "
                          << time + bucketLength << ")");

                TFeatureSizeFeatureDataPrVecPrVec featureData;
                gatherer.featureData(time, bucketLength, featureData);
                LOG_DEBUG(<< "featureData = " << featureData);
                BOOST_REQUIRE_EQUAL(1, featureData.size());
                BOOST_REQUIRE_EQUAL(model_t::E_IndividualIndicatorOfBucketPerson,
                                    featureData[0].first);
                BOOST_REQUIRE_EQUAL(
                    expectedPersonIndicator[j],
                    core::CContainerPrinter::print(featureData[0].second));

                testPersistence(params, gatherer, model_t::E_EventRate);
            }

            if (j < 5) {
                addArrival(gatherer, m_ResourceMonitor, data[i], "p");
            }
        }
    }
}

BOOST_FIXTURE_TEST_CASE(testMultipleSeries, CTestFixture) {

    // Test that the various statistics come back as we expect
    // for multiple people.

    constexpr core_t::TTime startTime = 0;
    constexpr core_t::TTime bucketLength = 600;

    const std::vector<core_t::TTime> data1 = {
        1,    15,   180, 190, 400,
        550, // bucket 1
        600,  799,
        1199, // bucket 2
        1200,
        1250, // bucket 3
        1900, // bucket 4
        2420, 2480,
        2490, // bucket 5
        10000 // sentinel
    };
    const std::vector<core_t::TTime> data2 = {
        1,    5,    15,   25,   180,  190,  400, 550, // bucket 1
        600,  605,  609,  799,  1199,                 // bucket 2
        1200, 1250, 1255, 1256, 1300, 1400,           // bucket 3
        1900, 1950,                                   // bucket 4
        2420, 2480, 2490, 2500, 2550, 2600,           // bucket 5
        10000                                         // sentinel
    };

    const std::vector expectedPersonCounts = {
        std::string("[(0, 6), (1, 8)]"), std::string("[(0, 3), (1, 5)]"),
        std::string("[(0, 2), (1, 6)]"), std::string("[(0, 1), (1, 2)]"),
        std::string("[(0, 3), (1, 6)]")};

    const SModelParams params(bucketLength);

    {
        TFeatureVec features;
        features.push_back(model_t::E_IndividualCountByBucketAndPerson);

        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .build();
        testGathererMultipleSeries(
            STestTimes{.s_StartTime = startTime, .s_BucketLength = bucketLength},
            STestData{.data1 = data1, .data2 = data2}, expectedPersonCounts,
            params, bucketLength, gatherer, m_ResourceMonitor);

        BOOST_REQUIRE_EQUAL(1, gatherer.numberByFieldValues());
    }

    {
        TFeatureVec features;
        features.push_back(model_t::E_IndividualCountByBucketAndPerson);

        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .build();
        testGathererMultipleSeries(startTime, bucketLength, gatherer, m_ResourceMonitor);

        BOOST_REQUIRE_EQUAL(2, gatherer.numberByFieldValues());
    }
}

BOOST_FIXTURE_TEST_CASE(testRemovePeople, CTestFixture) {
    // Test various combinations of removed people.

    constexpr core_t::TTime startTime = 0;
    constexpr core_t::TTime bucketLength = 600;

    TFeatureVec features;
    features.push_back(model_t::E_IndividualCountByBucketAndPerson);
    features.push_back(model_t::E_IndividualNonZeroCountByBucketAndPerson);
    features.push_back(model_t::E_IndividualTotalBucketCountByPerson);
    features.push_back(model_t::E_IndividualIndicatorOfBucketPerson);
    features.push_back(model_t::E_IndividualLowCountsByBucketAndPerson);
    features.push_back(model_t::E_IndividualHighCountsByBucketAndPerson);
    const SModelParams params(bucketLength);
    CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                  params, key, startTime)
                                 .build();
    BOOST_REQUIRE_EQUAL(0, addPerson(gatherer, m_ResourceMonitor, "p1"));
    BOOST_REQUIRE_EQUAL(1, addPerson(gatherer, m_ResourceMonitor, "p2"));
    BOOST_REQUIRE_EQUAL(2, addPerson(gatherer, m_ResourceMonitor, "p3"));
    BOOST_REQUIRE_EQUAL(3, addPerson(gatherer, m_ResourceMonitor, "p4"));
    BOOST_REQUIRE_EQUAL(4, addPerson(gatherer, m_ResourceMonitor, "p5"));
    BOOST_REQUIRE_EQUAL(5, addPerson(gatherer, m_ResourceMonitor, "p6"));
    BOOST_REQUIRE_EQUAL(6, addPerson(gatherer, m_ResourceMonitor, "p7"));
    BOOST_REQUIRE_EQUAL(7, addPerson(gatherer, m_ResourceMonitor, "p8"));

    constexpr std::array<core_t::TTime, 8> counts = {0, 3, 5, 2, 0, 5, 7, 10};
    for (std::size_t i = 0; i < std::size(counts); ++i) {
        for (core_t::TTime time = 0; time < counts[i]; ++time) {
            addArrival(gatherer, m_ResourceMonitor, startTime + time,
                       gatherer.personName(i));
        }
    }

    {
        TSizeVec peopleToRemove;
        peopleToRemove.push_back(0);
        peopleToRemove.push_back(1);
        gatherer.recyclePeople(peopleToRemove);

        CDataGatherer expectedGatherer =
            CDataGathererBuilder(model_t::E_EventRate, features, params, key, startTime)
                .build();
        BOOST_REQUIRE_EQUAL(0, addPerson(expectedGatherer, m_ResourceMonitor, "p3"));
        BOOST_REQUIRE_EQUAL(1, addPerson(expectedGatherer, m_ResourceMonitor, "p4"));
        BOOST_REQUIRE_EQUAL(2, addPerson(expectedGatherer, m_ResourceMonitor, "p5"));
        BOOST_REQUIRE_EQUAL(3, addPerson(expectedGatherer, m_ResourceMonitor, "p6"));
        BOOST_REQUIRE_EQUAL(4, addPerson(expectedGatherer, m_ResourceMonitor, "p7"));
        BOOST_REQUIRE_EQUAL(5, addPerson(expectedGatherer, m_ResourceMonitor, "p8"));

        constexpr std::array<core_t::TTime, 6> expectedCounts = {5, 2, 0,
                                                                 5, 7, 10};
        for (std::size_t i = 0; i < std::size(expectedCounts); ++i) {
            for (core_t::TTime time = 0; time < expectedCounts[i]; ++time) {
                addArrival(expectedGatherer, m_ResourceMonitor,
                           startTime + time, expectedGatherer.personName(i));
            }
        }

        LOG_DEBUG(<< "checksum          = " << gatherer.checksum());
        LOG_DEBUG(<< "expected checksum = " << expectedGatherer.checksum());
        BOOST_REQUIRE_EQUAL(gatherer.checksum(), expectedGatherer.checksum());
    }
    {
        TSizeVec peopleToRemove;
        peopleToRemove.push_back(3);
        peopleToRemove.push_back(4);
        peopleToRemove.push_back(7);
        gatherer.recyclePeople(peopleToRemove);

        CDataGatherer expectedGatherer =
            CDataGathererBuilder(model_t::E_EventRate, features, params, key, startTime)
                .build();
        BOOST_REQUIRE_EQUAL(0, addPerson(expectedGatherer, m_ResourceMonitor, "p3"));
        BOOST_REQUIRE_EQUAL(1, addPerson(expectedGatherer, m_ResourceMonitor, "p6"));
        BOOST_REQUIRE_EQUAL(2, addPerson(expectedGatherer, m_ResourceMonitor, "p7"));

        constexpr std::array<core_t::TTime, 3> expectedCounts = {5, 5, 7};
        for (std::size_t i = 0; i < std::size(expectedCounts); ++i) {
            for (core_t::TTime time = 0; time < expectedCounts[i]; ++time) {
                addArrival(expectedGatherer, m_ResourceMonitor,
                           startTime + time, expectedGatherer.personName(i));
            }
        }

        LOG_DEBUG(<< "checksum          = " << gatherer.checksum());
        LOG_DEBUG(<< "expected checksum = " << expectedGatherer.checksum());
        BOOST_REQUIRE_EQUAL(gatherer.checksum(), expectedGatherer.checksum());
    }
    {
        TSizeVec peopleToRemove;
        peopleToRemove.push_back(2);
        peopleToRemove.push_back(5);
        peopleToRemove.push_back(6);
        gatherer.recyclePeople(peopleToRemove);

        const CDataGatherer expectedGatherer =
            CDataGathererBuilder(model_t::E_EventRate, features, params, key, startTime)
                .build();

        LOG_DEBUG(<< "checksum          = " << gatherer.checksum());
        LOG_DEBUG(<< "expected checksum = " << expectedGatherer.checksum());
        BOOST_REQUIRE_EQUAL(gatherer.checksum(), expectedGatherer.checksum());
    }

    TSizeVec expectedRecycled;
    expectedRecycled.push_back(addPerson(gatherer, m_ResourceMonitor, "p1"));
    expectedRecycled.push_back(addPerson(gatherer, m_ResourceMonitor, "p7"));

    LOG_DEBUG(<< "recycled          = " << gatherer.recycledPersonIds());
    LOG_DEBUG(<< "expected recycled = " << expectedRecycled);
    BOOST_REQUIRE_EQUAL(core::CContainerPrinter::print(expectedRecycled),
                        core::CContainerPrinter::print(gatherer.recycledPersonIds()));
}

BOOST_FIXTURE_TEST_CASE(testSingleSeriesOutOfOrderFinalResult, CTestFixture) {

    // Test that the various statistics come back as we expect.

    constexpr core_t::TTime startTime = 0;
    constexpr core_t::TTime bucketLength = 600;
    constexpr std::size_t latencyBuckets(3);
    constexpr core_t::TTime latencyTime =
        bucketLength * static_cast<core_t::TTime>(latencyBuckets);
    SModelParams params(bucketLength);
    params.s_LatencyBuckets = latencyBuckets;

    const std::array<core_t::TTime, 15> data = {
        1, 180, 1200, 190, 400,
        600, // bucket 1, 2 & 3
        550, 799, 1199,
        15,   // bucket 1 & 2
        2490, // bucket 5
              // bucket 4 is empty
        2420, 2480,
        1250, // bucket 3 & 5
        10000 // sentinel
    };

    const std::array expectedPersonCounts = {
        std::string("[(0, 6)]"), std::string("[(0, 3)]"), std::string("[(0, 2)]"),
        std::string("[(0, 0)]"), std::string("[(0, 3)]")};

    const std::array expectedPersonNonZeroCounts = {
        std::string("[(0, 6)]"), std::string("[(0, 3)]"),
        std::string("[(0, 2)]"), std::string("[]"), std::string("[(0, 3)]")};

    const std::array expectedPersonIndicator = {
        std::string("[(0, 1)]"), std::string("[(0, 1)]"),
        std::string("[(0, 1)]"), std::string("[]"), std::string("[(0, 1)]")};

    // Test the count by bucket and person and bad feature
    // (which should be ignored).
    {
        TFeatureVec features;
        features.push_back(model_t::E_IndividualCountByBucketAndPerson);
        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .build();
        addPerson(gatherer, m_ResourceMonitor, "p");

        core_t::TTime time = startTime;
        for (std::size_t i = 0, j = 0; i < std::size(data); ++i) {
            for (/**/; j < 5 && data[i] >= time + latencyTime;
                 time += bucketLength, ++j, gatherer.timeNow(time)) {
                LOG_DEBUG(<< "Processing bucket [" << time << ", "
                          << time + bucketLength << ")");

                TFeatureSizeFeatureDataPrVecPrVec featureData;
                gatherer.featureData(time, bucketLength, featureData);
                LOG_DEBUG(<< "featureData = " << featureData);
                BOOST_REQUIRE_EQUAL(1, featureData.size());
                BOOST_REQUIRE_EQUAL(model_t::E_IndividualCountByBucketAndPerson,
                                    featureData[0].first);
                BOOST_REQUIRE_EQUAL(
                    expectedPersonCounts[j],
                    core::CContainerPrinter::print(featureData[0].second));

                testPersistence(params, gatherer, model_t::E_EventRate);
            }

            if (j < 5) {
                LOG_DEBUG(<< "Arriving = " << data[i]);
                addArrival(gatherer, m_ResourceMonitor, data[i], "p");
            }
        }
    }

    // Test non-zero count and person bucket count.
    {
        TFeatureVec features;
        features.push_back(model_t::E_IndividualNonZeroCountByBucketAndPerson);
        features.push_back(model_t::E_IndividualTotalBucketCountByPerson);
        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .build();
        BOOST_REQUIRE_EQUAL(0, addPerson(gatherer, m_ResourceMonitor, "p"));

        core_t::TTime time = startTime;
        for (std::size_t i = 0, j = 0; i < std::size(data); ++i) {
            for (/**/; j < 5 && data[i] >= time + latencyTime;
                 time += bucketLength, ++j, gatherer.timeNow(time)) {
                LOG_DEBUG(<< "Processing bucket [" << time << ", "
                          << time + bucketLength << ")");

                TFeatureSizeFeatureDataPrVecPrVec featureData;
                gatherer.featureData(time, bucketLength, featureData);
                LOG_DEBUG(<< "featureData = " << featureData);
                BOOST_REQUIRE_EQUAL(2, featureData.size());
                BOOST_REQUIRE_EQUAL(model_t::E_IndividualNonZeroCountByBucketAndPerson,
                                    featureData[0].first);
                BOOST_REQUIRE_EQUAL(
                    expectedPersonNonZeroCounts[j],
                    core::CContainerPrinter::print(featureData[0].second));
                BOOST_REQUIRE_EQUAL(model_t::E_IndividualTotalBucketCountByPerson,
                                    featureData[1].first);
                BOOST_REQUIRE_EQUAL(
                    expectedPersonNonZeroCounts[j],
                    core::CContainerPrinter::print(featureData[1].second));

                testPersistence(params, gatherer, model_t::E_EventRate);
            }

            if (j < 5) {
                addArrival(gatherer, m_ResourceMonitor, data[i], "p");
            }
        }
    }

    // Test person indicator by bucket.
    {
        TFeatureVec features;
        features.push_back(model_t::E_IndividualIndicatorOfBucketPerson);
        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .build();

        BOOST_REQUIRE_EQUAL(0, addPerson(gatherer, m_ResourceMonitor, "p"));

        core_t::TTime time = startTime;
        for (std::size_t i = 0, j = 0; i < std::size(data); ++i) {
            for (/**/; j < 5 && data[i] >= time + latencyTime;
                 time += bucketLength, ++j, gatherer.timeNow(time)) {
                LOG_DEBUG(<< "Processing bucket [" << time << ", "
                          << time + bucketLength << ")");

                TFeatureSizeFeatureDataPrVecPrVec featureData;
                gatherer.featureData(time, bucketLength, featureData);
                LOG_DEBUG(<< "featureData = " << featureData);
                BOOST_REQUIRE_EQUAL(1, featureData.size());
                BOOST_REQUIRE_EQUAL(model_t::E_IndividualIndicatorOfBucketPerson,
                                    featureData[0].first);
                BOOST_REQUIRE_EQUAL(
                    expectedPersonIndicator[j],
                    core::CContainerPrinter::print(featureData[0].second));

                testPersistence(params, gatherer, model_t::E_EventRate);
            }

            if (j < 5) {
                addArrival(gatherer, m_ResourceMonitor, data[i], "p");
            }
        }
    }
}

BOOST_FIXTURE_TEST_CASE(testSingleSeriesOutOfOrderInterimResult, CTestFixture) {

    constexpr core_t::TTime startTime = 0;
    constexpr core_t::TTime bucketLength = 600;
    constexpr std::size_t latencyBuckets(3);
    SModelParams params(bucketLength);
    params.s_LatencyBuckets = latencyBuckets;

    constexpr std::array<core_t::TTime, 8> data = {
        1, 1200,
        600, // bucket 1, 3 & 2
        1199,
        15,   // bucket 2 & 1
        2490, // bucket 5
              // bucket 4 is empty
        2420,
        1250 // bucket 5 & 3
    };

    TFeatureVec features;
    features.push_back(model_t::E_IndividualCountByBucketAndPerson);
    CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                  params, key, startTime)
                                 .build();

    addPerson(gatherer, m_ResourceMonitor, "p");
    TFeatureSizeFeatureDataPrVecPrVec featureData;

    // Bucket 1 only
    addArrival(gatherer, m_ResourceMonitor, data[0], "p");

    gatherer.featureData(0, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    // Bucket 1, 2 & 3
    addArrival(gatherer, m_ResourceMonitor, data[1], "p");

    gatherer.featureData(0, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));
    gatherer.featureData(600, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 0)]"),
                        core::CContainerPrinter::print(featureData[0].second));
    gatherer.featureData(1200, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    // Bucket 1, 2 & 3
    addArrival(gatherer, m_ResourceMonitor, data[2], "p");

    gatherer.featureData(0, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));
    gatherer.featureData(600, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));
    gatherer.featureData(1200, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    // Bucket 1, 2 & 3
    addArrival(gatherer, m_ResourceMonitor, data[3], "p");

    gatherer.featureData(0, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));
    gatherer.featureData(600, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 2)]"),
                        core::CContainerPrinter::print(featureData[0].second));
    gatherer.featureData(1200, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    // Bucket 1, 2 & 3
    addArrival(gatherer, m_ResourceMonitor, data[4], "p");

    gatherer.featureData(0, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 2)]"),
                        core::CContainerPrinter::print(featureData[0].second));
    gatherer.featureData(600, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 2)]"),
                        core::CContainerPrinter::print(featureData[0].second));
    gatherer.featureData(1200, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    // Bucket 3, 4 & 5
    addArrival(gatherer, m_ResourceMonitor, data[5], "p");

    gatherer.featureData(1200, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));
    gatherer.featureData(1800, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 0)]"),
                        core::CContainerPrinter::print(featureData[0].second));
    gatherer.featureData(2400, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    // Bucket 3, 4 & 5
    addArrival(gatherer, m_ResourceMonitor, data[6], "p");

    gatherer.featureData(1200, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));
    gatherer.featureData(1800, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 0)]"),
                        core::CContainerPrinter::print(featureData[0].second));
    gatherer.featureData(2400, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 2)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    // Bucket 3, 4 & 5
    addArrival(gatherer, m_ResourceMonitor, data[7], "p");

    gatherer.featureData(1200, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 2)]"),
                        core::CContainerPrinter::print(featureData[0].second));
    gatherer.featureData(1800, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 0)]"),
                        core::CContainerPrinter::print(featureData[0].second));
    gatherer.featureData(2400, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 2)]"),
                        core::CContainerPrinter::print(featureData[0].second));
}

BOOST_FIXTURE_TEST_CASE(testMultipleSeriesOutOfOrderFinalResult, CTestFixture) {

    // Test that the various statistics come back as we expect
    // for multiple people.

    constexpr core_t::TTime startTime = 0;
    constexpr core_t::TTime bucketLength = 600;
    constexpr std::size_t latencyBuckets(3);
    constexpr core_t::TTime latencyTime =
        bucketLength * static_cast<core_t::TTime>(latencyBuckets);
    SModelParams params(bucketLength);
    params.s_LatencyBuckets = latencyBuckets;

    const std::vector<core_t::TTime> data1 = {
        1,    15,   1200, 190, 400,
        550, // bucket 1, 2 & 3
        600,  1250,
        1199, // bucket 2 & 3
        180,
        799,  // bucket 1 & 2
        2480, // bucket 5
        2420, 1900,
        2490, // bucket 4 & 5
        10000 // sentinel
    };
    const std::vector<core_t::TTime> data2 = {
        1250, 5,    15,   600,  180,  190,  400, 550, // bucket 1, 2 & 3
        25,   605,  609,  799,  1199,                 // bucket 1 & 2
        1200, 1,    1255, 1950, 1400,                 // bucket 1, 3 & 4
        2550, 1300, 2500,                             // bucket 3 & 5
        2420, 2480, 2490, 1256, 1900, 2600,           // bucket 3, 4 & 5
        10000                                         // sentinel
    };

    {
        const std::vector expectedPersonCounts = {
            std::string("[(0, 6), (1, 8)]"), std::string("[(0, 3), (1, 5)]"),
            std::string("[(0, 2), (1, 6)]"), std::string("[(0, 1), (1, 2)]"),
            std::string("[(0, 3), (1, 6)]")};
        TFeatureVec features;
        features.push_back(model_t::E_IndividualCountByBucketAndPerson);
        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .build();

        testGathererMultipleSeries(
            STestTimes{.s_StartTime = startTime, .s_BucketLength = bucketLength},
            STestData{.data1 = data1, .data2 = data2}, expectedPersonCounts,
            params, latencyTime, gatherer, m_ResourceMonitor);
    }

    {
        TFeatureVec features;
        features.push_back(model_t::E_IndividualCountByBucketAndPerson);
        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .build();

        testGathererMultipleSeries(startTime, bucketLength, gatherer, m_ResourceMonitor);
    }
}

BOOST_FIXTURE_TEST_CASE(testArrivalBeforeLatencyWindowIsIgnored, CTestFixture) {
    constexpr core_t::TTime startTime = 0;
    constexpr core_t::TTime bucketLength = 600;
    constexpr std::size_t latencyBuckets(2);
    SModelParams params(bucketLength);
    params.s_LatencyBuckets = latencyBuckets;

    constexpr std::array<core_t::TTime, 2> data = {
        1800, // Bucket 4, thus bucket 1's values are already out of latency window
        1     // Bucket 1
    };

    TFeatureVec features;
    features.push_back(model_t::E_IndividualCountByBucketAndPerson);
    CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                  params, key, startTime)
                                 .build();

    addPerson(gatherer, m_ResourceMonitor, "p");

    addArrival(gatherer, m_ResourceMonitor, data[0], "p");
    addArrival(gatherer, m_ResourceMonitor, data[1], "p");

    TFeatureSizeFeatureDataPrVecPrVec featureData;

    gatherer.featureData(0, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(0, featureData.size());

    gatherer.featureData(600, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 0)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    gatherer.featureData(1200, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 0)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    gatherer.featureData(1800, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));
}

BOOST_FIXTURE_TEST_CASE(testResetBucketGivenSingleSeries, CTestFixture) {
    constexpr core_t::TTime startTime = 0;
    constexpr core_t::TTime bucketLength = 600;
    constexpr std::size_t latencyBuckets(2);
    SModelParams params(bucketLength);
    params.s_LatencyBuckets = latencyBuckets;

    constexpr std::array<core_t::TTime, 6> data = {
        100,
        300, // Bucket 1
        600, 800,
        850, // Bucket 2
        1200 // Bucket 3
    };

    TFeatureVec features;
    features.push_back(model_t::E_IndividualCountByBucketAndPerson);
    CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                  params, key, startTime)
                                 .build();

    addPerson(gatherer, m_ResourceMonitor, "p");

    for (const auto i : data) {
        addArrival(gatherer, m_ResourceMonitor, i, "p");
    }

    TFeatureSizeFeatureDataPrVecPrVec featureData;

    gatherer.featureData(0, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 2)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    gatherer.featureData(600, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 3)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    gatherer.featureData(1200, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    gatherer.resetBucket(600);

    gatherer.featureData(0, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 2)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    gatherer.featureData(600, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 0)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    gatherer.featureData(1200, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));
}

BOOST_FIXTURE_TEST_CASE(testResetBucketGivenMultipleSeries, CTestFixture) {
    constexpr core_t::TTime startTime = 0;
    constexpr core_t::TTime bucketLength = 600;
    constexpr std::size_t latencyBuckets(2);
    SModelParams params(bucketLength);
    params.s_LatencyBuckets = latencyBuckets;

    constexpr std::array<core_t::TTime, 6> data = {
        100,
        300, // Bucket 1
        600, 800,
        850, // Bucket 2
        1200 // Bucket 3
    };

    TFeatureVec features;
    features.push_back(model_t::E_IndividualCountByBucketAndPerson);
    CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                  params, key, startTime)
                                 .build();

    addPerson(gatherer, m_ResourceMonitor, "p1");
    addPerson(gatherer, m_ResourceMonitor, "p2");
    addPerson(gatherer, m_ResourceMonitor, "p3");

    for (const auto i : data) {
        addArrival(gatherer, m_ResourceMonitor, i, "p1");
        addArrival(gatherer, m_ResourceMonitor, i, "p2");
        addArrival(gatherer, m_ResourceMonitor, i, "p3");
    }

    TFeatureSizeFeatureDataPrVecPrVec featureData;

    gatherer.featureData(0, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 2), (1, 2), (2, 2)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    gatherer.featureData(600, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 3), (1, 3), (2, 3)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    gatherer.featureData(1200, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1), (1, 1), (2, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    gatherer.resetBucket(600);

    gatherer.featureData(0, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 2), (1, 2), (2, 2)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    gatherer.featureData(600, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 0), (1, 0), (2, 0)]"),
                        core::CContainerPrinter::print(featureData[0].second));

    gatherer.featureData(1200, bucketLength, featureData);
    BOOST_REQUIRE_EQUAL(std::string("[(0, 1), (1, 1), (2, 1)]"),
                        core::CContainerPrinter::print(featureData[0].second));
}

BOOST_FIXTURE_TEST_CASE(testResetBucketGivenBucketNotAvailable, CTestFixture) {
    constexpr core_t::TTime startTime = 0;
    constexpr core_t::TTime bucketLength = 600;
    constexpr std::size_t latencyBuckets(1);
    SModelParams params(bucketLength);
    params.s_LatencyBuckets = latencyBuckets;

    TFeatureVec features;
    features.push_back(model_t::E_IndividualCountByBucketAndPerson);
    CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                  params, key, startTime)
                                 .build();
    addPerson(gatherer, m_ResourceMonitor, "p");

    addArrival(gatherer, m_ResourceMonitor, 1200, "p");

    BOOST_TEST_REQUIRE(gatherer.resetBucket(0) == false);
    BOOST_TEST_REQUIRE(gatherer.resetBucket(600));
    BOOST_TEST_REQUIRE(gatherer.resetBucket(1200));
    BOOST_TEST_REQUIRE(gatherer.resetBucket(1800) == false);
}

BOOST_FIXTURE_TEST_CASE(testInfluencerBucketStatistics, CTestFixture) {
    constexpr std::array<core_t::TTime, 15> data = {
        1, 15, 180, 190, 400,
        550, // bucket 1
        600, 799,
        1199, // bucket 2
        1200,
        1250, // bucket 3
              // bucket 4
        2420, 2480,
        2490, // bucket 5
        10000 // sentinel
    };
    const TTimeVec dataVec(data.begin(), data.end());

    const TStrVecVec influencers(14, TStrVec(1, "i"));

    const TStrVec expectedPersonCountsVec{
        std::string("[(0, 6, [[(i, ([6], 1))]])]"),
        std::string("[(0, 3, [[(i, ([3], 1))]])]"),
        std::string("[(0, 2, [[(i, ([2], 1))]])]"), std::string("[(0, 0, [[]])]"),
        std::string("[(0, 3, [[(i, ([3], 1))]])]")};

    const TStrVec expectedPersonNonZeroCountsVec{
        std::string("[(0, 6, [[(i, ([6], 1))]])]"),
        std::string("[(0, 3, [[(i, ([3], 1))]])]"),
        std::string("[(0, 2, [[(i, ([2], 1))]])]"), std::string("[]"),
        std::string("[(0, 3, [[(i, ([3], 1))]])]")};

    const TStrVec expectedPersonIndicatorVec{
        std::string("[(0, 1, [[(i, ([1], 1))]])]"),
        std::string("[(0, 1, [[(i, ([1], 1))]])]"),
        std::string("[(0, 1, [[(i, ([1], 1))]])]"), std::string("[]"),
        std::string("[(0, 1, [[(i, ([1], 1))]])]")};

    const TStrVec expectedArrivalTimeVec(6, std::string("[]"));

    const TStrVec expectedInfoContentVec{
        std::string("[(0, 13, [[(i, ([13], 1))]])]"),
        std::string("[(0, 13, [[(i, ([13], 1))]])]"),
        std::string("[(0, 13, [[(i, ([13], 1))]])]"), std::string("[]"),
        std::string("[(0, 13, [[(i, ([13], 1))]])]")};

    testInfluencerPerFeature(model_t::E_IndividualCountByBucketAndPerson, dataVec,
                             influencers, expectedPersonCountsVec, "", m_ResourceMonitor);

    testInfluencerPerFeature(model_t::E_IndividualNonZeroCountByBucketAndPerson,
                             dataVec, influencers, expectedPersonNonZeroCountsVec,
                             "", m_ResourceMonitor);

    testInfluencerPerFeature(model_t::E_IndividualLowCountsByBucketAndPerson, dataVec,
                             influencers, expectedPersonCountsVec, "", m_ResourceMonitor);

    testInfluencerPerFeature(model_t::E_IndividualArrivalTimesByPerson, dataVec,
                             influencers, expectedArrivalTimeVec, "", m_ResourceMonitor);

    testInfluencerPerFeature(model_t::E_IndividualLowNonZeroCountByBucketAndPerson,
                             dataVec, influencers, expectedPersonNonZeroCountsVec,
                             "", m_ResourceMonitor);

    testInfluencerPerFeature(model_t::E_IndividualUniqueCountByBucketAndPerson,
                             dataVec, influencers, expectedPersonIndicatorVec,
                             "value", m_ResourceMonitor);

    testInfluencerPerFeature(model_t::E_IndividualInfoContentByBucketAndPerson,
                             dataVec, influencers, expectedInfoContentVec,
                             "value", m_ResourceMonitor);
}

class CDistinctStringsTestFixture : public CTestFixture {
protected:
    // Type aliases for convenience.
    using TOptionalStr = std::optional<std::string>;
    using TOptionalStrVec = std::vector<TOptionalStr>;

    // Helper that creates a SEventRateFeatureData object, populates it using the distinct count method,
    // and checks that its print() output matches the expected value.
    static void verifyDistinctCountFeature(const CUniqueStringFeatureData& data,
                                           const std::string& expected) {
        SEventRateFeatureData featureData(0);
        data.populateDistinctCountFeatureData(featureData);
        BOOST_REQUIRE_EQUAL(expected, featureData.print());
    }

    // Similar helper for the info-content feature data.
    static void verifyInfoContentFeature(const CUniqueStringFeatureData& data,
                                         const std::string& expected) {
        SEventRateFeatureData featureData(0);
        data.populateInfoContentFeatureData(featureData);
        BOOST_REQUIRE_EQUAL(expected, featureData.print());
    }

    // Helper to sort influence values (when needed) using the ordering defined in maths::common.
    static void sortInfluenceValues(SEventRateFeatureData& featureData) {
        for (auto& influenceGroup : featureData.s_InfluenceValues) {
            std::sort(influenceGroup.begin(), influenceGroup.end(),
                      maths::common::COrderings::SFirstLess());
        }
    }

    // ----- Block 1: Distinct Count with NO influences -----
    static void testDistinctCountNoInfluence() {
        // Create an empty (constexpr) vector of optional strings.
        const TOptionalStrVec influencers{};

        CUniqueStringFeatureData data;
        // Initially, no strings have been inserted.
        verifyDistinctCountFeature(data, "0");

        // Insert "str1" repeatedly and verify that distinct count remains "1"
        for (std::size_t i = 0; i < 100; ++i) {
            data.insert("str1", influencers);
            verifyDistinctCountFeature(data, "1");
        }
        // Insert "str2" and "str3" repeatedly so that eventually the distinct count becomes "3"
        for (std::size_t i = 0; i < 100; ++i) {
            data.insert("str2", influencers);
            data.insert("str3", influencers);
            verifyDistinctCountFeature(data, "3");
        }
        // For additional inserts, check that the internal count equals max(3, i)
        for (std::size_t i = 1; i < 100; ++i) {
            std::stringstream ss;
            ss << "str" << i;
            data.insert(ss.str(), influencers);
            SEventRateFeatureData featureData(0);
            data.populateDistinctCountFeatureData(featureData);
            BOOST_REQUIRE_EQUAL(std::max(static_cast<std::uint64_t>(3),
                                         static_cast<std::uint64_t>(i)),
                                featureData.s_Count);
        }
    }

    // ----- Block 2: Distinct Count with a SINGLE influencer -----
    static void testDistinctCountSingleInfluence() {
        TOptionalStrVec influencers;
        influencers.emplace_back(); // initially, the optional is not set

        CUniqueStringFeatureData data;
        data.insert("str1", influencers);
        verifyDistinctCountFeature(data, "1, [[]]");

        // Now set the influencer value.
        influencers.back() = "inf1";
        data.insert("str1", influencers);
        verifyDistinctCountFeature(data, "1, [[(inf1, ([1], 1))]]");

        // Insert additional values.
        data.insert("str2", influencers);
        data.insert("str2", influencers);
        data.insert("str2", influencers);
        data.insert("str2", influencers);
        influencers.back() = "inf2";
        data.insert("str1", influencers);
        data.insert("str3", influencers);
        influencers.back() = "inf3";
        data.insert("str3", influencers);

        SEventRateFeatureData featureData(0);
        data.populateDistinctCountFeatureData(featureData);
        std::sort(featureData.s_InfluenceValues[0].begin(),
                  featureData.s_InfluenceValues[0].end(),
                  maths::common::COrderings::SFirstLess());
        BOOST_REQUIRE_EQUAL(std::string("3, [[(inf1, ([2], 1)), (inf2, ([2], 1)), (inf3, ([1], 1))]]"),
                            featureData.print());
    }

    // ----- Block 3: Distinct Count with MULTIPLE influencers -----
    static void testDistinctCountMultipleInfluence() {
        TOptionalStrVec influencers;
        influencers.emplace_back();
        influencers.emplace_back();

        CUniqueStringFeatureData data;
        data.insert("str1", influencers);
        data.insert("str2", influencers);
        data.insert("str1", influencers);
        verifyDistinctCountFeature(data, "2, [[], []]");

        influencers[0] = "inf1";
        data.insert("str1", influencers);
        data.insert("str2", influencers);
        verifyDistinctCountFeature(data, "2, [[(inf1, ([2], 1))], []]");

        influencers[1] = "inf_v2";
        data.insert("str2", influencers);
        influencers[0] = "inf2";
        influencers[1] = "inf_v3";
        data.insert("str3", influencers);
        data.insert("str1", influencers);
        data.insert("str3", influencers);

        SEventRateFeatureData featureData(0);
        data.populateDistinctCountFeatureData(featureData);
        for (std::size_t i = 0; i < 2; i++) {
            std::sort(featureData.s_InfluenceValues[i].begin(),
                      featureData.s_InfluenceValues[i].end(),
                      maths::common::COrderings::SFirstLess());
        }
        BOOST_REQUIRE_EQUAL(std::string("3, [[(inf1, ([2], 1)), (inf2, ([2], 1))], [(inf_v2, ([1], 1)), (inf_v3, ([2], 1))]]"),
                            featureData.print());
    }

    // ----- Block 4: Info Content with NO influences -----
    static void testInfoContentNoInfluence() {
        const TOptionalStrVec influencers{}; // empty
        CUniqueStringFeatureData data;
        verifyInfoContentFeature(data, "0");

        data.insert("str1", influencers);
        verifyInfoContentFeature(data, "12");

        data.insert("str2", influencers);
        data.insert("str3", influencers);
        verifyInfoContentFeature(data, "18");

        // For further inserts, ensure the info content count (offset by 12) is within expected bounds.
        for (std::size_t i = 1; i < 100; ++i) {
            std::stringstream ss;
            ss << "str" << i;
            data.insert(ss.str(), influencers);
            SEventRateFeatureData featureData(0);
            data.populateInfoContentFeatureData(featureData);
            BOOST_TEST_REQUIRE((featureData.s_Count - 12) >=
                               std::max(static_cast<std::uint64_t>(3),
                                        static_cast<std::uint64_t>(i)));
            BOOST_TEST_REQUIRE(
                (featureData.s_Count - 12) <=
                std::max(static_cast<std::uint64_t>(3), static_cast<std::uint64_t>(i)) * 3);
        }
    }

    // ----- Block 5: Info Content with a SINGLE influencer -----
    static void testInfoContentSingleInfluence() {
        TOptionalStrVec influencers;
        influencers.emplace_back();

        CUniqueStringFeatureData data;
        data.insert("str1", influencers);
        verifyInfoContentFeature(data, "12, [[]]");

        influencers.back() = "inf1";
        data.insert("str1", influencers);
        verifyInfoContentFeature(data, "12, [[(inf1, ([12], 1))]]");

        data.insert("str2", influencers);
        data.insert("str2", influencers);
        data.insert("str2", influencers);
        data.insert("str2", influencers);
        influencers.back() = "inf2";
        data.insert("str1", influencers);
        data.insert("str3", influencers);
        influencers.back() = "inf3";
        data.insert("str3", influencers);

        SEventRateFeatureData featureData(0);
        data.populateInfoContentFeatureData(featureData);
        std::sort(featureData.s_InfluenceValues[0].begin(),
                  featureData.s_InfluenceValues[0].end(),
                  maths::common::COrderings::SFirstLess());
        BOOST_REQUIRE_EQUAL(std::string("18, [[(inf1, ([16], 1)), (inf2, ([16], 1)), (inf3, ([12], 1))]]"),
                            featureData.print());
    }

    // ----- Block 6: Info Content with MULTIPLE influencers -----
    static void testInfoContentMultipleInfluence() {
        TOptionalStrVec influencers;
        influencers.emplace_back();
        influencers.emplace_back();

        CUniqueStringFeatureData data;
        data.insert("str1", influencers);
        data.insert("str2", influencers);
        data.insert("str1", influencers);
        verifyInfoContentFeature(data, "16, [[], []]");

        influencers[0] = "inf1";
        data.insert("str1", influencers);
        data.insert("str2", influencers);
        verifyInfoContentFeature(data, "16, [[(inf1, ([16], 1))], []]");

        influencers[1] = "inf_v2";
        data.insert("str2", influencers);
        influencers[0] = "inf2";
        influencers[1] = "inf_v3";
        data.insert("str3", influencers);
        data.insert("str1", influencers);
        data.insert("str3", influencers);

        SEventRateFeatureData featureData(0);
        data.populateInfoContentFeatureData(featureData);
        for (std::size_t i = 0; i < 2; i++) {
            std::sort(featureData.s_InfluenceValues[i].begin(),
                      featureData.s_InfluenceValues[i].end(),
                      maths::common::COrderings::SFirstLess());
        }
        BOOST_REQUIRE_EQUAL(std::string("18, [[(inf1, ([16], 1)), (inf2, ([16], 1))], [(inf_v2, ([12], 1)), (inf_v3, ([16], 1))]]"),
                            featureData.print());
    }

    // ----- Block 7: Distinct Strings in Latency Buckets -----
    void testLatencyBucketsDistinctStrings() {
        constexpr core_t::TTime bucketLength = 1800;
        constexpr core_t::TTime startTime = 1432733400;
        constexpr std::size_t latencyBuckets = 3;
        SModelParams params(bucketLength);
        params.s_LatencyBuckets = latencyBuckets;

        TFeatureVec features;
        features.push_back(model_t::E_IndividualUniqueCountByBucketAndPerson);
        CDataGatherer gatherer = CDataGathererBuilder(model_t::E_EventRate, features,
                                                      params, key, startTime)
                                     .personFieldName("P")
                                     .valueFieldName("V")
                                     .influenceFieldNames({"INF"})
                                     .build();

        BOOST_TEST_REQUIRE(!gatherer.isPopulation());
        BOOST_REQUIRE_EQUAL(0, addPerson(gatherer, m_ResourceMonitor, "p", "v", 1));
        BOOST_REQUIRE_EQUAL(1, gatherer.numberFeatures());
        for (std::size_t i = 0; i < 1; ++i) {
            BOOST_REQUIRE_EQUAL(features[i], gatherer.feature(i));
        }
        BOOST_TEST_REQUIRE(gatherer.hasFeature(model_t::E_IndividualUniqueCountByBucketAndPerson));

        BOOST_REQUIRE_EQUAL(1, gatherer.numberActivePeople());
        BOOST_REQUIRE_EQUAL(1, gatherer.numberByFieldValues());
        BOOST_REQUIRE_EQUAL(std::string("p"), gatherer.personName(0));
        constexpr core_t::TTime time = startTime;
        BOOST_REQUIRE_EQUAL(bucketLength, gatherer.bucketLength());
        testPersistence(params, gatherer, model_t::E_EventRate);

        // Add data (some out-of-order) for distinct strings.
        addArrival(gatherer, m_ResourceMonitor, time - (2 * bucketLength), "p",
                   "stringOne", "inf1");
        addArrival(gatherer, m_ResourceMonitor, time - (2 * bucketLength), "p",
                   "stringTwo", "inf2");
        addArrival(gatherer, m_ResourceMonitor, time - (1 * bucketLength), "p",
                   "stringThree", "inf3");
        addArrival(gatherer, m_ResourceMonitor, time - (1 * bucketLength), "p",
                   "stringFour", "inf1");
        addArrival(gatherer, m_ResourceMonitor, time, "p", "stringFive", "inf2");
        addArrival(gatherer, m_ResourceMonitor, time, "p", "stringSix", "inf3");
        testPersistence(params, gatherer, model_t::E_EventRate);
    }
};

BOOST_FIXTURE_TEST_CASE(testDistinctStrings, CDistinctStringsTestFixture) {
    testDistinctCountNoInfluence();
    testDistinctCountSingleInfluence();
    testDistinctCountMultipleInfluence();
    testInfoContentNoInfluence();
    testInfoContentSingleInfluence();
    testInfoContentMultipleInfluence();
    testLatencyBucketsDistinctStrings();
}

class CDiurnalTestFixture : public CTestFixture {
protected:
    // Common constants.
    static constexpr core_t::TTime BUCKET_LENGTH{3600};
    static constexpr core_t::TTime START_TIME{1432731600};
    static constexpr std::size_t LATENCY_BUCKETS{3};
    const std::string PERSON{"p"};
    const std::string ATTRIBUTE{"a"};

    // Create and initialize the model parameters.
    static SModelParams createParams() {
        SModelParams params(BUCKET_LENGTH);
        params.s_LatencyBuckets = LATENCY_BUCKETS;
        return params;
    }

    // Compute the expected count.
    // If isDay is true, the modulo is 86400 (day), otherwise 604800 (week).
    static std::uint64_t computeExpected(const core_t::TTime time,
                                         const std::uint64_t addition,
                                         const bool isDay) {
        return static_cast<std::uint64_t>(time % (isDay ? 86400 : 604800)) + addition;
    }

    // Build a gatherer based on the features, parameters, start time, and type.
    // When useAttribute is true the attribute field is set (for population tests).
    static CDataGatherer createGatherer(const TFeatureVec& features,
                                        const SModelParams& params,
                                        core_t::TTime startTime,
                                        bool isPopulation,
                                        bool useAttribute = false) {
        auto builder = CDataGathererBuilder(model_t::E_EventRate, features,
                                            params, key, startTime);
        if (useAttribute) {
            return builder.gathererType(model_t::E_PopulationEventRate)
                .attributeFieldName("att")
                .build();
        }

        if (!isPopulation) {
            return builder.personFieldName("person").build();
        }
        return builder.build();
    }

    // Verify that the gatherer contains the proper features.
    static void verifyGathererFeatures(const CDataGatherer& gatherer,
                                       const TFeatureVec& features,
                                       model_t::EFeature expectedFeature,
                                       bool isPopulation) {
        BOOST_REQUIRE_EQUAL(1, gatherer.numberFeatures());
        for (std::size_t i = 0; i < features.size(); ++i) {
            BOOST_REQUIRE_EQUAL(features[i], gatherer.feature(i));
        }
        BOOST_TEST_REQUIRE(gatherer.hasFeature(expectedFeature));
        if (isPopulation) {
            BOOST_TEST_REQUIRE(gatherer.isPopulation());
        } else {
            BOOST_TEST_REQUIRE(!gatherer.isPopulation());
        }
    }

    // Helper to add an arrival with or without an attribute.
    void addArrivalHelper(CDataGatherer& gatherer, core_t::TTime t, bool useAttribute) {
        if (useAttribute) {
            addArrival(gatherer, m_ResourceMonitor, t, PERSON, ATTRIBUTE);
        } else {
            addArrival(gatherer, m_ResourceMonitor, t, PERSON);
        }
    }

    // Template to verify the feature data.
    // FeatureDataT is one of:
    //  - TFeatureSizeFeatureDataPrVecPrVec for tests by person,
    //  - TFeatureSizeSizePrFeatureDataPrVecPrVec for tests over person.
    template<typename FeatureDataT>
    void verifyFeatureData(const CDataGatherer& gatherer, core_t::TTime time, std::uint64_t expectedCount) {
        FeatureDataT featureData;
        gatherer.featureData(time, BUCKET_LENGTH, featureData);
        BOOST_REQUIRE_EQUAL(1, featureData.size());
        BOOST_REQUIRE_EQUAL(1, featureData[0].second.size());
        BOOST_REQUIRE_EQUAL(expectedCount, featureData[0].second[0].second.s_Count);
    }

    // Run a sequence of arrivals and verifications.
    // The isDay flag selects the modulo (day or week), and useAttribute toggles between
    // person-only and attribute-including arrivals.
    template<typename FeatureDataT>
    void runTestSequence(CDataGatherer& gatherer, bool isDay, bool useAttribute) {
        core_t::TTime time = START_TIME;

        // Check bucket length and persistence.
        BOOST_REQUIRE_EQUAL(BUCKET_LENGTH, gatherer.bucketLength());
        testPersistence(createParams(), gatherer, model_t::E_EventRate);

        // Arrival 1: time + 0
        addArrivalHelper(gatherer, time + 0, useAttribute);
        verifyFeatureData<FeatureDataT>(gatherer, time, computeExpected(time, 0, isDay));

        // Arrival 2: time + 100, expected additional count of 50.
        addArrivalHelper(gatherer, time + 100, useAttribute);
        verifyFeatureData<FeatureDataT>(gatherer, time, computeExpected(time, 50, isDay));

        time += BUCKET_LENGTH;
        // Arrival 3: new bucket, time + 0.
        addArrivalHelper(gatherer, time + 0, useAttribute);
        verifyFeatureData<FeatureDataT>(gatherer, time, computeExpected(time, 0, isDay));

        // Arrival 4: time + 200, expected additional count of 100.
        addArrivalHelper(gatherer, time + 200, useAttribute);
        verifyFeatureData<FeatureDataT>(gatherer, time, computeExpected(time, 100, isDay));

        time += BUCKET_LENGTH;
        // Arrival 5: time + 0.
        addArrivalHelper(gatherer, time + 0, useAttribute);
        verifyFeatureData<FeatureDataT>(gatherer, time, computeExpected(time, 0, isDay));

        // Arrival 6: time + 300, expected additional count of 150.
        addArrivalHelper(gatherer, time + 300, useAttribute);
        verifyFeatureData<FeatureDataT>(gatherer, time, computeExpected(time, 150, isDay));

        // Check latency: go back two buckets.
        time -= BUCKET_LENGTH * 2;
        addArrivalHelper(gatherer, time + 200, useAttribute);
        verifyFeatureData<FeatureDataT>(gatherer, time, computeExpected(time, 100, isDay));

        time += BUCKET_LENGTH;
        addArrivalHelper(gatherer, time + 400, useAttribute);
        verifyFeatureData<FeatureDataT>(gatherer, time, computeExpected(time, 200, isDay));
    }

    // Verify summary information for the gatherer.
    static void verifyGathererSummary(const CDataGatherer& gatherer,
                                      bool isPopulation,
                                      bool useAttribute) {
        if (isPopulation) {
            if (useAttribute) {
                BOOST_REQUIRE_EQUAL(1, gatherer.numberActivePeople());
                BOOST_REQUIRE_EQUAL(1, gatherer.numberActiveAttributes());
                BOOST_REQUIRE_EQUAL(std::string("a"), gatherer.attributeName(0));
            }
        } else {
            BOOST_REQUIRE_EQUAL(1, gatherer.numberActivePeople());
            BOOST_REQUIRE_EQUAL(std::string("p"), gatherer.personName(0));
        }
        BOOST_REQUIRE_EQUAL(1, gatherer.numberByFieldValues());
    }
};

BOOST_FIXTURE_TEST_CASE(testDiurnalFeatures, CDiurnalTestFixture) {
    {
        // Test: time_of_day by person
        LOG_DEBUG(<< "Testing time_of_day by person");
        SModelParams const params = createParams();
        TFeatureVec const features{model_t::E_IndividualTimeOfDayByBucketAndPerson};
        CDataGatherer gatherer = createGatherer(features, params, START_TIME, false);
        verifyGathererFeatures(gatherer, features,
                               model_t::E_IndividualTimeOfDayByBucketAndPerson, false);
        runTestSequence<TFeatureSizeFeatureDataPrVecPrVec>(gatherer, /*isDay=*/true,
                                                           /*useAttribute=*/false);
        verifyGathererSummary(gatherer, false, false);
        testPersistence(params, gatherer, model_t::E_EventRate);
    }
    {
        // Test: time_of_week by person
        LOG_DEBUG(<< "Testing time_of_week by person");
        SModelParams const params = createParams();
        TFeatureVec const features{model_t::E_IndividualTimeOfWeekByBucketAndPerson};
        CDataGatherer gatherer = createGatherer(features, params, START_TIME, false);
        verifyGathererFeatures(gatherer, features,
                               model_t::E_IndividualTimeOfWeekByBucketAndPerson, false);
        runTestSequence<TFeatureSizeFeatureDataPrVecPrVec>(
            gatherer, /*isDay=*/false, /*useAttribute=*/false);
        verifyGathererSummary(gatherer, false, false);
        testPersistence(params, gatherer, model_t::E_EventRate);
    }
    {
        // Test: time_of_week over person (with attribute)
        LOG_DEBUG(<< "Testing time_of_week over person");
        SModelParams const params = createParams();
        TFeatureVec const features{model_t::E_PopulationTimeOfWeekByBucketPersonAndAttribute};
        CDataGatherer gatherer = createGatherer(features, params, START_TIME,
                                                true, /*useAttribute=*/true);
        verifyGathererFeatures(gatherer, features, model_t::E_PopulationTimeOfWeekByBucketPersonAndAttribute,
                               true);
        runTestSequence<TFeatureSizeSizePrFeatureDataPrVecPrVec>(
            gatherer, /*isDay=*/false, /*useAttribute=*/true);
        verifyGathererSummary(gatherer, true, true);
        testPersistence(params, gatherer, model_t::E_EventRate);
    }
    {
        // Test: time_of_day over person (with attribute)
        LOG_DEBUG(<< "Testing time_of_day over person");
        SModelParams const params = createParams();
        TFeatureVec const features{model_t::E_PopulationTimeOfDayByBucketPersonAndAttribute};
        CDataGatherer gatherer = createGatherer(features, params, START_TIME,
                                                true, /*useAttribute=*/true);
        verifyGathererFeatures(gatherer, features, model_t::E_PopulationTimeOfDayByBucketPersonAndAttribute,
                               true);
        runTestSequence<TFeatureSizeSizePrFeatureDataPrVecPrVec>(
            gatherer, /*isDay=*/true, /*useAttribute=*/true);
        verifyGathererSummary(gatherer, true, true);
        testPersistence(params, gatherer, model_t::E_EventRate);
    }
}

BOOST_AUTO_TEST_SUITE_END()
