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
#include <core/CompressUtils.h>

#include <model/CDataGatherer.h>
#include <model/CEventData.h>
#include <model/CEventRateBucketGatherer.h>
#include <model/CEventRatePopulationModelFactory.h>
#include <model/CResourceMonitor.h>
#include <model/SModelParams.h>

#include <test/BoostTestCloseAbsolute.h>
#include <test/CRandomNumbers.h>

#include <boost/lexical_cast.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <memory>
#include <set>
#include <vector>

BOOST_AUTO_TEST_SUITE(CEventRatePopulationDataGathererTest)

using namespace ml;
using namespace model;

namespace {

struct SMessage {
    SMessage(core_t::TTime time, const std::string& attribute, const std::string& person)
        : s_Time(time), s_Attribute(attribute), s_Person(person) {}

    bool operator<(const SMessage& other) const {
        return s_Time < other.s_Time;
    }

    core_t::TTime s_Time;
    std::string s_Attribute;
    std::string s_Person;
};

using TMessageVec = std::vector<SMessage>;
using TSizeVec = std::vector<std::size_t>;
using TStrVec = std::vector<std::string>;
using TEventRateDataGathererPtr = std::shared_ptr<CDataGatherer>;
using TSizeSizePr = std::pair<std::size_t, std::size_t>;
using TSizeSizePrUInt64Map = std::map<TSizeSizePr, std::uint64_t>;
using TSizeSizePrUInt64MapCItr = TSizeSizePrUInt64Map::iterator;
using TSizeSet = std::set<std::size_t>;
using TSizeSizeSetMap = std::map<std::size_t, TSizeSet>;
using TStrSet = std::set<std::string>;
using TSizeStrSetMap = std::map<std::size_t, TStrSet>;
using TSizeStrSetMapItr = std::map<std::size_t, TStrSet>::iterator;
using TFeatureData = SEventRateFeatureData;
using TStrFeatureDataPr = std::pair<std::string, TFeatureData>;
using TStrFeatureDataPrVec = std::vector<TStrFeatureDataPr>;
using TSizeSizePrFeatureDataPr = std::pair<TSizeSizePr, TFeatureData>;
using TSizeSizePrFeatureDataPrVec = std::vector<TSizeSizePrFeatureDataPr>;
using TFeatureSizeSizePrFeatureDataPrVecPr =
    std::pair<model_t::EFeature, TSizeSizePrFeatureDataPrVec>;
using TFeatureSizeSizePrFeatureDataPrVecPrVec = std::vector<TFeatureSizeSizePrFeatureDataPrVecPr>;

TStrVec allCategories() {
    const std::size_t numberCategories = 30;
    TStrVec categories;
    for (std::size_t i = 0; i < numberCategories; ++i) {
        categories.push_back("c" + boost::lexical_cast<std::string>(i));
    }
    return categories;
}

TStrVec allPeople() {
    const std::size_t numberPeople = 5;
    TStrVec people;
    for (std::size_t i = 0; i < numberPeople; ++i) {
        people.push_back("p" + boost::lexical_cast<std::string>(i));
    }
    return people;
}

void generateTestMessages(test::CRandomNumbers& rng,
                          core_t::TTime time,
                          core_t::TTime bucketLength,
                          TMessageVec& messages) {
    using TUIntVec = std::vector<unsigned int>;
    using TDoubleVec = std::vector<double>;

    LOG_DEBUG(<< "bucket = [" << time << ", " << time + bucketLength << ")");

    TStrVec categories = allCategories();
    TStrVec people = allPeople();

    const TDoubleVec rates{1.0, 0.3, 10.1, 25.0, 105.0};

    TSizeVec bucketCounts;
    for (std::size_t j = 0; j < categories.size(); ++j) {
        double rate = rates[j % rates.size()];
        TUIntVec sample;
        rng.generatePoissonSamples(rate, 1, sample);
        bucketCounts.push_back(sample[0]);
    }

    TDoubleVec personRange;
    rng.generateUniformSamples(0.0, static_cast<double>(people.size()) - 1e-3, 2u, personRange);
    std::sort(personRange.begin(), personRange.end());
    std::size_t a = static_cast<std::size_t>(personRange[0]);
    std::size_t b = static_cast<std::size_t>(personRange[1]) + 1;
    TSizeVec bucketPeople;
    for (std::size_t i = a; i < b; ++i) {
        bucketPeople.push_back(i);
    }
    LOG_DEBUG(<< "bucketPeople = " << bucketPeople);

    for (std::size_t i = 0; i < categories.size(); ++i) {
        TDoubleVec offsets;
        rng.generateUniformSamples(0.0, static_cast<double>(bucketLength) - 1.0,
                                   bucketCounts[i], offsets);

        for (std::size_t j = 0; j < offsets.size(); ++j) {
            messages.push_back(SMessage(
                time + static_cast<core_t::TTime>(offsets[j]), categories[i],
                people[bucketPeople[j % bucketPeople.size()]]));
        }
    }

    std::sort(messages.begin(), messages.end());
    LOG_DEBUG(<< "Generated " << messages.size() << " messages");
}

const TSizeSizePrFeatureDataPrVec&
extract(const TFeatureSizeSizePrFeatureDataPrVecPrVec& featureData, model_t::EFeature feature) {
    for (std::size_t i = 0; i < featureData.size(); ++i) {
        if (featureData[i].first == feature) {
            return featureData[i].second;
        }
    }

    static const TSizeSizePrFeatureDataPrVec EMPTY;
    return EMPTY;
}

const TSizeSizePrFeatureDataPrVec&
extractPeoplePerAttribute(TFeatureSizeSizePrFeatureDataPrVecPrVec& featureData) {
    return extract(featureData, model_t::E_PopulationUniquePersonCountByAttribute);
}

const TSizeSizePrFeatureDataPrVec&
extractNonZeroAttributeCounts(TFeatureSizeSizePrFeatureDataPrVecPrVec& featureData) {
    return extract(featureData, model_t::E_PopulationCountByBucketPersonAndAttribute);
}

const TSizeSizePrFeatureDataPrVec&
extractAttributeIndicator(TFeatureSizeSizePrFeatureDataPrVecPrVec& featureData) {
    return extract(featureData, model_t::E_PopulationIndicatorOfBucketPersonAndAttribute);
}

const TSizeSizePrFeatureDataPrVec&
extractBucketAttributesPerPerson(TFeatureSizeSizePrFeatureDataPrVecPrVec& featureData) {
    return extract(featureData, model_t::E_PopulationUniqueCountByBucketPersonAndAttribute);
}

const TSizeSizePrFeatureDataPrVec&
extractCompressedLengthPerPerson(TFeatureSizeSizePrFeatureDataPrVecPrVec& featureData) {
    return extract(featureData, model_t::E_PopulationInfoContentByBucketPersonAndAttribute);
}

CEventData addArrival(core_t::TTime time,
                      const std::string& p,
                      const std::string& a,
                      CDataGatherer& gatherer,
                      CResourceMonitor& resourceMonitor) {
    CDataGatherer::TStrCPtrVec fields;
    fields.push_back(&p);
    fields.push_back(&a);

    CEventData result;
    result.time(time);

    gatherer.addArrival(fields, result, resourceMonitor);
    return result;
}

CEventData addArrival(core_t::TTime time,
                      const std::string& p,
                      const std::string& a,
                      const std::string& v,
                      CDataGatherer& gatherer,
                      CResourceMonitor& resourceMonitor) {
    CDataGatherer::TStrCPtrVec fields;
    fields.push_back(&p);
    fields.push_back(&a);
    fields.push_back(&v);

    CEventData result;
    result.time(time);

    gatherer.addArrival(fields, result, resourceMonitor);
    return result;
}

CSearchKey searchKey;
const std::string EMPTY_STRING;
}

