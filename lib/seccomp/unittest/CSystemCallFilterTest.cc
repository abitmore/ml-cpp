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

#include <core/CBlockingCallCancellingTimer.h>
#include <core/CLogger.h>
#include <core/COsFileFuncs.h>
#ifdef Linux
#include <core/CRegex.h>
#include <core/CUname.h>
#endif

#include <seccomp/CSystemCallFilter.h>

#include <test/CTestTmpDir.h>
#include <test/CThreadDataReader.h>
#include <test/CThreadDataWriter.h>

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cstdlib>
#include <string>

BOOST_AUTO_TEST_SUITE(CSystemCallFilterTest)

namespace {

const std::uint32_t SLEEP_TIME_MS{100};
const std::size_t TEST_SIZE{10000};
const std::size_t MAX_ATTEMPTS{20};
const char TEST_CHAR{'a'};
// CTestTmpDir::tmpDir() fails to get the current user after the system call
// filter is installed, so cache the value early
const std::string TMP_DIR{ml::test::CTestTmpDir::tmpDir()};
#ifdef Windows
const std::string TEST_READ_PIPE_NAME{"\\\\.\\pipe\\testreadpipe"};
const std::string TEST_WRITE_PIPE_NAME{"\\\\.\\pipe\\testwritepipe"};
#else
const std::string TEST_READ_PIPE_NAME{TMP_DIR + "/testreadpipe"};
const std::string TEST_WRITE_PIPE_NAME{TMP_DIR + "/testwritepipe"};
#endif

bool systemCall() {
    return std::system("hostname") == 0;
}

void openPipeAndRead(const std::string& filename) {

    ml::test::CThreadDataWriter threadWriter{SLEEP_TIME_MS, filename, TEST_CHAR, TEST_SIZE};
    BOOST_TEST_REQUIRE(threadWriter.start());

    std::atomic_bool dummy{false};
    ml::core::CNamedPipeFactory::TIStreamP strm{
        ml::core::CNamedPipeFactory::openPipeStreamRead(filename, dummy)};
    BOOST_TEST_REQUIRE(strm);

    static const std::streamsize BUF_SIZE{512};
    std::string readData;
    readData.reserve(TEST_SIZE);
    char buffer[BUF_SIZE];
    do {
        strm->read(buffer, BUF_SIZE);
        BOOST_TEST_REQUIRE(!strm->bad());
        if (strm->gcount() > 0) {
            readData.append(buffer, static_cast<size_t>(strm->gcount()));
        }
    } while (!strm->eof());

    BOOST_REQUIRE_EQUAL(TEST_SIZE, readData.length());
    BOOST_REQUIRE_EQUAL(std::string(TEST_SIZE, TEST_CHAR), readData);

    BOOST_TEST_REQUIRE(threadWriter.waitForFinish());

    strm.reset();
}

void openPipeAndWrite(const std::string& filename) {
    ml::test::CThreadDataReader threadReader{SLEEP_TIME_MS, MAX_ATTEMPTS, filename};
    BOOST_TEST_REQUIRE(threadReader.start());

    std::atomic_bool dummy{false};
    ml::core::CNamedPipeFactory::TOStreamP strm{
        ml::core::CNamedPipeFactory::openPipeStreamWrite(filename, dummy)};
    BOOST_TEST_REQUIRE(strm);

    std::size_t charsLeft{TEST_SIZE};
    std::size_t blockSize{7};
    while (charsLeft > 0) {
        if (blockSize > charsLeft) {
            blockSize = charsLeft;
        }
        (*strm) << std::string(blockSize, TEST_CHAR);
        BOOST_TEST_REQUIRE(!strm->bad());
        charsLeft -= blockSize;
    }

    strm.reset();

    BOOST_TEST_REQUIRE(threadReader.waitForFinish());
    BOOST_TEST_REQUIRE(threadReader.attemptsTaken() <= MAX_ATTEMPTS);
    BOOST_TEST_REQUIRE(threadReader.streamWentBad() == false);

    BOOST_REQUIRE_EQUAL(TEST_SIZE, threadReader.data().length());
    BOOST_REQUIRE_EQUAL(std::string(TEST_SIZE, TEST_CHAR), threadReader.data());
}

void cancelBlockingCall() {
    ml::core::CBlockingCallCancellingTimer cancellerThread{
        ml::core::CThread::currentThreadId(), std::chrono::seconds{1}};
    BOOST_TEST_REQUIRE(cancellerThread.start());

    // The CBlockingCallCancellingTimer should wake up the blocking open
    // of the named pipe "seccomp_test_pipe".  Without this wake up, it would
    // block indefinitely as nothing will ever connect to the other end.  The
    // wake up happens after 1 second.

    std::string testPipeName{ml::core::CNamedPipeFactory::defaultPath() + "seccomp_test_pipe"};
    ml::core::CNamedPipeFactory::TIStreamP pipeStrm{ml::core::CNamedPipeFactory::openPipeStreamRead(
        testPipeName, cancellerThread.hasCancelledBlockingCall())};
    BOOST_TEST_REQUIRE(pipeStrm == nullptr);

    BOOST_TEST_REQUIRE(cancellerThread.stop());

    BOOST_REQUIRE_EQUAL(true, cancellerThread.hasCancelledBlockingCall().load());

    // Confirm that cancellation of the named pipe connection deleted the pipe
    BOOST_REQUIRE_EQUAL(-1, ml::core::COsFileFuncs::access(
                                testPipeName.c_str(), ml::core::COsFileFuncs::EXISTS));
    BOOST_REQUIRE_EQUAL(ENOENT, errno);
}

void makeAndRemoveDirectory(const std::string& dirname) {

    boost::filesystem::path temporaryFolder{dirname};
    temporaryFolder /= "test-directory";

    boost::system::error_code errorCode;
    boost::filesystem::create_directories(temporaryFolder, errorCode);
    BOOST_REQUIRE_EQUAL(boost::system::error_code(), errorCode);
    boost::filesystem::remove_all(temporaryFolder, errorCode);
    BOOST_REQUIRE_EQUAL(boost::system::error_code(), errorCode);
}

#ifdef Linux
bool versionIsBefore3_5(int major, int minor) {
    if (major < 3) {
        return true;
    }
    if (major == 3 && minor < 5) {
        return true;
    }
    return false;
}
#endif
}

