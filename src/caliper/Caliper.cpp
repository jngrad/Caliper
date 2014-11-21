/// @file Caliper.cpp
/// Caliper main class
///

#include "Caliper.h"
#include "Context.h"
#include "MemoryPool.h"
#include "SigsafeRWLock.h"

#include <Services.h>

#include <AttributeStore.h>
#include <ContextRecord.h>
#include <MetadataWriter.h>
#include <Node.h>
#include <Log.h>
#include <RuntimeConfig.h>

#include <signal.h>

#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <vector>
#include <utility>

using namespace cali;
using namespace std;


//
// --- Caliper implementation
//

struct Caliper::CaliperImpl
{
    // --- static data

    static volatile sig_atomic_t  s_siglock;
    static std::mutex             s_mutex;
    
    static unique_ptr<Caliper>    s_caliper;

    static const ConfigSet::Entry s_configdata[];


    // --- data

    ConfigSet             m_config;
    
    function<cali_id_t()> m_env_cb;
    
    MemoryPool            m_mempool;

    mutable SigsafeRWLock m_nodelock;
    vector<Node*>         m_nodes;
    Node                  m_root;

    mutable SigsafeRWLock m_attribute_lock;
    AttributeStore        m_attributes;
    Context               m_context;

    Events                m_events;


    // --- constructor

    CaliperImpl()
        : m_config { RuntimeConfig::init("caliper", s_configdata) }, 
        m_root { CALI_INV_ID, Attribute::invalid, 0, 0 } 
    { }

    ~CaliperImpl() {
        Log(1).stream() << "Finished" << endl;

        for ( auto &n : m_nodes )
            n->~Node();
    }

    // deferred initialization: called when it's safe to use the public Caliper interface

    void init() {
        m_nodes.reserve(m_config.get("node_pool_size").to_uint());

        Services::register_services(s_caliper.get());

        Log(1).stream() << "Initialized" << endl;

        if (Log::verbosity() >= 2)
            RuntimeConfig::print( Log(2).stream() << "Configuration:\n" );
    }

    // --- helpers

    Node* create_node(const Attribute& attr, const void* data, size_t size) {
        const size_t align = 8;
        const size_t pad   = align - sizeof(Node)%align;

        char* ptr  = static_cast<char*>(m_mempool.allocate(sizeof(Node) + pad + size));

        m_nodelock.wlock();

        Node* node = new(ptr) 
            Node(m_nodes.size(), attr, memcpy(ptr+sizeof(Node)+pad, data, size), size);        

        m_nodes.push_back(node);
        m_nodelock.unlock();

        return node;
    }


    // --- Context interface

    std::size_t 
    get_context(cali_id_t env, uint64_t buf[], std::size_t len) {
        // invoke callbacks
        m_events.queryEvt(s_caliper.get(), env);

        return m_context.get_context(env, buf, len);
    }


    // --- Annotation interface

    cali_err begin(cali_id_t env, const Attribute& attr, const void* data, size_t size) {
        cali_err ret = CALI_EINV;

        if (attr == Attribute::invalid)
            return CALI_EINV;

        cali_id_t key = attr.id();

        if (attr.store_as_value() && size == sizeof(uint64_t)) {
            uint64_t val = 0;
            memcpy(&val, data, sizeof(uint64_t));
            ret = m_context.set(env, key, val, attr.is_global());
        } else {
            auto p = m_context.get(env, key);

            m_nodelock.rlock();

            Node* parent = p.first ? m_nodes[p.second] : &m_root;
            Node* node   = parent ? parent->first_child() : nullptr;

            while ( node && !node->equals(attr.id(), data, size) )
                node = node->next_sibling();

            m_nodelock.unlock();

            if (!node) {
                node = create_node(attr, data, size);

                if (parent) {
                    m_nodelock.wlock();
                    parent->append(node);
                    m_nodelock.unlock();
                }
            }

            ret = m_context.set(env, key, node->id(), attr.is_global());
        }

        // invoke callbacks
        m_events.beginEvt(s_caliper.get(), env, attr);

        return ret;
    }

