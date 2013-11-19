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

#ifndef METHCLA_ENGINE_HPP_INCLUDED
#define METHCLA_ENGINE_HPP_INCLUDED

#include <methcla/engine.h>
#include <methcla/plugin.h>
#include <methcla/types.h>

#include <exception>
#include <future>
#include <iostream>
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <oscpp/client.hpp>
#include <oscpp/server.hpp>
#include <oscpp/print.hpp>

namespace Methcla
{
    inline static const char* version()
    {
        return methcla_version();
    }

    inline static void dumpRequest(std::ostream& out, const OSCPP::Client::Packet& packet)
    {
        out << "Request (send): " << packet << std::endl;
    }

    namespace detail
    {
        template <class D, typename T> class Id
        {
        public:
            explicit Id(T id)
                : m_id(id)
            { }
            Id(const D& other)
                : m_id(other.m_id)
            { }

            T id() const
            {
                return m_id;
            }

            bool operator==(const D& other) const
            {
                return m_id == other.m_id;
            }

            bool operator!=(const D& other) const
            {
                return m_id != other.m_id;
            }

        private:
            T m_id;
        };

        inline static void throwError(Methcla_Error err, const char* msg)
        {
            if (err != kMethcla_NoError)
            {
                if (err == kMethcla_ArgumentError) {
                    throw std::invalid_argument(msg);
                } else if (err == kMethcla_LogicError) {
                    throw std::logic_error(msg);
                } else if (err == kMethcla_MemoryError) {
                    throw std::bad_alloc();
                } else {
                    throw std::runtime_error(msg);
                }
            }
        }

        inline static void checkReturnCode(Methcla_Error err)
        {
            throwError(err, methcla_error_message(err));
        }
    }

    class NodeId : public detail::Id<NodeId,int32_t>
    {
    public:
        NodeId(int32_t id)
            : Id<NodeId,int32_t>(id)
        { }
        NodeId()
            : NodeId(-1)
        { }
    };

    class GroupId : public NodeId
    {
    public:
        // Inheriting constructors not supported by clang 3.2
        // using NodeId::NodeId;
        GroupId(int32_t id)
            : NodeId(id)
        { }
        GroupId()
            : NodeId()
        { }
    };

    class SynthId : public NodeId
    {
    public:
        SynthId(int32_t id)
            : NodeId(id)
        { }
        SynthId()
            : NodeId()
        { }
    };

    class AudioBusId : public detail::Id<AudioBusId,int32_t>
    {
    public:
        AudioBusId(int32_t id)
            : Id<AudioBusId,int32_t>(id)
        { }
        AudioBusId()
            : AudioBusId(0)
        { }
    };

    // Node placement specification given a target.
    class NodePlacement
    {
        NodeId                m_target;
        Methcla_NodePlacement m_placement;

    public:
        NodePlacement(NodeId target, Methcla_NodePlacement placement)
            : m_target(target)
            , m_placement(placement)
        { }

        NodePlacement(GroupId target)
            : NodePlacement(target, kMethcla_NodePlacementTailOfGroup)
        { }

        NodeId target() const
        {
            return m_target;
        }

        Methcla_NodePlacement placement() const
        {
            return m_placement;
        }

        static NodePlacement head(GroupId target)
        {
            return NodePlacement(target, kMethcla_NodePlacementHeadOfGroup);
        }

        static NodePlacement tail(GroupId target)
        {
            return NodePlacement(target, kMethcla_NodePlacementTailOfGroup);
        }

        static NodePlacement before(NodeId target)
        {
            return NodePlacement(target, kMethcla_NodePlacementBeforeNode);
        }

        static NodePlacement after(NodeId target)
        {
            return NodePlacement(target, kMethcla_NodePlacementAfterNode);
        }
    };

    enum BusMappingFlags
    {
        kBusMappingInternal = kMethcla_BusMappingInternal,
        kBusMappingExternal = kMethcla_BusMappingExternal,
        kBusMappingFeedback = kMethcla_BusMappingFeedback,
        kBusMappingReplace  = kMethcla_BusMappingReplace
    };

