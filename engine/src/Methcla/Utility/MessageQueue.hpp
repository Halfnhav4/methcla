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

#ifndef METHCLA_UTILITY_MESSAGEQUEUE_HPP_INCLUDED
#define METHCLA_UTILITY_MESSAGEQUEUE_HPP_INCLUDED

#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <boost/lockfree/spsc_queue.hpp>
#pragma GCC diagnostic pop
#include <boost/noncopyable.hpp>

#include "Methcla/Utility/Semaphore.hpp"

namespace Methcla { namespace Utility {

//* MWSR queue for sending commands to the engine.
// Request payload lifetime: from request until response callback.
// Caller is responsible for freeing request payload after the response callback has been called.
template <typename T, size_t queueSize> class MessageQueue : boost::noncopyable
{
public:
    inline void send(const T& msg)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool success = m_queue.push(msg);
        if (!success) throw std::runtime_error("Message queue overflow");
    }

    inline bool next(T& msg)
    {
        return m_queue.pop(msg);
    }

private:
    typedef boost::lockfree::spsc_queue<T,boost::lockfree::capacity<queueSize>> Queue;
    Queue      m_queue;
    std::mutex m_mutex;
};

template <class Command> class Transport : boost::noncopyable
{
public:
    Transport(size_t queueSize)
        : m_queue(queueSize)
    { }
    virtual ~Transport()
    { }

    virtual void send(const Command& cmd) = 0;

    virtual bool dequeue(Command& cmd) = 0;

    void drain()
    {
        Command cmd;
        while (dequeue(cmd)) {
            cmd.perform();
        }
    }

protected:
    void sendCommand(const Command& cmd)
    {
        bool success = m_queue.push(cmd);
        if (!success) throw std::runtime_error("Channel overflow");
    }

protected:
    typedef boost::lockfree::spsc_queue<Command> Queue;
    // typedef boost::lockfree::queue<Command,boost::lockfree::capacity<queueSize>> Queue;
    Queue m_queue;
};

template <class Command> class ToWorker : public Transport<Command>
{
public:
    ToWorker(size_t queueSize, std::function<void()> signal)
        : Transport<Command>(queueSize)
        , m_signal(signal)
    { }

    virtual void send(const Command& cmd) override
    {
        this->sendCommand(cmd);
        if (m_signal) m_signal();
    }

    bool dequeue(Command& cmd)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return this->m_queue.pop(cmd);
    }

private:
    std::function<void()> m_signal;
    std::mutex            m_mutex;
};

template <class Command> class FromWorker : public Transport<Command>
{
public:
    FromWorker(size_t queueSize)
        : Transport<Command>(queueSize)
    { }

    virtual void send(const Command& cmd) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        this->sendCommand(cmd);
    }

    bool dequeue(Command& cmd) override
    {
        return this->m_queue.pop(cmd);
    }

private:
    std::mutex m_mutex;
};

template <typename Command> class Worker : boost::noncopyable
{
public:
    Worker(size_t queueSize)
        : m_queueSize(queueSize)
        , m_toWorker(queueSize, [this](){ this->signalWorker(); })
        , m_fromWorker(queueSize)
    { }

    size_t maxCapacity() const
    {
        return m_queueSize - 1;
    }

    void sendToWorker(const Command& cmd)
    {
        m_toWorker.send(cmd);
    }

    void sendFromWorker(const Command& cmd)
    {
        m_fromWorker.send(cmd);
    }

    void perform()
    {
        m_fromWorker.drain();
    }

protected:
    void work()
    {
        m_toWorker.drain();
    }

    virtual void signalWorker() { }

private:
    size_t              m_queueSize;
    ToWorker<Command>   m_toWorker;
    FromWorker<Command> m_fromWorker;
};

template <typename Command> class WorkerThread : public Worker<Command>
{
public:
    WorkerThread(size_t queueSize, size_t numThreads=1)
        : Worker<Command>(queueSize)
        , m_continue(true)
    {
        for (size_t i=0; i < std::max((size_t)1, numThreads); i++) {
            m_threads.emplace_back([this](){ this->process(); });
        }
    }

    ~WorkerThread()
    {
        m_continue.store(false, std::memory_order_relaxed);
        m_sem.post();
        for (auto& t : m_threads) { t.join(); }
    }

private:
    void process()
    {
        for (;;) {
            m_sem.wait();
            bool cont = m_continue.load(std::memory_order_relaxed);
            if (cont) {
                this->work();
            } else {
                break;
            }
        }
    }

    virtual void signalWorker() override
    {
        m_sem.post();
    }

private:
    Semaphore                   m_sem;
    std::atomic<bool>           m_continue;
    std::vector<std::thread>    m_threads;
};

} }

#endif // METHCLA_UTILITY_MESSAGEQUEUE_HPP_INCLUDED