class CTestFixture {
protected:
    CResourceMonitor m_ResourceMonitor;
};

BOOST_FIXTURE_TEST_CASE(testAttributeCounts, CTestFixture) {
    // We check that we correctly sample the unique people per
    // attribute and (attribute, person) pair counts.

    using TStrSizeMap = std::map<std::string, std::size_t>;

    const core_t::TTime startTime = 1367280000;
    const core_t::TTime bucketLength = 3600;
    const std::size_t numberBuckets = 20;

    test::CRandomNumbers rng;

    CDataGatherer::TFeatureVec features;
    features.push_back(model_t::E_PopulationCountByBucketPersonAndAttribute);
    features.push_back(model_t::E_PopulationUniquePersonCountByAttribute);
    SModelParams params(bucketLength);
    CDataGatherer dataGatherer(model_t::E_PopulationEventRate, model_t::E_None, params,
                               EMPTY_STRING, EMPTY_STRING, EMPTY_STRING, EMPTY_STRING,
                               EMPTY_STRING, {}, searchKey, features, startTime, 0);
    BOOST_TEST_REQUIRE(dataGatherer.isPopulation());

    BOOST_REQUIRE_EQUAL(startTime, dataGatherer.currentBucketStartTime());
    BOOST_REQUIRE_EQUAL(bucketLength, dataGatherer.bucketLength());

    TSizeSizeSetMap expectedAttributePeople;
    std::size_t attributeOrder = 0;
    TStrSizeMap expectedAttributeOrder;
    std::size_t personOrder = 0;
    TStrSizeMap expectedPeopleOrder;
    core_t::TTime time = startTime;
    for (std::size_t i = 0; i < numberBuckets; ++i, time += bucketLength) {
        TMessageVec messages;
        generateTestMessages(rng, time, bucketLength, messages);

        TSizeSizePrUInt64Map expectedAttributeCounts;
        for (std::size_t j = 0; j < messages.size(); ++j) {
            addArrival(messages[j].s_Time, messages[j].s_Person,
                       messages[j].s_Attribute, dataGatherer, m_ResourceMonitor);

            std::size_t cid;
            dataGatherer.attributeId(messages[j].s_Attribute, cid);

            std::size_t pid;
            dataGatherer.personId(messages[j].s_Person, pid);

            ++expectedAttributeCounts[std::make_pair(pid, cid)];
            expectedAttributePeople[cid].insert(pid);
            if (expectedAttributeOrder
                    .insert(TStrSizeMap::value_type(messages[j].s_Attribute, attributeOrder))
                    .second) {
                ++attributeOrder;
            }
            if (expectedPeopleOrder
                    .insert(TStrSizeMap::value_type(messages[j].s_Person, personOrder))
                    .second) {
                ++personOrder;
            }
        }

        BOOST_TEST_REQUIRE(dataGatherer.dataAvailable(time));
        BOOST_TEST_REQUIRE(!dataGatherer.dataAvailable(time - 1));

        TFeatureSizeSizePrFeatureDataPrVecPrVec featureData;
        dataGatherer.featureData(time, bucketLength, featureData);

        const TSizeSizePrFeatureDataPrVec& peoplePerAttribute =
            extractPeoplePerAttribute(featureData);
        BOOST_REQUIRE_EQUAL(expectedAttributePeople.size(), peoplePerAttribute.size());
        TSizeSizePrFeatureDataPrVec expectedPeoplePerAttribute;
        for (std::size_t j = 0; j < peoplePerAttribute.size(); ++j) {
            expectedPeoplePerAttribute.push_back(TSizeSizePrFeatureDataPr(
                std::make_pair(size_t(0), j), expectedAttributePeople[j].size()));
        }
        BOOST_REQUIRE_EQUAL(core::CContainerPrinter::print(expectedPeoplePerAttribute),
                            core::CContainerPrinter::print(peoplePerAttribute));

        const TSizeSizePrFeatureDataPrVec& personAttributeCounts =
            extractNonZeroAttributeCounts(featureData);
        BOOST_REQUIRE_EQUAL(core::CContainerPrinter::print(expectedAttributeCounts),
                            core::CContainerPrinter::print(personAttributeCounts));

        const TSizeSizePrFeatureDataPrVec& attributeIndicator =
            extractAttributeIndicator(featureData);
        BOOST_TEST_REQUIRE(attributeIndicator.empty());

        const TSizeSizePrFeatureDataPrVec& bucketAttributesPerPerson =
            extractBucketAttributesPerPerson(featureData);
        BOOST_TEST_REQUIRE(bucketAttributesPerPerson.empty());

        dataGatherer.timeNow(time + bucketLength);
    }

    TStrVec categories = allCategories();
    TSizeVec attributeIds;
    for (std::size_t i = 0; i < categories.size(); ++i) {
        std::size_t cid;
        BOOST_TEST_REQUIRE(dataGatherer.attributeId(categories[i], cid));
        attributeIds.push_back(cid);
        BOOST_REQUIRE_EQUAL(expectedAttributeOrder[categories[i]], cid);
    }
    LOG_DEBUG(<< "attribute ids = " << attributeIds);
    LOG_DEBUG(<< "expected attribute ids = " << expectedAttributeOrder);

    TStrVec people = allPeople();
    TSizeVec peopleIds;
    for (std::size_t i = 0; i < people.size(); ++i) {
        std::size_t pid;
        BOOST_TEST_REQUIRE(dataGatherer.personId(people[i], pid));
        peopleIds.push_back(pid);
        BOOST_REQUIRE_EQUAL(expectedPeopleOrder[people[i]], pid);
    }
    LOG_DEBUG(<< "people ids = " << peopleIds);
    LOG_DEBUG(<< "expected people ids = " << expectedPeopleOrder);
}

