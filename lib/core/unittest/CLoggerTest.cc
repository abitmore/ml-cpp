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
#include <core/CNamedPipeFactory.h>
#include <core/COsFileFuncs.h>

#include <boost/json.hpp>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <ios>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace json = boost::json;

BOOST_AUTO_TEST_SUITE(CLoggerTest)

namespace {
#ifdef Windows
const char* const TEST_PIPE_NAME = "\\\\.\\pipe\\testpipe";
#else
const char* const TEST_PIPE_NAME = "testfiles/testpipe";
#endif

using TStrVec = std::vector<std::string>;

class CTestFixture {
public:
    ~CTestFixture() {
        // Tests in this file can leave the logger in an unusual state, so reset it
        // after each test
        ml::core::CLogger::instance().reset();
    }
};

std::function<void()> makeReader(std::ostringstream& loggedData, const std::string& pipeName) {
    return [&loggedData, &pipeName]() {

        for (std::size_t attempt = 1; attempt <= 100; ++attempt) {
            // wait a bit so that pipe has been created
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::ifstream strm(pipeName);
            if (strm.is_open()) {
                std::copy(std::istreambuf_iterator<char>(strm),
                          std::istreambuf_iterator<char>(),
                          std::ostreambuf_iterator<char>(loggedData));
                return;
            }
        }
        BOOST_FAIL("Failed to connect to logging pipe " + pipeName + " within a reasonable time");
    };
}

void loggedExpectedMessages(const std::string& logging, const TStrVec& messages) {
    std::istringstream inputStream{logging};
    std::string line;
    std::size_t foundMessages{0};

    json::parser p;
    // test that we found the messages we put in,
    while (std::getline(inputStream, line)) {
        if (line.empty()) {
            continue;
        }
        json::value doc;
        json::error_code ec;
        p.write(line, ec);
        doc = p.release();
        LOG_INFO(<< "doc: " << doc);
        BOOST_TEST_REQUIRE(ec.failed() == false);
        BOOST_TEST_REQUIRE(doc.is_object());
        BOOST_TEST_REQUIRE(doc.as_object().contains("message"));
        BOOST_TEST_REQUIRE(doc.as_object().at("message").is_string());
        const json::value& messageValue = doc.as_object().at("message");
        std::string_view messageString = messageValue.as_string();

        // we expect messages to be in order, so we only need to test the current one
        if (messageString.find(messages[foundMessages]) != std::string::npos) {
            ++foundMessages;
        } else if (foundMessages > 0) {
            BOOST_FAIL(std::string(messageString) + " did not contain " +
                       messages[foundMessages]);
        }
    }
    BOOST_REQUIRE_EQUAL(messages.size(), foundMessages);
}
}

BOOST_FIXTURE_TEST_CASE(testLogging, CTestFixture) {
    std::string t{"Test message"};

    BOOST_TEST_REQUIRE(ml::core::CLogger::instance().hasBeenReconfigured() == false);

    LOG_TRACE(<< "Trace");
    LOG_AT_LEVEL(ml::core::CLogger::E_Trace, << "Dynamic TRACE " << 1);
    LOG_DEBUG(<< "Debug");
    LOG_AT_LEVEL(ml::core::CLogger::E_Debug, << "Dynamic DEBUG " << 2.0);
    LOG_INFO(<< "Info " << std::boolalpha << true);
    LOG_AT_LEVEL(ml::core::CLogger::E_Info, << "Dynamic INFO " << false);
    LOG_WARN(<< "Warn " << t);
    LOG_AT_LEVEL(ml::core::CLogger::E_Warn, << "Dynamic WARN "
                                            << "abc");
    LOG_ERROR(<< "Error " << 1000 << ' ' << 0.23124F);
    LOG_AT_LEVEL(ml::core::CLogger::E_Error, << "Dynamic ERROR");
    LOG_FATAL(<< "Fatal - application to handle exit");
    LOG_AT_LEVEL(ml::core::CLogger::E_Fatal, << "Dynamic FATAL " << t);

    // It's not possible to test LOG_ABORT as it calls std::terminate
}

BOOST_FIXTURE_TEST_CASE(testReconfiguration, CTestFixture) {
    ml::core::CLogger& logger{ml::core::CLogger::instance()};

    LOG_DEBUG(<< "Starting logger reconfiguration test");

    LOG_TRACE(<< "This shouldn't be seen because the hardcoded default log level is DEBUG");
    BOOST_TEST_REQUIRE(logger.hasBeenReconfigured() == false);

    BOOST_TEST_REQUIRE(logger.reconfigureFromFile("nonexistantfile") == false);

    BOOST_TEST_REQUIRE(logger.reconfigureLogJson());
    LOG_INFO(<< "This should be logged as JSON!");

    // The test boost.log.ini is very similar to the hardcoded default, but
    // with the level set to TRACE rather than DEBUG
    BOOST_TEST_REQUIRE(logger.reconfigureFromFile("testfiles/boost.log.ini"));

    LOG_TRACE(<< "This should be seen because the reconfigured log level is TRACE");
    BOOST_TEST_REQUIRE(logger.hasBeenReconfigured());
}

