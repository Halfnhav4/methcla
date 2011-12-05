#ifndef MESCALINE_AUDIO_NODE_HPP_INCLUDED
#define MESCALINE_AUDIO_NODE_HPP_INCLUDED

#include <boost/cstdint.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/utility.hpp>

namespace Mescaline { namespace Audio {

    typedef int32_t NodeId;
    const NodeId InvalidNodeId = -1;

    class Environment;

    class Node : boost::noncopyable, public boost::intrusive::list_base_hook<>
    {
    protected:
        Node(Environment& env, const NodeId& id)
            : m_env(env)
            , m_id(id)
        { }

    public:
        /// Return environment.
        const Environment& environment() const { return m_env; }
        Environment& environment() { return m_env; }
        
        /// Return the node's id.
        const NodeId& id() const { return m_id; }

        // Process a number of frames.
        virtual void process(size_t numFrames) = 0;

    protected:
        void* operator new(size_t numBytes, void* where);
        void operator delete(void* ptr);
        void* operator new(size_t numBytes, Environment& env);
        void operator delete(void* ptr, Environment& env);
        template <class T> static void free(T* node);
        friend class NodeMap;
        virtual void free() = 0;

    private:
        Environment&    m_env;
        NodeId          m_id;
    };

    typedef boost::intrusive::list<Node> NodeList;
}; };

#endif // MESCALINE_AUDIO_NODE_HPP_INCLUDED