BOOST_FIXTURE_TEST_CASE(testAttributeIndicator, CTestFixture) {
    // We check that we correctly sample the (attribute, person)
    // indicator.

    const core_t::TTime startTime = 1367280000;
    const core_t::TTime bucketLength = 3600;
    const std::size_t numberBuckets = 20;

    test::CRandomNumbers rng;

    CDataGatherer::TFeatureVec features;
    features.push_back(model_t::E_PopulationIndicatorOfBucketPersonAndAttribute);
    SModelParams params(bucketLength);
    CDataGatherer dataGatherer(model_t::E_PopulationEventRate, model_t::E_None, params,
                               EMPTY_STRING, EMPTY_STRING, EMPTY_STRING, EMPTY_STRING,
                               EMPTY_STRING, {}, searchKey, features, startTime, 0);

    core_t::TTime time = startTime;
    for (std::size_t i = 0; i < numberBuckets; ++i, time += bucketLength) {
        TMessageVec messages;
        generateTestMessages(rng, time, bucketLength, messages);

        TSizeSizePrUInt64Map expectedAttributeIndicator;
        for (std::size_t j = 0; j < messages.size(); ++j) {
            addArrival(messages[j].s_Time, messages[j].s_Person,
                       messages[j].s_Attribute, dataGatherer, m_ResourceMonitor);

            std::size_t cid;
            dataGatherer.attributeId(messages[j].s_Attribute, cid);

            std::size_t pid;
            dataGatherer.personId(messages[j].s_Person, pid);

            expectedAttributeIndicator[std::make_pair(pid, cid)] = 1;
        }

        BOOST_TEST_REQUIRE(dataGatherer.dataAvailable(time));
        BOOST_TEST_REQUIRE(!dataGatherer.dataAvailable(time - 1));

        TFeatureSizeSizePrFeatureDataPrVecPrVec featureData;
        dataGatherer.featureData(time, bucketLength, featureData);

        const TSizeSizePrFeatureDataPrVec& peoplePerAttribute =
            extractPeoplePerAttribute(featureData);
        BOOST_TEST_REQUIRE(peoplePerAttribute.empty());

        const TSizeSizePrFeatureDataPrVec& attributeIndicator =
            extractAttributeIndicator(featureData);
        BOOST_REQUIRE_EQUAL(core::CContainerPrinter::print(expectedAttributeIndicator),
                            core::CContainerPrinter::print(attributeIndicator));

        const TSizeSizePrFeatureDataPrVec& bucketAttributesPerPerson =
            extractBucketAttributesPerPerson(featureData);
        BOOST_TEST_REQUIRE(bucketAttributesPerPerson.empty());

        dataGatherer.timeNow(time + bucketLength);
    }
}

