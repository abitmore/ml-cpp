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
#include <core/CStateMachine.h>
#include <core/CThread.h>

#include <test/CRandomNumbers.h>

#include <algorithm>
#include <vector>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(CStateMachineTest)

using namespace ml;

namespace {

using TSizeVec = std::vector<std::size_t>;
using TSizeVecVec = std::vector<TSizeVec>;
using TStrVec = std::vector<std::string>;

class CStateMachineClearer : core::CStateMachine {
public:
    static void clear() { core::CStateMachine::clear(); }
};

struct SMachine {
    TStrVec s_Alphabet;
    TStrVec s_States;
    TSizeVecVec s_TransitionFunction;

    bool operator==(const SMachine& rhs) const {
        return s_Alphabet == rhs.s_Alphabet && s_States == rhs.s_States &&
               s_TransitionFunction == rhs.s_TransitionFunction;
    }

    bool operator<(const SMachine& rhs) const {
        return s_Alphabet < rhs.s_Alphabet ||
               (s_Alphabet == rhs.s_Alphabet && s_States < rhs.s_States) ||
               (s_Alphabet == rhs.s_Alphabet && s_States == rhs.s_States &&
                s_TransitionFunction < rhs.s_TransitionFunction);
    }
};

using TMachineVec = std::vector<SMachine>;

class CTestThread : public core::CThread {
public:
    explicit CTestThread(const TMachineVec& machines)
        : m_Machines(machines), m_Failures(0) {}

    std::size_t failures() const { return m_Failures; }

    const TSizeVec& states() const { return m_States; }

private:
    void run() override {
        std::size_t n = 10000;
        m_States.reserve(n);
        TSizeVec machine;
        for (std::size_t i = 0; i < n; ++i) {
            m_Rng.generateUniformSamples(0, m_Machines.size(), 1, machine);
            core::CStateMachine sm = core::CStateMachine::create(
                m_Machines[machine[0]].s_Alphabet, m_Machines[machine[0]].s_States,
                m_Machines[machine[0]].s_TransitionFunction,
                0); // initial state
            if (!sm.apply(0)) {
                ++m_Failures;
            }
            m_States.push_back(sm.state());
        }
    }

    void shutdown() override {}

private:
    test::CRandomNumbers m_Rng;
    TMachineVec m_Machines;
    std::size_t m_Failures;
    TSizeVec m_States;
};

void randomMachines(std::size_t n, TMachineVec& result) {
    TStrVec states{"A", "B", "C", "D", "E", "F", "G", "H", "I", "J"};
    TStrVec alphabet{"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};

    test::CRandomNumbers rng;

    TSizeVec ns;
    rng.generateUniformSamples(2, states.size(), n, ns);

    TSizeVec na;
    rng.generateUniformSamples(1, alphabet.size(), n, na);

    result.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        result[i].s_States.assign(states.begin(), states.begin() + ns[i]);
        result[i].s_Alphabet.assign(alphabet.begin(), alphabet.begin() + na[i]);
        result[i].s_TransitionFunction.resize(na[i]);
        for (std::size_t j = 0; j < na[i]; ++j) {
            rng.generateUniformSamples(0, ns[i], ns[i], result[i].s_TransitionFunction[j]);
        }

        std::next_permutation(states.begin(), states.end());
        std::next_permutation(alphabet.begin(), alphabet.end());
    }
}
}

BOOST_AUTO_TEST_CASE(testBasics) {
    // Test errors on create.

    // Test transitions.

    TMachineVec machines;
    randomMachines(5, machines);

    for (std::size_t m = 0; m < machines.size(); ++m) {
        LOG_DEBUG(<< "machine " << m);
        for (std::size_t i = 0; i < machines[m].s_Alphabet.size(); ++i) {
            for (std::size_t j = 0; j < machines[m].s_States.size(); ++j) {
                core::CStateMachine sm = core::CStateMachine::create(
                    machines[m].s_Alphabet, machines[m].s_States, machines[m].s_TransitionFunction,
                    j); // initial state

                const std::string& oldState = machines[m].s_States[j];

                sm.apply(i);

                const std::string& newState = machines[m].s_States[sm.state()];

                LOG_DEBUG(<< "  " << oldState << " -> " << newState);
                BOOST_REQUIRE_EQUAL(
                    machines[m].s_States[machines[m].s_TransitionFunction[i][j]],
                    sm.printState(sm.state()));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(testPersist) {
    // Check persist maintains the checksum and is idempotent.

    TMachineVec machine;
    randomMachines(2, machine);

    core::CStateMachine original = core::CStateMachine::create(
        machine[0].s_Alphabet, machine[0].s_States, machine[0].s_TransitionFunction,
        1); // initial state
    std::ostringstream origJson;
    core::CJsonStatePersistInserter::persist(
        origJson, [&original](core::CJsonStatePersistInserter& inserter) {
            original.acceptPersistInserter(inserter);
        });

    LOG_DEBUG(<< "State machine JSON representation:\n" << origJson.str());

    // The traverser expects the state json in a embedded document
    std::istringstream is("{\"topLevel\" : " + origJson.str() + "}");
    core::CJsonStateRestoreTraverser traverser(is);

    core::CStateMachine restored = core::CStateMachine::create(
        machine[0].s_Alphabet, machine[0].s_States, machine[0].s_TransitionFunction,
        0); // initial state
    traverser.traverseSubLevel([&restored](core::CStateRestoreTraverser& traverser_) {
        return restored.acceptRestoreTraverser(traverser_);
    });

    BOOST_REQUIRE_EQUAL(original.checksum(), restored.checksum());
    std::ostringstream newJson;
    core::CJsonStatePersistInserter::persist(
        newJson, [&restored](core::CJsonStatePersistInserter& inserter) {
            restored.acceptPersistInserter(inserter);
        });
    BOOST_REQUIRE_EQUAL(origJson.str(), newJson.str());
}

BOOST_AUTO_TEST_CASE(testMultithreaded) {
    // Check that we create each machine once and we don't get any
    // errors updating due to stale reads.

    CStateMachineClearer::clear();

    TMachineVec machines;
    randomMachines(100, machines);

    LOG_DEBUG(<< "# machines = " << machines.size());

    std::sort(machines.begin(), machines.end());
    machines.erase(std::unique(machines.begin(), machines.end()), machines.end());

    using TThreadPtr = std::shared_ptr<CTestThread>;
    using TThreadVec = std::vector<TThreadPtr>;
    TThreadVec threads;
    for (std::size_t i = 0; i < 20; ++i) {
        threads.push_back(TThreadPtr(new CTestThread(machines)));
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        BOOST_TEST_REQUIRE(threads[i]->start());
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        BOOST_TEST_REQUIRE(threads[i]->waitForFinish());
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        // No failed reads.
        BOOST_REQUIRE_EQUAL(0, threads[i]->failures());
    }
    for (std::size_t i = 1; i < threads.size(); ++i) {
        // No wrong reads.
        BOOST_TEST_REQUIRE(threads[i]->states() == threads[i - 1]->states());
    }
    // No duplicates.
    BOOST_REQUIRE_EQUAL(machines.size(), core::CStateMachine::numberMachines());
}

BOOST_AUTO_TEST_SUITE_END()
