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

#ifndef METHCLA_AUDIO_NODE_HPP_INCLUDED
#define METHCLA_AUDIO_NODE_HPP_INCLUDED

#include "Methcla/Audio/Resource.hpp"
#include "Methcla/Memory/Manager.hpp"

#include <boost/serialization/strong_typedef.hpp>
#include <cstdint>

namespace Methcla { namespace Audio {

    BOOST_STRONG_TYPEDEF(uint32_t, NodeId);
    // const NodeId InvalidNodeId = -1;

    class Environment;
    class Group;

    class Node : public Resource<NodeId>
    {
    public:
        Node(const Node&) = delete;
        Node& operator=(const Node&) = delete;

        enum AddAction
        {
            kAddToHead
          , kAddToTail
          // , kAddBefore
          // , kAddAfter
          // , kReplace
        };

        //* Return true if this node is a group.
        virtual bool isGroup() const { return false; }
        //* Return true if this node is a synth.
        virtual bool isSynth() const { return false; }

        //* Return the node's parent group or nullptr if it's the root node.
        const Group* parent() const { return m_parent; }
        Group* parent() { return m_parent; }

        // Process a number of frames.
        void process(size_t numFrames);

        // Remove node from parent.
        void unlink();

    protected:
        Node(Environment& env, NodeId nodeId);
        ~Node();

        //* Free a node.
        virtual void free() override;

        virtual void doProcess(size_t numFrames);

    private:
        friend class Group;

        Group*  m_parent;
        Node*   m_prev;
        Node*   m_next;
    };
} }

#endif // METHCLA_AUDIO_NODE_HPP_INCLUDED
