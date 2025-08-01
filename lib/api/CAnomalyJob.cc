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
#include <api/CAnomalyJob.h>

#include <core/CDataAdder.h>
#include <core/CDataSearcher.h>
#include <core/CJsonStatePersistInserter.h>
#include <core/CJsonStateRestoreTraverser.h>
#include <core/CLogger.h>
#include <core/CPersistUtils.h>
#include <core/CProgramCounters.h>
#include <core/CScopedBoostJsonPoolAllocator.h>
#include <core/CStateCompressor.h>
#include <core/CStateDecompressor.h>
#include <core/CStopWatch.h>
#include <core/CStringUtils.h>
#include <core/CTimeUtils.h>
#include <core/UnwrapRef.h>

#include <maths/common/CIntegerTools.h>
#include <maths/common/COrderings.h>

#include <model/CHierarchicalResultsAggregator.h>
#include <model/CHierarchicalResultsPopulator.h>
#include <model/CHierarchicalResultsProbabilityFinalizer.h>
#include <model/CLimits.h>
#include <model/CModelFactory.h>
#include <model/CSearchKey.h>
#include <model/CSimpleCountDetector.h>

#include <api/CAnnotationJsonWriter.h>
#include <api/CAnomalyJobConfig.h>
#include <api/CConfigUpdater.h>
#include <api/CHierarchicalResultsWriter.h>
#include <api/CJsonOutputWriter.h>
#include <api/CModelPlotDataJsonWriter.h>
#include <api/CPersistenceManager.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

