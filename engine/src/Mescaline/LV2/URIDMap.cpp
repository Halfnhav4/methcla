#include <Mescaline/Exception.hpp>
#include <Mescaline/LV2/URIDMap.hpp>

using namespace Mescaline::LV2;

static LV2_URID URIDMap_map(LV2_URID_Map_Handle handle,
                            const char*         uri)
{
    return static_cast<URIDMap*>(handle)->map(uri);
}

static const char* URIDMap_unmap(LV2_URID_Unmap_Handle handle,
                                 LV2_URID              urid)
{
    return static_cast<URIDMap*>(handle)->unmap(urid);
}

URIDMap::URIDMap()
{
    m_lv2Map.handle = this;
    m_lv2Map.map = URIDMap_map;

    m_lv2Unmap.handle = this;
    m_lv2Unmap.unmap = URIDMap_unmap;
}

LV2_URID URIDMap::insert(const char* uri)
{
    LV2_URID urid = m_uriToId.size() + 1;
    if (urid == 0)
        BOOST_THROW_EXCEPTION(Exception() << ErrorInfoString("No more URIDs left"));
    m_uriToId[uri] = urid;
    m_idToUri[urid] = uri;
    return urid;
}

LV2_URID URIDMap::map(const char* uri)
{
    UriToId::const_iterator it = m_uriToId.find(uri);
    return it == m_uriToId.end()
            ? insert(uri)
            : it->second;
}

const char* URIDMap::unmap(LV2_URID urid) const
{
    IdToUri::const_iterator it = m_idToUri.find(urid);
    return it == m_idToUri.end() ? 0 : it->second;
}

LV2_URID_Map* URIDMap::lv2Map()
{
    return &m_lv2Map;
}

LV2_URID_Unmap* URIDMap::lv2Unmap()
{
    return &m_lv2Unmap;
}
