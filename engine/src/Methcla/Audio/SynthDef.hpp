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

#ifndef METHCLA_AUDIO_SYNTHDEF_HPP_INCLUDED
#define METHCLA_AUDIO_SYNTHDEF_HPP_INCLUDED

#include <methcla/engine.h>
#include <methcla/plugin.h>

#include "Methcla/Plugin/Loader.hpp"
#include "Methcla/Utility/Hash.hpp"

#include <boost/utility.hpp>
#include <cstring>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Methcla { namespace Audio {

class Port
{
public:
    Port(Methcla_Port port, uint32_t index, const char* symbol="")
        : m_port(port)
        , m_index(index)
        , m_symbol(symbol)
    { }

    Methcla_PortType type() const { return m_port.type; }
    Methcla_PortDirection direction() const { return m_port.direction; }
    uint32_t index() const { return m_index; }
    const char* symbol() const { return m_symbol.c_str(); }

private:
    Methcla_Port    m_port;
    uint32_t        m_index;
    std::string     m_symbol;
};

class SynthDef : boost::noncopyable
{
public:
    SynthDef(const Methcla_SynthDef* def);
    ~SynthDef();

    inline const char* uri() const { return m_descriptor->uri; }

    inline size_t instanceSize () const { return m_descriptor->instance_size; }

    inline size_t numPorts() const { return m_ports.size(); }
    inline const Port& port(size_t i) const { return m_ports.at(i); }

    inline size_t numAudioInputs    () const { return m_numAudioInputs;    }
    inline size_t numAudioOutputs   () const { return m_numAudioOutputs;   }
    inline size_t numControlInputs  () const { return m_numControlInputs;  }
    inline size_t numControlOutputs () const { return m_numControlOutputs; }

    inline Methcla_Synth* construct(const Methcla_World* world, Methcla_Synth* synth) const
    {
        m_descriptor->construct(m_descriptor, world, synth);
        return synth;
    }

    inline void connect(Methcla_Synth* synth, uint32_t port, void* data) const
    {
        m_descriptor->connect(synth, port, data);
    }

    inline void activate(const Methcla_World* world, Methcla_Synth* synth) const
    {
        if (m_descriptor->activate) m_descriptor->activate(world, synth);
    }

    inline void process(Methcla_Synth* synth, size_t numFrames) const
    {
        m_descriptor->process(synth, numFrames);
    }

    inline void destroy(const Methcla_World* world, Methcla_Synth* synth) const
    {
        if (m_descriptor->destroy) m_descriptor->destroy(world, synth);
    }

private:
    const Methcla_SynthDef* m_descriptor;
    std::vector<Port>       m_ports;
    size_t                  m_numAudioInputs;
    size_t                  m_numAudioOutputs;
    size_t                  m_numControlInputs;
    size_t                  m_numControlOutputs;
};

typedef std::unordered_map<const char*,
                           std::shared_ptr<SynthDef>,
                           Utility::Hash::cstr_hash,
                           Utility::Hash::cstr_equal>
        SynthDefMap;

//* Plugin library.
class PluginLibrary : boost::noncopyable
{
public:
    PluginLibrary(const Methcla_Library* lib, std::shared_ptr<Methcla::Plugin::Library> plugin=nullptr);
    ~PluginLibrary();

private:
    const Methcla_Library*                      m_lib;
    std::shared_ptr<Methcla::Plugin::Library>   m_plugin;
};

class PluginManager : boost::noncopyable
{
public:
    //* Load plugins from static functions.
    void loadPlugins(const Methcla_Host* host, const std::list<Methcla_LibraryFunction>& funcs);

    //* Load plugins from directory.
    void loadPlugins(const Methcla_Host* host, const std::string& directory);

private:
    typedef std::list<std::shared_ptr<PluginLibrary>>
            Libraries;
    Libraries m_libs;
};

}; };

#endif // METHCLA_AUDIO_SYNTHDEF_HPP_INCLUDED