namespace ml {
namespace api {

// We use short field names to reduce the state size
namespace {
using TStrCRef = std::reference_wrapper<const std::string>;

//! Convert a (string, key) pair to something readable.
template<typename T>
inline std::string pairDebug(const T& t) {
    return core::unwrap_ref(t.second).debug() + '/' + core::unwrap_ref(t.first);
}

const std::string TOP_LEVEL_DETECTOR_TAG("detector"); // do not shorten this
const std::string RESULTS_AGGREGATOR_TAG("aggregator");
const std::string TIME_TAG("a");
const std::string VERSION_TAG("b");
const std::string KEY_TAG("c");
const std::string PARTITION_FIELD_TAG("d");
const std::string DETECTOR_TAG("e");

// This is no longer used - removed in 6.6
// const std::string HIERARCHICAL_RESULTS_TAG("f");
const std::string LATEST_RECORD_TIME_TAG("h");

// This is no longer used - removed in 6.6
// const std::string MODEL_PLOT_TAG("i");

const std::string LAST_RESULTS_TIME_TAG("j");
const std::string INTERIM_BUCKET_CORRECTOR_TAG("k");
const std::string INITIAL_LAST_FINALISED_BUCKET_END_TIME("l");

//! The minimum version required to read the state corresponding to a model snapshot.
//! This should be updated every time there is a breaking change to the model state.
//! Newer versions are able to read the model state of older versions, but older
//! versions cannot read the model state of newer versions following a breaking
//! change.  This constant tells the node assignment code not to load new model states
//! on old nodes in a mixed version cluster.  (Most recently this has been updated to
//! 9.0.0 so that we have a clean break of state compatibility on the major version
//! boundary.  Model snapshots generated in 9.x will not be loadable by 8.x, and
//! when 8.x is end-of-life we'll be able to remove all the 8.x state backwards
//! compatibility code.)
const std::string MODEL_SNAPSHOT_MIN_VERSION("9.0.0");

//! Persist state as JSON with meaningful tag names.
class CReadableJsonStatePersistInserter : public core::CJsonStatePersistInserter {
public:
    explicit CReadableJsonStatePersistInserter(std::ostream& outputStream)
        : core::CJsonStatePersistInserter(outputStream) {}
    bool readableTags() const override { return true; }
};
}

// Statics
const std::string CAnomalyJob::STATE_TYPE("model_state");
const std::string CAnomalyJob::DEFAULT_TIME_FIELD_NAME("time");
const std::string CAnomalyJob::EMPTY_STRING;

const CAnomalyJob::TAnomalyDetectorPtr CAnomalyJob::NULL_DETECTOR;

CAnomalyJob::CAnomalyJob(std::string jobId,
                         model::CLimits& limits,
                         CAnomalyJobConfig& jobConfig,
                         model::CAnomalyDetectorModelConfig& modelConfig,
                         core::CJsonOutputStreamWrapper& outputStream,
                         TPersistCompleteFunc persistCompleteFunc,
                         CPersistenceManager* persistenceManager,
                         core_t::TTime maxQuantileInterval,
                         const std::string& timeFieldName,
                         const std::string& timeFieldFormat,
                         size_t maxAnomalyRecords)
    : CDataProcessor{timeFieldName, timeFieldFormat}, m_JobId{std::move(jobId)}, m_Limits{limits},
      m_OutputStream{outputStream}, m_ForecastRunner{m_JobId, m_OutputStream,
                                                     limits.resourceMonitor()},
      m_JsonOutputWriter{m_JobId, m_OutputStream}, m_JobConfig{jobConfig},
      m_ModelConfig{modelConfig}, m_PersistCompleteFunc{std::move(persistCompleteFunc)},
      m_MaxDetectors{std::numeric_limits<size_t>::max()},
      m_PersistenceManager{persistenceManager}, m_MaxQuantileInterval{maxQuantileInterval},
      m_LastNormalizerPersistTime{core::CTimeUtils::now()},
      m_Aggregator{modelConfig}, m_Normalizer{limits, modelConfig} {
    m_JsonOutputWriter.limitNumberRecords(maxAnomalyRecords);

    m_Limits.resourceMonitor().memoryUsageReporter(
        [ObjectPtr = &m_JsonOutputWriter]<typename T>(T && PH1) {
            ObjectPtr->reportMemoryUsage(std::forward<T>(PH1));
        });
}

CAnomalyJob::~CAnomalyJob() {
    m_ForecastRunner.finishForecasts();
}

bool CAnomalyJob::handleRecord(const TStrStrUMap& dataRowFields, TOptionalTime time) {
    // Non-empty control fields take precedence over everything else
    if (TStrStrUMapCItr const iter = dataRowFields.find(CONTROL_FIELD_NAME);
        iter != dataRowFields.end() && !iter->second.empty()) {
        return this->handleControlMessage(iter->second);
    }

    // Time may have been parsed already further back along the chain
    if (time == std::nullopt) {
        time = this->parseTime(dataRowFields);
        if (time == std::nullopt) {
            // Time is compulsory for anomaly detection - the base class will
            // have logged the parse error
            return true;
        }
    }

    // This record must be within the specified latency. If latency
    // is zero, then it should be after the current bucket end. If
    // latency is non-zero, then it should be after the current bucket
    // end minus the latency.
    if (*time < m_LastFinalisedBucketEndTime) {
        ++core::CProgramCounters::counter(counter_t::E_TSADNumberTimeOrderErrors);
        std::ostringstream ss;
        ss << "Records must be in ascending time order. "
           << "Record '" << ml::api::CAnomalyJob::debugPrintRecord(dataRowFields) << "' time "
           << *time << " is before bucket time " << m_LastFinalisedBucketEndTime;
        LOG_ERROR(<< ss.str());
        return true;
    }

    LOG_TRACE(<< "Handling record " << this->debugPrintRecord(dataRowFields));

    this->outputBucketResultsUntil(*time);

    if (m_DetectorKeys.empty()) {
        ml::api::CAnomalyJob::populateDetectorKeys(m_JobConfig, m_DetectorKeys);
    }

    for (const auto& m_DetectorKey : m_DetectorKeys) {
        const std::string& partitionFieldName(m_DetectorKey.partitionFieldName());

        // An empty partitionFieldName means no partitioning
        TStrStrUMapCItr const itr = partitionFieldName.empty()
                                        ? dataRowFields.end()
                                        : dataRowFields.find(partitionFieldName);
        const std::string& partitionFieldValue(
            itr == dataRowFields.end() ? EMPTY_STRING : itr->second);

        // TODO(valeriy): - should usenull apply to the partition field too?

        const TAnomalyDetectorPtr& detector = this->detectorForKey(
            false, // not restoring
            *time, m_DetectorKey, partitionFieldValue, m_Limits.resourceMonitor());
        if (detector == nullptr) {
            // There wasn't enough memory to create the detector
            continue;
        }

        ml::api::CAnomalyJob::addRecord(detector, *time, dataRowFields);
    }

    ++core::CProgramCounters::counter(counter_t::E_TSADNumberApiRecordsHandled);
    core::CProgramCounters::counter(counter_t::E_TSADSystemMemoryUsage) =
        model::CResourceMonitor::systemMemory();
    core::CProgramCounters::counter(counter_t::E_TSADMaxSystemMemoryUsage) =
        model::CResourceMonitor::maxSystemMemory();

    ++m_NumRecordsHandled;
    m_LatestRecordTime = std::max(m_LatestRecordTime, *time);

    return true;
}

void CAnomalyJob::finalise() {
    // Persist final state of normalizer iff an input record has been handled or time has been advanced.
    if (this->isPersistenceNeeded("quantiles state and model size stats")) {
        m_JsonOutputWriter.persistNormalizer(m_Normalizer, m_LastNormalizerPersistTime);

        // Prune the models so that the final persisted state is as neat as possible
        this->pruneAllModels();

        this->refreshMemoryAndReport();
    }

    // Wait for any ongoing periodic persist to complete, so that the data adder
    // is not used by both a periodic background persist and foreground persist at the
    // same time
    if (m_PersistenceManager != nullptr) {
        m_PersistenceManager->waitForIdle();
    }

    m_JsonOutputWriter.finalise();
}

bool CAnomalyJob::initNormalizer(const std::string& quantilesStateFile) {
    std::ifstream inputStream(quantilesStateFile.c_str());
    return m_Normalizer.fromJsonStream(inputStream) ==
           model::CHierarchicalResultsNormalizer::E_Ok;
}

std::uint64_t CAnomalyJob::numRecordsHandled() const {
    return m_NumRecordsHandled;
}

void CAnomalyJob::description() const {
    if (m_Detectors.empty()) {
        return;
    }

    TKeyCRefAnomalyDetectorPtrPrVec detectors;
    this->sortedDetectors(detectors);

    LOG_INFO(<< "Anomaly detectors:");
    TStrCRef partition = detectors[0].first.first;
    LOG_INFO(<< "\tpartition " << partition.get());
    LOG_INFO(<< "\t\tkey " << detectors[0].first.second.get());
    LOG_INFO(<< "\t\t\t" << detectors[0].second->description());
    for (std::size_t i = 1; i < detectors.size(); ++i) {
        if (detectors[i].first.first.get() != partition.get()) {
            partition = detectors[i].first.first;
            LOG_INFO(<< "\tpartition " << partition.get());
        }
        LOG_INFO(<< "\t\tkey " << detectors[i].first.second.get());
        LOG_INFO(<< "\t\t\t" << detectors[i].second->description());
    }
}

void CAnomalyJob::descriptionAndDebugMemoryUsage() const {
    if (m_Detectors.empty()) {
        LOG_INFO(<< "No detectors");
        return;
    }

    TKeyCRefAnomalyDetectorPtrPrVec detectors;
    this->sortedDetectors(detectors);

    std::ostringstream ss;
    ss << "Anomaly detectors:" << '\n';
    TStrCRef partition = detectors[0].first.first;
    ss << "\tpartition " << partition.get() << '\n';
    ss << "\t\tkey " << detectors[0].first.second.get() << '\n';
    ss << "\t\t\t" << detectors[0].second->description() << '\n';
    detectors[0].second->showMemoryUsage(ss);

    for (std::size_t i = 1; i < detectors.size(); ++i) {
        ss << '\n';
        if (detectors[i].first.first.get() != partition.get()) {
            partition = detectors[i].first.first;
            ss << "\tpartition " << partition.get() << '\n';
        }
        ss << "\t\tkey " << detectors[i].first.second.get() << '\n';
        ss << "\t\t\t" << detectors[i].second->description() << '\n';
        detectors[i].second->showMemoryUsage(ss);
    }
    LOG_INFO(<< ss.str());
}

const CAnomalyJob::SRestoredStateDetail& CAnomalyJob::restoreStateStatus() const {
    return m_RestoredStateDetail;
}

bool CAnomalyJob::handleControlMessage(const std::string& controlMessage) {
    if (controlMessage.empty()) {
        LOG_ERROR(<< "Programmatic error - handleControlMessage should only be "
                     "called with non-empty control messages");
        return false;
    }

    bool refreshRequired{true};
    switch (controlMessage[0]) {
    case ' ':
        // Spaces are just used to fill the buffers and force prior messages
        // through the system - we don't need to do anything else
        LOG_TRACE(<< "Received space control message of length "
                  << controlMessage.length());
        break;
    case CONTROL_FIELD_NAME_CHAR:
        // Silent no-op.  This is a simple way to ignore repeated header
        // rows in input.
        break;
    case 'f':
        // Flush ID comes after the initial f
        this->acknowledgeFlush(controlMessage.substr(1));
        break;
    case 'i':
        this->generateInterimResults(controlMessage);
        break;
    case 'r':
        this->resetBuckets(controlMessage);
        break;
    case 's':
        this->skipTime(controlMessage.substr(1));
        break;
    case 't':
        this->advanceTime(controlMessage.substr(1));
        break;
    case 'u':
        this->updateConfig(controlMessage.substr(1));
        break;
    case 'p':
        this->doForecast(controlMessage);
        break;
    case 'w':
        this->processPersistControlMessage(controlMessage.substr(1));
        break;
    case 'z':
        LOG_TRACE(<< "Received control message '" << controlMessage << "'");
        // "refreshRequired" parameter comes after the initial z.
        if (core::CStringUtils::stringToType(controlMessage.substr(1), refreshRequired) == false) {
            LOG_ERROR(<< "Received request to flush with invalid control message '"
                      << controlMessage << "'");
        } else {
            m_RefreshRequired = refreshRequired;
        }

        break;
    default:
        LOG_WARN(<< "Ignoring unknown control message of length "
                 << controlMessage.length() << " beginning with '"
                 << controlMessage[0] << '\'');
        // Don't return false here (for the time being at least), as it
        // seems excessive to cause the entire job to fail
        break;
    }

    return true;
}

bool CAnomalyJob::parsePersistControlMessageArgs(const std::string& controlMessageArgs,
                                                 core_t::TTime& snapshotTimestamp,
                                                 std::string& snapshotId,
                                                 std::string& snapshotDescription) {
    // Expect at least 3 space separated strings - timestamp snapshotId snapshotDescription, where:
    // timestamp = string representation of seconds since epoch
    // snapshotId = short string identifier for snapshot - containing no spaces
    // snapshotDescription = description of snapshot. May contain spaces.

    std::size_t const pos{controlMessageArgs.find(' ')};
    if (pos == std::string::npos) {
        LOG_ERROR(<< "Invalid control message format: \"" << controlMessageArgs << "\"");
        return false;
    }

    std::string const timestampStr{controlMessageArgs.substr(0, pos)};
    if (timestampStr.empty()) {
        LOG_ERROR(<< "Received empty snapshot timestamp.");
        return false;
    }

    if (core::CStringUtils::stringToType(timestampStr, snapshotTimestamp) == false) {
        LOG_ERROR(<< "Received invalid snapshotTimestamp " << timestampStr);
        return false;
    }

    std::size_t const pos2{controlMessageArgs.find(' ', pos + 1)};
    if (pos2 == std::string::npos) {
        LOG_ERROR(<< "Invalid control message format: \"" << controlMessageArgs << "\"");
        return false;
    }
    snapshotId = controlMessageArgs.substr(pos + 1, pos2 - pos - 1);
    snapshotDescription = controlMessageArgs.substr(pos2 + 1);

    if (snapshotId.empty()) {
        LOG_ERROR(<< "Received empty snapshotId.");
        return false;
    }

    return true;
}

void CAnomalyJob::processPersistControlMessage(const std::string& controlMessageArgs) {
    if (m_PersistenceManager != nullptr) {
        // There is a subtle difference between these two cases.  When there
        // are no control message arguments this triggers persistence of all
        // chained processors, i.e. maybe the categorizer as well as the anomaly
        // detector if there is one.  But when control message arguments are
        // passed, ONLY the persistence of the anomaly detector is triggered.
        if (controlMessageArgs.empty()) {
            if (this->isPersistenceNeeded("state")) {
                m_PersistenceManager->startPersist(core::CTimeUtils::now());
            }
        } else {
            core_t::TTime snapshotTimestamp{0};
            std::string snapshotId;
            std::string snapshotDescription;
            if (parsePersistControlMessageArgs(controlMessageArgs, snapshotTimestamp,
                                               snapshotId, snapshotDescription)) {
                // Since this is not going through the full persistence call
                // chain, make sure model size stats are up to date before
                // persisting
                m_Limits.resourceMonitor().forceRefreshAll();
                if (m_PersistenceManager->doForegroundPersist(
                        [this, &snapshotDescription, &snapshotId,
                         &snapshotTimestamp](core::CDataAdder& persister) {
                            return this->doPersistStateInForeground(
                                persister, snapshotDescription, snapshotId, snapshotTimestamp);
                        }) == false) {
                    LOG_ERROR(<< "Failed to persist state with parameters \""
                              << controlMessageArgs << "\"");
                }
            }
        }
    }
}

void CAnomalyJob::acknowledgeFlush(const std::string& flushId) {
    if (flushId.empty()) {
        LOG_ERROR(<< "Received flush control message with no ID");
    } else {
        LOG_TRACE(<< "Received flush control message with ID " << flushId);
    }
    m_JsonOutputWriter.acknowledgeFlush(flushId, m_LastFinalisedBucketEndTime, m_RefreshRequired);
}

void CAnomalyJob::updateConfig(const std::string& config) {
    LOG_DEBUG(<< "Received update config request: " << config);
    CConfigUpdater configUpdater(m_JobConfig, m_ModelConfig);
    if (configUpdater.update(config) == false) {
        LOG_ERROR(<< "Failed to update configuration");
    }
}

void CAnomalyJob::advanceTime(const std::string& time_) {
    if (time_.empty()) {
        LOG_ERROR(<< "Received request to advance time with no time");
        return;
    }

    core_t::TTime time(0);
    if (core::CStringUtils::stringToType(time_, time) == false) {
        LOG_ERROR(<< "Received request to advance time to invalid time " << time_);
        return;
    }

    if (m_LastFinalisedBucketEndTime == 0) {
        LOG_DEBUG(<< "Manually advancing time to " << time
                  << " before any valid data has been seen");
    } else {
        LOG_TRACE(<< "Received request to advance time to " << time);
    }

    m_TimeAdvanced = true;

    this->outputBucketResultsUntil(time);

    this->timeNow(time);
}

bool CAnomalyJob::isPersistenceNeeded(const std::string& description) const {
    if ((m_NumRecordsHandled == 0) && (m_TimeAdvanced == false)) {
        LOG_DEBUG(<< "Will not attempt to persist " << description
                  << ". Zero records were handled and time has not been advanced.");
        return false;
    }

    return true;
}

void CAnomalyJob::outputBucketResultsUntil(core_t::TTime time) {
    // If the bucket time has increased, output results for all field names
    core_t::TTime const bucketLength = m_ModelConfig.bucketLength();
    core_t::TTime const latency = m_ModelConfig.latency();

    if (m_LastFinalisedBucketEndTime == 0) {
        m_LastFinalisedBucketEndTime = std::max(
            m_LastFinalisedBucketEndTime,
            maths::common::CIntegerTools::floor(time, bucketLength) - latency);
        m_InitialLastFinalisedBucketEndTime = m_LastFinalisedBucketEndTime;
    }

    m_Normalizer.resetBigChange();

    for (core_t::TTime lastBucketEndTime = m_LastFinalisedBucketEndTime;
         lastBucketEndTime + bucketLength + latency <= time;
         lastBucketEndTime += bucketLength) {
        if (lastBucketEndTime == m_InitialLastFinalisedBucketEndTime &&
            m_RestoredStateDetail.s_RestoredStateStatus == E_Success) {
            LOG_DEBUG(<< "Skipping incomplete first bucket with lastBucketEndTime = "
                      << lastBucketEndTime << ", detected after state restoration");
            continue;
        }
        this->outputResults(lastBucketEndTime);
        m_Limits.resourceMonitor().decreaseMargin(bucketLength);
        m_Limits.resourceMonitor().sendMemoryUsageReportIfSignificantlyChanged(
            lastBucketEndTime, bucketLength);
        m_LastFinalisedBucketEndTime = lastBucketEndTime + bucketLength;

        // Check for periodic persistence immediately after calculating results
        // for the last bucket but before adding the first piece of data for the
        // next bucket
        if (m_PersistenceManager != nullptr) {
            m_PersistenceManager->startPersistIfAppropriate();
        }
    }

    if (m_Normalizer.hasLastUpdateCausedBigChange() ||
        (m_MaxQuantileInterval > 0 &&
         core::CTimeUtils::now() > m_LastNormalizerPersistTime + m_MaxQuantileInterval)) {
        m_JsonOutputWriter.persistNormalizer(m_Normalizer, m_LastNormalizerPersistTime);
    }
}

void CAnomalyJob::skipTime(const std::string& time_) {
    if (time_.empty()) {
        LOG_ERROR(<< "Received request to skip time with no time");
        return;
    }

    core_t::TTime time(0);
    if (core::CStringUtils::stringToType(time_, time) == false) {
        LOG_ERROR(<< "Received request to skip time to invalid time " << time_);
        return;
    }

    this->skipSampling(
        maths::common::CIntegerTools::ceil(time, m_ModelConfig.bucketLength()));
}

void CAnomalyJob::skipSampling(core_t::TTime endTime) {
    LOG_INFO(<< "Skipping time to: " << endTime);

    for (const auto& detector_ : m_Detectors) {
        model::CAnomalyDetector* detector(detector_.second.get());
        if (detector == nullptr) {
            LOG_ERROR(<< "Unexpected NULL pointer for key '"
                      << pairDebug(detector_.first) << '\'');
            continue;
        }
        detector->skipSampling(endTime);
    }

    m_LastFinalisedBucketEndTime = endTime;
}

void CAnomalyJob::timeNow(core_t::TTime time) {
    for (const auto& detector_ : m_Detectors) {
        model::CAnomalyDetector* detector(detector_.second.get());
        if (detector == nullptr) {
            LOG_ERROR(<< "Unexpected NULL pointer for key '"
                      << pairDebug(detector_.first) << '\'');
            continue;
        }
        detector->timeNow(time);
    }
}

void CAnomalyJob::generateInterimResults(const std::string& controlMessage) {
    LOG_TRACE(<< "Generating interim results");

    if (m_LastFinalisedBucketEndTime == 0) {
        LOG_TRACE(<< "Cannot create interim results having seen data for less than one bucket ever");
        return;
    }

    core_t::TTime start = m_LastFinalisedBucketEndTime;
    core_t::TTime end =
        m_LastFinalisedBucketEndTime +
        ((m_ModelConfig.latencyBuckets() + 1) * m_ModelConfig.bucketLength());

    if (ml::api::CAnomalyJob::parseTimeRangeInControlMessage(controlMessage, start, end)) {
        LOG_TRACE(<< "Time range for results: " << start << " : " << end);
        this->outputResultsWithinRange(true, start, end);
    }
}

bool CAnomalyJob::parseTimeRangeInControlMessage(const std::string& controlMessage,
                                                 core_t::TTime& start,
                                                 core_t::TTime& end) {
    using TStrVec = core::CStringUtils::TStrVec;
    TStrVec tokens;
    std::string remainder;
    core::CStringUtils::tokenise(" ", controlMessage.substr(1, std::string::npos),
                                 tokens, remainder);
    if (!remainder.empty()) {
        tokens.push_back(remainder);
    }
    std::size_t const tokensSize = tokens.size();
    if (tokensSize == 0) {
        // Default range
        return true;
    }
    if (tokensSize != 2) {
        LOG_ERROR(<< "Control message " << controlMessage << " has " << tokensSize
                  << " parameters when only zero or two are allowed.");
        return false;
    }
    if (core::CStringUtils::stringToType(tokens[0], start) &&
        core::CStringUtils::stringToType(tokens[1], end)) {
        return true;
    }
    LOG_ERROR(<< "Cannot parse control message: " << controlMessage);
    return false;
}

void CAnomalyJob::doForecast(const std::string& controlMessage) {
    // make a copy of the detectors vector, note: this is a shallow, not a deep copy
    TAnomalyDetectorPtrVec detectorVector;
    this->detectors(detectorVector);

    // push request into forecast queue, validates
    if (!m_ForecastRunner.pushForecastJob(controlMessage, detectorVector, m_LastResultsTime)) {
        // ForecastRunner already logged about it and send a status, so no need to log at info here
        LOG_DEBUG(<< "Forecast request failed");
    }
}

void CAnomalyJob::outputResults(core_t::TTime bucketStartTime) {
    core::CStopWatch timer(true);

    core_t::TTime const bucketLength = m_ModelConfig.bucketLength();

    model::CHierarchicalResults results;
    TModelPlotDataVec modelPlotData;
    TAnnotationVec annotations;

    TKeyCRefAnomalyDetectorPtrPrVec detectors;
    this->sortedDetectors(detectors);

    for (const auto& detector_ : detectors) {
        model::CAnomalyDetector* detector(detector_.second.get());
        if (detector == nullptr) {
            LOG_ERROR(<< "Unexpected NULL pointer for key '"
                      << pairDebug(detector_.first) << '\'');
            continue;
        }
        detector->buildResults(bucketStartTime, bucketStartTime + bucketLength, results);
        detector->releaseMemory(bucketStartTime - m_ModelConfig.samplingAgeCutoff());

        this->generateModelPlot(bucketStartTime, bucketStartTime + bucketLength,
                                *detector, modelPlotData);
        detector->generateAnnotations(bucketStartTime,
                                      bucketStartTime + bucketLength, annotations);
    }

    if (!results.empty()) {
        results.buildHierarchy();

        this->updateAggregatorAndAggregate(false, results);

        model::CHierarchicalResultsProbabilityFinalizer finalizer;
        results.bottomUpBreadthFirst(finalizer);
        results.pivotsBottomUpBreadthFirst(finalizer);

        model::CHierarchicalResultsPopulator populator(m_Limits);
        results.bottomUpBreadthFirst(populator);
        results.pivotsBottomUpBreadthFirst(populator);

        this->updateNormalizerAndNormalizeResults(false, results);
    }

    std::uint64_t const processingTime = timer.stop();

    // Model plots must be written first so the Java persists them
    // once the bucket result is processed
    this->writeOutModelPlot(modelPlotData);
    this->writeOutAnnotations(annotations);
    this->writeOutResults(false, results, bucketStartTime, processingTime);

    if (m_ModelConfig.modelPruneWindow() > 0) {
        // Ensure that bucketPruneWindow is always rounded _up_
        // to the next whole number of buckets (this doesn't really matter if we enforce
        // that the model prune window always be an exact multiple of bucket span in the
        // corresponding Java code)
        core_t::TTime const bucketPruneWindow{
            (m_ModelConfig.modelPruneWindow() + m_ModelConfig.bucketLength() - 1) /
            m_ModelConfig.bucketLength()};
        this->pruneAllModels(bucketPruneWindow);
    }

    // Prune models based on memory resource limits
    m_Limits.resourceMonitor().pruneIfRequired(bucketStartTime);
}

void CAnomalyJob::outputInterimResults(core_t::TTime bucketStartTime) {
    core::CStopWatch timer(true);

    core_t::TTime const bucketLength = m_ModelConfig.bucketLength();

    model::CHierarchicalResults results;
    results.setInterim();

    TKeyCRefAnomalyDetectorPtrPrVec detectors;
    this->sortedDetectors(detectors);

    for (const auto& detector_ : detectors) {
        model::CAnomalyDetector* detector(detector_.second.get());
        if (detector == nullptr) {
            LOG_ERROR(<< "Unexpected NULL pointer for key '"
                      << pairDebug(detector_.first) << '\'');
            continue;
        }
        detector->buildInterimResults(bucketStartTime, bucketStartTime + bucketLength, results);
    }

    if (!results.empty()) {
        results.buildHierarchy();

        this->updateAggregatorAndAggregate(true, results);

        model::CHierarchicalResultsProbabilityFinalizer finalizer;
        results.bottomUpBreadthFirst(finalizer);
        results.pivotsBottomUpBreadthFirst(finalizer);

        model::CHierarchicalResultsPopulator populator(m_Limits);
        results.bottomUpBreadthFirst(populator);
        results.pivotsBottomUpBreadthFirst(populator);

        this->updateNormalizerAndNormalizeResults(true, results);
    }

    std::uint64_t const processingTime = timer.stop();
    this->writeOutResults(true, results, bucketStartTime, processingTime);
}

void CAnomalyJob::writeOutResults(bool interim,
                                  model::CHierarchicalResults& results,
                                  core_t::TTime bucketTime,
                                  std::uint64_t processingTime) {
    if (!results.empty()) {
        LOG_TRACE(<< "Got results object here: " << results.root()->s_RawAnomalyScore
                  << " / " << results.root()->s_NormalizedAnomalyScore
                  << ", count " << results.resultCount() << " at " << bucketTime);

        using TScopedAllocator = core::CScopedBoostJsonPoolAllocator<CJsonOutputWriter>;
        static const std::string ALLOCATOR_ID("CAnomalyJob::writeOutResults");
        TScopedAllocator const scopedAllocator(ALLOCATOR_ID, m_JsonOutputWriter);

        api::CHierarchicalResultsWriter writer(
            m_Limits,
            [ObjectPtr = &m_JsonOutputWriter]<typename T>(T && PH1) {
                return ObjectPtr->acceptResult(std::forward<T>(PH1));
            },
            [ObjectPtr = &m_JsonOutputWriter]<typename T, typename U, typename V>(
                T && PH1, U && PH2, V && PH3) {
                return ObjectPtr->acceptInfluencer(
                    std::forward<T>(PH1), std::forward<U>(PH2), std::forward<V>(PH3));
            });
        results.bottomUpBreadthFirst(writer);
        results.pivotsBottomUpBreadthFirst(writer);

        // Add the bucketTime bucket influencer.
        // Note that the influencer will only be accepted if there are records.
        m_JsonOutputWriter.acceptBucketTimeInfluencer(
            bucketTime, results.root()->s_AnnotatedProbability.s_Probability,
            results.root()->s_RawAnomalyScore, results.root()->s_NormalizedAnomalyScore);

        core::CProgramCounters::counter(counter_t::E_TSADOutputMemoryAllocatorUsage) =
            m_JsonOutputWriter.getOutputMemoryAllocatorUsage();

        if (m_JsonOutputWriter.endOutputBatch(interim, processingTime) == false) {
            LOG_ERROR(<< "Problem writing anomaly output");
        }
        m_LastResultsTime = bucketTime;
    }
}

void CAnomalyJob::resetBuckets(const std::string& controlMessage) {
    if (controlMessage.length() == 1) {
        LOG_ERROR(<< "Received reset buckets control message without time range");
        return;
    }
    core_t::TTime start = 0;
    core_t::TTime end = 0;
    if (ml::api::CAnomalyJob::parseTimeRangeInControlMessage(controlMessage, start,
                                                             end) == false) {
        return;
    }
    core_t::TTime const bucketLength = m_ModelConfig.bucketLength();
    core_t::TTime time = maths::common::CIntegerTools::floor(start, bucketLength);
    core_t::TTime const bucketEnd = maths::common::CIntegerTools::ceil(end, bucketLength);
    while (time < bucketEnd) {
        for (const auto& detector_ : m_Detectors) {
            model::CAnomalyDetector* detector = detector_.second.get();
            if (detector == nullptr) {
                LOG_ERROR(<< "Unexpected NULL pointer for key '"
                          << pairDebug(detector_.first) << '\'');
                continue;
            }
            LOG_TRACE(<< "Resetting bucket = " << time);
            detector->resetBucket(time);
        }
        time += bucketLength;
    }
}

void CAnomalyJob::setDetectorsLastBucketEndTime(core_t::TTime lastBucketEndTime) {
    for (const auto& detector_ : m_Detectors) {
        model::CAnomalyDetector* detector(detector_.second.get());
        if (detector == nullptr) {
            LOG_ERROR(<< "Unexpected NULL pointer for key '"
                      << pairDebug(detector_.first) << '\'');
            continue;
        }

        const std::string& description = detector->description();
        LOG_DEBUG(<< "Setting lastBucketEndTime to " << lastBucketEndTime
                  << " in detector for '" << description << '\'');
        detector->lastBucketEndTime() = lastBucketEndTime;
    }
}

bool CAnomalyJob::restoreState(core::CDataSearcher& restoreSearcher,
                               core_t::TTime& completeToTime) {
    size_t numDetectors(0);
    try {
        // Restore from Elasticsearch compressed data.
        // (To restore from uncompressed data for testing, comment the next line
        // and substitute decompressor with restoreSearcher two lines below.)
        core::CStateDecompressor decompressor(restoreSearcher);

        core::CDataSearcher::TIStreamP const strm(decompressor.search(1, 1));
        if (strm == nullptr) {
            LOG_ERROR(<< "Unable to connect to data store");
            return false;
        }

        if (strm->bad()) {
            LOG_ERROR(<< "State restoration search returned bad stream");
            return false;
        }

        if (strm->fail()) {
            // This is fatal. If the stream exists and has failed then state is missing
            LOG_ERROR(<< "State restoration search returned failed stream");
            return false;
        }

        // We're dealing with streaming JSON state
        core::CJsonStateRestoreTraverser traverser(*strm);

        if (this->restoreState(traverser, completeToTime, numDetectors) == false ||
            traverser.haveBadState()) {
            LOG_ERROR(<< "Failed to restore detectors");
            return false;
        }
        LOG_DEBUG(<< "Finished restoration, with " << numDetectors << " detectors");

        if (numDetectors == 1 && m_Detectors.empty()) {
            // non fatal error
            m_RestoredStateDetail.s_RestoredStateStatus = E_NoDetectorsRecovered;
            return true;
        }

        if (completeToTime > 0) {
            core_t::TTime const lastBucketEndTime(maths::common::CIntegerTools::ceil(
                completeToTime, m_ModelConfig.bucketLength()));

            this->setDetectorsLastBucketEndTime(lastBucketEndTime);
        } else {
            if (!m_Detectors.empty()) {
                LOG_ERROR(<< "Inconsistency - " << m_Detectors.size()
                          << " detectors have been restored but completeToTime is "
                          << completeToTime);
            }
        }
    } catch (std::exception& e) {
        LOG_ERROR(<< "Failed to restore state! " << e.what());
        return false;
    }

    return true;
}

bool CAnomalyJob::restoreState(core::CStateRestoreTraverser& traverser,
                               core_t::TTime& completeToTime,
                               std::size_t& numDetectors) {
    m_RestoredStateDetail.s_RestoredStateStatus = E_Failure;
    m_RestoredStateDetail.s_Extra = std::nullopt;

    // Call name() to prime the traverser if it hasn't started
    traverser.name();
    if (traverser.isEof()) {
        m_RestoredStateDetail.s_RestoredStateStatus = E_NoDetectorsRecovered;
        LOG_ERROR(<< "Expected persisted state but no state exists");
        return false;
    }

    core_t::TTime lastBucketEndTime(0);
    if (traverser.name() != TIME_TAG ||
        core::CStringUtils::stringToType(traverser.value(), lastBucketEndTime) == false) {
        m_RestoredStateDetail.s_RestoredStateStatus = E_UnexpectedTag;
        LOG_ERROR(<< "Cannot restore anomaly detector - '" << TIME_TAG << "' element expected but found "
                  << traverser.name() << '=' << traverser.value());
        return false;
    }
    m_LastFinalisedBucketEndTime = lastBucketEndTime;

    if (lastBucketEndTime > completeToTime) {
        LOG_INFO(<< "Processing is already complete to time " << lastBucketEndTime);
        completeToTime = lastBucketEndTime;
    }

    if ((traverser.next() == false) || (traverser.name() != VERSION_TAG)) {
        m_RestoredStateDetail.s_RestoredStateStatus = E_UnexpectedTag;
        LOG_ERROR(<< "Cannot restore anomaly detector " << VERSION_TAG << " was expected");
        return false;
    }

    const std::string& stateVersion = traverser.value();
    if (stateVersion != model::CAnomalyDetector::STATE_VERSION) {
        m_RestoredStateDetail.s_RestoredStateStatus = E_IncorrectVersion;
        LOG_ERROR(<< "Restored anomaly detector state version is "
                  << stateVersion << " - ignoring it as current state version is "
                  << model::CAnomalyDetector::STATE_VERSION);

        // This counts as successful restoration
        return true;
    }

    while (traverser.next()) {
        const std::string& name = traverser.name();
        if (name == INTERIM_BUCKET_CORRECTOR_TAG) {
            // Note that this has to be persisted and restored before any detectors.
            auto interimBucketCorrector = std::make_shared<model::CInterimBucketCorrector>(
                m_ModelConfig.bucketLength());
            if (traverser.traverseSubLevel(
                    [capture0 = interimBucketCorrector.get()]<typename T>(T && PH1) {
                        return capture0->acceptRestoreTraverser(std::forward<T>(PH1));
                    }) == false) {
                LOG_ERROR(<< "Cannot restore interim bucket corrector");
                return false;
            }
            m_ModelConfig.interimBucketCorrector(interimBucketCorrector);
        } else if (name == TOP_LEVEL_DETECTOR_TAG) {
            if (traverser.traverseSubLevel([this]<typename T>(T && PH1) {
                    return restoreSingleDetector(std::forward<T>(PH1));
                }) == false) {
                LOG_ERROR(<< "Cannot restore anomaly detector");
                return false;
            }
            ++numDetectors;
        } else if (name == RESULTS_AGGREGATOR_TAG) {
            if (traverser.traverseSubLevel([ObjectPtr = &m_Aggregator]<typename T>(T && PH1) {
                    return ObjectPtr->acceptRestoreTraverser(std::forward<T>(PH1));
                }) == false) {
                LOG_ERROR(<< "Cannot restore results aggregator");
                return false;
            }
        } else if (name == LATEST_RECORD_TIME_TAG) {
            core::CPersistUtils::restore(LATEST_RECORD_TIME_TAG, m_LatestRecordTime, traverser);
        } else if (name == LAST_RESULTS_TIME_TAG) {
            core::CPersistUtils::restore(LAST_RESULTS_TIME_TAG, m_LastResultsTime, traverser);
        } else if (name == INITIAL_LAST_FINALISED_BUCKET_END_TIME) {
            core::CPersistUtils::restore(INITIAL_LAST_FINALISED_BUCKET_END_TIME,
                                         m_InitialLastFinalisedBucketEndTime, traverser);
        }
    }

    m_RestoredStateDetail.s_RestoredStateStatus = E_Success;

    return true;
}

bool CAnomalyJob::restoreSingleDetector(core::CStateRestoreTraverser& traverser) {
    if (traverser.name() != KEY_TAG) {
        LOG_ERROR(<< "Cannot restore anomaly detector - " << KEY_TAG << " element expected but found "
                  << traverser.name() << '=' << traverser.value());

        m_RestoredStateDetail.s_RestoredStateStatus = E_UnexpectedTag;
        return false;
    }

    model::CSearchKey key;
    if (traverser.traverseSubLevel([&key]<typename T>(T && PH1) {
            return model::CAnomalyDetector::keyAcceptRestoreTraverser(
                std::forward<T>(PH1), key);
        }) == false) {
        LOG_ERROR(<< "Cannot restore anomaly detector - no key found in " << KEY_TAG);

        m_RestoredStateDetail.s_RestoredStateStatus = E_UnexpectedTag;
        return false;
    }

    if (traverser.next() == false) {
        LOG_ERROR(<< "Cannot restore anomaly detector - end of object reached when "
                  << PARTITION_FIELD_TAG << " was expected");

        m_RestoredStateDetail.s_RestoredStateStatus = E_UnexpectedTag;
        return false;
    }

    if (traverser.name() != PARTITION_FIELD_TAG) {
        LOG_ERROR(<< "Cannot restore anomaly detector - " << PARTITION_FIELD_TAG << " element expected but found "
                  << traverser.name() << '=' << traverser.value());

        m_RestoredStateDetail.s_RestoredStateStatus = E_UnexpectedTag;
        return false;
    }

    std::string partitionFieldValue;
    if (traverser.traverseSubLevel([&partitionFieldValue]<typename T>(T && PH1) {
            return model::CAnomalyDetector::partitionFieldAcceptRestoreTraverser(
                std::forward<T>(PH1), partitionFieldValue);
        }) == false) {
        LOG_ERROR(<< "Cannot restore anomaly detector - "
                     "no partition field value found in "
                  << PARTITION_FIELD_TAG);

        m_RestoredStateDetail.s_RestoredStateStatus = E_UnexpectedTag;
        return false;
    }

    if (traverser.next() == false) {
        LOG_ERROR(<< "Cannot restore anomaly detector - end of object reached when "
                  << DETECTOR_TAG << " was expected");

        m_RestoredStateDetail.s_RestoredStateStatus = E_UnexpectedTag;
        return false;
    }

    if (traverser.name() != DETECTOR_TAG) {
        LOG_ERROR(<< "Cannot restore anomaly detector - " << DETECTOR_TAG << " element expected but found "
                  << traverser.name() << '=' << traverser.value());

        m_RestoredStateDetail.s_RestoredStateStatus = E_UnexpectedTag;
        return false;
    }

    if (this->restoreDetectorState(key, partitionFieldValue, traverser) == false) {
        LOG_ERROR(<< "Delegated portion of anomaly detector restore failed");
        m_RestoredStateDetail.s_RestoredStateStatus = E_Failure;
        return false;
    }

    LOG_TRACE(<< "Restored state for " << key.toCue() << "/" << partitionFieldValue);
    return true;
}

bool CAnomalyJob::restoreDetectorState(const model::CSearchKey& key,
                                       const std::string& partitionFieldValue,
                                       core::CStateRestoreTraverser& traverser) {
    const TAnomalyDetectorPtr& detector =
        this->detectorForKey(true, // for restoring
                             0,    // time reset later
                             key, partitionFieldValue, m_Limits.resourceMonitor());
    if (!detector) {
        LOG_ERROR(<< "Detector with key '" << key.debug() << '/' << partitionFieldValue
                  << "' was not recreated on restore - "
                     "memory limit is too low to continue this job");

        m_RestoredStateDetail.s_RestoredStateStatus = E_MemoryLimitReached;
        return false;
    }

    LOG_DEBUG(<< "Restoring state for detector with key '" << key.debug() << '/'
              << partitionFieldValue << '\'');

    if (traverser.traverseSubLevel([
            capture0 = detector.get(), capture1 = std::cref(partitionFieldValue)
        ]<typename T>(T && PH1) {
            return capture0->acceptRestoreTraverser(capture1, std::forward<T>(PH1));
        }) == false) {
        LOG_ERROR(<< "Error restoring anomaly detector for key '" << key.debug()
                  << '/' << partitionFieldValue << '\'');
        return false;
    }

    return true;
}

bool CAnomalyJob::persistModelsState(core::CDataAdder& persister, core_t::TTime timestamp) {
    TKeyCRefAnomalyDetectorPtrPrVec detectors;
    this->sortedDetectors(detectors);

    // Persistence operates on a cached collection of counters rather than on the live counters directly.
    // This is in order that background persistence operates on a consistent set of counters however we
    // also must ensure that foreground persistence has access to an up-to-date cache of counters as well.
    core::CProgramCounters::cacheCounters();

    return this->persistModelsState(detectors, persister, timestamp);
}

bool CAnomalyJob::persistStateInForeground(core::CDataAdder& persister,
                                           const std::string& descriptionPrefix) {
    if (m_LastFinalisedBucketEndTime == 0) {
        LOG_INFO(<< "Will not persist detectors as no results have been output");
        return true;
    }

    core_t::TTime const snapshotTimestamp{core::CTimeUtils::now()};
    const std::string snapshotId{core::CStringUtils::typeToString(snapshotTimestamp)};
    const std::string description{descriptionPrefix +
                                  core::CTimeUtils::toIso8601(snapshotTimestamp)};
    return this->doPersistStateInForeground(persister, description, snapshotId, snapshotTimestamp);
}

bool CAnomalyJob::doPersistStateInForeground(core::CDataAdder& persister,
                                             const std::string& description,
                                             const std::string& snapshotId,
                                             core_t::TTime snapshotTimestamp) {
    if (m_PersistenceManager != nullptr) {
        // This will not happen if finalise() was called before persisting state
        if (m_PersistenceManager->isBusy()) {
            LOG_ERROR(<< "Cannot perform foreground persistence of state - periodic "
                         "background persistence is still in progress");
            return false;
        }
    }

    TKeyCRefAnomalyDetectorPtrPrVec detectors;
    this->sortedDetectors(detectors);
    std::string normaliserState;
    m_Normalizer.toJson(m_LastResultsTime, "api", normaliserState, true);

    // Persistence of static counters is expected to operate on a cached collection of counters rather
    // than on the live counters directly. This is in order that the more frequently used background persistence
    // operates on a consistent set of counters. Hence, to avoid an error regarding the cache not existing, we
    // also must ensure that foreground persistence has access to an up-to-date cache of counters.
    core::CProgramCounters::cacheCounters();

    return this->persistCopiedState(
        description, snapshotId, snapshotTimestamp, m_LastFinalisedBucketEndTime, detectors,
        m_Limits.resourceMonitor().createMemoryUsageReport(
            m_LastFinalisedBucketEndTime - m_ModelConfig.bucketLength()),
        m_ModelConfig.interimBucketCorrector(), m_Aggregator, normaliserState, m_LatestRecordTime,
        m_LastResultsTime, m_InitialLastFinalisedBucketEndTime, persister);
}

bool CAnomalyJob::backgroundPersistState() {
    LOG_INFO(<< "Background persist starting data copy");

    if (m_PersistenceManager == nullptr) {
        return false;
    }

    // Pass arguments by value: this is what we want for
    // passing to a new thread.
    // Do NOT add std::ref wrappers around these arguments - they
    // MUST be copied for thread safety
    auto const args = std::make_shared<SBackgroundPersistArgs>(
        m_LastFinalisedBucketEndTime,
        m_Limits.resourceMonitor().createMemoryUsageReport(
            m_LastFinalisedBucketEndTime - m_ModelConfig.bucketLength()),
        m_ModelConfig.interimBucketCorrector(), m_Aggregator, m_LatestRecordTime,
        m_LastResultsTime, m_InitialLastFinalisedBucketEndTime);

    // The normaliser is non-copyable, so we have to make do with JSONifying it now;
    // it should be relatively fast though
    m_Normalizer.toJson(m_LastResultsTime, "api", args->s_NormalizerState, true);

    TKeyCRefAnomalyDetectorPtrPrVec& copiedDetectors = args->s_Detectors;
    copiedDetectors.reserve(m_Detectors.size());

    for (const auto& detector_ : m_Detectors) {
        model::CAnomalyDetector* detector(detector_.second.get());
        if (detector == nullptr) {
            LOG_ERROR(<< "Unexpected NULL pointer for key '"
                      << pairDebug(detector_.first) << '\'');
            continue;
        }
        model::CSearchKey::TStrCRefKeyCRefPr const key(
            std::cref(detector_.first.first), std::cref(detector_.first.second));
        if (detector->isSimpleCount()) {
            copiedDetectors.emplace_back(
                key, TAnomalyDetectorPtr(std::make_shared<model::CSimpleCountDetector>(
                         true, *detector)));
        } else {
            copiedDetectors.emplace_back(
                key, std::make_shared<model::CAnomalyDetector>(true, *detector));
        }
    }
    std::sort(copiedDetectors.begin(), copiedDetectors.end(),
              maths::common::COrderings::SFirstLess());

    if (m_PersistenceManager->addPersistFunc([ this, args ]<typename T>(T && PH1) {
            return runBackgroundPersist(args, std::forward<T>(PH1));
        }) == false) {
        LOG_ERROR(<< "Failed to add anomaly detector background persistence function");
        return false;
    }

    m_PersistenceManager->useBackgroundPersistence();

    return true;
}

bool CAnomalyJob::runForegroundPersist(core::CDataAdder& persister) {
    LOG_INFO(<< "Foreground persist commencing...");

    // Prune the models so that the persisted state is as neat as possible
    this->pruneAllModels();

    return this->persistStateInForeground(persister, "Periodic foreground persist at ");
}

bool CAnomalyJob::runBackgroundPersist(const TBackgroundPersistArgsPtr& args,
                                       core::CDataAdder& persister) {
    if (!args) {
        LOG_ERROR(<< "Unexpected NULL pointer passed to background persist");
        return false;
    }

    core_t::TTime const snapshotTimestamp(core::CTimeUtils::now());
    const std::string snapshotId(core::CStringUtils::typeToString(snapshotTimestamp));
    const std::string description{"Periodic background persist at " +
                                  core::CTimeUtils::toIso8601(snapshotTimestamp)};

    return this->persistCopiedState(
        description, snapshotId, snapshotTimestamp, args->s_Time, args->s_Detectors,
        args->s_ModelSizeStats, args->s_InterimBucketCorrector, args->s_Aggregator,
        args->s_NormalizerState, args->s_LatestRecordTime, args->s_LastResultsTime,
        args->s_InitialLastFinalizedBucketEndTime, persister);
}

bool CAnomalyJob::persistModelsState(const TKeyCRefAnomalyDetectorPtrPrVec& detectors,
                                     core::CDataAdder& persister,
                                     core_t::TTime timestamp) {
    try {
        const std::string snapShotId{core::CStringUtils::typeToString(timestamp)};
        core::CDataAdder::TOStreamP strm =
            persister.addStreamed(m_JobId + '_' + STATE_TYPE + '_' + snapShotId);
        if (strm != nullptr) {
            {
                // The JSON inserter must be destroyed before the stream is complete
                using TStatePersistInserterUPtr = std::unique_ptr<core::CStatePersistInserter>;
                TStatePersistInserterUPtr inserter{[&strm]() -> TStatePersistInserterUPtr {
                    return std::make_unique<CReadableJsonStatePersistInserter>(*strm);
                }()};

                for (const auto& detector_ : detectors) {
                    const model::CAnomalyDetector* detector(detector_.second.get());
                    if (detector == nullptr) {
                        LOG_ERROR(<< "Unexpected NULL pointer for key '"
                                  << pairDebug(detector_.first) << '\'');
                        continue;
                    }

                    detector->persistModelsState(*inserter);

                    const std::string& description = detector->description();

                    LOG_DEBUG(<< "Persisted state for '" << description << "', at time "
                              << timestamp << "detector->lastBucketEndTime() = "
                              << detector->lastBucketEndTime());
                }
            }

            if (persister.streamComplete(strm, true) == false || strm->bad()) {
                LOG_ERROR(<< "Failed to complete last persistence stream");
                return false;
            }
        }
    } catch (std::exception& e) {
        LOG_ERROR(<< "Failed to persist state! " << e.what());
        return false;
    }

    return true;
}

bool CAnomalyJob::persistCopiedState(const std::string& description,
                                     const std::string& snapshotId,
                                     core_t::TTime snapshotTimestamp,
                                     core_t::TTime time,
                                     const TKeyCRefAnomalyDetectorPtrPrVec& detectors,
                                     const model::CResourceMonitor::SModelSizeStats& modelSizeStats,
                                     const model::CInterimBucketCorrector& interimBucketCorrector,
                                     const model::CHierarchicalResultsAggregator& aggregator,
                                     const std::string& normalizerState,
                                     core_t::TTime latestRecordTime,
                                     core_t::TTime lastResultsTime,
                                     core_t::TTime initialLastFinalisedBucketEndTime,
                                     core::CDataAdder& persister) {
    // Ensure that the cache of program counters is cleared upon exiting the current scope.
    // As the cache is cleared when the simple count detector is persisted this may seem
    // unnecessary at first, but there are occasions when the simple count detector does not exist,
    // e.g. when no data is seen but time is advanced.
    core::CProgramCounters::CCacheManager const cacheMgr;

    // Persist state for each detector separately by streaming
    try {
        core::CStateCompressor compressor(persister);

        core::CDataAdder::TOStreamP strm =
            compressor.addStreamed(m_JobId + '_' + STATE_TYPE + '_' + snapshotId);
        if (strm != nullptr) {
            // IMPORTANT - this method can run in a background thread while the
            // analytics carries on processing new buckets in the main thread.
            // Therefore, this method must NOT access any member variables whose
            // values can change.  There should be no use of m_ variables in the
            // following code block.
            {
                // The JSON inserter must be destructed before the stream is complete
                core::CJsonStatePersistInserter inserter(*strm);
                inserter.insertValue(TIME_TAG, time);
                inserter.insertValue(VERSION_TAG, model::CAnomalyDetector::STATE_VERSION);
                inserter.insertLevel(
                    INTERIM_BUCKET_CORRECTOR_TAG,
                    [ObjectPtr = &interimBucketCorrector]<typename T>(T && PH1) {
                        ObjectPtr->acceptPersistInserter(std::forward<T>(PH1));
                    });

                for (const auto& detector_ : detectors) {
                    const model::CAnomalyDetector* detector(detector_.second.get());
                    if (detector == nullptr) {
                        LOG_ERROR(<< "Unexpected NULL pointer for key '"
                                  << pairDebug(detector_.first) << '\'');
                        continue;
                    }
                    if (detector->shouldPersistDetector() == false) {
                        LOG_TRACE(<< "Not persisting state for '"
                                  << detector->description() << "'");
                        continue;
                    }
                    inserter.insertLevel(
                        TOP_LEVEL_DETECTOR_TAG,
                        [capture0 = std::cref(*detector)]<typename T>(T && PH1) {
                            CAnomalyJob::persistIndividualDetector(
                                capture0, std::forward<T>(PH1));
                        });

                    LOG_DEBUG(<< "Persisted state for '" << detector->description() << "'");
                }

                inserter.insertLevel(
                    RESULTS_AGGREGATOR_TAG, [ObjectPtr = &aggregator]<typename T>(T && PH1) {
                        ObjectPtr->acceptPersistInserter(std::forward<T>(PH1));
                    });

                core::CPersistUtils::persist(LATEST_RECORD_TIME_TAG,
                                             latestRecordTime, inserter);
                core::CPersistUtils::persist(LAST_RESULTS_TIME_TAG, lastResultsTime, inserter);

                core::CPersistUtils::persist(INITIAL_LAST_FINALISED_BUCKET_END_TIME,
                                             initialLastFinalisedBucketEndTime, inserter);
            }

            if (compressor.streamComplete(strm, true) == false || strm->bad()) {
                LOG_ERROR(<< "Failed to complete last persistence stream");
                return false;
            }

            if (m_PersistCompleteFunc) {
                CModelSnapshotJsonWriter::SModelSnapshotReport const modelSnapshotReport{
                    MODEL_SNAPSHOT_MIN_VERSION, snapshotTimestamp, description,
                    snapshotId, compressor.numCompressedDocs(), modelSizeStats,
                    normalizerState, latestRecordTime,
                    // This needs to be the last final result time as it serves
                    // as the time after which all results are deleted when a
                    // model snapshot is reverted
                    time - m_ModelConfig.bucketLength()};

                m_PersistCompleteFunc(modelSnapshotReport);
            }
        }
    } catch (std::exception& e) {
        LOG_ERROR(<< "Failed to persist state! " << e.what());
        return false;
    }

    return true;
}

bool CAnomalyJob::periodicPersistStateInBackground() {

    // Prune the models so that the persisted state is as neat as possible
    this->pruneAllModels();

    // Make sure model size stats are up to date
    for (const auto& detector_ : m_Detectors) {
        model::CAnomalyDetector* detector = detector_.second.get();
        if (detector == nullptr) {
            LOG_ERROR(<< "Unexpected NULL pointer for key '"
                      << pairDebug(detector_.first) << '\'');
            continue;
        }
        m_Limits.resourceMonitor().forceRefresh(*detector);
    }

    return this->backgroundPersistState();
}

bool CAnomalyJob::periodicPersistStateInForeground() {
    // Do NOT pass this request on to the output chainer.
    // That logic is already present in persistStateInForeground.

    if (m_PersistenceManager == nullptr) {
        return false;
    }

    if (m_PersistenceManager->addPersistFunc([this]<typename T>(T && PH1) {
            return runForegroundPersist(std::forward<T>(PH1));
        }) == false) {
        LOG_ERROR(<< "Failed to add anomaly detector foreground persistence function");
        return false;
    }

    m_PersistenceManager->useForegroundPersistence();

    return true;
}

void CAnomalyJob::updateAggregatorAndAggregate(bool isInterim,
                                               model::CHierarchicalResults& results) {
    m_Aggregator.refresh(m_ModelConfig);

    m_Aggregator.setJob(model::CHierarchicalResultsAggregator::E_Correct);

    // The equalizers are NOT updated with interim results.
    if (isInterim == false) {
        m_Aggregator.setJob(model::CHierarchicalResultsAggregator::E_UpdateAndCorrect);
        m_Aggregator.propagateForwardByTime(1.0);
    }

    results.bottomUpBreadthFirst(m_Aggregator);
    results.createPivots();
    results.pivotsBottomUpBreadthFirst(m_Aggregator);
}

void CAnomalyJob::updateNormalizerAndNormalizeResults(bool isInterim,
                                                      model::CHierarchicalResults& results) {
    m_Normalizer.setJob(model::CHierarchicalResultsNormalizer::E_RefreshSettings);
    results.bottomUpBreadthFirst(m_Normalizer);
    results.pivotsBottomUpBreadthFirst(m_Normalizer);

    // The normalizers are NOT updated with interim results, in other
    // words interim results are normalized with respect to previous
    // final results.
    if (isInterim == false) {
        m_Normalizer.propagateForwardByTime(1.0);
        m_Normalizer.setJob(model::CHierarchicalResultsNormalizer::E_UpdateQuantiles);
        results.bottomUpBreadthFirst(m_Normalizer);
        results.pivotsBottomUpBreadthFirst(m_Normalizer);
    }

    m_Normalizer.setJob(model::CHierarchicalResultsNormalizer::E_NormalizeScores);
    results.bottomUpBreadthFirst(m_Normalizer);
    results.pivotsBottomUpBreadthFirst(m_Normalizer);
}

void CAnomalyJob::outputResultsWithinRange(bool isInterim, core_t::TTime start, core_t::TTime end) {
    if (m_LastFinalisedBucketEndTime <= 0) {
        return;
    }
    if (start < m_LastFinalisedBucketEndTime) {
        LOG_WARN(<< "Cannot output results for range (" << start << ", " << m_LastFinalisedBucketEndTime
                 << "): Start time is before last finalized bucket end time "
                 << m_LastFinalisedBucketEndTime << '.');
        start = m_LastFinalisedBucketEndTime;
    }
    if (start > end) {
        LOG_ERROR(<< "Cannot output results for range (" << start << ", " << end
                  << "): Start time is later than end time.");
        return;
    }
    core_t::TTime const bucketLength = m_ModelConfig.bucketLength();
    core_t::TTime time = maths::common::CIntegerTools::floor(start, bucketLength);
    core_t::TTime const bucketEnd = maths::common::CIntegerTools::ceil(end, bucketLength);
    while (time < bucketEnd) {
        if (isInterim) {
            this->outputInterimResults(time);
        } else {
            this->outputResults(time);
        }
        m_Limits.resourceMonitor().sendMemoryUsageReportIfSignificantlyChanged(time, bucketLength);
        time += bucketLength;
    }
}

void CAnomalyJob::generateModelPlot(core_t::TTime startTime,
                                    core_t::TTime endTime,
                                    const model::CAnomalyDetector& detector,
                                    TModelPlotDataVec& modelPlotData) {
    double const modelPlotBoundsPercentile(m_ModelConfig.modelPlotBoundsPercentile());
    if (modelPlotBoundsPercentile > 0.0) {
        LOG_TRACE(<< "Generating model debug data at " << startTime);
        detector.generateModelPlot(startTime, endTime,
                                   m_ModelConfig.modelPlotBoundsPercentile(),
                                   m_ModelConfig.modelPlotTerms(), modelPlotData);
    }
}

void CAnomalyJob::writeOutModelPlot(const TModelPlotDataVec& modelPlotData) {
    CModelPlotDataJsonWriter modelPlotWriter(m_OutputStream);
    for (const auto& plot : modelPlotData) {
        modelPlotWriter.writeFlat(m_JobId, plot);
    }
}

void CAnomalyJob::writeOutAnnotations(const TAnnotationVec& annotations) {
    CAnnotationJsonWriter annotationWriter(m_OutputStream);
    for (const auto& annotation : annotations) {
        annotationWriter.writeResult(m_JobId, annotation);
    }
}

void CAnomalyJob::refreshMemoryAndReport() {
    core_t::TTime const bucketLength{m_ModelConfig.bucketLength()};
    if (m_LastFinalisedBucketEndTime < bucketLength) {
        LOG_ERROR(<< "Cannot report memory usage because last finalized bucket end time ("
                  << m_LastFinalisedBucketEndTime
                  << ") is smaller than bucket span (" << bucketLength << ')');
        return;
    }
    // Make sure model size stats are up to date and then send a final memory
    // usage report
    for (const auto& detector_ : m_Detectors) {
        model::CAnomalyDetector* detector = detector_.second.get();
        if (detector == nullptr) {
            LOG_ERROR(<< "Unexpected NULL pointer for key '"
                      << pairDebug(detector_.first) << '\'');
            continue;
        }
        m_Limits.resourceMonitor().forceRefresh(*detector);
    }
    m_Limits.resourceMonitor().sendMemoryUsageReport(
        m_LastFinalisedBucketEndTime - bucketLength, bucketLength);
}

void CAnomalyJob::persistIndividualDetector(const model::CAnomalyDetector& detector,
                                            core::CStatePersistInserter& inserter) {
    inserter.insertLevel(KEY_TAG, [ObjectPtr = &detector]<typename T>(T && PH1) {
        ObjectPtr->keyAcceptPersistInserter(std::forward<T>(PH1));
    });
    inserter.insertLevel(PARTITION_FIELD_TAG, [ObjectPtr = &detector]<typename T>(T && PH1) {
        ObjectPtr->partitionFieldAcceptPersistInserter(std::forward<T>(PH1));
    });
    inserter.insertLevel(DETECTOR_TAG, [ObjectPtr = &detector]<typename T>(T && PH1) {
        ObjectPtr->acceptPersistInserter(std::forward<T>(PH1));
    });
}

void CAnomalyJob::detectors(TAnomalyDetectorPtrVec& detectors) const {
    detectors.clear();
    detectors.reserve(m_Detectors.size());
    for (const auto& detector : m_Detectors) {
        detectors.push_back(detector.second);
    }
}

void CAnomalyJob::sortedDetectors(TKeyCRefAnomalyDetectorPtrPrVec& detectors) const {
    detectors.reserve(m_Detectors.size());
    for (const auto& detector : m_Detectors) {
        detectors.emplace_back(
            model::CSearchKey::TStrCRefKeyCRefPr(std::cref(detector.first.first),
                                                 std::cref(detector.first.second)),
            detector.second);
    }
    std::sort(detectors.begin(), detectors.end(), maths::common::COrderings::SFirstLess());
}

const CAnomalyJob::TKeyAnomalyDetectorPtrUMap& CAnomalyJob::detectorPartitionMap() const {
    return m_Detectors;
}

const CAnomalyJob::TAnomalyDetectorPtr&
CAnomalyJob::detectorForKey(bool isRestoring,
                            core_t::TTime time,
                            const model::CSearchKey& key,
                            const std::string& partitionFieldValue,
                            const model::CResourceMonitor& resourceMonitor) {
    // The simple count detector always lives in a special null partition.
    const std::string& partition = key.isSimpleCount() ? EMPTY_STRING : partitionFieldValue;

    // Try and get the detector.
    auto itr = m_Detectors.find(
        model::CSearchKey::TStrCRefKeyCRefPr(std::cref(partition), std::cref(key)),
        model::CStrKeyPrHash(), model::CStrKeyPrEqual());

    // Check if we need to and are allowed to create a new detector.
    if (itr == m_Detectors.end() && resourceMonitor.areAllocationsAllowed()) {
        // Create an placeholder for the anomaly detector.
        TAnomalyDetectorPtr& detector =
            m_Detectors
                .emplace(model::CSearchKey::TStrKeyPr(partition, key), TAnomalyDetectorPtr())
                .first->second;

        LOG_TRACE(<< "Creating new detector for key '" << key.debug() << '/'
                  << partition << '\'' << ", time " << time);
        LOG_TRACE(<< "Detector count " << m_Detectors.size());

        detector = ml::api::CAnomalyJob::makeDetector(
            m_ModelConfig, m_Limits, partition, time, m_ModelConfig.factory(key));
        if (detector == nullptr) {
            // This should never happen as CAnomalyDetectorUtils::makeDetector()
            // contracts to never return NULL
            LOG_ABORT(<< "Failed to create anomaly detector for key '"
                      << key.debug() << '\'');
        }

        detector->zeroModelsToTime(time - m_ModelConfig.latency());

        if (isRestoring == false) {
            m_Limits.resourceMonitor().forceRefresh(*detector);
        }
        return detector;
    }
    if (itr == m_Detectors.end()) {
        LOG_TRACE(<< "No memory to create new detector for key '" << key.debug()
                  << '/' << partition << '\'');
        return NULL_DETECTOR;
    }

    return itr->second;
}

void CAnomalyJob::pruneAllModels(std::size_t buckets) const {
    if (buckets == 0) {
        LOG_INFO(<< "Pruning obsolete models");
    } else {
        LOG_DEBUG(<< "Pruning all models older than " << buckets << " buckets");
    }

    for (const auto& detector_ : m_Detectors) {
        model::CAnomalyDetector* detector = detector_.second.get();
        if (detector == nullptr) {
            LOG_ERROR(<< "Unexpected NULL pointer for key '"
                      << pairDebug(detector_.first) << '\'');
            continue;
        }
        (buckets == 0) ? detector->pruneModels() : detector->pruneModels(buckets);
    }
}
const model::CHierarchicalResultsNormalizer& CAnomalyJob::normalizer() const {
    return m_Normalizer;
}

CAnomalyJob::TAnomalyDetectorPtr
CAnomalyJob::makeDetector(const model::CAnomalyDetectorModelConfig& modelConfig,
                          model::CLimits& limits,
                          const std::string& partitionFieldValue,
                          core_t::TTime firstTime,
                          const model::CAnomalyDetector::TModelFactoryCPtr& modelFactory) {
    return modelFactory->isSimpleCount()
               ? std::make_shared<model::CSimpleCountDetector>(
                     modelFactory->summaryMode(), modelConfig, std::ref(limits),
                     partitionFieldValue, firstTime, modelFactory)
               : std::make_shared<model::CAnomalyDetector>(std::ref(limits), modelConfig,
                                                           partitionFieldValue,
                                                           firstTime, modelFactory);
}

void CAnomalyJob::populateDetectorKeys(const CAnomalyJobConfig& jobConfig, TKeyVec& keys) {
    keys.clear();

    // Always add a key for the simple count detector.
    keys.push_back(model::CSearchKey::simpleCountKey());

    for (const auto& fieldOptions : jobConfig.analysisConfig().detectorsConfig()) {
        keys.emplace_back(fieldOptions.detectorIndex(), fieldOptions.function(),
                          fieldOptions.useNull(), fieldOptions.excludeFrequent(),
                          fieldOptions.fieldName(), fieldOptions.byFieldName(),
                          fieldOptions.overFieldName(), fieldOptions.partitionFieldName(),
                          jobConfig.analysisConfig().influencers());
    }
}

const std::string* CAnomalyJob::fieldValue(const std::string& fieldName,
                                           const TStrStrUMap& dataRowFields) {
    TStrStrUMapCItr const itr = fieldName.empty() ? dataRowFields.end()
                                                  : dataRowFields.find(fieldName);
    const std::string& fieldValue(itr == dataRowFields.end() ? EMPTY_STRING : itr->second);
    return !fieldName.empty() && fieldValue.empty() ? nullptr : &fieldValue;
}

void CAnomalyJob::addRecord(const TAnomalyDetectorPtr& detector,
                            core_t::TTime time,
                            const TStrStrUMap& dataRowFields) {
    model::CAnomalyDetector::TStrCPtrVec fieldValues;
    const TStrVec& fieldNames = detector->fieldsOfInterest();
    fieldValues.reserve(fieldNames.size());
    for (const auto& fieldName : fieldNames) {
        fieldValues.push_back(fieldValue(fieldName, dataRowFields));
    }

    detector->addRecord(time, fieldValues);
}

CAnomalyJob::SBackgroundPersistArgs::SBackgroundPersistArgs(
    core_t::TTime time,
    const model::CResourceMonitor::SModelSizeStats& modelSizeStats,
    model::CInterimBucketCorrector interimBucketCorrector,
    const model::CHierarchicalResultsAggregator& aggregator,
    core_t::TTime latestRecordTime,
    core_t::TTime lastResultsTime,
    core_t::TTime initialLastFinalisedBucketEndTime)
    : s_Time(time), s_ModelSizeStats(modelSizeStats),
      s_InterimBucketCorrector(std::move(interimBucketCorrector)),
      s_Aggregator(aggregator), s_LatestRecordTime(latestRecordTime),
      s_LastResultsTime(lastResultsTime),
      s_InitialLastFinalizedBucketEndTime(initialLastFinalisedBucketEndTime) {
}
}
}
