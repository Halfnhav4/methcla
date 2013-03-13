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
#include "Methcla/LV2/Atom.hpp"

#include <cstdlib>
#include <iostream>

#include "lv2/lv2plug.in/ns/ext/atom/util.h"

using namespace Methcla;
using namespace Methcla::Audio;
using namespace Methcla::Memory;
using namespace std;

void NodeMap::insert(Node* node)
{
    NodeId id = node->id();
    if (m_nodes[id] != 0)
        BOOST_THROW_EXCEPTION(DuplicateNodeId() << ErrorInfoNodeId(id));
    m_nodes[id] = node;
}

void NodeMap::release(const NodeId& nodeId)
{
    if (m_nodes[nodeId] == 0)
        BOOST_THROW_EXCEPTION(InvalidNodeId() << ErrorInfoNodeId(nodeId));
    m_nodes[nodeId] = 0;
}

Environment::Environment(PluginManager& pluginManager, const Options& options)
    : m_sampleRate(options.sampleRate)
    , m_blockSize(options.blockSize)
    , m_plugins(pluginManager)
    , m_audioBuses    (options.numHardwareInputChannels+options.numHardwareOutputChannels+options.maxNumAudioBuses)
    , m_freeAudioBuses(options.numHardwareInputChannels+options.numHardwareOutputChannels+options.maxNumAudioBuses)
    , m_nodes(options.maxNumNodes)
    , m_rootNode(Group::construct(*this, nullptr, Node::kAddToTail))
    , m_epoch(0)
    , m_worker(uriMap())
    , m_uris(uriMap())
{
    const Epoch prevEpoch = epoch() - 1;

    m_audioInputChannels.reserve(options.numHardwareInputChannels);
    for (uint32_t i=0; i < options.numHardwareInputChannels; i++) {
        ExternalAudioBus* bus = new ExternalAudioBus(*this, AudioBusId(i), blockSize(), prevEpoch);
        m_audioBuses.insert(bus->id(), bus);
        m_audioInputChannels.push_back(bus);
    }

    m_audioOutputChannels.reserve(options.numHardwareOutputChannels);
    for (uint32_t i=0; i < options.numHardwareOutputChannels; i++) {
        ExternalAudioBus* bus = new ExternalAudioBus(*this, AudioBusId(i), blockSize(), prevEpoch);
        m_audioBuses.insert(bus->id(), bus);
        m_audioOutputChannels.push_back(bus);
    }

    for (uint32_t i=options.numHardwareInputChannels+options.numHardwareOutputChannels; i < m_freeAudioBuses.size(); i++) {
        AudioBus* bus = new InternalAudioBus(*this, AudioBusId(i), blockSize(), prevEpoch);
        m_freeAudioBuses.insert(bus->id(), bus);
    }
}

Environment::~Environment()
{
}

AudioBus* Environment::audioBus(const AudioBusId& id)
{
    return m_audioBuses.lookup(id);
}

AudioBus& Environment::externalAudioOutput(size_t index)
{
    return *m_audioOutputChannels[index];
}

AudioBus& Environment::externalAudioInput(size_t index)
{
    return *m_audioInputChannels[index];
}

