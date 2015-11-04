///@file  Timestamp.cpp
///@brief Timestamp provider for caliper records

#include "../CaliperService.h"

#include <Caliper.h>
#include <SigsafeRWLock.h>
#include <Snapshot.h>

#include <RuntimeConfig.h>
#include <ContextRecord.h>
#include <Log.h>

#include <cassert>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

using namespace cali;
using namespace std;

namespace 
{

chrono::time_point<chrono::high_resolution_clock> tstart;

Attribute timestamp_attr { Attribute::invalid } ;
Attribute timeoffs_attr  { Attribute::invalid } ;
Attribute snapshot_duration_attr { Attribute::invalid };
Attribute phase_duration_attr    { Attribute::invalid };

ConfigSet config;

bool      record_timestamp;
bool      record_offset;
bool      record_duration;
bool      record_phases;

Attribute begin_evt_attr { Attribute::invalid };
Attribute end_evt_attr   { Attribute::invalid };
Attribute lvl_attr       { Attribute::invalid };

typedef std::map<uint64_t, Attribute> OffsetAttributeMap;

std::mutex         offset_attributes_mutex;
OffsetAttributeMap offset_attributes;

static const ConfigSet::Entry s_configdata[] = {
    { "snapshot_duration", CALI_TYPE_BOOL, "true",
      "Include duration of snapshot epoch with each context record",
      "Include duration of snapshot epoch with each context record"
    },
    { "offset", CALI_TYPE_BOOL, "false",
      "Include time offset (time since program start) with each context record",
      "Include time offset (time since program start) with each context record"
    },
    { "timestamp", CALI_TYPE_BOOL, "false",
      "Include absolute timestamp (POSIX time) with each context record",
      "Include absolute timestamp (POSIX time) with each context record"
    },
    { "phase_duration", CALI_TYPE_BOOL, "true",
      "Record begin/end phase durations.",
      "Record begin/end phase durations."
    },
    ConfigSet::Terminator
};


uint64_t make_offset_attribute_key(cali_id_t attr_id, unsigned level)
{
    // assert((level   & 0xFFFF)         == 0);
    // assert((attr_id & 0xFFFFFFFFFFFF) == 0);

    uint64_t lvl_k = static_cast<uint64_t>(level) << 48;

    return lvl_k | attr_id;    
}

// Get or create a hidden offset attribute for the given attribute and its
// current hierarchy level

Attribute make_offset_attribute(Caliper* c, cali_id_t attr_id, unsigned level)
{
    uint64_t key = make_offset_attribute_key(attr_id, level);

    {
        std::lock_guard<std::mutex>  lock(offset_attributes_mutex);
        OffsetAttributeMap::iterator it = offset_attributes.find(key);
    
        if (it != offset_attributes.end())
            return it->second;
    }

    // Not found -> create new entry

    std::string name = "offs.";
    name.append(std::to_string(attr_id));
    name.append(".");
    name.append(std::to_string(level));

    Attribute offs_attr = 
        c->create_attribute(name, CALI_TYPE_UINT,
                            CALI_ATTR_ASVALUE     |
                            CALI_ATTR_SKIP_EVENTS |
                            CALI_ATTR_HIDDEN      |
                            CALI_ATTR_SCOPE_THREAD); // FIXME: needs to be original attribute's scope

    std::lock_guard<std::mutex> lock(offset_attributes_mutex);
    offset_attributes.insert(std::make_pair(key, offs_attr));

    return offs_attr;
}

// Get existing hidden offset attribute for the given attribute and its
// current hierarchy level

Attribute find_offset_attribute(Caliper* c, cali_id_t attr_id, unsigned level)
{
    uint64_t key = make_offset_attribute_key(attr_id, level);

    {
        std::lock_guard<std::mutex>  lock(offset_attributes_mutex);
        OffsetAttributeMap::iterator it = offset_attributes.find(key);
    
        if (it != offset_attributes.end())
            return it->second;
    }

    return Attribute::invalid;
}

void snapshot_cb(Caliper* c, int scope, const Entry* trigger_info, Snapshot* sbuf) {
    Snapshot::Sizes     sizes = sbuf->capacity();
    Snapshot::Addresses addr  = sbuf->addresses();

    auto now = chrono::high_resolution_clock::now();

    int capacity = std::min(sizes.n_attr, sizes.n_data);

    sizes = { 0, 0, 0 };

    if ((record_duration || record_phases || record_offset) && scope & CALI_SCOPE_THREAD) {
        uint64_t  usec = chrono::duration_cast<chrono::microseconds>(now - tstart).count();
        Variant v_usec = Variant(usec);
        Variant v_offs = c->exchange(timeoffs_attr, v_usec);

        if (capacity-- > 0 && record_duration && !v_offs.empty()) {
            uint64_t duration = usec - v_offs.to_uint();

            addr.immediate_attr[sizes.n_attr++] = snapshot_duration_attr.id();
            addr.immediate_data[sizes.n_data++] = Variant(duration);
        }

        if (record_phases && trigger_info) {
            if (trigger_info->attribute() == begin_evt_attr.id()) {
                // begin/set event: save time for current entry

                cali_id_t evt_attr_id = trigger_info->value().to_id();
                Variant   v_level     = trigger_info->value(lvl_attr);

                if (evt_attr_id == CALI_INV_ID || v_level.empty())
                    goto record_phases_exit;

                c->set(make_offset_attribute(c, evt_attr_id, v_level.to_uint()), v_usec);
            } else if (trigger_info->attribute() == end_evt_attr.id())   {
                // end event: get saved time for current entry and calculate duration

                cali_id_t evt_attr_id = trigger_info->value().to_id();
                Variant   v_level     = trigger_info->value(lvl_attr);

                if (evt_attr_id == CALI_INV_ID || v_level.empty())
                    goto record_phases_exit;

                Attribute offs_attr = 
                    find_offset_attribute(c, evt_attr_id, v_level.to_uint());

                if (offs_attr == Attribute::invalid)
                    goto record_phases_exit;

                Variant v_p_usec    = 
                    c->exchange(offs_attr, Variant());

                if (!v_p_usec.empty()) {
                    addr.immediate_attr[sizes.n_attr++] = phase_duration_attr.id();
                    addr.immediate_data[sizes.n_data++] = usec - v_p_usec.to_uint();
                }
            }
record_phases_exit:
            ;
        }
    }

    if (capacity-- > 0 && record_timestamp && (scope & CALI_SCOPE_PROCESS)) {
        addr.immediate_attr[sizes.n_attr++] = timestamp_attr.id();
        addr.immediate_data[sizes.n_data++] = Variant(static_cast<int>(chrono::system_clock::to_time_t(chrono::system_clock::now())));
    }

    sbuf->commit(sizes);
}

void post_init_cb(Caliper* c)
{
    // Find begin/end event snapshot event info attributes

    begin_evt_attr = c->get_attribute("cali.snapshot.event.begin");
    end_evt_attr   = c->get_attribute("cali.snapshot.event.end");
    lvl_attr       = c->get_attribute("cali.snapshot.event.attr.level");

    if (begin_evt_attr == Attribute::invalid || 
        end_evt_attr   == Attribute::invalid ||
        lvl_attr       == Attribute::invalid) {
        if (record_phases)
            Log(1).stream() << "Warning: \"event\" service with snapshot info\n"
                "    is required for phase timers." << std::endl;

        record_phases = false;
    }
}


/// Initialization handler
void timestamp_service_register(Caliper* c)
{
    // set start time and create time attribute
    tstart = chrono::high_resolution_clock::now();

    config = RuntimeConfig::init("timer", s_configdata);

    record_duration  = config.get("snapshot_duration").to_bool();
    record_offset    = config.get("offset").to_bool();
    record_timestamp = config.get("timestamp").to_bool();
    record_phases    = config.get("phase_duration").to_bool();

    int hide_offset  = (record_duration && !record_offset ? CALI_ATTR_HIDDEN : 0);

    timestamp_attr = 
        c->create_attribute("time.timestamp",   CALI_TYPE_UINT, 
                            CALI_ATTR_ASVALUE | CALI_ATTR_SCOPE_PROCESS | CALI_ATTR_SKIP_EVENTS);
    timeoffs_attr = 
        c->create_attribute("time.offset",      CALI_TYPE_UINT, 
                            CALI_ATTR_ASVALUE | CALI_ATTR_SCOPE_THREAD  | CALI_ATTR_SKIP_EVENTS | hide_offset);
    snapshot_duration_attr = 
        c->create_attribute("time.duration",    CALI_TYPE_UINT, 
                            CALI_ATTR_ASVALUE | CALI_ATTR_SCOPE_THREAD  | CALI_ATTR_SKIP_EVENTS);
    phase_duration_attr = 
        c->create_attribute("time.phase.duration", CALI_TYPE_UINT, 
                            CALI_ATTR_ASVALUE | CALI_ATTR_SCOPE_THREAD  | CALI_ATTR_SKIP_EVENTS);

    c->set(timeoffs_attr, Variant(static_cast<unsigned>(0)));

    // c->events().create_attr_evt.connect(&create_attr_cb);
    c->events().post_init_evt.connect(&post_init_cb);
    c->events().snapshot.connect(&snapshot_cb);

    Log(1).stream() << "Registered timestamp service" << endl;
}

} // namespace


namespace cali
{
    CaliperService TimestampService = { "timestamp", ::timestamp_service_register };
} // namespace cali