BOOST_FIXTURE_TEST_CASE(testUniqueValueCounts, CTestFixture) {
    // We check that we correctly sample the unique counts
    // of values per person.

    const core_t::TTime startTime = 1367280000;
    const core_t::TTime bucketLength = 3600;
    const std::size_t numberBuckets = 20;

    test::CRandomNumbers rng;

    CDataGatherer::TFeatureVec features;
    features.push_back(model_t::E_PopulationUniqueCountByBucketPersonAndAttribute);
    SModelParams params(bucketLength);
    CDataGatherer dataGatherer(model_t::E_PopulationEventRate, model_t::E_None, params,
                               EMPTY_STRING, EMPTY_STRING, EMPTY_STRING, EMPTY_STRING,
                               "value", {}, searchKey, features, startTime, 0);

    core_t::TTime time = startTime;
    for (std::size_t i = 0; i < numberBuckets; ++i, time += bucketLength) {
        TMessageVec messages;
        generateTestMessages(rng, time, bucketLength, messages);

        TSizeSizePrUInt64Map expectedUniqueCounts;

        TSizeSizeSetMap bucketPeopleCategories;
        for (std::size_t j = 0; j < messages.size(); ++j) {
            std::ostringstream ss;
            ss << "thing"
               << "_" << time << "_" << i;
            std::string value(ss.str());
            addArrival(messages[j].s_Time, messages[j].s_Person, messages[j].s_Attribute,
                       value, dataGatherer, m_ResourceMonitor);

            std::size_t cid;
            dataGatherer.attributeId(messages[j].s_Attribute, cid);

            std::size_t pid;
            dataGatherer.personId(messages[j].s_Person, pid);

            expectedUniqueCounts[std::make_pair(pid, cid)] = 1;

            bucketPeopleCategories[pid].insert(cid);
        }

        BOOST_TEST_REQUIRE(dataGatherer.dataAvailable(time));
        BOOST_TEST_REQUIRE(!dataGatherer.dataAvailable(time - 1));

        TFeatureSizeSizePrFeatureDataPrVecPrVec featureData;
        dataGatherer.featureData(time, bucketLength, featureData);

        const TSizeSizePrFeatureDataPrVec& peoplePerAttribute =
            extractPeoplePerAttribute(featureData);
        BOOST_TEST_REQUIRE(peoplePerAttribute.empty());

        const TSizeSizePrFeatureDataPrVec& attributeIndicator =
            extractAttributeIndicator(featureData);
        BOOST_TEST_REQUIRE(attributeIndicator.empty());

        const TSizeSizePrFeatureDataPrVec& bucketAttributesPerPerson =
            extractBucketAttributesPerPerson(featureData);

        BOOST_REQUIRE_EQUAL(core::CContainerPrinter::print(expectedUniqueCounts),
                            core::CContainerPrinter::print(bucketAttributesPerPerson));

        dataGatherer.timeNow(time + bucketLength);
    }
}

