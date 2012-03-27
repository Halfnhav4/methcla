#ifndef MESCALINE_AUDIO_SYNTHDEF_HPP_INCLUDED
#define MESCALINE_AUDIO_SYNTHDEF_HPP_INCLUDED

#include <Mescaline/Utility/Hash.hpp>

#include <boost/filesystem.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/unordered_map.hpp>
#include <boost/utility.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "lilv/lilv.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"
#include "lv2/puesnada.es/ext/rt-instantiate/rt-instantiate.h"

namespace Mescaline { namespace Audio {

using namespace boost::filesystem;
using namespace std;

class Port
{
public:
    enum Type
    {
        kInput      = 1
      , kOutput     = 2
      , kAudio      = 4
      , kControl    = 8
    };

    Port(Type type, uint32_t index, const char* symbol)
        : m_type(type)
        , m_index(index)
        , m_symbol(symbol)
    { }

    Type type() const { return m_type; }
    bool isa(Type t) const { return (m_type & t) == t; }
    bool isa(Type t1, Type t2) const { return isa(t1) && isa(t2); }

    uint32_t index() const { return m_index; }
    const char* symbol() const { return m_symbol.c_str(); }

private:
    Type        m_type;
    uint32_t    m_index;
    string      m_symbol;
};

class FloatPort : public Port
{
public:
    FloatPort( Type type, uint32_t index, const char* symbol
             , float minValue, float maxValue, float defaultValue );

    float minValue() const { return m_minValue; }
    float maxValue() const { return m_maxValue; }
    float defaultValue() const { return m_defaultValue; }

private:
    float   m_minValue;
    float   m_maxValue;
    float   m_defaultValue;
};

namespace Plugin{

class Binary
{
public:
    virtual ~Binary() { }
    virtual LV2_Descriptor_Function descriptorFunction() = 0;
};

class Loader
{
public:
    virtual boost::shared_ptr<Binary> load(const LilvPlugin* plugin) = 0;
};

class StaticBinary : public Binary
{
public:
    StaticBinary(LV2_Descriptor_Function function);
    virtual LV2_Descriptor_Function descriptorFunction();

private:
    LV2_Descriptor_Function m_descriptorFunction;
};

class StaticLoader : public Loader
{
public:
    typedef boost::unordered_map<std::string, LV2_Descriptor_Function>
            DescriptorFunctionMap;

    StaticLoader(const DescriptorFunctionMap& functions);
    virtual boost::shared_ptr<Binary> load(const LilvPlugin* plugin);

private:
    DescriptorFunctionMap m_functions;
};

//class Descriptor
//{
//public:
//    static Descriptor* load(boost::shared_ptr<Binary> binary, const LilvPlugin* plugin);
//
//    const char* uri() const { return m_descriptor->URI; }
//
//    template <class T> const T* extensionData(const char* uri) const
//    {
//        return static_cast<const T*>(m_descriptor->extension_data(uri));
//    }
//
//protected:
//    Descriptor(boost::shared_ptr<Binary> binary, const LV2_Descriptor* descriptor);
//
//private:
//    boost::shared_ptr<Binary> m_binary;
//    const LV2_Descriptor* m_descriptor;
//};

class Node : boost::noncopyable
{
public:
    Node(LilvNode* node);
    ~Node();

    const LilvNode* impl() const { return m_impl; }

private:
    LilvNode* m_impl;
};

typedef boost::shared_ptr<Node> NodePtr;

class Nodes : boost::noncopyable
{
public:
    Nodes(LilvNodes* nodes);
    ~Nodes();
    
    const LilvNodes* impl() const { return m_impl; }

private:
    LilvNodes* m_impl;
};

typedef boost::shared_ptr<Nodes> NodesPtr;

// class Constructor
// {
// public:
//     Constructor( const LV2_Descriptor* descriptor
//                , double sampleRate
//                , const char* bundlePath
//                , const LV2_Feature** features )
//         : m_descriptor(descriptor)
//         , m_sampleRate(sampleRate)
//         , m_bundlePath(bundlePath)
//         , m_features
//     { }
// 
//     const LV2_Descriptor* descriptor() const { return m_descriptor; }
// 
//     virtual size_t instanceSize() const = 0;
//     virtual size_t instanceAlignment() const = 0;
//     virtual void construct( double sample_rate
//                           , const char* bundle_path
//                           , const LV2_Feature *const *features
//                           ) const = 0;
// 
// private:
//     const LV2_Descriptor* m_descriptor;
//     double m_sampleRate;
//     const char* m_bundlePath;
//     const LV2_Feature** m_features;
// };
// 
// class PlacementConstructor : public Constructor
// {
// public:
//     PlacementConstructor(const LV2_Descriptor* descriptor, const LV2_Placement_Instantiate_Interface* interface);
// 
//     virtual size_t instanceSize() const;
//     virtual size_t instanceAlignment() const;
//     virtual void construct( double sample_rate
//                           , const char* bundle_path
//                           , const LV2_Feature *const *features
//                           ) const;
// 
// private:
//     const LV2_Placement_Instantiate_Interface* m_interface;
// };
// 
// class HostFeatures
// {
// public:
//     
//     hardRTCapable
//     hardRTInstantiable
// 
//     placement-instantiate:location
// };

class Manager;

class Plugin : boost::noncopyable
{
public:
    Plugin(Manager& manager, const LilvPlugin* plugin);
    ~Plugin();

