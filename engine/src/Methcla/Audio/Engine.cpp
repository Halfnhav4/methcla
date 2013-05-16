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

#include "Methcla/Audio/Engine.hpp"
#include "Methcla/Audio/Group.hpp"
#include "Methcla/Audio/Synth.hpp"

#include <boost/current_function.hpp>
#include <cstdlib>
#include <iostream>
#include <oscpp/print.hpp>

using namespace Methcla;
using namespace Methcla::Audio;
using namespace Methcla::Memory;

static void methclaHostRegisterSynthDef(const Methcla_Host* host, const Methcla_SynthDef* synthDef)
{
    return static_cast<Environment*>(host->handle)->registerSynthDef(synthDef);
}

static const Methcla_SoundFileAPI* methclaHostSoundFileAPI(const Methcla_Host* host, const char* mimeType)
{
    return static_cast<Environment*>(host->handle)->soundFileAPI(mimeType);
}

static double methclaWorldSampleRate(const Methcla_World* world)
{
    return static_cast<Environment*>(world->handle)->sampleRate();
}

static void* methclaWorldAlloc(const Methcla_World* world, size_t size)
{
    return static_cast<Environment*>(world->handle)->rtMem().alloc(size);
}

static void* methclaWorldAllocAligned(const Methcla_World* world, size_t alignment, size_t size)
{
    return static_cast<Environment*>(world->handle)->rtMem().allocAligned(alignment, size);
}

static void methclaWorldFree(const Methcla_World* world, void* ptr)
{
    return static_cast<Environment*>(world->handle)->rtMem().free(ptr);
}

static void methcla_api_world_resource_retain(const Methcla_World*, Methcla_Resource resource)
{
    static_cast<Reference*>(resource)->retain();
}

static void methcla_api_world_resource_release(const Methcla_World*, Methcla_Resource resource)
{
    static_cast<Reference*>(resource)->release();
}

static Methcla_Resource methcla_api_world_synth_get_resource(const Methcla_World*, Methcla_Synth* synth)
{
    return Synth::asSynth(synth);
}

static Methcla_Synth* methcla_api_host_resource_get_synth(const Methcla_Host*, Methcla_Resource resource)
{
    return static_cast<Synth*>(resource)->asHandle();
}

Environment::Environment(PluginManager& pluginManager, const PacketHandler& handler, const Options& options)
    : m_sampleRate(options.sampleRate)
    , m_blockSize(options.blockSize)
    , m_rtMem(options.realtimeMemorySize)
    , m_plugins(pluginManager)
    , m_listener(handler)
    , m_audioBuses    (options.numHardwareInputChannels+options.numHardwareOutputChannels+options.maxNumAudioBuses)
    , m_freeAudioBuses(options.numHardwareInputChannels+options.numHardwareOutputChannels+options.maxNumAudioBuses)
    , m_nodes(options.maxNumNodes)
    , m_rootNode(Group::construct(*this, nodes().nextId(), nullptr, Node::kAddToTail))
    , m_epoch(0)
    , m_worker(2)
{
    m_nodes.insert(m_rootNode->id(), m_rootNode);

    const Epoch prevEpoch = epoch() - 1;

    m_audioInputChannels.reserve(options.numHardwareInputChannels);
    for (uint32_t i=0; i < options.numHardwareInputChannels; i++) {
        ExternalAudioBus* bus = new ExternalAudioBus(*this, AudioBusId(i), blockSize(), prevEpoch);
        m_audioBuses.insert(bus->id(), bus);
        m_audioInputChannels.push_back(bus);
    }

    m_audioOutputChannels.reserve(options.numHardwareOutputChannels);
    for (uint32_t i=options.numHardwareInputChannels;
         i < options.numHardwareInputChannels+options.numHardwareOutputChannels;
         i++)
    {
        ExternalAudioBus* bus = new ExternalAudioBus(*this, AudioBusId(i), blockSize(), prevEpoch);
        m_audioBuses.insert(bus->id(), bus);
        m_audioOutputChannels.push_back(bus);
    }

    for (uint32_t i=options.numHardwareInputChannels+options.numHardwareOutputChannels;
         i < m_freeAudioBuses.size();
         i++)
    {
        AudioBus* bus = new InternalAudioBus(*this, AudioBusId(i), blockSize(), prevEpoch);
        m_freeAudioBuses.insert(bus->id(), bus);
    }

    // Initialize Methcla_Host interface
    m_host = {
        .handle = this,
        .registerSynthDef = methclaHostRegisterSynthDef,
        .soundFileAPI = methclaHostSoundFileAPI,
        .performCommand = methcla_api_host_perform_command,
        .resource_get_synth = methcla_api_host_resource_get_synth
    };

    // Initialize Methcla_World interface
    m_world = {
        .handle = this,
        .sampleRate = methclaWorldSampleRate,
        .alloc = methclaWorldAlloc,
        .allocAligned = methclaWorldAllocAligned,
        .free = methclaWorldFree,
        .performCommand = methclaWorldPerformCommand,
        .retain = methcla_api_world_resource_retain,
        .release = methcla_api_world_resource_release,
        .synth_get_resource = methcla_api_world_synth_get_resource,
    };
}