BOOST_FIXTURE_TEST_CASE(testCompressedLength, CTestFixture) {
    // We check that we correctly sample the compressed length of unique
    // values per person.

    const core_t::TTime startTime = 1367280000;
    const core_t::TTime bucketLength = 3600;
    const std::size_t numberBuckets = 20;

    test::CRandomNumbers rng;

    CDataGatherer::TFeatureVec features;
    features.push_back(model_t::E_PopulationInfoContentByBucketPersonAndAttribute);
    SModelParams params(bucketLength);
    CDataGatherer dataGatherer(model_t::E_PopulationEventRate, model_t::E_None, params,
                               EMPTY_STRING, EMPTY_STRING, EMPTY_STRING, EMPTY_STRING,
                               "value", {}, searchKey, features, startTime, 0);

    core_t::TTime time = startTime;
    for (std::size_t i = 0; i < numberBuckets; ++i, time += bucketLength) {
        TMessageVec messages;
        generateTestMessages(rng, time, bucketLength, messages);

        TSizeStrSetMap bucketPeopleCategories;
        for (std::size_t j = 0; j < messages.size(); ++j) {
            addArrival(messages[j].s_Time, messages[j].s_Person, "attribute",
                       messages[j].s_Attribute, dataGatherer, m_ResourceMonitor);

            std::size_t cid;
            dataGatherer.attributeId(messages[j].s_Attribute, cid);

            std::size_t pid;
            dataGatherer.personId(messages[j].s_Person, pid);

            bucketPeopleCategories[pid].insert(messages[j].s_Attribute);
        }

        BOOST_TEST_REQUIRE(dataGatherer.dataAvailable(time));
        BOOST_TEST_REQUIRE(!dataGatherer.dataAvailable(time - 1));

        TFeatureSizeSizePrFeatureDataPrVecPrVec featureData;
        dataGatherer.featureData(time, bucketLength, featureData);

        const TSizeSizePrFeatureDataPrVec& peoplePerAttribute =
            extractPeoplePerAttribute(featureData);
        BOOST_TEST_REQUIRE(peoplePerAttribute.empty());

        const TSizeSizePrFeatureDataPrVec& attributeIndicator =
            extractAttributeIndicator(featureData);
        BOOST_TEST_REQUIRE(attributeIndicator.empty());

        const TSizeSizePrFeatureDataPrVec& bucketCompressedLengthPerPerson =
            extractCompressedLengthPerPerson(featureData);
        BOOST_REQUIRE_EQUAL(bucketPeopleCategories.size(),
                            bucketCompressedLengthPerPerson.size());

        TSizeSizePrUInt64Map expectedBucketCompressedLengthPerPerson;
        for (TSizeStrSetMapItr iter = bucketPeopleCategories.begin();
             iter != bucketPeopleCategories.end(); ++iter) {
            TSizeSizePr key(iter->first, 0);
            const TStrSet& uniqueValues = iter->second;

            core::CDeflator compressor(false);
            BOOST_REQUIRE_EQUAL(
                uniqueValues.size(),
                static_cast<size_t>(std::count_if(
                    uniqueValues.begin(), uniqueValues.end(),
                    std::bind_front(&core::CCompressUtil::addString, &compressor))));
            std::size_t length(0);
            BOOST_TEST_REQUIRE(compressor.length(true, length));
            expectedBucketCompressedLengthPerPerson[key] = length;
        }
        LOG_DEBUG(<< "Time " << time << " bucketCompressedLengthPerPerson "
                  << bucketCompressedLengthPerPerson);
        BOOST_REQUIRE_EQUAL(expectedBucketCompressedLengthPerPerson.size(),
                            bucketCompressedLengthPerPerson.size());
        for (TSizeSizePrFeatureDataPrVec::const_iterator j =
                 bucketCompressedLengthPerPerson.begin();
             j != bucketCompressedLengthPerPerson.end(); ++j) {
            double expectedLength = static_cast<double>(
                expectedBucketCompressedLengthPerPerson[j->first]);
            double actual = static_cast<double>(j->second.s_Count);
            BOOST_REQUIRE_CLOSE_ABSOLUTE(expectedLength, actual, expectedLength * 0.1);
        }

        dataGatherer.timeNow(time + bucketLength);
    }
}