void Environment::request(MessageQueue::Respond respond, void* data, const LV2_Atom* msg)
{
    m_requests.send(respond, data, msg);
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

//struct ReturnEnvelope
//{
    //MessageQueue::Reply reply;
    //void* data;
    //const LV2_Atom* request;
//};

static void forgeReturnEnvelope(::LV2::Forge& forge, const Uris& uris, const Environment::MessageQueue::Message& msg)
{
    forge.atom(sizeof(msg), uris.atom_Chunk);
    forge.write(&msg, sizeof(msg));
}

static void forgeException(::LV2::Forge& forge, const Uris& uris, const Exception& e)
{
    ::LV2::ObjectFrame frame(forge, 0, uris.patch_Error);
    const std::string* errorInfo = boost::get_error_info<ErrorInfoString>(e);
    const std::string errorMessage = errorInfo == nullptr ? "Unknown error" : *errorInfo;
    forge << ::LV2::Property(uris.methcla_errorMessage)
          << errorMessage;
}

static void sendReply(void* /* data */, const LV2_Atom* payload, Environment::Worker::Writer& /* writer */)
{
    auto tuple = reinterpret_cast<const LV2_Atom_Tuple*>(payload);
    auto iter = lv2_atom_tuple_begin(tuple);
    auto msg = reinterpret_cast<Environment::MessageQueue::Message*>(LV2_ATOM_BODY(iter));
    auto response = lv2_atom_tuple_next(iter);
    msg->respond(response);
}

void Environment::processRequests()
{
    MessageQueue::Message msg;
    while (m_requests.next(msg)) {
        try {
            handleRequest(msg);
        } catch(Exception& e) {
            // TODO: Send Error response
            ::LV2::Forge forge(*prepare(sendReply, nullptr));
            {
                ::LV2::TupleFrame frame(forge);
                forgeReturnEnvelope(frame, uris(), msg);
                forgeException(frame, uris(), e);
            }
            commit();
        }
    }
}

/*
*/
void Environment::handleRequest(MessageQueue::Message& request)
{
    const LV2_Atom* atom = request.payload();
    cout << "Message: " << atom << endl
         << "    atom size: " << atom->size << endl
         << "    atom type: " << atom->type << endl
         << "    atom uri:  " << unmapUri(atom->type) << endl;
    if (uris().isObject(atom))
        handleMessageRequest(request, reinterpret_cast<const LV2_Atom_Object*>(atom));
    else if (atom->type == uris().atom_Sequence)
        handleSequenceRequest(request, reinterpret_cast<const LV2_Atom_Sequence*>(atom));
    else
        BOOST_THROW_EXCEPTION(Exception() << ErrorInfoString("Invalid request type"));
}

void Environment::handleMessageRequest(MessageQueue::Message& request, const LV2_Atom_Object* msg)
{
    // const char* atom_type = unmapUri(msg->atom.type);
    // const char* uri_type = unmapUri(msg->body.otype);
    // if (msg->atom.type == uris().atom_Blank) {
    //     cout << atom_type << " " << msg->body.id << " " << uri_type << endl;
    // } else {
    //     const char* uri_id = unmapUri(msg->body.id);
    //     cout << atom_type << " " << uri_id << " " << uri_type << endl;
    // }
    // LV2_ATOM_OBJECT_FOREACH(msg, prop) {
    //     cout << "  " << unmapUri(prop->key) << " " << prop->context << ": " << unmapUri(prop->value.type) << endl;
    // }
    const LV2_Atom* subjectAtom = nullptr;
    const LV2_Atom* bodyAtom = nullptr;
    const LV2_URID requestType = msg->body.otype;

    int matches = lv2_atom_object_get(
                    msg
                  , uris().patch_subject, &subjectAtom
                  , uris().patch_body, &bodyAtom
                  , nullptr );

    if (subjectAtom == nullptr)
        BOOST_THROW_EXCEPTION( Exception() << ErrorInfoString("Message must have subject property") );

    const LV2_Atom_Object* subject = uris().toObject(subjectAtom);
    if (subject == nullptr)
        BOOST_THROW_EXCEPTION( Exception() << ErrorInfoString("Subject must be an object") );
    const LV2_Atom_Object* body = uris().toObject(bodyAtom);

    if (subject->body.otype == uris().methcla_Node) {
        const LV2_Atom* targetAtom = nullptr;
        lv2_atom_object_get(subject, uris().methcla_id, &targetAtom, nullptr);
        BOOST_ASSERT_MSG( targetAtom != nullptr, "methcla:id property not found" );
        BOOST_ASSERT_MSG( targetAtom->type == uris().atom_Int, "methcla:id must be an Int" );
        NodeId targetId(reinterpret_cast<const LV2_Atom_Int*>(targetAtom)->body);
        Node* targetNode = m_nodes.lookup(targetId);
        BOOST_ASSERT_MSG( targetNode != nullptr, "target node not found" );
        Synth* targetSynth = dynamic_cast<Synth*>(targetNode);
        Group* targetGroup = targetSynth == nullptr ? dynamic_cast<Group*>(targetNode) : targetSynth->parent();

        if (requestType == uris().patch_Insert) {
            // get add target specification

            // get plugin URI
            const LV2_Atom* pluginAtom = nullptr;
            lv2_atom_object_get(body, uris().methcla_plugin, &pluginAtom, nullptr);
            BOOST_ASSERT_MSG( pluginAtom != nullptr, "methcla:plugin property not found" );
            BOOST_ASSERT_MSG( pluginAtom->type == uris().atom_URID, "methcla:plugin property value must be a URID" );

            // get params from body

            // uris().methcla_plugin
            const std::shared_ptr<Plugin> def = plugins().lookup(reinterpret_cast<const LV2_Atom_URID*>(pluginAtom)->body);
            Synth* synth = Synth::construct(*this, targetGroup, Node::kAddToTail, *def);

            // Send reply with synth ID (from NRT thread)
            ::LV2::Forge forge(*prepare(sendReply, nullptr));
            {
                ::LV2::TupleFrame frame(forge);
                forgeReturnEnvelope(frame, uris(), request);
                {
                    ::LV2::ObjectFrame frame(forge, 0, uris().patch_Ack);
                    forge << ::LV2::Property(uris().methcla_id)
                          << (int32_t)synth->id();
                }
            }
            commit();
        } else if (requestType == uris().patch_Delete) {

        } else if (requestType == uris().patch_Set) {

        }
    }
}

void Environment::handleSequenceRequest(MessageQueue::Message& request, const LV2_Atom_Sequence* bdl)
{
    std::cerr << "Sequence requests not supported yet\n";
}


Engine::Engine(PluginManager& pluginManager, const boost::filesystem::path& lv2Directory)
{
    m_driver = IO::defaultPlatformDriver();
    m_driver->setProcessCallback(processCallback, this);

    Environment::Options options;
    options.sampleRate = m_driver->sampleRate();
    options.blockSize = m_driver->bufferSize();
    options.numHardwareInputChannels = m_driver->numInputs();
    options.numHardwareOutputChannels = m_driver->numOutputs();
    m_env = new Environment(pluginManager, options);

    pluginManager.loadPlugins(lv2Directory);
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
