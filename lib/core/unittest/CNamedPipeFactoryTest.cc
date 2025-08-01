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
#include <core/CThread.h>

#include <test/CThreadDataReader.h>
#include <test/CThreadDataWriter.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#ifndef Windows
#include <unistd.h>
#endif

BOOST_AUTO_TEST_SUITE(CNamedPipeFactoryTest)

namespace {

const std::uint32_t SLEEP_TIME_MS{100};
const std::uint32_t PAUSE_TIME_MS{10};
const std::size_t MAX_ATTEMPTS{100};
const std::size_t TEST_SIZE{10000};
const char TEST_CHAR{'a'};
#ifdef Windows
const std::string TEST_PIPE_NAME{"\\\\.\\pipe\\testpipe"};
#else
const std::string TEST_PIPE_NAME{"testfiles/testpipe"};
#endif

class CThreadBlockCanceller : public ml::core::CThread {
public:
    CThreadBlockCanceller(ml::core::CThread::TThreadId threadId)
        : m_ThreadId{threadId}, m_HasCancelledBlockingCall{false} {}

    const std::atomic_bool& hasCancelledBlockingCall() {
        return m_HasCancelledBlockingCall;
    }

protected:
    void run() override {
        // Wait for the file to exist
        std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_TIME_MS));

        // Cancel the open() or read() operation on the file
        m_HasCancelledBlockingCall.store(true);
        BOOST_TEST_REQUIRE(ml::core::CThread::cancelBlockedIo(m_ThreadId));
    }

    void shutdown() override {}

private:
    ml::core::CThread::TThreadId m_ThreadId;
    std::atomic_bool m_HasCancelledBlockingCall;
};
}