Environment::~Environment()
{
}

AudioBus* Environment::audioBus(const AudioBusId& id)
{
    return m_audioBuses.lookup(id).get();
}

AudioBus& Environment::externalAudioOutput(size_t index)
{
    return *m_audioOutputChannels[index];
}

AudioBus& Environment::externalAudioInput(size_t index)
{
    return *m_audioInputChannels[index];
}

static void freePacket(void* packet)
{
    Memory::free(packet);
}

void Environment::send(const void* packet, size_t size)
{
    char* myPacket = Memory::allocAlignedOf<char>(OSC::kAlignment, size);
    memcpy(myPacket, packet, size);
    Request req;
    req.packet = myPacket;
    req.size = size;
    req.free = freePacket;
    m_requests.send(req);
}

void Environment::process(size_t numFrames, sample_t** inputs, sample_t** outputs)
{
    BOOST_ASSERT_MSG( numFrames <= blockSize(), "numFrames exceeds blockSize()" );

    // Process external requests
    processRequests();

    // Process non-realtime commands
    m_worker.perform();

    const size_t numInputs = m_audioInputChannels.size();
    const size_t numOutputs = m_audioOutputChannels.size();

    // Connect input and output buses
    for (size_t i=0; i < numInputs; i++) {
        m_audioInputChannels[i]->setData(inputs[i]);
        m_audioInputChannels[i]->setEpoch(epoch());
    }
    for (size_t i=0; i < numOutputs; i++) {
        m_audioOutputChannels[i]->setData(outputs[i]);
    }

    // Run DSP graph
    m_rootNode->process(numFrames);

    // Zero outputs that haven't been written to
    for (size_t i=0; i < numOutputs; i++) {
        if (m_audioOutputChannels[i]->epoch() != epoch()) {
            memset(outputs[i], 0, numFrames * sizeof(sample_t));
        }
    }

    m_epoch++;
}

void Environment::perform_free(Command& cmd)
{
    cmd.data.free.func(cmd.data.free.ptr);
}

void Environment::perform_response_ack(Command& cmd)
{
    const char address[] = "/ack";
    const size_t numArgs = 1;
    const size_t packetSize = OSC::Size::message(address, numArgs)
                                + OSC::Size::int32();
    OSC::Client::StaticPacket<packetSize> packet;
    // OSC::Client::DynamicPacket packet(kPacketSize);
    packet
        .openMessage(address, numArgs)
            .int32(cmd.data.response.requestId)
        .closeMessage();
    cmd.env->reply(cmd.data.response.requestId, packet);
}

void Environment::perform_response_nodeId(Command& cmd)
{
    const char address[] = "/ack";
    const size_t numArgs = 2;
    const size_t packetSize = OSC::Size::message(address, numArgs)
                                + 2 * OSC::Size::int32();
    OSC::Client::StaticPacket<packetSize> packet;
    // OSC::Client::DynamicPacket packet(kPacketSize);
    packet
        .openMessage(address, numArgs)
            .int32(cmd.data.response.requestId)
            .int32(cmd.data.response.data.nodeId)
        .closeMessage();
    cmd.env->reply(cmd.data.response.requestId, packet);
}

void Environment::perform_response_error(Command& cmd)
{
    const char address[] = "/error";
    const size_t numArgs = 2;
    const size_t packetSize = OSC::Size::message(address, numArgs)
                                + OSC::Size::int32()
                                + OSC::Size::string(sizeof(cmd.data.response.data.error));
    OSC::Client::StaticPacket<packetSize> packet;
    // OSC::Client::DynamicPacket packet(kPacketSize);
    packet
        .openMessage(address, numArgs)
            .int32(cmd.data.response.requestId)
            .string(cmd.data.response.data.error)
        .closeMessage();
    cmd.env->reply(cmd.data.response.requestId, packet);
}

void Environment::perform_response_query_external_inputs(Command& cmd)
{
    Environment* env = cmd.env;
    const char address[] = "/ack";
    const size_t numBuses = env->numExternalAudioInputs();
    const size_t numArgs = 1 + numBuses;
    const size_t packetSize = OSC::Size::message(address, numArgs) + numArgs * OSC::Size::int32();
    OSC::Client::DynamicPacket packet(packetSize);
    packet.openMessage(address, numArgs);
    packet.int32(cmd.data.response.requestId);
    for (size_t i=0; i < numBuses; i++) {
        packet.int32(env->externalAudioInput(i).id());
    }
    packet.closeMessage();
    env->reply(cmd.data.response.requestId, packet);
}