    enum NodeDoneFlags
    {
        kNodeDoneDoNothing       = kMethcla_NodeDoneDoNothing
      , kNodeDoneFreeSelf        = kMethcla_NodeDoneFreeSelf
      , kNodeDoneFreePreceeding  = kMethcla_NodeDoneFreePreceeding
      , kNodeDoneFreeFollowing   = kMethcla_NodeDoneFreeFollowing
      , kNodeDoneFreeAllSiblings = kMethcla_NodeDoneFreeAllSiblings
      , kNodeDoneFreeParent      = kMethcla_NodeDoneFreeParent
    };

    struct NodeTreeStatistics
    {
        size_t numGroups;
        size_t numSynths;
    };

    template <typename T> class ResourceIdAllocator
    {
    public:
        ResourceIdAllocator(T minValue, size_t n)
            : m_offset(minValue)
            , m_bits(n)
            , m_pos(0)
        { }

        T alloc()
        {
            for (size_t i=m_pos; i < m_bits.size(); i++) {
                if (!m_bits[i]) {
                    m_bits[i] = true;
                    m_pos = (i+1) == m_bits.size() ? 0 : i+1;
                    return T(m_offset + i);
                }
            }
            for (size_t i=0; i < m_pos; i++) {
                if (!m_bits[i]) {
                    m_bits[i] = true;
                    m_pos = i+1;
                    return T(m_offset + i);
                }
            }
            throw std::runtime_error("No free ids");
        }

        void free(T id)
        {
            T i = id - m_offset;
            if ((i >= 0) && (i < (T)m_bits.size()) && m_bits[i]) {
                m_bits[i] = false;
#if 0 // Don't throw exception for now
            } else {
                throw std::runtime_error("Invalid id");
#endif
            }
        }

    private:
        T                 m_offset;
        std::vector<bool> m_bits;
        size_t            m_pos;
    };

    class PacketPool
    {
    public:
        PacketPool(const PacketPool&) = delete;
        PacketPool& operator=(const PacketPool&) = delete;

        PacketPool(size_t packetSize)
            : m_packetSize(packetSize)
        { }
        ~PacketPool()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            while (!m_freeList.empty()) {
                void* ptr = m_freeList.front();
                delete [] (char*)ptr;
                m_freeList.pop_front();
            }
        }

        size_t packetSize() const
        {
            return m_packetSize;
        }

        void* alloc()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_freeList.empty())
                return new char[m_packetSize];
            void* result = m_freeList.back();
            m_freeList.pop_back();
            return result;
        }

        void free(void* ptr)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_freeList.push_back(ptr);
        }

    private:
        size_t m_packetSize;
        // TODO: Use boost::lockfree::queue for free list
        std::list<void*> m_freeList;
        std::mutex m_mutex;
    };

    class Packet
    {
    public:
        Packet(PacketPool& pool)
            : m_pool(pool)
            , m_packet(pool.alloc(), pool.packetSize())
        { }
        ~Packet()
        {
            m_pool.free(m_packet.data());
        }

        Packet(const Packet&) = delete;
        Packet& operator=(const Packet&) = delete;

        const OSCPP::Client::Packet& packet() const
        {
            return m_packet;
        }

        OSCPP::Client::Packet& packet()
        {
            return m_packet;
        }

    private:
        PacketPool&             m_pool;
        OSCPP::Client::Packet   m_packet;
    };

