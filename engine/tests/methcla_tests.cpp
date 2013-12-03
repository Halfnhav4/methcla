// Copyright 2012-2013 Samplecount S.L.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "Methcla/Utility/Macros.h"
#include "methcla_tests.hpp"

METHCLA_WITHOUT_WARNINGS_BEGIN
# define CATCH_CONFIG_RUNNER
# include <catch.hpp>
METHCLA_WITHOUT_WARNINGS_END

#include "Methcla/Utility/MessageQueue.hpp"
#include "Methcla/Utility/Semaphore.hpp"

#include <atomic>
#include <mutex>
#include <thread>

std::string Methcla::Tests::inputFile(const std::string& name)
{
    return std::string("tests/input/") + name;
}

std::string Methcla::Tests::outputFile(const std::string& name)
{
    return std::string("tests/output/") + name;
}

namespace test_Methcla_Utility_Worker
{
    struct Command
    {
        void perform() { }
    };
};

namespace Methcla { namespace Test {

class Log
{
public:
    Log()
        : m_lock(s_mutex)
    { }

    template <typename T> Log& operator<<(const T& x)
    {
#if DEBUG
        std::cerr << x;
#endif
        return *this;
    }

private:
    std::lock_guard<std::mutex> m_lock;
    static std::mutex s_mutex;
};

std::mutex Log::s_mutex;
} }

TEST_CASE("Methcla/Utility/Semaphore/constructor", "Test constructor.")
{
    for (size_t n : { 1, 2, 3, 10, 20, 50, 100, 1000, 1024, 10000 }) {
        Methcla::Utility::Semaphore sem(n);
        size_t count(0);

        for (size_t i=0; i < n; i++) {
            sem.wait();
            count++;
        }

        REQUIRE(count == n);
    }
}

TEST_CASE("Methcla/Utility/Semaphore/post", "Test post/wait.")
{
    for (size_t n : { 1, 2, 3, 10, 20, 50, 100, 1000, 1024, 10000 }) {
        Methcla::Utility::Semaphore sem;
        std::atomic<size_t> count(0);

        std::thread thread([&](){
            for (size_t i=0; i < n; i++) {
                count++;
                sem.post();
            }
        });
        for (size_t i=0; i < n; i++) {
            sem.wait();
        }
        REQUIRE(count.load() == n);
        thread.join();
    }
}

TEST_CASE("Methcla/Utility/Worker", "Check for queue overflow.")
{
    using test_Methcla_Utility_Worker::Command;

    const size_t queueSize = 1024;

    Methcla::Utility::Worker<Command> worker(queueSize, false);

    for (size_t i=0; i < worker.maxCapacity(); i++) {
        worker.sendToWorker(Command());
    }

    REQUIRE_THROWS(worker.sendToWorker(Command()));
}

namespace test_Methcla_Utility_WorkerThread
{
    struct Command
    {
        void perform()
        {
            (*m_count)++;
            m_sem->post();
            Methcla::Test::Log() << "POST " << m_id << "\n";
        }

        size_t m_id;
        std::atomic<size_t>* m_count;
        Methcla::Utility::Semaphore* m_sem;
    };
};

TEST_CASE("Methcla/Utility/WorkerThread", "Check that all commands pushed to a worker thread are executed.")
{
    using test_Methcla_Utility_WorkerThread::Command;

    const size_t queueSize = 16;

    for (size_t threadCount=1; threadCount <= 4; threadCount++) {
        Methcla::Test::Log() << "threads " << threadCount << "\n";

        Methcla::Utility::WorkerThread<Command> worker(queueSize, threadCount);

        std::atomic<size_t> count(0);
        Methcla::Utility::Semaphore sem;

        for (size_t i=0; i < worker.maxCapacity(); i++) {
            Command cmd;
            cmd.m_id = i;
            cmd.m_count = &count;
            cmd.m_sem = &sem;
            worker.sendToWorker(cmd);
        }

        for (size_t i=0; i < worker.maxCapacity(); i++) {
            sem.wait();
            Methcla::Test::Log() << "WAIT " << i << " " << count.load() << "\n";
        }

        REQUIRE(count.load() == worker.maxCapacity());
    }
}

#if !defined(__ANDROID__) && !defined(__native_client__)
int main(int argc, char* const argv[])
{
    return Catch::Session().run(argc, argv);
}
#endif // defined(__ANDROID__)

// ============================================================================
// NaCl

#if defined(__native_client__)

#include <string>

#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/var.h"

#include "catch_callback_reporter.hpp"

class MethclaTestInstance : public pp::Instance
{
public:
    explicit MethclaTestInstance(PP_Instance instance)
        : pp::Instance(instance)
    {}

private:
    virtual void HandleMessage(const pp::Var& var_message)
    {
        Catch::registerCallbackReporter("callback", [this](const std::string& str) {
            this->PostMessage(pp::Var(str));
        });

        Catch::Session session; // There must be exactly once instance
        session.configData().reporterName = "callback";
        session.run();
    }
};

class MethclaTestModule : public pp::Module
{
public:
    virtual ~MethclaTestModule() {}

    virtual pp::Instance* CreateInstance(PP_Instance instance)
    {
        return new MethclaTestInstance(instance);
    }
};

namespace pp
{
    Module* CreateModule()
    {
        return new MethclaTestModule();
    }
}

#endif // __native_client__
