///@file  Papi.cpp
///@brief PAPI provider for caliper records

#include "../CaliperService.h"

#include <Caliper.h>

#include <RuntimeConfig.h>
#include <ContextRecord.h>
#include <Log.h>

#include <util/split.hpp>

#include <pthread.h>

using namespace cali;
using namespace std;

#include <papi.h>

#define MAX_COUNTERS 16

namespace 
{

Variant   counter_attrbs[MAX_COUNTERS];

int       counter_events[MAX_COUNTERS];
long long counter_values[MAX_COUNTERS];

int       num_counters { 0 };


static const ConfigSet::Entry s_configdata[] = {
    { "counters", CALI_TYPE_STRING, "",
      "List of PAPI events to record",
      "List of PAPI events to record, separated by ':'" 
    },
    ConfigSet::Terminator
};

void push_counter(Caliper* c, int scope, WriteRecordFn fn) {
    if (num_counters < 1)
        return;

    Variant v_data[MAX_COUNTERS];

    if (PAPI_accum_counters(counter_values, num_counters) != PAPI_OK) {
        Log(1).stream() << "PAPI failed to accumulate counters!" << endl;
        return;
    }

    for (int i = 0; i < num_counters; ++i)
        v_data[i] = static_cast<uint64_t>(counter_values[i]);

    int               n[3] = {       0, num_counters,   num_counters };
    const Variant* data[3] = { nullptr, counter_attrbs, v_data       };

    fn(ContextRecord::record_descriptor(), n, data);
}

void papi_init(Caliper* c) {
    if (PAPI_start_counters(counter_events, num_counters) != PAPI_OK)
        Log(1).stream() << "PAPI counters failed to initialize!" << endl;
    else
        Log(1).stream() << "PAPI counters initialized successfully" << endl;
}

void papi_finish(Caliper* c) {
    if (PAPI_stop_counters(counter_values, num_counters) != PAPI_OK)
        Log(1).stream() << "PAPI counters failed to stop!" << endl;
    else
        Log(1).stream() << "PAPI counters stopped successfully" << endl;
}

void setup_events(Caliper* c, const string& eventstring)
{
    vector<string> events;

    util::split(eventstring, ':', back_inserter(events));

    num_counters = 0;

    for (string& event : events) {
        int code;

        if (PAPI_event_name_to_code(const_cast<char*>(event.c_str()), &code) != PAPI_OK) {
            Log(0).stream() << "Unable to register PAPI counter \"" << event << '"' << endl;
            continue;
        }

        if (num_counters < MAX_COUNTERS) {
            Attribute attr =
                c->create_attribute(string("papi.")+event, CALI_TYPE_UINT,
                                    CALI_ATTR_ASVALUE | CALI_ATTR_SCOPE_THREAD | CALI_ATTR_SKIP_EVENTS);

            counter_events[num_counters] = code;
            counter_attrbs[num_counters] = attr.id();

            ++num_counters;
        } else
            Log(0).stream() << "Maximum number of PAPI counters exceeded; dropping \"" 
                            << event << '"' << endl;
    }
}

// Initialization handler
void papi_register(Caliper* c) {
    int ret = PAPI_library_init(PAPI_VER_CURRENT);
    
    if (ret != PAPI_VER_CURRENT && ret > 0) {
        Log(0).stream() << "PAPI version mismatch: found " 
                        << ret << ", expected " << PAPI_VER_CURRENT << endl;
        return;
    }        

    // PAPI_thread_init(pthread_self);
    
    if (PAPI_is_initialized() == PAPI_NOT_INITED) {
        Log(0).stream() << "PAPI library is not initialized" << endl;
        return;
    }

    setup_events(c, RuntimeConfig::init("papi", s_configdata).get("counters").to_string());

    if (num_counters < 1) {
        Log(1).stream() << "No PAPI counters registered, dropping PAPI service" << endl;
        return;
    }

    // add callback for Caliper::get_context() event
    c->events().post_init_evt.connect(&papi_init);
    c->events().finish_evt.connect(&papi_finish);
    c->events().measure.connect(&push_counter);

    Log(1).stream() << "Registered PAPI service" << endl;
}

} // namespace


namespace cali 
{
    CaliperService PapiService = { "papi", ::papi_register };
} // namespace cali