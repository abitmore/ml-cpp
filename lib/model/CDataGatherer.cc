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

#include <model/CDataGatherer.h>

#include <core/CLogger.h>
#include <core/CMemoryDef.h>
#include <core/CProgramCounters.h>
#include <core/CStatePersistInserter.h>
#include <core/CStateRestoreTraverser.h>
#include <core/CStringUtils.h>
#include <core/RestoreMacros.h>

#include <maths/common/CChecksum.h>
#include <maths/common/CMathsFuncs.h>

#include <model/CEventRateBucketGatherer.h>
#include <model/CMetricBucketGatherer.h>
#include <model/CSampleCounts.h>
#include <model/CSearchKey.h>

#include <algorithm>
#include <utility>

namespace ml {
namespace model {

namespace {

const std::string FEATURE_TAG("a");
const std::string PEOPLE_REGISTRY_TAG("b");
const std::string ATTRIBUTES_REGISTRY_TAG("c");
const std::string SAMPLE_COUNTS_TAG("d");
const std::string BUCKET_GATHERER_TAG("e");
const std::string DEFAULT_PERSON_NAME("-");
const std::string DEFAULT_ATTRIBUTE_NAME("-");

const std::string PERSON("person");
const std::string ATTRIBUTE("attribute");

const std::string EMPTY_STRING;

namespace detail {

//! Make sure \p features only includes supported features, doesn't
//! contain any duplicates, etc.
const CDataGatherer::TFeatureVec& sanitize(CDataGatherer::TFeatureVec& features,
                                           model_t::EAnalysisCategory gathererType) {
    std::size_t j = 0;

    for (std::size_t i = 0; i < features.size(); ++i) {
        switch (gathererType) {
        case model_t::E_EventRate:
        case model_t::E_PopulationEventRate:
            switch (features[i]) {
            CASE_INDIVIDUAL_COUNT:
                features[j] = features[i];
                ++j;
                break;

            CASE_INDIVIDUAL_METRIC:
                LOG_ERROR(<< "Unexpected feature = " << model_t::print(features[i]));
                break;

            CASE_POPULATION_COUNT:
                features[j] = features[i];
                ++j;
                break;

            CASE_POPULATION_METRIC:
                LOG_ERROR(<< "Unexpected feature = " << model_t::print(features[i]));
                break;
            }
            break;

        case model_t::E_Metric:
        case model_t::E_PopulationMetric:
            switch (features[i]) {
            CASE_INDIVIDUAL_COUNT:
                LOG_ERROR(<< "Unexpected feature = " << model_t::print(features[i]));
                break;

            CASE_INDIVIDUAL_METRIC:
                features[j] = features[i];
                ++j;
                break;

            CASE_POPULATION_COUNT:
                LOG_ERROR(<< "Unexpected feature = " << model_t::print(features[i]));
                break;

            CASE_POPULATION_METRIC:
                features[j] = features[i];
                ++j;
                break;
            }
            break;
        }
    }

    features.erase(features.begin() + j, features.end());
    std::sort(features.begin(), features.end());
    features.erase(std::unique(features.begin(), features.end()), features.end());

    return features;
}

//! Wrapper which copies \p features.
CDataGatherer::TFeatureVec sanitize(const CDataGatherer::TFeatureVec& features,
                                    model_t::EAnalysisCategory gathererType) {
    CDataGatherer::TFeatureVec result(features);
    return sanitize(result, gathererType);
}

//! Check if the gatherer is for population modelling.
bool isPopulation(model_t::EAnalysisCategory gathererType) {
    switch (gathererType) {
    case model_t::E_EventRate:
    case model_t::E_Metric:
        return false;

    case model_t::E_PopulationEventRate:
    case model_t::E_PopulationMetric:
        return true;
    }
    return false;
}

} // detail::
} // unnamed::

const std::string CDataGatherer::EXPLICIT_NULL("null");
const std::size_t
    CDataGatherer::EXPLICIT_NULL_SUMMARY_COUNT(std::numeric_limits<std::size_t>::max());
const std::size_t CDataGatherer::ESTIMATED_MEM_USAGE_PER_BY_FIELD(20000);
const std::size_t CDataGatherer::ESTIMATED_MEM_USAGE_PER_OVER_FIELD(1000);

CDataGatherer::CDataGatherer(model_t::EAnalysisCategory gathererType,
                             model_t::ESummaryMode summaryMode,
                             const SModelParams& modelParams,
                             std::string partitionFieldValue,
                             const CSearchKey& key,
                             const TFeatureVec& features,
                             const CBucketGatherer::SBucketGathererInitData& bucketGathererInitData)
    : m_GathererType(gathererType),
      m_Features(detail::sanitize(features, gathererType)),
      m_SummaryMode(summaryMode), m_Params(modelParams), m_SearchKey(key),
      m_PartitionFieldValue(std::move(partitionFieldValue)),
      m_PeopleRegistry(PERSON,
                       counter_t::E_TSADNumberNewPeople,
                       counter_t::E_TSADNumberNewPeopleNotAllowed,
                       counter_t::E_TSADNumberNewPeopleRecycled),
      m_AttributesRegistry(ATTRIBUTE,
                           counter_t::E_TSADNumberNewAttributes,
                           counter_t::E_TSADNumberNewAttributesNotAllowed,
                           counter_t::E_TSADNumberNewAttributesRecycled),
      m_Population(detail::isPopulation(gathererType)), m_UseNull(key.useNull()) {

    std::sort(m_Features.begin(), m_Features.end());
    this->createBucketGatherer(gathererType, bucketGathererInitData);
}

CDataGatherer::CDataGatherer(model_t::EAnalysisCategory gathererType,
                             model_t::ESummaryMode summaryMode,
                             const SModelParams& modelParams,
                             std::string partitionFieldValue,
                             const CSearchKey& key,
                             const CBucketGatherer::SBucketGathererInitData& bucketGathererInitData,
                             core::CStateRestoreTraverser& traverser)
    : m_GathererType(gathererType), m_SummaryMode(summaryMode), m_Params(modelParams),
      m_SearchKey(key), m_PartitionFieldValue(std::move(partitionFieldValue)),
      m_PeopleRegistry(PERSON,
                       counter_t::E_TSADNumberNewPeople,
                       counter_t::E_TSADNumberNewPeopleNotAllowed,
                       counter_t::E_TSADNumberNewPeopleRecycled),
      m_AttributesRegistry(ATTRIBUTE,
                           counter_t::E_TSADNumberNewAttributes,
                           counter_t::E_TSADNumberNewAttributesNotAllowed,
                           counter_t::E_TSADNumberNewAttributesRecycled),
      m_Population(detail::isPopulation(gathererType)), m_UseNull(key.useNull()) {
    auto func = [this, &bucketGathererInitData](core::CStateRestoreTraverser& traverser_) {
        return acceptRestoreTraverser(bucketGathererInitData, traverser_);
    };
    if (traverser.traverseSubLevel(func) == false) {
        LOG_ERROR(<< "Failed to correctly restore data gatherer");
    }
}

CDataGatherer::CDataGatherer(bool isForPersistence, const CDataGatherer& other)
    : m_GathererType(other.m_GathererType), m_Features(other.m_Features),
      m_SummaryMode(other.m_SummaryMode), m_Params(other.m_Params),
      m_SearchKey(other.m_SearchKey),
      m_PartitionFieldValue(other.m_PartitionFieldValue),
      m_PeopleRegistry(isForPersistence, other.m_PeopleRegistry),
      m_AttributesRegistry(isForPersistence, other.m_AttributesRegistry),
      m_Population(other.m_Population), m_UseNull(other.m_UseNull) {
    if (!isForPersistence) {
        LOG_ABORT(<< "This constructor only creates clones for persistence");
    }
    m_BucketGatherer.reset(other.m_BucketGatherer->cloneForPersistence());
    if (other.m_SampleCounts) {
        m_SampleCounts.reset(other.m_SampleCounts->cloneForPersistence());
    }
}

CDataGatherer::~CDataGatherer() = default;

CDataGatherer* CDataGatherer::cloneForPersistence() const {
    return new CDataGatherer(true, *this);
}

model_t::ESummaryMode CDataGatherer::summaryMode() const {
    return m_SummaryMode;
}

model::function_t::EFunction CDataGatherer::function() const {
    return function_t::function(this->features());
}

bool CDataGatherer::isPopulation() const {
    return m_Population;
}

std::string CDataGatherer::description() const {
    return m_BucketGatherer->description();
}

std::size_t CDataGatherer::maxDimension() const {
    return std::max(this->numberPeople(), this->numberAttributes());
}

const std::string& CDataGatherer::partitionFieldName() const {
    return ml::core::unwrap_ref(m_SearchKey).partitionFieldName();
}

const std::string& CDataGatherer::partitionFieldValue() const {
    return m_PartitionFieldValue;
}

const CSearchKey& CDataGatherer::searchKey() const {
    return m_SearchKey;
}

CDataGatherer::TStrVecCItr CDataGatherer::beginInfluencers() const {
    return m_BucketGatherer->beginInfluencers();
}

CDataGatherer::TStrVecCItr CDataGatherer::endInfluencers() const {
    return m_BucketGatherer->endInfluencers();
}

const std::string& CDataGatherer::personFieldName() const {
    return m_BucketGatherer->personFieldName();
}

const std::string& CDataGatherer::attributeFieldName() const {
    return m_BucketGatherer->attributeFieldName();
}

const std::string& CDataGatherer::valueFieldName() const {
    return m_BucketGatherer->valueFieldName();
}

const CDataGatherer::TStrVec& CDataGatherer::fieldsOfInterest() const {
    return m_BucketGatherer->fieldsOfInterest();
}

std::size_t CDataGatherer::numberByFieldValues() const {
    return this->isPopulation() ? this->numberActiveAttributes()
                                : this->numberActivePeople();
}

std::size_t CDataGatherer::numberOverFieldValues() const {
    return this->isPopulation() ? this->numberActivePeople() : 0;
}

bool CDataGatherer::processFields(const TStrCPtrVec& fieldValues,
                                  CEventData& result,
                                  CResourceMonitor& resourceMonitor) {
    return m_BucketGatherer->processFields(fieldValues, result, resourceMonitor);
}

bool CDataGatherer::addArrival(const TStrCPtrVec& fieldValues,
                               CEventData& data,
                               CResourceMonitor& resourceMonitor) {
    // We process fields even if we are in the first partial bucket so that
    // we add enough extra memory to the resource monitor in order to control
    // the number of partitions created.
    m_BucketGatherer->processFields(fieldValues, data, resourceMonitor);

    if (core_t::TTime const time = data.time();
        time < m_BucketGatherer->earliestBucketStartTime()) {
        // Ignore records that are out of the latency window.
        // Records in an incomplete first bucket will end up here,
        // but we don't want to model these.
        return false;
    }

    return m_BucketGatherer->addEventData(data);
}

void CDataGatherer::sampleNow(core_t::TTime sampleBucketStart) {
    m_BucketGatherer->sampleNow(sampleBucketStart);
}

void CDataGatherer::skipSampleNow(core_t::TTime sampleBucketStart) {
    m_BucketGatherer->skipSampleNow(sampleBucketStart);
}

std::size_t CDataGatherer::numberFeatures() const {
    return m_Features.size();
}

bool CDataGatherer::hasFeature(model_t::EFeature feature) const {
    return std::binary_search(m_Features.begin(), m_Features.end(), feature);
}

model_t::EFeature CDataGatherer::feature(std::size_t i) const {
    return m_Features[i];
}

const CDataGatherer::TFeatureVec& CDataGatherer::features() const {
    return m_Features;
}

std::size_t CDataGatherer::numberActivePeople() const {
    return m_PeopleRegistry.numberActiveNames();
}

std::size_t CDataGatherer::numberPeople() const {
    return m_PeopleRegistry.numberNames();
}

bool CDataGatherer::personId(const std::string& person, std::size_t& result) const {
    return m_PeopleRegistry.id(person, result);
}

bool CDataGatherer::anyPersonId(std::size_t& result) const {
    return m_PeopleRegistry.anyId(result);
}

const std::string& CDataGatherer::personName(std::size_t pid) const {
    return this->personName(pid, DEFAULT_PERSON_NAME);
}

const std::string& CDataGatherer::personName(std::size_t pid, const std::string& fallback) const {
    return m_PeopleRegistry.name(pid, fallback);
}

void CDataGatherer::personNonZeroCounts(core_t::TTime time, TSizeUInt64PrVec& result) const {
    m_BucketGatherer->personNonZeroCounts(time, result);
}

void CDataGatherer::recyclePeople(const TSizeVec& peopleToRemove) {
    if (peopleToRemove.empty()) {
        return;
    }

    m_BucketGatherer->recyclePeople(peopleToRemove);

    if (!this->isPopulation() && m_SampleCounts) {
        m_SampleCounts->recycle(peopleToRemove);
    }

    m_PeopleRegistry.recycleNames(peopleToRemove, DEFAULT_PERSON_NAME);
    core::CProgramCounters::counter(counter_t::E_TSADNumberPrunedItems) +=
        peopleToRemove.size();
}

void CDataGatherer::removePeople(std::size_t lowestPersonToRemove) {
    if (lowestPersonToRemove >= this->numberPeople()) {
        return;
    }

    if (!this->isPopulation() && m_SampleCounts) {
        m_SampleCounts->remove(lowestPersonToRemove);
    }

    m_BucketGatherer->removePeople(lowestPersonToRemove);

    m_PeopleRegistry.removeNames(lowestPersonToRemove);
}

CDataGatherer::TSizeVec& CDataGatherer::recycledPersonIds() {
    return m_PeopleRegistry.recycledIds();
}

bool CDataGatherer::isPersonActive(std::size_t pid) const {
    return m_PeopleRegistry.isIdActive(pid);
}

std::size_t CDataGatherer::addPerson(const std::string& person,
                                     CResourceMonitor& resourceMonitor,
                                     bool& addedPerson) {
    return m_PeopleRegistry.addName(person, m_BucketGatherer->currentBucketStartTime(),
                                    resourceMonitor, addedPerson);
}

std::size_t CDataGatherer::numberActiveAttributes() const {
    return m_AttributesRegistry.numberActiveNames();
}

std::size_t CDataGatherer::numberAttributes() const {
    return m_AttributesRegistry.numberNames();
}

bool CDataGatherer::attributeId(const std::string& attribute, std::size_t& result) const {
    return m_AttributesRegistry.id(attribute, result);
}

const std::string& CDataGatherer::attributeName(std::size_t cid) const {
    return this->attributeName(cid, DEFAULT_ATTRIBUTE_NAME);
}

const std::string& CDataGatherer::attributeName(std::size_t cid,
                                                const std::string& fallback) const {
    return m_AttributesRegistry.name(cid, fallback);
}

void CDataGatherer::recycleAttributes(const TSizeVec& attributesToRemove) {
    if (attributesToRemove.empty()) {
        return;
    }

    if (this->isPopulation() && m_SampleCounts) {
        m_SampleCounts->recycle(attributesToRemove);
    }

    m_BucketGatherer->recycleAttributes(attributesToRemove);

    m_AttributesRegistry.recycleNames(attributesToRemove, DEFAULT_ATTRIBUTE_NAME);
    core::CProgramCounters::counter(counter_t::E_TSADNumberPrunedItems) +=
        attributesToRemove.size();
}

void CDataGatherer::removeAttributes(std::size_t lowestAttributeToRemove) {
    if (lowestAttributeToRemove >= this->numberAttributes()) {
        return;
    }

    if (this->isPopulation() && m_SampleCounts) {
        m_SampleCounts->remove(lowestAttributeToRemove);
    }

    m_BucketGatherer->removeAttributes(lowestAttributeToRemove);

    m_AttributesRegistry.removeNames(lowestAttributeToRemove);
}

CDataGatherer::TSizeVec& CDataGatherer::recycledAttributeIds() {
    return m_AttributesRegistry.recycledIds();
}

bool CDataGatherer::isAttributeActive(std::size_t cid) const {
    return m_AttributesRegistry.isIdActive(cid);
}

std::size_t CDataGatherer::addAttribute(const std::string& attribute,
                                        CResourceMonitor& resourceMonitor,
                                        bool& addedAttribute) {
    return m_AttributesRegistry.addName(attribute,
                                        m_BucketGatherer->currentBucketStartTime(),
                                        resourceMonitor, addedAttribute);
}

double CDataGatherer::sampleCount(std::size_t id) const {
    if (m_SampleCounts) {
        return static_cast<double>(m_SampleCounts->count(id));
    }
    LOG_ERROR(<< "Sample count for non-metric gatherer");
    return 0.0;
}

double CDataGatherer::effectiveSampleCount(std::size_t id) const {
    if (m_SampleCounts) {
        return m_SampleCounts->effectiveSampleCount(id);
    }
    LOG_ERROR(<< "Effective sample count for non-metric gatherer");
    return 0.0;
}

void CDataGatherer::resetSampleCount(std::size_t id) {
    if (m_SampleCounts) {
        m_SampleCounts->resetSampleCount(*this, id);
    }
}

const CDataGatherer::TSampleCountsPtr& CDataGatherer::sampleCounts() const {
    return m_SampleCounts;
}

core_t::TTime CDataGatherer::currentBucketStartTime() const {
    return m_BucketGatherer->currentBucketStartTime();
}

core_t::TTime CDataGatherer::bucketLength() const {
    return m_BucketGatherer->bucketLength();
}

bool CDataGatherer::dataAvailable(core_t::TTime time) const {
    return m_BucketGatherer->dataAvailable(time);
}

bool CDataGatherer::validateSampleTimes(core_t::TTime& startTime, core_t::TTime endTime) const {
    return m_BucketGatherer->validateSampleTimes(startTime, endTime);
}

void CDataGatherer::timeNow(core_t::TTime time) {
    m_BucketGatherer->timeNow(time);
}

std::string CDataGatherer::printCurrentBucket() const {
    return m_BucketGatherer->printCurrentBucket();
}

const CDataGatherer::TSizeSizePrUInt64UMap& CDataGatherer::bucketCounts(core_t::TTime time) const {
    return m_BucketGatherer->bucketCounts(time);
}

const CDataGatherer::TSizeSizePrOptionalStrPrUInt64UMapVec&
CDataGatherer::influencerCounts(core_t::TTime time) const {
    return m_BucketGatherer->influencerCounts(time);
}

std::uint64_t CDataGatherer::checksum() const {
    std::uint64_t result = m_PeopleRegistry.checksum();
    result = maths::common::CChecksum::calculate(result, m_AttributesRegistry);
    result = maths::common::CChecksum::calculate(result, m_SummaryMode);
    result = maths::common::CChecksum::calculate(result, m_Features);
    if (m_SampleCounts) {
        result = maths::common::CChecksum::calculate(result, m_SampleCounts->checksum(*this));
    }
    result = maths::common::CChecksum::calculate(result, m_BucketGatherer);

    LOG_TRACE(<< "checksum = " << result);

    return result;
}

void CDataGatherer::debugMemoryUsage(const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CDataGatherer");
    core::memory_debug::dynamicSize("m_Features", m_Features, mem);
    core::memory_debug::dynamicSize("m_PeopleRegistry", m_PeopleRegistry, mem);
    core::memory_debug::dynamicSize("m_AttributesRegistry", m_AttributesRegistry, mem);
    core::memory_debug::dynamicSize("m_SampleCounts", m_SampleCounts, mem);
    core::memory_debug::dynamicSize("m_BucketGatherer", m_BucketGatherer, mem);
}

std::size_t CDataGatherer::memoryUsage() const {
    std::size_t mem = core::memory::dynamicSize(m_Features);
    mem += core::memory::dynamicSize(m_PartitionFieldValue);
    mem += core::memory::dynamicSize(m_PeopleRegistry);
    mem += core::memory::dynamicSize(m_AttributesRegistry);
    mem += core::memory::dynamicSize(m_SampleCounts);
    mem += core::memory::dynamicSize(m_BucketGatherer);
    return mem;
}

bool CDataGatherer::useNull() const {
    return m_UseNull;
}

void CDataGatherer::clear() {
    m_PeopleRegistry.clear();
    m_AttributesRegistry.clear();
    if (m_SampleCounts) {
        m_SampleCounts->clear();
    }
    if (m_BucketGatherer) {
        m_BucketGatherer->clear();
    }
}

bool CDataGatherer::resetBucket(core_t::TTime bucketStart) {
    return m_BucketGatherer->resetBucket(bucketStart);
}

void CDataGatherer::releaseMemory(core_t::TTime samplingCutoffTime) {
    if (this->isPopulation()) {
        m_BucketGatherer->releaseMemory(samplingCutoffTime);
    }
}

const SModelParams& CDataGatherer::params() const {
    return m_Params;
}

void CDataGatherer::acceptPersistInserter(core::CStatePersistInserter& inserter) const {
    for (auto m_Feature : m_Features) {
        inserter.insertValue(FEATURE_TAG, static_cast<int>(m_Feature));
    }
    inserter.insertLevel(PEOPLE_REGISTRY_TAG, [this](core::CStatePersistInserter& inserter_) {
        m_PeopleRegistry.acceptPersistInserter(inserter_);
    });
    inserter.insertLevel(ATTRIBUTES_REGISTRY_TAG, [this](core::CStatePersistInserter& inserter_) {
        m_AttributesRegistry.acceptPersistInserter(inserter_);
    });

    if (m_SampleCounts) {
        inserter.insertLevel(SAMPLE_COUNTS_TAG, [sampleCounts = m_SampleCounts.get()](
                                                    core::CStatePersistInserter & inserter_) {
            sampleCounts->acceptPersistInserter(inserter_);
        });
    }

    inserter.insertLevel(BUCKET_GATHERER_TAG, [this](core::CStatePersistInserter& inserter_) {
        persistBucketGatherers(inserter_);
    });
}

bool CDataGatherer::determineMetricCategory(TMetricCategoryVec& fieldMetricCategories) const {
    if (m_Features.empty()) {
        LOG_WARN(<< "No features to determine metric category from");
        return false;
    }

    if (m_Features.size() > 1) {
        LOG_WARN(<< m_Features.size()
                 << " features to determine metric category "
                    "from - only the first will be used");
    }

    model_t::EMetricCategory result;
    if (model_t::metricCategory(m_Features.front(), result) == false) {
        LOG_ERROR(<< "Unable to map feature " << model_t::print(m_Features.front())
                  << " to a metric category");
        return false;
    }

    fieldMetricCategories.push_back(result);

    return true;
}

bool CDataGatherer::extractCountFromField(const std::string& fieldName,
                                          const std::string* fieldValue,
                                          std::size_t& count) {
    if (fieldValue == nullptr) {
        // Treat not present as explicit null
        count = EXPLICIT_NULL_SUMMARY_COUNT;
        return true;
    }

    std::string fieldValueCopy(*fieldValue);
    core::CStringUtils::trimWhitespace(fieldValueCopy);
    if (fieldValueCopy.empty() || fieldValueCopy == EXPLICIT_NULL) {
        count = EXPLICIT_NULL_SUMMARY_COUNT;
        return true;
    }

    double count_;
    if (core::CStringUtils::stringToType(fieldValueCopy, count_) == false || count_ < 0.0) {
        LOG_ERROR(<< "Unable to extract count " << fieldName << " from " << fieldValueCopy);
        return false;
    }
    count = static_cast<std::size_t>(count_ + 0.5);

    // Treat count of 0 as a failure to extract. This will cause the record to be ignored.
    return count > 0;
}

bool CDataGatherer::extractMetricFromField(const std::string& fieldName,
                                           std::string fieldValue,
                                           TDouble1Vec& result) const {
    result.clear();

    core::CStringUtils::trimWhitespace(fieldValue);
    if (fieldValue.empty()) {
        LOG_WARN(<< "Configured metric " << fieldName << " not present in event");
        return false;
    }

    const std::string& delimiter = m_Params.get().s_MultivariateComponentDelimiter;

    // Split the string up by the delimiter and parse each token separately.
    std::size_t first = 0;
    do {
        std::size_t const last = fieldValue.find(delimiter, first);
        double value;
        // Avoid a string duplication in the (common) case of only one value
        bool const convertedOk = (first == 0 && last == std::string::npos)
                                     ? core::CStringUtils::stringToType(fieldValue, value)
                                     : core::CStringUtils::stringToType(
                                           fieldValue.substr(first, last - first), value);
        if (!convertedOk) {
            LOG_ERROR(<< "Unable to extract " << fieldName << " from " << fieldValue);
            result.clear();
            return false;
        }
        if (maths::common::CMathsFuncs::isFinite(value) == false) {
            LOG_ERROR(<< "Bad value for " << fieldName << " from " << fieldValue);
            result.clear();
            return false;
        }
        result.push_back(value);
        first = last + (last != std::string::npos ? delimiter.length() : 0);
    } while (first != std::string::npos);

    return true;
}

core_t::TTime CDataGatherer::earliestBucketStartTime() const {
    return m_BucketGatherer->earliestBucketStartTime();
}

bool CDataGatherer::checkInvariants() const {
    if (m_BucketGatherer == nullptr) {
        LOG_ERROR(<< "No bucket gatherer");
        return false;
    }
    if (m_PeopleRegistry.checkInvariants() == false) {
        LOG_ERROR(<< "People registry invariants violated");
        return false;
    }
    if (m_AttributesRegistry.checkInvariants() == false) {
        LOG_ERROR(<< "Attributes registry invariants violated");
        return false;
    }
    return true;
}

bool CDataGatherer::acceptRestoreTraverser(const CBucketGatherer::SBucketGathererInitData& bucketGathererInitData,
                                           core::CStateRestoreTraverser& traverser) {
    this->clear();
    m_Features.clear();

    do {
        const std::string& name = traverser.name();
        if (name == FEATURE_TAG) {
            int feature(-1);
            if (core::CStringUtils::stringToType(traverser.value(), feature) == false ||
                feature < 0) {
                LOG_ERROR(<< "Invalid feature in " << traverser.value());
                return false;
            }
            m_Features.push_back(static_cast<model_t::EFeature>(feature));
            continue;
        }
        RESTORE(PEOPLE_REGISTRY_TAG, traverser.traverseSubLevel(std::bind(
                                         &CDynamicStringIdRegistry::acceptRestoreTraverser,
                                         &m_PeopleRegistry, std::placeholders::_1)))
        RESTORE(ATTRIBUTES_REGISTRY_TAG,
                traverser.traverseSubLevel(
                    std::bind(&CDynamicStringIdRegistry::acceptRestoreTraverser,
                              &m_AttributesRegistry, std::placeholders::_1)))
        RESTORE_SETUP_TEARDOWN(
            SAMPLE_COUNTS_TAG, m_SampleCounts = std::make_unique<CSampleCounts>(0),
            traverser.traverseSubLevel(std::bind(&CSampleCounts::acceptRestoreTraverser,
                                                 m_SampleCounts.get(), std::placeholders::_1)),
            /**/)
        RESTORE(BUCKET_GATHERER_TAG,
                traverser.traverseSubLevel(std::bind(
                    &CDataGatherer::restoreBucketGatherer, this,
                    std::cref(bucketGathererInitData), std::placeholders::_1)))
    } while (traverser.next());

    return true;
}

bool CDataGatherer::restoreBucketGatherer(const CBucketGatherer::SBucketGathererInitData& bucketGathererInitData,
                                          core::CStateRestoreTraverser& traverser) {
    do {
        const std::string& name = traverser.name();
        if (name == CBucketGatherer::EVENTRATE_BUCKET_GATHERER_TAG) {
            m_BucketGatherer = std::make_unique<CEventRateBucketGatherer>(
                *this, bucketGathererInitData, traverser);
            if (m_BucketGatherer == nullptr) {
                LOG_ERROR(<< "Failed to create event rate bucket gatherer");
                return false;
            }
        } else if (name == CBucketGatherer::METRIC_BUCKET_GATHERER_TAG) {
            m_BucketGatherer = std::make_unique<CMetricBucketGatherer>(
                *this, bucketGathererInitData, traverser);
            if (m_BucketGatherer == nullptr) {
                LOG_ERROR(<< "Failed to create metric bucket gatherer");
                return false;
            }
        }
    } while (traverser.next());

    if (m_BucketGatherer == nullptr) {
        LOG_ERROR(<< "Failed to restore any bucket gatherer");
        return false;
    }

    return true;
}

void CDataGatherer::persistBucketGatherers(core::CStatePersistInserter& inserter) const {
    inserter.insertLevel(
        m_BucketGatherer->persistenceTag(), [capture0 = m_BucketGatherer.get()](
                                                core::CStatePersistInserter & inserter_) {
            capture0->acceptPersistInserter(inserter_);
        });
}

void CDataGatherer::createBucketGatherer(model_t::EAnalysisCategory gathererType,
                                         const CBucketGatherer::SBucketGathererInitData& initData) {
    switch (gathererType) {
    case model_t::E_EventRate:
    case model_t::E_PopulationEventRate:
        m_BucketGatherer = std::make_unique<CEventRateBucketGatherer>(*this, initData);
        break;
    case model_t::E_Metric:
    case model_t::E_PopulationMetric:
        m_SampleCounts = std::make_unique<CSampleCounts>(initData.s_SampleOverrideCount);
        m_BucketGatherer = std::make_unique<CMetricBucketGatherer>(*this, initData);
        break;
    }
}
}
}