#if 1
    namespace detail
    {
        class Result
        {
        public:
            Result()
                : m_cond(false)
                , m_error(kMethcla_NoError)
            { }
            Result(const Result&) = delete;
            Result& operator=(const Result&) = delete;

            inline static void checkResponse(const char* requestAddress, const OSCPP::Server::Message& msg, Result& result)
            {
                if (msg == "/error")
                {
                    auto args(msg.args());
                    Methcla_Error errorCode = Methcla_Error(args.int32());
                    const char* errorMessage = args.string();
                    result.setError(errorCode, errorMessage);
                }
                else if (msg != requestAddress)
                {
                    std::stringstream s;
                    s << "Unexpected response message address " << msg.address() << " (expected " << requestAddress << ")";
                    result.setError(kMethcla_LogicError, s.str().c_str());
                }
            }

        protected:
            inline void notify()
            {
                m_cond = true;
                m_cond_var.notify_one();
            }

            inline void wait()
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                while (!m_cond) {
                    m_cond_var.wait(lock);
                }
                if (m_error != kMethcla_NoError) {
                    throwError(m_error, m_errorMessage.c_str());
                }
            }

            void setError(Methcla_Error error, const char* message)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_cond)
                {
                    m_error = kMethcla_LogicError;
                    m_errorMessage = "Result error already set";
                }
                else
                {
                    m_error = error;
                    m_errorMessage = message;
                }
                notify();
            }

            std::mutex              m_mutex;
            std::condition_variable m_cond_var;
            bool                    m_cond;
            Methcla_Error           m_error;
            std::string             m_errorMessage;
        };
    };

    template <class T> class Result : public detail::Result
    {
    public:
        void set(Methcla_Error error, const char* message)
        {
            detail::Result::setError(error, message);
        }

        void set(const T& value)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_error == kMethcla_NoError)
            {
                if (m_cond)
                {
                    m_error = kMethcla_LogicError;
                    m_errorMessage = "Result already set";
                }
                else
                {
                    m_value = value;
                    notify();
                }
            }
        }

        const T& get()
        {
            wait();
            return m_value;
        }

    private:
        T m_value;
    };

    template <> class Result<void> : public detail::Result
    {
    public:
        void set(Methcla_Error error, const char* message)
        {
            detail::Result::setError(error, message);
        }

        void set()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_error == kMethcla_NoError)
            {
                if (m_cond)
                {
                    m_error = kMethcla_LogicError;
                    m_errorMessage = "Result already set";
                }
                else
                {
                    notify();
                }
            }
        }

        void get()
        {
            wait();
        }
    };
