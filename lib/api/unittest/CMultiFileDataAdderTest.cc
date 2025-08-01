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

#include <core/CDataAdder.h>
#include <core/CJsonOutputStreamWrapper.h>
#include <core/COsFileFuncs.h>
#include <core/CoreTypes.h>

#include <maths/common/CModelWeight.h>

#include <model/CAnomalyDetectorModelConfig.h>
#include <model/CLimits.h>

#include <api/CAnomalyJobConfig.h>
#include <api/CCsvInputParser.h>
#include <api/CJsonOutputWriter.h>
#include <api/CNdJsonInputParser.h>

#include <test/CMultiFileDataAdder.h>
#include <test/CMultiFileSearcher.h>
#include <test/CTestTmpDir.h>

#include "CTestAnomalyJob.h"

#include <boost/filesystem.hpp>
#include <boost/test/unit_test.hpp>

#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <random> // For random number generation facilities
#include <sstream>
#include <string>
#include <vector>

BOOST_AUTO_TEST_SUITE(CMultiFileDataAdderTest)

namespace {

using TStrVec = std::vector<std::string>;

void reportPersistComplete(ml::api::CModelSnapshotJsonWriter::SModelSnapshotReport modelSnapshotReport,
                           std::string& snapshotIdOut,
                           size_t& numDocsOut) {
    LOG_INFO(<< "Persist complete with description: " << modelSnapshotReport.s_Description);
    snapshotIdOut = modelSnapshotReport.s_SnapshotId;
    numDocsOut = modelSnapshotReport.s_NumDocs;
}

void detectorPersistHelper(const std::string& configFileName,
                           const std::string& inputFilename,
                           int latencyBuckets,
                           const std::string& timeFormat = std::string()) {
    // Start by creating a detector with non-trivial state
    static const ml::core_t::TTime BUCKET_SIZE(3600);
    static const std::string JOB_ID("job");

    // Open the input and output files
    std::ifstream inputStrm(inputFilename);
    BOOST_TEST_REQUIRE(inputStrm.is_open());

    std::ofstream outputStrm(ml::core::COsFileFuncs::NULL_FILENAME);
    BOOST_TEST_REQUIRE(outputStrm.is_open());
    ml::core::CJsonOutputStreamWrapper wrappedOutputStream(outputStrm);

    ml::model::CLimits limits;
    ml::api::CAnomalyJobConfig jobConfig;
    BOOST_TEST_REQUIRE(jobConfig.initFromFile(configFileName));

    ml::model::CAnomalyDetectorModelConfig modelConfig =
        ml::model::CAnomalyDetectorModelConfig::defaultConfig(
            BUCKET_SIZE, ml::model_t::E_None, "", BUCKET_SIZE * latencyBuckets, false);

    std::string origSnapshotId;
    std::size_t numOrigDocs(0);
    CTestAnomalyJob origJob(JOB_ID, limits, jobConfig, modelConfig, wrappedOutputStream,
                            std::bind(&reportPersistComplete, std::placeholders::_1,
                                      std::ref(origSnapshotId), std::ref(numOrigDocs)),
                            nullptr, -1, "time", timeFormat);

    using TInputParserUPtr = std::unique_ptr<ml::api::CInputParser>;
    const TInputParserUPtr parser{[&inputFilename, &inputStrm]() -> TInputParserUPtr {
        if (inputFilename.rfind(".csv") == inputFilename.length() - 4) {
            return std::make_unique<ml::api::CCsvInputParser>(inputStrm);
        }
        return std::make_unique<ml::api::CNdJsonInputParser>(inputStrm);
    }()};

    BOOST_TEST_REQUIRE(parser->readStreamIntoMaps(
        [&origJob](const CTestAnomalyJob::TStrStrUMap& dataRowFields) {
            return origJob.handleRecord(dataRowFields);
        }));

    // Persist the detector state to file(s)

    // Create a random number to use to generate a unique file name for each test
    // this allows tests to be run successfully in parallel
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(1, 100);
    std::ostringstream oss;
    oss << distrib(gen);

    std::string baseOrigOutputFilename(ml::test::CTestTmpDir::tmpDir() +
                                       "/orig_" + oss.str());
    {
        // Clean up any leftovers of previous failures
        boost::filesystem::path origDir(baseOrigOutputFilename);
        BOOST_REQUIRE_NO_THROW(boost::filesystem::remove_all(origDir));

        ml::test::CMultiFileDataAdder persister(baseOrigOutputFilename);
        BOOST_TEST_REQUIRE(origJob.persistStateInForeground(persister, ""));
    }

    std::string origBaseDocId(JOB_ID + '_' + CTestAnomalyJob::STATE_TYPE + '_' + origSnapshotId);

    std::string temp;
    TStrVec origFileContents(numOrigDocs);
    for (size_t index = 0; index < numOrigDocs; ++index) {
        std::string expectedOrigFilename(baseOrigOutputFilename);
        expectedOrigFilename += "/_index/";
        expectedOrigFilename +=
            ml::core::CDataAdder::makeCurrentDocId(origBaseDocId, 1 + index);
        expectedOrigFilename += ml::test::CMultiFileDataAdder::JSON_FILE_EXT;
        LOG_DEBUG(<< "Trying to open file: " << expectedOrigFilename);
        std::ifstream origFile(expectedOrigFilename);
        BOOST_TEST_REQUIRE(origFile.is_open());
        std::string json((std::istreambuf_iterator<char>(origFile)),
                         std::istreambuf_iterator<char>());
        origFileContents[index] = json;

        // Ensure that the JSON is valid, by parsing string using boost::json
        json::error_code ec;
        json::value document = json::parse(origFileContents[index].c_str(), ec);
        BOOST_TEST_REQUIRE(ec.failed() == false);
        BOOST_TEST_REQUIRE(document.is_object());
    }

    // Now restore the state into a different detector
    std::string restoredSnapshotId;
    std::size_t numRestoredDocs(0);
    CTestAnomalyJob restoredJob(
        JOB_ID, limits, jobConfig, modelConfig, wrappedOutputStream,
        std::bind(&reportPersistComplete, std::placeholders::_1,
                  std::ref(restoredSnapshotId), std::ref(numRestoredDocs)));

    {
        ml::core_t::TTime completeToTime(0);

        ml::test::CMultiFileSearcher retriever(baseOrigOutputFilename, origBaseDocId);
        BOOST_TEST_REQUIRE(restoredJob.restoreState(retriever, completeToTime));
        BOOST_TEST_REQUIRE(completeToTime > 0);
    }

    // Finally, persist the new detector state to a file

    std::string baseRestoredOutputFilename(ml::test::CTestTmpDir::tmpDir() +
                                           "/restored_" + oss.str());
    {
        // Clean up any leftovers of previous failures
        boost::filesystem::path restoredDir(baseRestoredOutputFilename);
        BOOST_REQUIRE_NO_THROW(boost::filesystem::remove_all(restoredDir));

        ml::test::CMultiFileDataAdder persister(baseRestoredOutputFilename);
        BOOST_TEST_REQUIRE(restoredJob.persistStateInForeground(persister, ""));
    }

    std::string restoredBaseDocId(JOB_ID + '_' + CTestAnomalyJob::STATE_TYPE +
                                  '_' + restoredSnapshotId);

    for (size_t index = 0; index < numRestoredDocs; ++index) {
        std::string expectedRestoredFilename(baseRestoredOutputFilename);
        expectedRestoredFilename += "/_index/";
        expectedRestoredFilename +=
            ml::core::CDataAdder::makeCurrentDocId(restoredBaseDocId, 1 + index);
        expectedRestoredFilename += ml::test::CMultiFileDataAdder::JSON_FILE_EXT;
        std::ifstream restoredFile(expectedRestoredFilename);
        BOOST_TEST_REQUIRE(restoredFile.is_open());
        std::string json((std::istreambuf_iterator<char>(restoredFile)),
                         std::istreambuf_iterator<char>());

        BOOST_REQUIRE_EQUAL(origFileContents[index], json);
    }

    // Clean up
    boost::filesystem::path origDir(baseOrigOutputFilename);
    BOOST_REQUIRE_NO_THROW(boost::filesystem::remove_all(origDir));
    boost::filesystem::path restoredDir(baseRestoredOutputFilename);
    BOOST_REQUIRE_NO_THROW(boost::filesystem::remove_all(restoredDir));
}
}