    cali_err end(cali_id_t env, const Attribute& attr) {
        if (attr == Attribute::invalid)
            return CALI_EINV;

        cali_err  ret = CALI_EINV;
        cali_id_t key = attr.id();

        if (attr.store_as_value())
            ret = m_context.unset(env, key);
        else {
            auto p = m_context.get(env, key);

            if (!p.first)
                return CALI_EINV;

            m_nodelock.rlock();

            Node* node = m_nodes[p.second];

            if (node->attribute() != attr.id()) {
                // For now, just continue before first node with this attribute
                while (node && node->attribute() != attr.id())
                    node = node->parent();

                if (!node)
                    return CALI_EINV;
            }

            node = node->parent();
            m_nodelock.unlock();

            if (node == &m_root)
                ret = m_context.unset(env, key);
            else if (node)
                ret = m_context.set(env, key, node->id());
        }

        // invoke callbacks
        m_events.endEvt(s_caliper.get(), env, attr);

        return ret;
    }

    cali_err set(cali_id_t env, const Attribute& attr, const void* data, size_t size) {
        if (attr == Attribute::invalid)
            return CALI_EINV;

        cali_err  ret = CALI_EINV;
        cali_id_t key = attr.id();

        if (attr.store_as_value() && size == sizeof(uint64_t)) {
            uint64_t val = 0;
            memcpy(&val, data, sizeof(uint64_t));
            ret = m_context.set(env, key, val, attr.is_global());
        } else {
            auto p = m_context.get(env, key);

            Node* parent { nullptr };

            m_nodelock.rlock();

            if (p.first)
                parent = m_nodes[p.second]->parent();
            if (!parent)
                parent = &m_root;

            Node* node = parent->first_child();

            while ( node && !node->equals(attr.id(), data, size) )
                node = node->next_sibling();

            m_nodelock.unlock();

            if (!node) {
                node = create_node(attr, data, size);

                if (parent) {
                    m_nodelock.wlock();
                    parent->append(node);
                    m_nodelock.unlock();
                }
            }

            ret = m_context.set(env, key, node->id(), attr.is_global());
        }

        // invoke callbacks
        m_events.setEvt(s_caliper.get(), env, attr);

        return ret;
    }


    // --- Retrieval

    const Node* get(cali_id_t id) const {
        if (id > m_nodes.size())
            return nullptr;

        const Node* ret = nullptr;

        m_nodelock.rlock();
        ret = m_nodes[id];
        m_nodelock.unlock();

        return ret;
    }


    // --- Serialization API

    void foreach_node(std::function<void(const Node&)> proc) {
        // Need locking?
        for (Node* node : m_nodes)
            if (node)
                proc(*node);
    }
};


// --- static member initialization

volatile sig_atomic_t  Caliper::CaliperImpl::s_siglock = 1;
mutex                  Caliper::CaliperImpl::s_mutex;

unique_ptr<Caliper>    Caliper::CaliperImpl::s_caliper;

const ConfigSet::Entry Caliper::CaliperImpl::s_configdata[] = {
    // key, type, value, short description, long description
    { "node_pool_size", CALI_TYPE_UINT, "100",
      "Size of the Caliper node pool",
      "Initial size of the Caliper node pool" 
    },
    { "output", CALI_TYPE_STRING, "csv",
      "Caliper metadata output format",
      "Caliper metadata output format. One of\n"
      "   csv:  CSV file output\n"
      "   none: No output" 
    },
    ConfigSet::Terminator 
};


//
// --- Caliper class definition
//

Caliper::Caliper()
    : mP(new CaliperImpl)
{ 
}

Caliper::~Caliper()
{
    mP.reset(nullptr);
}

// --- Events interface

Caliper::Events&
Caliper::events()
{
    return mP->m_events;
}


// --- Context API

cali_id_t 
Caliper::current_environment() const
{
    return mP->m_env_cb ? mP->m_env_cb() : 0;
}