BOOST_FIXTURE_TEST_CASE(testRemovePeople, CTestFixture) {
    using TStrSizeMap = std::map<std::string, std::size_t>;
    using TSizeUInt64Pr = std::pair<std::size_t, std::uint64_t>;
    using TSizeUInt64PrVec = std::vector<TSizeUInt64Pr>;

    const core_t::TTime startTime = 1367280000;
    const core_t::TTime bucketLength = 3600;
    const std::size_t numberBuckets = 50;

    test::CRandomNumbers rng;

    CDataGatherer::TFeatureVec features;
    features.push_back(model_t::E_PopulationCountByBucketPersonAndAttribute);
    SModelParams params(bucketLength);
    CDataGatherer gatherer(model_t::E_PopulationEventRate, model_t::E_None, params,
                           EMPTY_STRING, EMPTY_STRING, EMPTY_STRING, EMPTY_STRING,
                           EMPTY_STRING, {}, searchKey, features, startTime, 0);
    core_t::TTime bucketStart = startTime;
    for (std::size_t i = 0; i < numberBuckets; ++i, bucketStart += bucketLength) {
        TMessageVec messages;
        generateTestMessages(rng, bucketStart, bucketLength, messages);
        for (std::size_t j = 0; j < messages.size(); ++j) {
            addArrival(messages[j].s_Time, messages[j].s_Person,
                       messages[j].s_Attribute, gatherer, m_ResourceMonitor);
        }
    }

    // Remove people 1 and 4.
    TSizeVec peopleToRemove;
    peopleToRemove.push_back(1u);
    peopleToRemove.push_back(4u);

    std::size_t numberPeople = gatherer.numberActivePeople();
    LOG_DEBUG(<< "numberPeople = " << numberPeople);
    BOOST_REQUIRE_EQUAL(numberPeople, gatherer.numberOverFieldValues());
    TStrVec expectedPersonNames;
    TSizeVec expectedPersonIds;
    for (std::size_t i = 0; i < numberPeople; ++i) {
        if (!std::binary_search(peopleToRemove.begin(), peopleToRemove.end(), i)) {
            expectedPersonNames.push_back(gatherer.personName(i));
            expectedPersonIds.push_back(i);
        } else {
            LOG_DEBUG(<< "Removing " << gatherer.personName(i));
        }
    }

    TStrSizeMap expectedNonZeroCounts;
    {
        TSizeUInt64PrVec nonZeroCounts;
        gatherer.personNonZeroCounts(bucketStart - bucketLength, nonZeroCounts);
        for (std::size_t i = 0; i < nonZeroCounts.size(); ++i) {
            if (!std::binary_search(peopleToRemove.begin(), peopleToRemove.end(),
                                    nonZeroCounts[i].first)) {
                const std::string& name = gatherer.personName(nonZeroCounts[i].first);
                expectedNonZeroCounts[name] =
                    static_cast<size_t>(nonZeroCounts[i].second);
            }
        }
    }
    LOG_DEBUG(<< "expectedNonZeroCounts = " << expectedNonZeroCounts);

    std::string expectedFeatureData;
    {
        LOG_DEBUG(<< "Expected");
        TStrFeatureDataPrVec expected;
        TFeatureSizeSizePrFeatureDataPrVecPrVec featureData;
        gatherer.featureData(bucketStart - bucketLength, bucketLength, featureData);
        for (std::size_t i = 0; i < featureData.size(); ++i) {
            const TSizeSizePrFeatureDataPrVec& data = featureData[i].second;
            for (std::size_t j = 0; j < data.size(); ++j) {
                if (!std::binary_search(peopleToRemove.begin(),
                                        peopleToRemove.end(), data[j].first.first)) {
                    std::string key = model_t::print(featureData[i].first) + " " +
                                      gatherer.personName(data[j].first.first) + " " +
                                      gatherer.attributeName(data[j].first.second);
                    expected.push_back(TStrFeatureDataPr(key, data[j].second));
                    LOG_DEBUG(<< "  " << key << " = " << data[j].second.s_Count);
                }
            }
        }
        expectedFeatureData = core::CContainerPrinter::print(expected);
    }

    gatherer.recyclePeople(peopleToRemove);

    BOOST_REQUIRE_EQUAL(numberPeople - peopleToRemove.size(), gatherer.numberActivePeople());
    for (std::size_t i = 0; i < expectedPersonNames.size(); ++i) {
        std::size_t pid;
        BOOST_TEST_REQUIRE(gatherer.personId(expectedPersonNames[i], pid));
        BOOST_REQUIRE_EQUAL(expectedPersonIds[i], pid);
    }

    TStrSizeMap actualNonZeroCounts;
    TSizeUInt64PrVec nonZeroCounts;
    gatherer.personNonZeroCounts(bucketStart - bucketLength, nonZeroCounts);
    for (std::size_t i = 0; i < nonZeroCounts.size(); ++i) {
        const std::string& name = gatherer.personName(nonZeroCounts[i].first);
        actualNonZeroCounts[name] = static_cast<size_t>(nonZeroCounts[i].second);
    }
    LOG_DEBUG(<< "actualNonZeroCounts = " << actualNonZeroCounts);

    BOOST_REQUIRE_EQUAL(core::CContainerPrinter::print(expectedNonZeroCounts),
                        core::CContainerPrinter::print(actualNonZeroCounts));

    std::string actualFeatureData;
    {
        LOG_DEBUG(<< "Actual");
        TStrFeatureDataPrVec actual;
        TFeatureSizeSizePrFeatureDataPrVecPrVec featureData;
        gatherer.featureData(bucketStart - bucketLength, bucketLength, featureData);
        for (std::size_t i = 0; i < featureData.size(); ++i) {
            const TSizeSizePrFeatureDataPrVec& data = featureData[i].second;
            for (std::size_t j = 0; j < data.size(); ++j) {
                std::string key = model_t::print(featureData[i].first) + " " +
                                  gatherer.personName(data[j].first.first) + " " +
                                  gatherer.attributeName(data[j].first.second);
                actual.push_back(TStrFeatureDataPr(key, data[j].second));
                LOG_DEBUG(<< "  " << key << " = " << data[j].second.s_Count);
            }
        }
        actualFeatureData = core::CContainerPrinter::print(actual);
    }

    BOOST_REQUIRE_EQUAL(expectedFeatureData, actualFeatureData);
}

