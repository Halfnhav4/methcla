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

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#include <lv2/methc.la/ext/rt-instantiate/rt-instantiate.h>
#include <lv2/methc.la/plugins/scope.lv2/scope.hpp>

#define SCOPE_URI "http://methc.la/lv2/plugins/sine"

using namespace Methcla::LV2;

typedef enum {
    SCOPE_INPUT = 0
  , SCOPE_OUTPUT = 1
} PortIndex;

typedef struct {
    float* input;
    LV2_Atom_Port_Buffer* output;
} Scope;

static uint32_t instance_size(const LV2_Descriptor* descriptor)
{
    return sizeof(Scope);
}

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            void*                     location,
            double                    rate,
            const char*               bundle_path,
            const LV2_Feature* const* features)
{
    Scope* scope = new (location) Scope();
    return (LV2_Handle)scope;
}

static void
activate(LV2_Handle instance)
{
}

static void
connect_port(LV2_Handle instance,
             uint32_t   port,
             void*      data)
{
    Scope* scope = (Scope*)instance;

    switch ((PortIndex)port) {
    case SCOPE_INPUT:
        scope->input = (float*)data;
        break;
    case SCOPE_OUTPUT:
        scope->output = (LV2_Atom_Port_Buffer*)data;
        break;
    }
}

static void
run(LV2_Handle instance, uint32_t numFrames)
{
    Scope* self = (Scope*)instance;

    float* const input = self->input;
    (LV2_Atom_Sequence*)self->output->data

    for (uint32_t k = 0; k < numFrames; k++) {
        output[k] = sinf(phase);
        phase += phaseInc;
    }
    
    sine->phase = phase;
}

static void
deactivate(LV2_Handle instance)
{
}

static void
cleanup(LV2_Handle instance)
{
    free(instance);
}

static const LV2_RT_Instantiate_Interface rtiInterface = {
   instance_size
 , instantiate
};

const void*
extension_data(const char* uri)
{
    if (strcmp(uri, LV2_RT_INSTANTIATE__INTERFACE) == 0) {
        return &rtiInterface;
    }
    return NULL;
}

static const LV2_Descriptor descriptor = {
    SINE_URI,
    lv2_rt_instantiate_default_instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
methcla_scope_lv2_descriptor(uint32_t index)
{
    switch (index) {
    case 0:
        return &descriptor;
    default:
        return NULL;
    }
}