cali_id_t 
Caliper::clone_environment(cali_id_t env)
{
    return mP->m_context.clone_environment(env);
}

std::size_t 
Caliper::context_size(cali_id_t env) const
{
    return mP->m_context.context_size(env);
}

std::size_t 
Caliper::get_context(cali_id_t env, uint64_t buf[], std::size_t len) 
{
    return mP->get_context(env, buf, len);
}

void 
Caliper::set_environment_callback(std::function<cali_id_t()> cb)
{
    mP->m_env_cb = cb;
}


// --- Annotation interface

cali_err 
Caliper::begin(cali_id_t env, const Attribute& attr, const void* data, size_t size)
{
    return mP->begin(env, attr, data, size);
}

cali_err 
Caliper::end(cali_id_t env, const Attribute& attr)
{
    return mP->end(env, attr);
}

cali_err 
Caliper::set(cali_id_t env, const Attribute& attr, const void* data, size_t size)
{
    return mP->set(env, attr, data, size);
}


// --- Attribute API

size_t
Caliper::num_attributes() const
{
    mP->m_attribute_lock.rlock();
    size_t s = mP->m_attributes.size();
    mP->m_attribute_lock.unlock();

    return s;
}

Attribute
Caliper::get_attribute(cali_id_t id) const
{
    mP->m_attribute_lock.rlock();
    Attribute a = mP->m_attributes.get(id);
    mP->m_attribute_lock.unlock();

    return a;
}

Attribute
Caliper::get_attribute(const std::string& name) const
{
    mP->m_attribute_lock.rlock();
    Attribute a = mP->m_attributes.get(name);
    mP->m_attribute_lock.unlock();

    return a;
}

Attribute 
Caliper::create_attribute(const std::string& name, cali_attr_type type, int prop)
{
    mP->m_attribute_lock.wlock();
    Attribute a = mP->m_attributes.create(name, type, prop);
    mP->m_attribute_lock.unlock();

    return a;
}


// --- Caliper query API

std::vector<RecordMap>
Caliper::unpack(const uint64_t buf[], size_t size) const
{
    return ContextRecord::unpack(
        [this](cali_id_t id){ return get_attribute(id); },
        [this](cali_id_t id){ return mP->get(id); },
        buf, size);                                 
}


// --- Serialization API

void
Caliper::foreach_node(std::function<void(const Node&)> proc)
{
    mP->foreach_node(proc);
}

void
Caliper::foreach_attribute(std::function<void(const Attribute&)> proc)
{
    mP->m_attribute_lock.rlock();
    mP->m_attributes.foreach_attribute(proc);
    mP->m_attribute_lock.unlock();
}

bool
Caliper::write_metadata()
{    
    string writer_service_name = mP->m_config.get("output").to_string();

    if (writer_service_name == "none")
        return true;

    auto w = Services::get_metadata_writer(writer_service_name.c_str());

    if (!w) {
        Log(0).stream() << "Writer service \"" << writer_service_name << "\" not found!" << endl;
        return false;
    }

    using std::placeholders::_1;

    return w->write(std::bind(&Caliper::foreach_attribute, this,     _1),
                    std::bind(&CaliperImpl::foreach_node,  mP.get(), _1));
}


// --- Caliper singleton API

Caliper* Caliper::instance()
{
    if (CaliperImpl::s_siglock == 1) {
        lock_guard<mutex> lock(CaliperImpl::s_mutex);

        if (!CaliperImpl::s_caliper) {
            CaliperImpl::s_caliper.reset(new Caliper);

            // now it is safe to use the Caliper interface
            CaliperImpl::s_caliper->mP->init();

            CaliperImpl::s_siglock = 0;
        }
    }

    return CaliperImpl::s_caliper.get();
}

Caliper* Caliper::try_instance()
{
    return CaliperImpl::s_siglock == 0 ? CaliperImpl::s_caliper.get() : nullptr;
}