BOOST_AUTO_TEST_CASE(testServerIsCppReader) {
    const std::string pipeName = TEST_PIPE_NAME + "_testServerIsCppReader";
    ml::test::CThreadDataWriter threadWriter{SLEEP_TIME_MS, pipeName, TEST_CHAR, TEST_SIZE};
    BOOST_TEST_REQUIRE(threadWriter.start());

    std::atomic_bool dummy{false};
    ml::core::CNamedPipeFactory::TIStreamP strm{
        ml::core::CNamedPipeFactory::openPipeStreamRead(pipeName, dummy)};
    BOOST_TEST_REQUIRE(strm);

    static const std::streamsize BUF_SIZE{512};
    std::string readData;
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

BOOST_AUTO_TEST_CASE(testServerIsCReader) {
    const std::string pipeName = TEST_PIPE_NAME + "_testServerIsCReader";

    ml::test::CThreadDataWriter threadWriter{SLEEP_TIME_MS, pipeName, TEST_CHAR, TEST_SIZE};
    BOOST_TEST_REQUIRE(threadWriter.start());

    std::atomic_bool dummy{false};
    ml::core::CNamedPipeFactory::TFileP file{
        ml::core::CNamedPipeFactory::openPipeFileRead(pipeName, dummy)};
    BOOST_TEST_REQUIRE(file);

    static const std::size_t BUF_SIZE{512};
    std::string readData;
    char buffer[BUF_SIZE];
    do {
        std::size_t charsRead{std::fread(buffer, sizeof(char), BUF_SIZE, file.get())};
        BOOST_TEST_REQUIRE(!std::ferror(file.get()));
        if (charsRead > 0) {
            readData.append(buffer, charsRead);
        }
    } while (!std::feof(file.get()));

    BOOST_REQUIRE_EQUAL(TEST_SIZE, readData.length());
    BOOST_REQUIRE_EQUAL(std::string(TEST_SIZE, TEST_CHAR), readData);

    BOOST_TEST_REQUIRE(threadWriter.waitForFinish());

    file.reset();
}

BOOST_AUTO_TEST_CASE(testServerIsCppWriter) {
    const std::string pipeName = TEST_PIPE_NAME + "_testServerIsCppWriter";

    ml::test::CThreadDataReader threadReader{PAUSE_TIME_MS, MAX_ATTEMPTS, pipeName};
    BOOST_TEST_REQUIRE(threadReader.start());

    std::atomic_bool dummy{false};
    ml::core::CNamedPipeFactory::TOStreamP strm{
        ml::core::CNamedPipeFactory::openPipeStreamWrite(pipeName, dummy)};
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

BOOST_AUTO_TEST_CASE(testServerIsCWriter) {
    const std::string pipeName = TEST_PIPE_NAME + "_testServerIsCWriter";

    ml::test::CThreadDataReader threadReader{PAUSE_TIME_MS, MAX_ATTEMPTS, pipeName};
    BOOST_TEST_REQUIRE(threadReader.start());

    std::atomic_bool dummy{false};
    ml::core::CNamedPipeFactory::TFileP file{
        ml::core::CNamedPipeFactory::openPipeFileWrite(pipeName, dummy)};
    BOOST_TEST_REQUIRE(file);

    std::size_t charsLeft{TEST_SIZE};
    std::size_t blockSize{7};
    while (charsLeft > 0) {
        if (blockSize > charsLeft) {
            blockSize = charsLeft;
        }
        BOOST_TEST_REQUIRE(std::fputs(std::string(blockSize, TEST_CHAR).c_str(),
                                      file.get()) >= 0);
        charsLeft -= blockSize;
    }

    file.reset();

    BOOST_TEST_REQUIRE(threadReader.waitForFinish());
    BOOST_TEST_REQUIRE(threadReader.attemptsTaken() <= MAX_ATTEMPTS);
    BOOST_TEST_REQUIRE(threadReader.streamWentBad() == false);

    BOOST_REQUIRE_EQUAL(TEST_SIZE, threadReader.data().length());
    BOOST_REQUIRE_EQUAL(std::string(TEST_SIZE, TEST_CHAR), threadReader.data());
}

BOOST_AUTO_TEST_CASE(testCancelBlock) {
    CThreadBlockCanceller cancellerThread{ml::core::CThread::currentThreadId()};
    BOOST_TEST_REQUIRE(cancellerThread.start());

    ml::core::CNamedPipeFactory::TOStreamP strm{ml::core::CNamedPipeFactory::openPipeStreamWrite(
        TEST_PIPE_NAME + "_testCancelBlock", cancellerThread.hasCancelledBlockingCall())};
    BOOST_TEST_REQUIRE(strm == nullptr);

    BOOST_TEST_REQUIRE(cancellerThread.stop());
}

BOOST_AUTO_TEST_CASE(testErrorIfRegularFile) {
    const std::atomic_bool dummy{false};
    ml::core::CNamedPipeFactory::TIStreamP strm{
        ml::core::CNamedPipeFactory::openPipeStreamRead("Main.cc", dummy)};
    BOOST_TEST_REQUIRE(strm == nullptr);
}

BOOST_AUTO_TEST_CASE(testErrorIfSymlink) {
#ifdef Windows
    // It's impossible to create a symlink to a named pipe on Windows - they
    // live under \\.\pipe\ and it's not possible to symlink to this part of
    // the file system
    LOG_DEBUG(<< "symlink test not relevant to Windows");
    // Suppress the error about no assertions in this case
    BOOST_REQUIRE(BOOST_IS_DEFINED(Windows));
#else
    const std::string TEST_SYMLINK_NAME{"test_symlink_testErrorIfSymlink"};
    const std::string testPipeName{TEST_PIPE_NAME + "_test_symlink_testErrorIfSymlink"};

    // Remove any files left behind by a previous failed test, but don't check
    // the return codes as these calls will usually fail
    ::unlink(TEST_SYMLINK_NAME.c_str());
    ::unlink(testPipeName.c_str());

    BOOST_REQUIRE_EQUAL(0, ::mkfifo(testPipeName.c_str(), S_IRUSR | S_IWUSR));
    BOOST_REQUIRE_EQUAL(0, ::symlink(testPipeName.c_str(), TEST_SYMLINK_NAME.c_str()));

    std::atomic_bool dummy{false};
    ml::core::CNamedPipeFactory::TIStreamP strm{
        ml::core::CNamedPipeFactory::openPipeStreamRead(TEST_SYMLINK_NAME, dummy)};
    BOOST_TEST_REQUIRE(strm == nullptr);

    BOOST_REQUIRE_EQUAL(0, ::unlink(TEST_SYMLINK_NAME.c_str()));
    BOOST_REQUIRE_EQUAL(0, ::unlink(testPipeName.c_str()));
#endif
}

BOOST_AUTO_TEST_SUITE_END()