BOOST_AUTO_TEST_CASE(testSimpleWrite) {
    static const std::string EVENT("Hello Event");
    static const std::string SUMMARY_EVENT("Hello Summary Event");

    static const std::string EXTENSION(".txt");
    std::string baseOutputFilename(ml::test::CTestTmpDir::tmpDir() + "/filepersister");

    std::string expectedFilename(baseOutputFilename);
    expectedFilename += "/_index/1";
    expectedFilename += EXTENSION;

    {
        // Clean up any leftovers of previous failures
        boost::filesystem::path workDir(baseOutputFilename);
        BOOST_REQUIRE_NO_THROW(boost::filesystem::remove_all(workDir));

        ml::test::CMultiFileDataAdder persister(baseOutputFilename, EXTENSION);
        ml::core::CDataAdder::TOStreamP strm = persister.addStreamed("1");
        BOOST_TEST_REQUIRE(strm);
        (*strm) << EVENT;
        BOOST_TEST_REQUIRE(persister.streamComplete(strm, true));
    }

    {
        std::ifstream persistedFile(expectedFilename);

        BOOST_TEST_REQUIRE(persistedFile.is_open());
        std::string line;
        std::getline(persistedFile, line);
        BOOST_REQUIRE_EQUAL(EVENT, line);
    }

    BOOST_REQUIRE_EQUAL(0, ::remove(expectedFilename.c_str()));

    expectedFilename = baseOutputFilename;
    expectedFilename += "/_index/2";
    expectedFilename += EXTENSION;

    {
        ml::test::CMultiFileDataAdder persister(baseOutputFilename, EXTENSION);
        ml::core::CDataAdder::TOStreamP strm = persister.addStreamed("2");
        BOOST_TEST_REQUIRE(strm);
        (*strm) << SUMMARY_EVENT;
        BOOST_TEST_REQUIRE(persister.streamComplete(strm, true));
    }

    {
        std::ifstream persistedFile(expectedFilename);

        BOOST_TEST_REQUIRE(persistedFile.is_open());
        std::string line;
        std::getline(persistedFile, line);
        BOOST_REQUIRE_EQUAL(SUMMARY_EVENT, line);
    }

    // Clean up
    boost::filesystem::path workDir(baseOutputFilename);
    BOOST_REQUIRE_NO_THROW(boost::filesystem::remove_all(workDir));
}

BOOST_AUTO_TEST_CASE(testDetectorPersistBy) {
    detectorPersistHelper("testfiles/new_mlfields.json",
                          "testfiles/big_ascending.txt", 0, "%d/%b/%Y:%T %z");
}

BOOST_AUTO_TEST_CASE(testDetectorPersistOver) {
    detectorPersistHelper("testfiles/new_mlfields_over.json",
                          "testfiles/big_ascending.txt", 0, "%d/%b/%Y:%T %z");
}

BOOST_AUTO_TEST_CASE(testDetectorPersistPartition) {
    detectorPersistHelper("testfiles/new_mlfields_partition.json",
                          "testfiles/big_ascending.txt", 0, "%d/%b/%Y:%T %z");
}

BOOST_AUTO_TEST_CASE(testDetectorPersistDc) {
    detectorPersistHelper("testfiles/new_persist_dc.json",
                          "testfiles/files_users_programs.csv", 5);
}

BOOST_AUTO_TEST_CASE(testDetectorPersistCount) {
    detectorPersistHelper("testfiles/new_persist_count.json",
                          "testfiles/files_users_programs.csv", 5);
}

BOOST_AUTO_TEST_SUITE_END()
