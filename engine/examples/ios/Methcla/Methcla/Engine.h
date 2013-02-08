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

#ifndef MethclaMobile_Engine_h
#define MethclaMobile_Engine_h

#include <Methcla/Audio/Client.hpp>
#include <Methcla/Audio/Engine.hpp>
#include <Methcla/Audio/IO/Client.hpp>
#include <Methcla/Audio/IO/Driver.hpp>
#include <Methcla/Audio/IO/RemoteIODriver.hpp>
#include <Methcla/Audio/Group.hpp>
#include <Methcla/Audio/Synth.hpp>
#include <Methcla/Audio/SynthDef.hpp>
#include <Methcla/Utility/MessageQueue.hpp>

#include <AudioToolbox/AudioServices.h>

#include <boost/filesystem.hpp>
#include <iostream>

#include "lilv/lilv.h"
#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/atom/forge.h"
#include "lv2/lv2plug.in/ns/ext/atom/util.h"

//METHCLA_EXPORT void METHCLA_INIT_FUNC(osc)(MethclaHost*);
//METHCLA_EXPORT void METHCLA_INIT_FUNC(Scope)(MethclaHost*);

class MyLoader : public Methcla::Audio::Plugin::StaticLoader
{
public:
    MyLoader()
        : Methcla::Audio::Plugin::StaticLoader(descriptorFunctions())
    {
//        setenv("LV2_PATH", "/Users/sk/Library/Audio/Plug-Ins/LV2", 1);
    }

    static Methcla::Audio::Plugin::StaticLoader::DescriptorFunctionMap descriptorFunctions()
    {
        Methcla::Audio::Plugin::StaticLoader::DescriptorFunctionMap dfs;
        extern const LV2_Descriptor* methcla_sine_lv2_descriptor(uint32_t index);
        dfs["http://methc.la/lv2/plugins/sine"] = methcla_sine_lv2_descriptor;
        return dfs;
    }
};

class MyEngine : public Methcla::Audio::Engine
{
public:
    MyEngine(MyLoader* loader)
        : Methcla::Audio::Engine(loader, lv2BundleDirectory())
        , m_osc(0)
//        , m_scope(0)
    { }

    static boost::filesystem::path lv2BundleDirectory()
    {
        NSString* resources = [[NSBundle mainBundle] resourcePath];
        NSString* bundles = [resources stringByAppendingPathComponent:@"lv2/bundles"];
        return boost::filesystem::path([bundles UTF8String]);
    }

    virtual void configure(const Methcla::Audio::IO::Driver& driver)
    {
        Methcla::Audio::Engine::configure(driver);

        // Create sine instance
        const Methcla::Audio::Plugin::Manager::PluginHandle& def = env().plugins().lookup(
            env().mapUri("http://methc.la/lv2/plugins/sine") );
        Methcla::Audio::Synth* synth = m_osc = Methcla::Audio::Synth::construct(env(), env().rootNode(), Methcla::Audio::Node::kAddToTail, *def);
        synth->mapOutput(0, env().externalAudioOutput(0).id(), Methcla::Audio::kOut);

//        const Methcla::Audio::SynthDef& scopeDef = environment()->lookupSynthDef("scope");
//        Methcla::Audio::Synth* scope = Methcla::Audio::Synth::construct(*environment(), 2, environment()->rootNode(), scopeDef);
//        environment()->rootNode()->addToTail(*scope);
//        scope->mapInput(0, Methcla::Audio::AudioBusId(Methcla::Audio::AudioBusId::kOutput, 0), Methcla::Audio::kIn);
//        m_scope = scope->synth<Methcla::Audio::ScopeSynth>();
    }

    Methcla::Audio::Synth* osc() { return m_osc; }
//    Methcla::Audio::ScopeSynth* scope() { return m_scope; }

private:
    Methcla::Audio::Synth* m_osc;
//    Methcla::Audio::ScopeSynth* m_scope;
};

#endif
