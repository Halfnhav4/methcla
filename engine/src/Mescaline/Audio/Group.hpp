#ifndef MESCALINE_AUDIO_GROUP_HPP_INCLUDED
#define MESCALINE_AUDIO_GROUP_HPP_INCLUDED

#include <Mescaline/Audio/Node.hpp>

namespace Mescaline { namespace Audio {

class Group : public Node
{
protected:
    Group(Environment& env, Group* target, Node::AddAction addAction)
        : Node(env, target, addAction)
    { }

public:
    static Group* construct(Environment& env, Group* target, AddAction addAction);
    virtual void free() override;

    const NodeList& children() const { return m_children; }
    void addToHead(Node& node) { m_children.push_front(node); }
    void addToTail(Node& node) { m_children.push_back(node); }

    virtual void process(size_t numFrames) override;

private:
    NodeList m_children;
};

}; };

#endif // MESCALINE_AUDIO_GROUP_HPP_INCLUDED