void Environment::perform_response_query_external_outputs(Command& cmd)
{
    Environment* env = cmd.env;
    const char address[] = "/ack";
    const size_t numBuses = env->numExternalAudioOutputs();
    const size_t numArgs = 1 + numBuses;
    const size_t packetSize = OSC::Size::message(address, numArgs) +  numArgs * OSC::Size::int32();
    OSC::Client::DynamicPacket packet(packetSize);
    packet.openMessage(address, numArgs);
    packet.int32(cmd.data.response.requestId);
    for (size_t i=0; i < numBuses; i++) {
        packet.int32(env->externalAudioOutput(i).id());
    }
    packet.closeMessage();
    env->reply(cmd.data.response.requestId, packet);
}

void Environment::replyError(Methcla_RequestId requestId, const char* msg)
{
    Command cmd(this, perform_response_error, requestId);
    strncpy(cmd.data.response.data.error, msg, sizeof(cmd.data.response.data.error));
    sendToWorker(cmd);
}

void Environment::processRequests()
{
    Request msg;
    while (m_requests.next(msg)) {
        try {
            OSC::Server::Packet packet(msg.packet, msg.size);
            if (packet.isBundle()) {
                processBundle(packet);
            } else {
                processMessage(packet);
            }
        } catch (std::exception& e) {
            std::cerr << "Unhandled exception in `processRequests': " << e.what() << std::endl;
        }
        // Free packet in NRT thread
        Command cmd(this, perform_free);
        cmd.data.free.func = msg.free;
        cmd.data.free.ptr  = msg.packet;
        sendToWorker(cmd);
    }
}

void Environment::processMessage(const OSC::Server::Message& msg)
{
    std::cerr << "Request (recv): " << msg << std::endl;

    auto args = msg.args();
    Methcla_RequestId requestId = args.int32();

    try {
        if (msg == "/s_new") {
            const char* defName = args.string();
            NodeId targetId = NodeId(args.int32());
            int32_t addAction = args.int32();

            const std::shared_ptr<SynthDef> def = synthDef(defName);

            auto synthControls = args.atEnd() ? OSC::Server::ArgStream() : args.array();
            // FIXME: Cannot be checked before the synth is instantiated.
            // if (def->numControlInputs() != synthControls.size()) {
            //     throw std::runtime_error("Missing synth control initialisers");
            // }
            auto synthArgs = args.atEnd() ? OSC::Server::ArgStream() : args.array();

            Node* targetNode = m_nodes.lookup(targetId).get();
            Group* targetGroup = targetNode->isGroup() ? dynamic_cast<Group*>(targetNode)
                                                       : dynamic_cast<Synth*>(targetNode)->parent();
            Synth* synth = Synth::construct(
                *this,
                nodes().nextId(),
                targetGroup,
                Node::kAddToTail,
                *def,
                synthControls,
                synthArgs);
            nodes().insert(synth->id(), synth);

            Command cmd(this, perform_response_nodeId, requestId);
            cmd.data.response.data.nodeId = synth->id();
            sendToWorker(cmd);
        } else if (msg == "/g_new") {
            NodeId targetId = NodeId(args.int32());
            int32_t addAction = args.int32();

            Node* targetNode = m_nodes.lookup(targetId).get();
            Group* targetGroup = targetNode->isGroup() ? dynamic_cast<Group*>(targetNode)
                                                       : dynamic_cast<Synth*>(targetNode)->parent();
            Group* group = Group::construct(*this, nodes().nextId(), targetGroup, Node::kAddToTail);
            nodes().insert(group->id(), group);

            Command cmd(this, perform_response_nodeId, requestId);
            cmd.data.response.data.nodeId = group->id();
            sendToWorker(cmd);
        } else if (msg == "/n_free") {
            NodeId nodeId = NodeId(args.int32());
            // Drop reference from node map
            m_nodes.remove(nodeId);

            Command cmd(this, perform_response_nodeId, requestId);
            cmd.data.response.data.nodeId = nodeId;
            sendToWorker(cmd);
        } else if (msg == "/n_set") {
            NodeId nodeId = NodeId(args.int32());
            int32_t index = args.int32();
            float value = args.float32();
            Node* node = m_nodes.lookup(nodeId).get();
            if (!node->isSynth())
                throw std::runtime_error("Node is not a synth");
            Synth* synth = dynamic_cast<Synth*>(node);
            if ((index < 0) || (index >= synth->numControlInputs())) {
                throw std::runtime_error("Control input index out of range");
            }
            synth->controlInput(index) = value;
            sendToWorker(Command(this, perform_response_ack, requestId));
        } else if (msg == "/synth/map/output") {
            NodeId nodeId = NodeId(args.int32());
            int32_t index = args.int32();
            AudioBusId busId = AudioBusId(args.int32());

            Node* node = m_nodes.lookup(nodeId).get();
            // Could traverse all synths in a group
            if (!node->isSynth())
                throw std::runtime_error("Node is not a synth");

            Synth* synth = dynamic_cast<Synth*>(node);
            if ((index < 0) || (index >= synth->numAudioOutputs()))
                throw std::runtime_error("Synth output index out of range");

            synth->mapOutput(index, busId, kOut);

            // Could reply with previous bus mapping
            sendToWorker(Command(this, perform_response_ack, requestId));
        } else if (msg == "/query/external_inputs") {
            sendToWorker(Command(this, perform_response_query_external_inputs, requestId));
        } else if (msg == "/query/external_outputs") {
            sendToWorker(Command(this, perform_response_query_external_outputs, requestId));
        }
    } catch (Exception& e) {
        const std::string* errorInfo = boost::get_error_info<ErrorInfoString>(e);
        const char* errorMessage = errorInfo == nullptr ? "Unknown error" : errorInfo->c_str();
        replyError(requestId, errorMessage);
    } catch (std::exception& e) {
        replyError(requestId, e.what());
    }
}

