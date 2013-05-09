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

#include "Methcla/Audio/Synth.hpp"

using namespace Methcla::Audio;
using namespace Methcla::Memory;

template <class T> T offset_cast(Synth* self, size_t offset)
{
    return reinterpret_cast<T>(reinterpret_cast<char*>(self) + offset);
}

static const Alignment kBufferAlignment = kSIMDAlignment;

Synth::Synth( Environment& env
            , Group* target
            , Node::AddAction addAction
            , const SynthDef& synthDef
            , OSC::Server::ArgStream controls
            , OSC::Server::ArgStream args
            , size_t synthOffset
            , size_t audioInputOffset
            , size_t audioOutputOffset
            , size_t controlBufferOffset
            , size_t audioBufferOffset
            , size_t audioBufferSize
            )
    : Node(env, target, addAction)
    , m_synthDef(synthDef)
{
    const size_t blockSize = env.blockSize();

    m_synth = synthDef.construct(env, offset_cast<void*>(this, synthOffset));
    m_controlBuffers = offset_cast<sample_t*>(this, controlBufferOffset);
    // Align audio buffers
    m_audioBuffers = kBufferAlignment.align(offset_cast<sample_t*>(this, audioBufferOffset));

    // Uninitialized audio connection memory
    AudioInputConnection* audioInputConnections   = offset_cast<AudioInputConnection*>(this, audioInputOffset);
    AudioOutputConnection* audioOutputConnections = offset_cast<AudioOutputConnection*>(this, audioOutputOffset);

    // Audio buffer memory
    sample_t* audioInputBuffers = m_audioBuffers;
    sample_t* audioOutputBuffers = m_audioBuffers + synthDef.numAudioInputs() * blockSize;

    // Connect ports
    for (size_t i=0; i < synthDef.numPorts(); i++) {
        const Port& port = synthDef.port(i);
        switch (port.type()) {
        case kMethcla_ControlPort:
            switch (port.direction()) {
            case kMethcla_Input: {
                // Initialize with control value
                m_controlBuffers[port.index()] = controls.next<float>();
                sample_t* buffer = &m_controlBuffers[port.index()];
                m_synthDef.connect(m_synth, i, buffer);
                };
                break;
            case kMethcla_Output: {
                sample_t* buffer = &m_controlBuffers[numControlInputs() + port.index()];
                m_synthDef.connect(m_synth, i, buffer);
                };
                break;
            };
            break;
        case kMethcla_AudioPort:
            switch (port.direction()) {
            case kMethcla_Input: {
                new (&audioInputConnections[port.index()]) AudioInputConnection(m_audioInputConnections.size());
                m_audioInputConnections.push_back(audioInputConnections[port.index()]);
                sample_t* buffer = audioInputBuffers + port.index() * blockSize;
                BOOST_ASSERT( kBufferAlignment.isAligned(reinterpret_cast<uintptr_t>(buffer)) );
                m_synthDef.connect(m_synth, i, buffer);
                };
                break;
            case kMethcla_Output: {
                new (&audioOutputConnections[port.index()]) AudioOutputConnection(m_audioOutputConnections.size());
                m_audioOutputConnections.push_back(audioOutputConnections[port.index()]);
                sample_t* buffer = audioOutputBuffers + port.index() * blockSize;
                BOOST_ASSERT( kBufferAlignment.isAligned(reinterpret_cast<uintptr_t>(buffer)) );
                m_synthDef.connect(m_synth, i, buffer);
                };
                break;
            };
            break;
        }
    }

    // Check for control input triggers
//    for (size_t i=0; i < numControlInputs(); i++) {
//        if (m_synthDef.controlInputSpec(i).flags & kMethclaControlTrigger) {
//            m_flags.set(kHasTriggerInput);
//            break;
//        }
//    }

    // Activate synth instance
    // This might be deferred to when the synth is actually started by the scheduler
    synthDef.activate(env, m_synth);
}

Synth::~Synth()
{
    m_synthDef.destroy(env(), m_synth);
}