BOOST_AUTO_TEST_CASE(testSystemCallFilter) {
#ifdef Linux
    std::string release{ml::core::CUname::release()};
    ml::core::CRegex semVersion;
    BOOST_TEST_REQUIRE(semVersion.init("(\\d)\\.(\\d{1,2})\\.(\\d{1,2}).*"));
    ml::core::CRegex::TStrVec tokens;
    BOOST_TEST_REQUIRE(semVersion.tokenise(release, tokens));
    // Seccomp is available in kernels since 3.5

    int major{std::stoi(tokens[0])};
    int minor{std::stoi(tokens[1])};
    if (versionIsBefore3_5(major, minor)) {
        LOG_INFO(<< "Cannot test seccomp on linux kernels before 3.5");
        return;
    }
#endif // Linux

    // Ensure actions are not prohibited before the
    // system call filters are applied
    BOOST_TEST_REQUIRE(systemCall());

    // Install the filter
    ml::seccomp::CSystemCallFilter::installSystemCallFilter();

    BOOST_REQUIRE_MESSAGE(systemCall() == false, "Calling std::system should fail");

    // Operations that must function after seccomp is initialised
    openPipeAndRead(TEST_READ_PIPE_NAME);
    openPipeAndWrite(TEST_WRITE_PIPE_NAME);

    makeAndRemoveDirectory(TMP_DIR);

    cancelBlockingCall();
}

BOOST_AUTO_TEST_SUITE_END()
