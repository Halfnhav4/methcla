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

#include <methcla/engine.hpp>
#include "lv2/methc.la/plugins/sine.lv2/sine.h"

Methcla::Engine* makeEngine()
{
    NSString* resources = [[NSBundle mainBundle] resourcePath];
    NSString* bundles = [resources stringByAppendingPathComponent:@"lv2/bundles"];

    const char* pluginPath = [bundles UTF8String];
    Methcla_PluginLibrary pluginLibs[] = { METHCLA_PLUGINS_SINE_LIB, METHCLA_END_PLUGIN_LIBRARIES };

    Methcla_Option options[] = {
        { METHCLA_OPTION__PLUGIN_PATH, pluginPath },
        { METHCLA_OPTION__PLUGIN_LIBRARIES, pluginLibs },
          METHCLA_END_OPTIONS };

    Methcla::Engine* enginePtr = new Methcla::Engine(options);
    Methcla::Engine& engine = *enginePtr;

    engine.start();

//    Methcla::Audio::Engine* engine = static_cast<Methcla::Audio::Engine*>(theEngine->impl());

//    const std::shared_ptr<Methcla::Audio::Plugin> def = engine->env().plugins().lookup(
//        engine->env().mapUri(METHCLA_LV2_URI "/plugins/sine") );
//    Methcla::Audio::Synth* synth = Methcla::Audio::Synth::construct(
//        engine->env(), engine->env().rootNode(), Methcla::Audio::Node::kAddToTail, *def);

    Methcla::SynthId synth1 = engine.synth(METHCLA_PLUGINS_SINE_URI);
    engine.mapOutput(synth1, 0, Methcla::AudioBusId(2));
    std::cout << "Synth " << synth1 << " started" << std::endl;

    Methcla::SynthId synth2 = engine.synth(METHCLA_PLUGINS_SINE_URI);
    engine.mapOutput(synth2, 0, Methcla::AudioBusId(2));
    std::cout << "Synth " << synth2 << " started" << std::endl;

    return enginePtr;
}

#endif