#endif

    class Value
    {
    public:
        enum Type
        {
            kInt,
            kFloat,
            kString
        };

        explicit Value(int x)
            : m_type(kInt)
            , m_int(x)
        { }
        explicit Value(float x)
            : m_type(kFloat)
            , m_float(x)
        { }
        Value(const std::string& x)
            : m_type(kString)
            , m_string(x)
        { }

        explicit Value(bool x)
            : m_type(kInt)
            , m_int(x)
        { }

        void put(OSCPP::Client::Packet& packet) const
        {
            switch (m_type) {
                case kInt:
                    packet.int32(m_int);
                    break;
                case kFloat:
                    packet.float32(m_float);
                    break;
                case kString:
                    packet.string(m_string.c_str());
                    break;
            }
        }

    private:
        Type m_type;
        int m_int;
        float m_float;
        std::string m_string;
    };

    class Option
    {
    public:
        virtual ~Option() { }
        virtual void put(OSCPP::Client::Packet& packet) const = 0;

        inline static std::shared_ptr<Option> pluginLibrary(Methcla_LibraryFunction f);
        inline static std::shared_ptr<Option> driverBufferSize(int32_t bufferSize);
    };

    class ValueOption : public Option
    {
    public:
        ValueOption(const char* key, Value value)
            : m_key(key)
            , m_value(value)
        { }

        virtual void put(OSCPP::Client::Packet& packet) const override
        {
            packet.openMessage(m_key.c_str(), 1);
            m_value.put(packet);
            packet.closeMessage();
        }

    private:
        std::string m_key;
        Value       m_value;
    };

    template <typename T> class BlobOption : public Option
    {
        static_assert(std::is_pod<T>::value, "Value type must be POD");

    public:
        BlobOption(const char* key, const T& value)
            : m_key(key)
            , m_value(value)
        { }

        virtual void put(OSCPP::Client::Packet& packet) const override
        {
            packet
                .openMessage(m_key.c_str(), 1)
                .blob(OSCPP::Blob(&m_value, sizeof(m_value)))
                .closeMessage();
        }

    private:
        std::string m_key;
        T m_value;
    };

    std::shared_ptr<Option> Option::pluginLibrary(Methcla_LibraryFunction f)
    {
        return std::make_shared<BlobOption<Methcla_LibraryFunction>>("/engine/option/plugin-library", f);
    }

    std::shared_ptr<Option> Option::driverBufferSize(int32_t bufferSize)
    {
        return std::make_shared<ValueOption>("/engine/option/driver/buffer-size", Value(bufferSize));
    }

    typedef std::vector<std::shared_ptr<Option>> Options;

    static const Methcla_Time immediately = 0.;

    typedef ResourceIdAllocator<int32_t> NodeIdAllocator;

    class EngineInterface
    {
    public:
        virtual ~EngineInterface() { }

        GroupId root() const
        {
            return GroupId(0);
        }

        virtual NodeIdAllocator& nodeIdAllocator() = 0;

        virtual std::unique_ptr<Packet> allocPacket() = 0;
        virtual void sendPacket(const std::unique_ptr<Packet>& packet) = 0;
    };

    class Request
    {
        struct Flags
        {
            bool isMessage : 1;
            bool isBundle : 1;
            bool isClosed : 1;
        };

        EngineInterface*        m_engine;
        std::unique_ptr<Packet> m_packet;
        size_t                  m_bundleCount;
        Flags                   m_flags;

    private:
        void beginMessage()
        {
            if (m_flags.isMessage)
                throw std::runtime_error("Cannot add more than one message to non-bundle packet");
            else if (m_flags.isBundle && m_flags.isClosed)
                throw std::runtime_error("Cannot add message to closed top-level bundle");
            else if (!m_flags.isBundle)
                m_flags.isMessage = true;
        }

        OSCPP::Client::Packet& oscPacket()
        {
            return m_packet->packet();
        }

    public:
        Request(EngineInterface* engine)
            : m_engine(engine)
            , m_packet(engine->allocPacket())
            , m_bundleCount(0)
        {
            m_flags.isMessage = false;
            m_flags.isBundle = false;
            m_flags.isClosed = false;
        }

        Request(EngineInterface& engine)
            : Request(&engine)
        { }

        Request(const Request&) = delete;
        Request& operator=(const Request&) = delete;

        //* Return size of request packet in bytes.
        size_t size() const
        {
            return m_packet->packet().size();
        }

        void openBundle(Methcla_Time time=immediately)
        {
            if (m_flags.isMessage)
            {
                throw std::runtime_error("Cannot open bundle within message packet");
            }
            else
            {
                m_flags.isBundle = true;
                m_bundleCount++;
                oscPacket().openBundle(methcla_time_to_uint64(time));
            }
        }

        // Close nested bundle
        void closeBundle()
        {
            if (m_flags.isMessage)
            {
                throw std::runtime_error("closeBundle called on a message request");
            }
            else if (m_bundleCount == 0)
            {
                throw std::runtime_error("closeBundle without matching openBundle");
            }
            else
            {
                oscPacket().closeBundle();
                m_bundleCount--;
                if (m_bundleCount == 0)
                    m_flags.isClosed = true;
            }
        }

        void bundle(Methcla_Time time, std::function<void(Request&)> func)
        {
            openBundle(time);
            func(*this);
            closeBundle();
        }

        //* Finalize request and send to the engine.
        void send()
        {
            if (m_flags.isBundle && m_bundleCount > 0)
                throw std::runtime_error("openBundle without matching closeBundle");
            m_engine->sendPacket(m_packet);
        }

        GroupId group(const NodePlacement& placement)
        {
            beginMessage();

            const int32_t nodeId = m_engine->nodeIdAllocator().alloc();

            oscPacket()
                .openMessage("/group/new", 3)
                    .int32(nodeId)
                    .int32(placement.target().id())
                    .int32(placement.placement())
                .closeMessage();

            return GroupId(nodeId);
        }

        void freeAll(GroupId group)
        {
            beginMessage();

            oscPacket()
                .openMessage("/group/freeAll", 1)
                    .int32(group.id())
                .closeMessage();
        }

        SynthId synth(const char* synthDef, const NodePlacement& placement, const std::vector<float>& controls, const std::list<Value>& options=std::list<Value>())
        {
            beginMessage();

            const int32_t nodeId = m_engine->nodeIdAllocator().alloc();

            oscPacket()
                .openMessage("/synth/new", 4 + OSCPP::Tags::array(controls.size()) + OSCPP::Tags::array(options.size()))
                    .string(synthDef)
                    .int32(nodeId)
                    .int32(placement.target().id())
                    .int32(placement.placement())
                    .putArray(controls.begin(), controls.end());

                    oscPacket().openArray();
                        for (const auto& x : options) {
                            x.put(oscPacket());
                        }
                    oscPacket().closeArray();

                oscPacket().closeMessage();

            return SynthId(nodeId);
        }

        void activate(SynthId synth)
        {
            beginMessage();

            oscPacket()
                .openMessage("/synth/activate", 1)
                .int32(synth.id())
                .closeMessage();
        }

        void mapInput(SynthId synth, size_t index, AudioBusId bus, BusMappingFlags flags=kBusMappingInternal)
        {
            beginMessage();

            oscPacket()
                .openMessage("/synth/map/input", 4)
                    .int32(synth.id())
                    .int32(index)
                    .int32(bus.id())
                    .int32(flags)
                .closeMessage();
        }

        void mapOutput(SynthId synth, size_t index, AudioBusId bus, BusMappingFlags flags=kBusMappingInternal)
        {
            beginMessage();

            oscPacket()
                .openMessage("/synth/map/output", 4)
                    .int32(synth.id())
                    .int32(index)
                    .int32(bus.id())
                    .int32(flags)
                .closeMessage();
        }

        void set(NodeId node, size_t index, double value)
        {
            beginMessage();

            oscPacket()
                .openMessage("/node/set", 3)
                    .int32(node.id())
                    .int32(index)
                    .float32(value)
                .closeMessage();
        }

        void free(NodeId node)
        {
            beginMessage();

            oscPacket()
                .openMessage("/node/free", 1)
                .int32(node.id())
                .closeMessage();
            m_engine->nodeIdAllocator().free(node.id());
        }

        void whenDone(SynthId synth, NodeDoneFlags flags)
        {
            beginMessage();

            oscPacket()
                .openMessage("/synth/property/doneFlags/set", 2)
                    .int32(synth.id())
                    .int32(flags)
                .closeMessage();
        }
    };

    class Engine : public EngineInterface
    {
    public:
        Engine(const Options& options={})
            : m_nodeIds(1, 1023) // FIXME: Get max number of nodes from options
            , m_requestId(kMethcla_Notification+1)
            , m_packets(8192)
        {
            OSCPP::Client::DynamicPacket bundle(8192);
            bundle.openBundle(1);
            for (auto option : options) {
                option->put(bundle);
            }
            bundle.closeBundle();
            const Methcla_OSCPacket packet = { .data = bundle.data(), .size = bundle.size() };
            detail::checkReturnCode(methcla_engine_new(handlePacket, this, &packet, &m_engine));
        }
        ~Engine()
        {
            methcla_engine_free(m_engine);
        }

        operator const Methcla_Engine* () const
        {
            return m_engine;
        }

        operator Methcla_Engine* ()
        {
            return m_engine;
        }

        void start()
        {
            detail::checkReturnCode(methcla_engine_start(m_engine));
        }

        void stop()
        {
            detail::checkReturnCode(methcla_engine_stop(m_engine));
        }

        Methcla_Time currentTime()
        {
            return methcla_engine_current_time(m_engine);
        }

        void setLogFlags(Methcla_EngineLogFlags flags)
        {
            methcla_engine_set_log_flags(m_engine, flags);
        }

        void bundle(Methcla_Time time, std::function<void(Request&)> func)
        {
            Request request(this);
            request.bundle(time, func);
            request.send();
        }

        GroupId group(const NodePlacement& placement)
        {
            Request request(this);
            GroupId result = request.group(placement);
            request.send();
            return result;
        }

        void freeAll(GroupId group)
        {
            Request request(this);
            request.freeAll(group);
            request.send();
        }

        SynthId synth(const char* synthDef, const NodePlacement& placement, const std::vector<float>& controls, const std::list<Value>& options=std::list<Value>())
        {
            Request request(this);
            SynthId result = request.synth(synthDef, placement, controls, options);
            request.send();
            return result;
        }

        void activate(SynthId synth)
        {
            Request request(this);
            request.activate(synth);
            request.send();
        }

        void mapInput(SynthId synth, size_t index, AudioBusId bus, BusMappingFlags flags=kBusMappingInternal)
        {
            Request request(this);
            request.mapInput(synth, index, bus, flags);
            request.send();
        }

        void mapOutput(SynthId synth, size_t index, AudioBusId bus, BusMappingFlags flags=kBusMappingInternal)
        {
            Request request(this);
            request.mapOutput(synth, index, bus, flags);
            request.send();
        }

        void set(NodeId node, size_t index, double value)
        {
            Request request(this);
            request.set(node, index, value);
            request.send();
        }

        void free(NodeId node)
        {
            Request request(this);
            request.free(node);
            request.send();
        }

        NodeTreeStatistics getNodeTreeStatistics()
        {
            std::unique_ptr<Packet> packet = allocPacket();
            Methcla_RequestId requestId = getRequestId();
            packet->packet()
                .openMessage("/node/tree/statistics", 1)
                .int32(requestId)
                .closeMessage();
            Result<NodeTreeStatistics> result;
            withRequest(requestId, packet->packet(), [&result](Methcla_RequestId, const OSCPP::Server::Message& response){
                detail::Result::checkResponse("/node/tree/statistics", response, result);
                OSCPP::Server::ArgStream args(response.args());
                NodeTreeStatistics value;
                value.numGroups = args.int32();
                value.numSynths = args.int32();
                result.set(value);
            });
            return result.get();
        }

        NodeIdAllocator& nodeIdAllocator() override
        {
            return m_nodeIds;
        }

        std::unique_ptr<Packet> allocPacket() override
        {
            return std::unique_ptr<Packet>(new Packet(m_packets));
        }

        void sendPacket(const std::unique_ptr<Packet>& packet) override
        {
            send(*packet);
        }

    private:
        static void handlePacket(void* data, Methcla_RequestId requestId, const void* packet, size_t size)
        {
            if (requestId == kMethcla_Notification)
                static_cast<Engine*>(data)->handleNotification(packet, size);
            else
                static_cast<Engine*>(data)->handleReply(requestId, packet, size);
        }

        void handleNotification(const void* /* packet */, size_t /* size */)
        {
        }

        void handleReply(Methcla_RequestId requestId, const void* packet, size_t size)
        {
            OSCPP::Server::Packet response(packet, size);
            OSCPP::Server::Message message(response);
            std::lock_guard<std::mutex> lock(m_callbacksMutex);
            // look up request id and invoke callback
            auto it = m_callbacks.find(requestId);
            if (it != m_callbacks.end()) {
                try {
                    it->second(requestId, message);
                    m_callbacks.erase(it);
                } catch (...) {
                    m_callbacks.erase(it);
                    throw;
                }
            }
        }

        void send(const void* packet, size_t size)
        {
            detail::checkReturnCode(methcla_engine_send(m_engine, packet, size));
        }

        void send(const OSCPP::Client::Packet& packet)
        {
            // dumpRequest(std::cout, packet);
            send(packet.data(), packet.size());
        }

        void send(const Packet& packet)
        {
            send(packet.packet());
        }

        Methcla_RequestId getRequestId()
        {
            std::lock_guard<std::mutex> lock(m_requestIdMutex);
            Methcla_RequestId result = m_requestId;
            if (result == kMethcla_Notification) {
                result++;
            }
            m_requestId = result + 1;
            return result;
        }

        void registerResponse(Methcla_RequestId requestId, std::function<void (Methcla_RequestId, const OSCPP::Server::Message&)> callback)
        {
            std::lock_guard<std::mutex> lock(m_callbacksMutex);
            if (m_callbacks.find(requestId) != m_callbacks.end()) {
                throw std::logic_error("Duplicate request id");
            }
            m_callbacks[requestId] = callback;
        }

        void withRequest(Methcla_RequestId requestId, const OSCPP::Client::Packet& request, std::function<void (Methcla_RequestId, const OSCPP::Server::Message&)> callback)
        {
            registerResponse(requestId, callback);
            send(request);
        }

        void execRequest(const char* requestAddress, Methcla_RequestId requestId, const OSCPP::Client::Packet& request)
        {
            Result<void> result;
            withRequest(requestId, request, [requestAddress,&result](Methcla_RequestId, const OSCPP::Server::Message& response){
                detail::Result::checkResponse(requestAddress, response, result);
                result.set();
            });
            result.get();
        }

    private:
        Methcla_Engine*     m_engine;
        ResourceIdAllocator<int32_t> m_nodeIds;
        Methcla_RequestId   m_requestId;
        std::mutex          m_requestIdMutex;
        std::unordered_map<Methcla_RequestId,std::function<void(Methcla_RequestId,const OSCPP::Server::Message&)>> m_callbacks;
        std::mutex          m_callbacksMutex;
        PacketPool          m_packets;
    };
};

#endif // METHCLA_ENGINE_HPP_INCLUDED
