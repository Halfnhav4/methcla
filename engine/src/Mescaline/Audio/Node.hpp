#ifndef MESCALINE_AUDIO_NODE_HPP_INCLUDED
#define MESCALINE_AUDIO_NODE_HPP_INCLUDED

#include <Mescaline/Memory/Manager.hpp>
#include <boost/cstdint.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/utility.hpp>

namespace Mescaline { namespace Audio {

    typedef int32_t NodeId;
    // const NodeId InvalidNodeId = -1;

    class Environment;
    class Group;

    class Node : public boost::noncopyable
               , public boost::intrusive::list_base_hook<>
               , public Memory::AllocatedBase<Node, Memory::RTMemoryManager>
    {
    public:
        Node(Environment& env, const NodeId& id, Group* parent)
            : m_env(env)
            , m_id(id)
            , m_parent(parent)
        { }
        virtual ~Node();

        /// Return environment.
        const Environment& environment() const { return m_env; }
        Environment& environment() { return m_env; }
        
        /// Return the node's id.
        const NodeId& id() const { return m_id; }

        const Group* parent() const { return m_parent; }
        Group* parent() { return m_parent; }
        bool isRootNode() const { return parent() == 0; }

        /// Free the node.
        virtual void free();

        // Process a number of frames.
        virtual void process(size_t numFrames) = 0;

    protected:
        void operator delete(void* ptr);

    private:
        Environment&    m_env;
        NodeId          m_id;
        Group*          m_parent;
    };

    typedef boost::intrusive::list<Node> NodeList;
}; };

#endif // MESCALINE_AUDIO_NODE_HPP_INCLUDED