BOOST_FIXTURE_TEST_CASE(testRemoveAttributes, CTestFixture) {
    const core_t::TTime startTime = 1367280000;
    const core_t::TTime bucketLength = 3600;

    test::CRandomNumbers rng;

    CDataGatherer::TFeatureVec features;
    features.push_back(model_t::E_PopulationCountByBucketPersonAndAttribute);
    features.push_back(model_t::E_PopulationUniquePersonCountByAttribute);
    SModelParams params(bucketLength);
    CDataGatherer gatherer(model_t::E_PopulationEventRate, model_t::E_None, params,
                           EMPTY_STRING, EMPTY_STRING, EMPTY_STRING, EMPTY_STRING,
                           EMPTY_STRING, {}, searchKey, features, startTime, 0);

    TMessageVec messages;
    generateTestMessages(rng, startTime, bucketLength, messages);

    core_t::TTime bucketStart = startTime;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        addArrival(messages[i].s_Time, messages[i].s_Person,
                   messages[i].s_Attribute, gatherer, m_ResourceMonitor);
    }

    // Remove attributes 1, 2, 3 and 15.
    TSizeVec attributesToRemove;
    attributesToRemove.push_back(1u);
    attributesToRemove.push_back(2u);
    attributesToRemove.push_back(3u);
    attributesToRemove.push_back(15u);

    std::size_t numberAttributes = gatherer.numberActiveAttributes();
    LOG_DEBUG(<< "numberAttributes = " << numberAttributes);
    BOOST_REQUIRE_EQUAL(numberAttributes, gatherer.numberByFieldValues());
    TStrVec expectedAttributeNames;
    TSizeVec expectedAttributeIds;
    for (std::size_t i = 0; i < numberAttributes; ++i) {
        if (!std::binary_search(attributesToRemove.begin(), attributesToRemove.end(), i)) {
            expectedAttributeNames.push_back(gatherer.attributeName(i));
            expectedAttributeIds.push_back(i);
        } else {
            LOG_DEBUG(<< "Removing " << gatherer.attributeName(i));
        }
    }

    std::string expectedFeatureData;
    {
        LOG_DEBUG(<< "Expected");
        TStrFeatureDataPrVec expected;
        TFeatureSizeSizePrFeatureDataPrVecPrVec featureData;
        gatherer.featureData(bucketStart, bucketLength, featureData);
        for (std::size_t i = 0; i < featureData.size(); ++i) {
            const TSizeSizePrFeatureDataPrVec& data = featureData[i].second;
            for (std::size_t j = 0; j < data.size(); ++j) {
                if (!std::binary_search(attributesToRemove.begin(),
                                        attributesToRemove.end(), data[j].first.second)) {
                    std::string key = model_t::print(featureData[i].first) + " " +
                                      gatherer.personName(data[j].first.first) + " " +
                                      gatherer.attributeName(data[j].first.second);
                    expected.push_back(TStrFeatureDataPr(key, data[j].second));
                    LOG_DEBUG(<< "  " << key << " = " << data[j].second.s_Count);
                }
            }
        }
        expectedFeatureData = core::CContainerPrinter::print(expected);
    }

    gatherer.recycleAttributes(attributesToRemove);

    BOOST_REQUIRE_EQUAL(numberAttributes - attributesToRemove.size(),
                        gatherer.numberActiveAttributes());
    for (std::size_t i = 0; i < expectedAttributeNames.size(); ++i) {
        std::size_t cid;
        BOOST_TEST_REQUIRE(gatherer.attributeId(expectedAttributeNames[i], cid));
        BOOST_REQUIRE_EQUAL(expectedAttributeIds[i], cid);
    }

    std::string actualFeatureData;
    {
        LOG_DEBUG(<< "Actual");
        TStrFeatureDataPrVec actual;
        TFeatureSizeSizePrFeatureDataPrVecPrVec featureData;
        gatherer.featureData(bucketStart, bucketLength, featureData);
        for (std::size_t i = 0; i < featureData.size(); ++i) {
            const TSizeSizePrFeatureDataPrVec& data = featureData[i].second;
            for (std::size_t j = 0; j < data.size(); ++j) {
                std::string key = model_t::print(featureData[i].first) + " " +
                                  gatherer.personName(data[j].first.first) + " " +
                                  gatherer.attributeName(data[j].first.second);
                actual.push_back(TStrFeatureDataPr(key, data[j].second));
                LOG_DEBUG(<< "  " << key << " = " << data[j].second.s_Count);
            }
        }
        actualFeatureData = core::CContainerPrinter::print(actual);
    }

    BOOST_REQUIRE_EQUAL(expectedFeatureData, actualFeatureData);
}