void Environment::processBundle(const OSC::Server::Bundle& bundle)
{
    // throw std::runtime_error("Bundle support not implemented yet");
    for (auto p : bundle) {
        if (p.isBundle()) {
            processBundle(p);
        } else {
            processMessage(p);
        }
    }
}

void Environment::registerSynthDef(const Methcla_SynthDef* def)
{
    auto synthDef = std::make_shared<SynthDef>(def);
    m_synthDefs[synthDef->uri()] = synthDef;
}

const std::shared_ptr<SynthDef>& Environment::synthDef(const char* uri) const
{
    auto it = m_synthDefs.find(uri);
    if (it == m_synthDefs.end())
        throw std::runtime_error("Synth definition not found");
    return it->second;
}

void Environment::registerSoundFileAPI(const char* mimeType, const Methcla_SoundFileAPI* api)
{
    m_soundFileAPIs.push_back(api);
}

const Methcla_SoundFileAPI* Environment::soundFileAPI(const char* mimeType) const
{
    return m_soundFileAPIs.empty() ? nullptr : m_soundFileAPIs.front();
}

void Environment::perform_worldCommand(Command& cmd)
{
    cmd.data.worldCommand.perform(static_cast<const Methcla_World*>(*cmd.env), cmd.data.worldCommand.data);
}

void Environment::perform_hostCommand(Command& cmd)
{
    cmd.data.hostCommand.perform(static_cast<const Methcla_Host*>(*cmd.env), cmd.data.hostCommand.data);
}

void Environment::methcla_api_host_perform_command(const Methcla_Host* host, Methcla_WorldPerformFunction perform, void* data)
{
    Environment* env = static_cast<Environment*>(host->handle);
    Command cmd(env, perform_worldCommand);
    cmd.data.worldCommand.perform = perform;
    cmd.data.worldCommand.data = data;
    env->sendFromWorker(cmd);
}

void Environment::methclaWorldPerformCommand(const Methcla_World* world, Methcla_HostPerformFunction perform, void* data)
{
    Environment* self = static_cast<Environment*>(world->handle);
    Command cmd(self, perform_hostCommand);
    cmd.data.hostCommand.perform = perform;
    cmd.data.hostCommand.data = data;
    self->sendToWorker(cmd);
}

Engine::Engine(PluginManager& pluginManager, const PacketHandler& handler, const std::string& pluginDirectory)
{
    m_driver = IO::defaultPlatformDriver();
    m_driver->setProcessCallback(processCallback, this);

    Environment::Options options;
    options.sampleRate = m_driver->sampleRate();
    options.blockSize = m_driver->bufferSize();
    options.numHardwareInputChannels = m_driver->numInputs();
    options.numHardwareOutputChannels = m_driver->numOutputs();
    m_env = new Environment(pluginManager, handler, options);

    pluginManager.loadPlugins(*m_env, pluginDirectory);
}

Engine::~Engine()
{
    stop();
    delete m_env;
    delete m_driver;
}

void Engine::start()
{
    m_driver->start();
}

void Engine::stop()
{
    m_driver->stop();
}

void Engine::processCallback(void* data, size_t numFrames, sample_t** inputs, sample_t** outputs)
{
    static_cast<Engine*>(data)->m_env->process(numFrames, inputs, outputs);
}