Synth* Synth::construct(Environment& env, Group* target, Node::AddAction addAction, const SynthDef& synthDef, OSC::Server::ArgStream controls, OSC::Server::ArgStream args)
{
    // TODO: This is not really necessary; each buffer could be aligned correctly, with some padding in between buffers.
    BOOST_ASSERT_MSG( kBufferAlignment.isAligned(env.blockSize() * sizeof(sample_t))
                    , "Environment.blockSize must be a multiple of kBufferAlignment" );

    const size_t numControlInputs           = synthDef.numControlInputs();
    const size_t numControlOutputs          = synthDef.numControlOutputs();
    const size_t numAudioInputs             = synthDef.numAudioInputs();
    const size_t numAudioOutputs            = synthDef.numAudioOutputs();
    const size_t blockSize                  = env.blockSize();

    const size_t synthAllocSize             = sizeof(Synth) + synthDef.instanceSize();
    const size_t audioInputOffset           = synthAllocSize;
    const size_t audioInputAllocSize        = numAudioInputs * sizeof(AudioInputConnection);
    const size_t audioOutputOffset          = audioInputOffset + audioInputAllocSize;
    const size_t audioOutputAllocSize       = numAudioOutputs * sizeof(AudioOutputConnection);
    const size_t controlBufferOffset        = audioOutputOffset + audioOutputAllocSize;
    const size_t controlBufferAllocSize     = (numControlInputs + numControlOutputs) * sizeof(sample_t);
    const size_t audioBufferOffset          = controlBufferOffset + controlBufferAllocSize;
    const size_t audioBufferAllocSize       = (numAudioInputs + numAudioOutputs) * blockSize * sizeof(sample_t);
    const size_t allocSize                  = audioBufferOffset + audioBufferAllocSize + kBufferAlignment /* alignment margin */;

    // Instantiate synth
    return new (env.rtMem(), allocSize - sizeof(Synth))
               Synth( env, target, addAction
                    , synthDef, controls, args
                    , sizeof(Synth)
                    , audioInputOffset
                    , audioOutputOffset
                    , controlBufferOffset
                    , audioBufferOffset
                    , audioBufferAllocSize );
}

template <class Connection>
struct IfConnectionIndex
{
    IfConnectionIndex(size_t index)
        : m_index(index)
    { }

    bool operator () (const Connection& conn)
    {
        return conn.index() == m_index;
    }

    size_t m_index;
};

template <class Connection>
struct SortByBusId
{
    bool operator () (const Connection& a, const Connection& b)
    {
        return a.busId() < b.busId();
    }
};

void Synth::mapInput(size_t index, const AudioBusId& busId, InputConnectionType type)
{
    AudioInputConnections::iterator conn =
        find_if( m_audioInputConnections.begin()
               , m_audioInputConnections.end()
               , IfConnectionIndex<AudioInputConnection>(index) );
    if (conn != m_audioInputConnections.end()) {
        if (conn->connect(busId, type)) {
            m_flags.set(kAudioInputConnectionsChanged);
        }
    }
}

void Synth::mapOutput(size_t index, const AudioBusId& busId, OutputConnectionType type)
{
    size_t offset = sampleOffset();
    sample_t* buffer = offset > 0 ? env().rtMem().allocAlignedOf<sample_t>(kBufferAlignment, offset) : 0;
    AudioOutputConnections::iterator conn =
        find_if( m_audioOutputConnections.begin()
               , m_audioOutputConnections.end()
               , IfConnectionIndex<AudioOutputConnection>(index) );
    if (conn != m_audioOutputConnections.end()) {
        conn->release(env());
        if (conn->connect(busId, type, offset, buffer)) {
            m_flags.set(kAudioOutputConnectionsChanged);
        }
    }
}

void Synth::process(size_t numFrames)
{
    // Sort connections by bus id (if necessary)
    if (m_flags.test(kAudioInputConnectionsChanged)) {
        m_audioInputConnections.sort(SortByBusId<AudioInputConnection>());
        m_flags.reset(kAudioInputConnectionsChanged);
    }
    if (m_flags.test(kAudioOutputConnectionsChanged)) {
        m_audioOutputConnections.sort(SortByBusId<AudioOutputConnection>());
        m_flags.reset(kAudioOutputConnectionsChanged);
    }

    Environment& env = this->env();
    const size_t blockSize = env.blockSize();

    sample_t* const inputBuffers = m_audioBuffers;
    for (auto& x : m_audioInputConnections) {
        x.read(env, numFrames, inputBuffers + x.index() * blockSize);
    }

    m_synthDef.process(m_synth, numFrames);

    sample_t* const outputBuffers = m_audioBuffers + m_synthDef.numAudioInputs() * blockSize;
    for (auto& x : m_audioOutputConnections) {
        x.write(env, numFrames, outputBuffers + x.index() * blockSize);
    }

    // Reset triggers
//    if (m_flags.test(kHasTriggerInput)) {
//        for (size_t i=0; i < numControlInputs(); i++) {
//            if (synthDef().controlInputSpec(i).flags & kMethclaControlTrigger) {
//                *controlInput(i) = 0.f;
//            }
//        }
//    }
}