BOOST_FIXTURE_TEST_CASE(testSetLevel, CTestFixture) {
    ml::core::CLogger& logger{ml::core::CLogger::instance()};

    LOG_DEBUG(<< "Starting logger level test");

    BOOST_TEST_REQUIRE(logger.setLoggingLevel(ml::core::CLogger::E_Error));

    LOG_TRACE(<< "SHOULD NOT BE SEEN");
    LOG_DEBUG(<< "SHOULD NOT BE SEEN");
    LOG_INFO(<< "SHOULD NOT BE SEEN");
    LOG_WARN(<< "SHOULD NOT BE SEEN");
    LOG_ERROR(<< "Should be seen");
    LOG_FATAL(<< "Should be seen");

    BOOST_TEST_REQUIRE(logger.setLoggingLevel(ml::core::CLogger::E_Info));

    LOG_TRACE(<< "SHOULD NOT BE SEEN");
    LOG_DEBUG(<< "SHOULD NOT BE SEEN");
    LOG_INFO(<< "Should be seen");
    LOG_WARN(<< "Should be seen");
    LOG_ERROR(<< "Should be seen");
    LOG_FATAL(<< "Should be seen");

    BOOST_TEST_REQUIRE(logger.setLoggingLevel(ml::core::CLogger::E_Trace));

    LOG_TRACE(<< "Should be seen");
    LOG_DEBUG(<< "Should be seen");
    LOG_INFO(<< "Should be seen");
    LOG_WARN(<< "Should be seen");
    LOG_ERROR(<< "Should be seen");
    LOG_FATAL(<< "Should be seen");

    BOOST_TEST_REQUIRE(logger.setLoggingLevel(ml::core::CLogger::E_Warn));

    LOG_TRACE(<< "SHOULD NOT BE SEEN");
    LOG_DEBUG(<< "SHOULD NOT BE SEEN");
    LOG_INFO(<< "SHOULD NOT BE SEEN");
    LOG_WARN(<< "Should be seen");
    LOG_ERROR(<< "Should be seen");
    LOG_FATAL(<< "Should be seen");

    BOOST_TEST_REQUIRE(logger.setLoggingLevel(ml::core::CLogger::E_Fatal));

    LOG_TRACE(<< "SHOULD NOT BE SEEN");
    LOG_DEBUG(<< "SHOULD NOT BE SEEN");
    LOG_INFO(<< "SHOULD NOT BE SEEN");
    LOG_WARN(<< "SHOULD NOT BE SEEN");
    LOG_ERROR(<< "SHOULD NOT BE SEEN");
    LOG_FATAL(<< "Should be seen");

    BOOST_TEST_REQUIRE(logger.setLoggingLevel(ml::core::CLogger::E_Debug));

    LOG_DEBUG(<< "Finished logger level test");
}

BOOST_FIXTURE_TEST_CASE(testNonAsciiJsonLogging, CTestFixture) {
    TStrVec messages{"Non-iso8859-15: 编码", "Non-ascii: üaöä",
                     "Non-iso8859-15: 编码 test", "surrogate pair: 𐐷 test"};

    std::ostringstream loggedData;
    const std::string& pipeName = std::string{TEST_PIPE_NAME} + "_testNonAsciiJsonLogging";
    std::thread reader(makeReader(loggedData, pipeName));

    ml::core::CLogger& logger = ml::core::CLogger::instance();
    // logger might have been reconfigured in previous tests, so reset and reconfigure it
    logger.reset();
    logger.reconfigure(pipeName, "");

    for (const auto& m : messages) {
        LOG_INFO(<< m);
    }

    // reset the logger to end the stream and revert state for following tests
    logger.reset();

    reader.join();
    loggedExpectedMessages(loggedData.str(), messages);
}

BOOST_FIXTURE_TEST_CASE(testWarnAndErrorThrottling, CTestFixture) {

    std::ostringstream loggedData;
    const std::string& pipeName = std::string{TEST_PIPE_NAME} + "_testWarnAndErrorThrottling";

    std::thread reader{makeReader(loggedData, pipeName)};

    TStrVec messages{"Warn should only be seen once", "Error should only be seen once"};

    ml::core::CLogger& logger = ml::core::CLogger::instance();
    // logger might have been reconfigured in previous tests, so reset and reconfigure it
    logger.reset();
    logger.reconfigure(pipeName, "");

    for (std::size_t i = 0; i < 10; ++i) {
        LOG_WARN(<< messages[0]);
        LOG_ERROR(<< messages[1]);
    }

    // reset the logger to end the stream and revert state for following tests
    logger.reset();

    reader.join();
    loggedExpectedMessages(loggedData.str(), messages);
}

// Disabled because it doesn't assert
// Run ad hoc if required
BOOST_FIXTURE_TEST_CASE(testLogEnvironment, CTestFixture, *boost::unit_test::disabled()) {
    ml::core::CLogger::instance().logEnvironment();
}

BOOST_AUTO_TEST_SUITE_END()
