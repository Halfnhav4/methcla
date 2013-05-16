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

using namespace Methcla::Audio;

Group* Group::construct(Environment& env, NodeId nodeId, Group* target, Node::AddAction addAction)
{
    return new (env.rtMem()) Group(env, nodeId, target, addAction);
}

void Group::free()
{
    if (isRootNode()) {
        BOOST_THROW_EXCEPTION(
            InvalidNodeId()
         << ErrorInfoNodeId(id())
         << ErrorInfoString("cannot free root node")
         );
    } else {
        Node::free();
    }
}

void Group::process(size_t numFrames)
{
    for (Node& node : m_children) { node.process(numFrames); }
}