namespace {
bool isSpace(const char x) {
    return x == ' ' || x == '\t';
}
}

void testPersistDataGatherer(const CDataGatherer& origDataGatherer,
                             const SModelParams& params) {
    std::ostringstream origJson;
    core::CJsonStatePersistInserter::persist(
        origJson, std::bind_front(&CDataGatherer::acceptPersistInserter, &origDataGatherer));
    LOG_DEBUG(<< "origJson = " << origJson.str());

    // Restore the Json into a new data gatherer
    // The traverser expects the state json in a embedded document
    std::istringstream origJsonStrm("{\"topLevel\" : " + origJson.str() + "}");
    core::CJsonStateRestoreTraverser traverser(origJsonStrm);

    CDataGatherer restoredDataGatherer(model_t::E_PopulationEventRate,
                                       model_t::E_None, params, EMPTY_STRING,
                                       EMPTY_STRING, EMPTY_STRING, EMPTY_STRING,
                                       EMPTY_STRING, {}, searchKey, traverser);

    // The Json representation of the new data gatherer should be the same as the
    // original
    std::ostringstream newJson;
    core::CJsonStatePersistInserter::persist(
        newJson, std::bind_front(&CDataGatherer::acceptPersistInserter, &restoredDataGatherer));
    BOOST_REQUIRE_EQUAL(origJson.str(), newJson.str());
}

BOOST_FIXTURE_TEST_CASE(testPersistence, CTestFixture) {
    const core_t::TTime startTime = 1367280000;
    const core_t::TTime bucketLength = 3600;

    {
        test::CRandomNumbers rng;

        CDataGatherer::TFeatureVec features;
        features.push_back(model_t::E_PopulationCountByBucketPersonAndAttribute);
        features.push_back(model_t::E_PopulationUniquePersonCountByAttribute);
        SModelParams params(bucketLength);
        CDataGatherer origDataGatherer(model_t::E_PopulationEventRate, model_t::E_None,
                                       params, EMPTY_STRING, EMPTY_STRING,
                                       EMPTY_STRING, EMPTY_STRING, EMPTY_STRING,
                                       {}, searchKey, features, startTime, 0);

        TMessageVec messages;
        generateTestMessages(rng, startTime, bucketLength, messages);

        for (std::size_t i = 0; i < messages.size(); ++i) {
            addArrival(messages[i].s_Time, messages[i].s_Person,
                       messages[i].s_Attribute, origDataGatherer, m_ResourceMonitor);
        }

        testPersistDataGatherer(origDataGatherer, params);
    }
    {
        // Check feature data for model_t::E_UniqueValues
        const std::size_t numberBuckets = 20;

        test::CRandomNumbers rng;
        CDataGatherer::TFeatureVec features;
        features.push_back(model_t::E_PopulationInfoContentByBucketPersonAndAttribute);
        SModelParams params(bucketLength);
        CDataGatherer dataGatherer(model_t::E_PopulationEventRate, model_t::E_None,
                                   params, EMPTY_STRING, EMPTY_STRING,
                                   EMPTY_STRING, EMPTY_STRING, "value", {},
                                   searchKey, features, startTime, 0);

        core_t::TTime time = startTime;
        for (std::size_t i = 0; i < numberBuckets; ++i, time += bucketLength) {
            TMessageVec messages;
            generateTestMessages(rng, time, bucketLength, messages);

            for (std::size_t j = 0; j < messages.size(); ++j) {
                addArrival(messages[j].s_Time, messages[j].s_Person, "attribute",
                           messages[j].s_Attribute, dataGatherer, m_ResourceMonitor);

                std::size_t cid;
                dataGatherer.attributeId(messages[j].s_Attribute, cid);

                std::size_t pid;
                dataGatherer.personId(messages[j].s_Person, pid);
            }

            BOOST_TEST_REQUIRE(dataGatherer.dataAvailable(time));
            BOOST_TEST_REQUIRE(!dataGatherer.dataAvailable(time - 1));

            dataGatherer.timeNow(time + bucketLength);
        }

        testPersistDataGatherer(dataGatherer, params);
    }
}

BOOST_AUTO_TEST_SUITE_END()