    const char* uri() const;
    const char* name() const;

    size_t instanceSize      () const;
    size_t instanceAlignment () const;

    size_t numPorts() const { return m_ports.size(); }
    const FloatPort& port(size_t i) const { return m_ports.at(i); }

    size_t numAudioInputs    () const { return m_numAudioInputs;    }
    size_t numAudioOutputs   () const { return m_numAudioOutputs;   }
    size_t numControlInputs  () const { return m_numControlInputs;  }
    size_t numControlOutputs () const { return m_numControlOutputs; }

    LV2_Handle construct(void* location, double sampleRate) const;

    void destroy(LV2_Handle instance) const
    {
        if (m_descriptor->cleanup) m_descriptor->cleanup(instance);
    }

    void activate(LV2_Handle instance) const
    {
        if (m_descriptor->activate) m_descriptor->activate(instance);
    }

    void deactivate(LV2_Handle instance) const
    {
        if (m_descriptor->deactivate) m_descriptor->deactivate(instance);
    }
    
    void connectPort(LV2_Handle instance, uint32_t port, void* data) const
    {
        m_descriptor->connect_port(instance, port, data);
    }

    void run(LV2_Handle instance, uint32_t numSamples) const
    {
        m_descriptor->run(instance, numSamples);
    }

private:
    const LilvPlugin*                   m_plugin;
    const LV2_Descriptor*               m_descriptor;
    boost::shared_ptr<Binary>           m_binary;
    const char*                         m_bundlePath;
    const LV2_Feature* const*           m_features;
    const LV2_RT_Instantiate_Interface* m_constructor;
    vector<FloatPort>                   m_ports;
    uint32_t                            m_numAudioInputs;
    uint32_t                            m_numAudioOutputs;
    uint32_t                            m_numControlInputs;
    uint32_t                            m_numControlOutputs;
};

class UriMap
{
public:
    UriMap();

    LV2_URID map(const char* uri);
    const char* unmap(LV2_URID urid) const;

private:
    LV2_URID insert(const char* uri);

private:
    typedef boost::unordered_map<
                const char*
              , LV2_URID
              , Mescaline::Utility::Hash::string_hash
              , Mescaline::Utility::Hash::string_equal_to >
            UriToId;
    typedef boost::unordered_map<
                LV2_URID
              , const char* >
            IdToUri;
    
    UriToId m_uriToId;
    IdToUri m_idToUri;
};

class Manager : boost::noncopyable
{
public:
    Manager(Loader& loader);
    ~Manager();

    // Features
    const LV2_Feature* const* features();

    // Node creation
    NodePtr newUri(const char* uri);

    // Plugin discovery and loading
    Loader& loader() { return m_loader; }
    void loadPlugins();

    // Plugin access
    typedef boost::shared_ptr<Plugin> PluginHandle;

    const PluginHandle& lookup(const char* uri) const;

    // Uri mapping
    const UriMap& uriMap() const { return m_uriMap; }
	UriMap& uriMap() { return m_uriMap; }

    LV2_URID_Map* lv2UridMap();
    LV2_URID_Unmap* lv2UridUnmap();

private:
    void addFeature(const char* uri, void* data=0);

    typedef boost::unordered_map<
                const char*
              , PluginHandle
              , Mescaline::Utility::Hash::string_hash
              , Mescaline::Utility::Hash::string_equal_to >
            Map;

private:
    typedef std::vector<const LV2_Feature*> Features;
    Loader&         m_loader;
    LilvWorld*      m_world;
    Features        m_features;
    Map             m_plugins;
    UriMap          m_uriMap;
    LV2_URID_Map    m_lv2UridMap;
    LV2_URID_Unmap  m_lv2UridUnmap;
};

};

}; };

#endif // MESCALINE_AUDIO_SYNTHDEF_HPP_INCLUDED