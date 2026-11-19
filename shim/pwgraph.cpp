// PipeWire's clock settings, for the audio buffer floor in alsaaudio.cpp (from PR #4's
// pwgraph.cpp, extended: the minimum cycle too, and re-read at every device setup).
//
// Through pipewire-alsa, Live's shared PCM feeds a PipeWire stream from a ring of
// 2 periods (alsaaudio.cpp). Normally the graph's cycle (quantum) follows the smallest
// latency its streams ask for, so it runs at Live's period. It cannot when
// clock.force-quantum is set, or when clock.min-quantum is above Live's period: the
// cycle then takes more frames than Live's ring holds and every cycle ends in an XRUN
// (measured: 3,025 in 30 s at a forced or minimum 512 against Live's 128).
//
// The keys come from the "settings" metadata as plain strings. One short connection
// (dlopen'd libpipewire, deadline 300 ms) per query; alsaaudio.cpp asks when Live sets
// up its device, and answers are kept 2 s (one setup asks for both directions).
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/utils/dict.h>
#include <spa/utils/hook.h>
#include <dlfcn.h>
#include <chrono>
#include <mutex>
#include <string>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

namespace {

struct GraphTiming {
    unsigned force, minimum, rate; // frames at the graph's rate (0: not set / unknown), the graph's rate
};

struct Api {
    void (*init)(int *argc, char **argv);
    struct pw_loop *(*loopNew)(const struct spa_dict *props);
    void (*loopDestroy)(struct pw_loop *loop);
    struct pw_context *(*contextNew)(struct pw_loop *loop, const struct spa_dict *props, size_t user_data_size);
    void (*contextDestroy)(struct pw_context *context);
    struct pw_core *(*connect)(struct pw_context *context, const struct spa_dict *props, size_t user_data_size);
    int (*coreDisconnect)(struct pw_core *core);
    struct pw_registry *(*getRegistry)(struct pw_core *core, uint32_t version, size_t user_data_size);
    // Bound globals take their events through the proxy, not through the
    // interface's own add_listener (that one is for server-side objects).
    void (*proxyAddObjectListener)(struct pw_proxy *proxy, struct spa_hook *listener, const void *events, void *data);
};

const Api *api()
{
    static Api api;
    static const bool loaded = [] {
        void *lib = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!lib)
            return false;
        bool ok = true;
        auto get = [&](auto &fn, const char *name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            ok = ok && fn;
        };
        get(api.init, "pw_init");
        get(api.loopNew, "pw_loop_new");
        get(api.loopDestroy, "pw_loop_destroy");
        get(api.contextNew, "pw_context_new");
        get(api.contextDestroy, "pw_context_destroy");
        get(api.connect, "pw_context_connect");
        get(api.coreDisconnect, "pw_core_disconnect");
        get(api.getRegistry, "pw_core_get_registry");
        get(api.proxyAddObjectListener, "pw_proxy_add_object_listener");
        return ok;
    }();
    return loaded ? &api : nullptr;
}

// The clock keys, filled in by the metadata's property events as they arrive.
struct Settings {
    std::string forceQuantum, forceRate, minQuantum, rate;
    bool done() const { return !forceQuantum.empty() && !minQuantum.empty() && !forceRate.empty() && !rate.empty(); }
};

// The registry walk and the metadata binding it leads to; the listeners'
// hooks live here, for as long as the query iterates.
struct Session {
    Settings settings;
    struct pw_registry *registry = nullptr;
    bool bound = false;
    spa_hook metadataHook;
};

const pw_metadata_events metadataEvents = [] {
    pw_metadata_events events {};
    events.version = PW_VERSION_METADATA_EVENTS;
    events.property = [](void *data, uint32_t, const char *key, const char *, const char *value) {
        auto *settings = static_cast<Settings *>(data);
        if (!key || !value)
            return 0;
        const std::string text = value;
        if (!strcmp(key, "clock.force-quantum"))
            settings->forceQuantum = text;
        else if (!strcmp(key, "clock.force-rate"))
            settings->forceRate = text;
        else if (!strcmp(key, "clock.min-quantum"))
            settings->minQuantum = text;
        else if (!strcmp(key, "clock.rate"))
            settings->rate = text;
        return 0;
    };
    return events;
}();

const pw_registry_events registryEvents = [] {
    pw_registry_events events {};
    events.version = PW_VERSION_REGISTRY_EVENTS;
    events.global = [](void *data, uint32_t id, uint32_t, const char *type, uint32_t, const struct spa_dict *props) {
        auto *session = static_cast<Session *>(data);
        if (session->bound || strcmp(type, PW_TYPE_INTERFACE_Metadata) != 0)
            return;
        const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (!name || strcmp(name, "settings") != 0)
            return;
        session->bound = true;
        void *proxy = pw_registry_bind(session->registry, id, type, PW_VERSION_METADATA, 0);
        if (!proxy)
            return;
        if (const Api *pw = api())
            pw->proxyAddObjectListener(static_cast<struct pw_proxy *>(proxy), &session->metadataHook, &metadataEvents,
                                       &session->settings);
    };
    return events;
}();

bool queryTiming(GraphTiming &out)
{
    const Api *pw = api();
    if (!pw)
        return false;
    if (pw->init)
        pw->init(nullptr, nullptr);
    struct pw_loop *loop = pw->loopNew(nullptr);
    struct pw_context *context = loop ? pw->contextNew(loop, nullptr, 0) : nullptr;
    struct pw_core *core = context ? pw->connect(context, nullptr, 0) : nullptr;
    Session session;
    bool ok = false;
    if (core) {
        session.registry = pw->getRegistry(core, PW_VERSION_REGISTRY, 0);
        spa_hook registryHook;
        spa_zero(registryHook);
        pw_registry_add_listener(session.registry, &registryHook, &registryEvents, &session);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (!session.settings.done()) {
            if (pw_loop_iterate(loop, 50) < 0)
                break;
            if (std::chrono::steady_clock::now() > deadline)
                break;
        }
        const Settings &set = session.settings;
        auto number = [](const std::string &text) { return text.empty() ? 0u : unsigned(strtoul(text.c_str(), nullptr, 10)); };
        out.force = number(set.forceQuantum);
        out.minimum = number(set.minQuantum);
        out.rate = number(set.forceRate) ? number(set.forceRate) : number(set.rate);
        ok = !set.forceQuantum.empty() || !set.minQuantum.empty();
        pw->coreDisconnect(core);
    }
    if (context)
        pw->contextDestroy(context);
    if (loop)
        pw->loopDestroy(loop);
    return ok;
}

} // namespace

// PipeWire's forced and minimum cycle (frames at the graph's rate, 0 = not set) and
// the graph's rate. false: PipeWire did not answer (no floor then).
bool pipewireClock(unsigned &force, unsigned &minimum, unsigned &rate)
{
    static GraphTiming cached {0, 0, 0};
    static bool valid = false;
    static std::chrono::steady_clock::time_point at;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!(at.time_since_epoch().count() && now < at + std::chrono::seconds(2))) {
        GraphTiming fresh {0, 0, 0};
        valid = queryTiming(fresh);
        cached = fresh;
        at = now;
        if (valid)
            menufixTrace("pwgraph: force-quantum %u, min-quantum %u, rate %u\n", int(fresh.force), int(fresh.minimum), int(fresh.rate));
        else
            menufixTrace("pwgraph: no clock settings from PipeWire\n");
    }
    force = cached.force;
    minimum = cached.minimum;
    rate = cached.rate;
    return valid;
}

namespace {

// The desktop's default devices as PipeWire itself has them ("default" metadata:
// default.audio.sink / default.audio.source, JSON {"name": "<node>"}). pipewire-alsa's
// "pipewire" PCM, which "System Default" opens, follows these; the PulseAudio API
// answers with a fallback instead when one is not set (a source that exists but is
// not the default), so it cannot tell that the default input is missing.
struct Defaults {
    std::string sink, source;
    bool bound = false;
    struct pw_registry *registry = nullptr;
    spa_hook metadataHook;
};

std::string jsonName(const char *value)
{
    const char *key = value ? strstr(value, "\"name\"") : nullptr;
    const char *start = key ? strchr(key + 6, '"') : nullptr;
    const char *end = start ? strchr(start + 1, '"') : nullptr;
    return end ? std::string(start + 1, end) : std::string();
}

const pw_metadata_events defaultEvents = [] {
    pw_metadata_events events {};
    events.version = PW_VERSION_METADATA_EVENTS;
    events.property = [](void *data, uint32_t, const char *key, const char *, const char *value) {
        auto *defaults = static_cast<Defaults *>(data);
        if (key && !strcmp(key, "default.audio.sink"))
            defaults->sink = jsonName(value);
        else if (key && !strcmp(key, "default.audio.source"))
            defaults->source = jsonName(value);
        return 0;
    };
    return events;
}();

const pw_registry_events defaultRegistryEvents = [] {
    pw_registry_events events {};
    events.version = PW_VERSION_REGISTRY_EVENTS;
    events.global = [](void *data, uint32_t id, uint32_t, const char *type, uint32_t, const struct spa_dict *props) {
        auto *defaults = static_cast<Defaults *>(data);
        if (defaults->bound || strcmp(type, PW_TYPE_INTERFACE_Metadata) != 0)
            return;
        const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (!name || strcmp(name, "default") != 0)
            return;
        defaults->bound = true;
        void *proxy = pw_registry_bind(defaults->registry, id, type, PW_VERSION_METADATA, 0);
        if (proxy && api())
            api()->proxyAddObjectListener(static_cast<struct pw_proxy *>(proxy), &defaults->metadataHook, &defaultEvents,
                                          defaults);
    };
    return events;
}();

} // namespace

// PipeWire's default output and input node names (empty: none set). false: PipeWire
// did not answer or has no "default" metadata (nothing known).
bool pipewireDefaults(std::string &sink, std::string &source)
{
    const Api *pw = api();
    if (!pw)
        return false;
    if (pw->init)
        pw->init(nullptr, nullptr);
    struct pw_loop *loop = pw->loopNew(nullptr);
    struct pw_context *context = loop ? pw->contextNew(loop, nullptr, 0) : nullptr;
    struct pw_core *core = context ? pw->connect(context, nullptr, 0) : nullptr;
    Defaults defaults;
    bool ok = false;
    if (core) {
        defaults.registry = pw->getRegistry(core, PW_VERSION_REGISTRY, 0);
        spa_hook registryHook;
        spa_zero(registryHook);
        pw_registry_add_listener(defaults.registry, &registryHook, &defaultRegistryEvents, &defaults);
        // The metadata sends its keys right after binding; without an end marker,
        // wait a short while once it is bound (both keys found: done at once).
        const auto start = std::chrono::steady_clock::now();
        while (!(defaults.bound && !defaults.sink.empty() && !defaults.source.empty())) {
            if (pw_loop_iterate(loop, 20) < 0)
                break;
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::milliseconds(defaults.bound ? 150 : 300))
                break;
        }
        ok = defaults.bound;
        pw->coreDisconnect(core);
    }
    if (context)
        pw->contextDestroy(context);
    if (loop)
        pw->loopDestroy(loop);
    sink = defaults.sink;
    source = defaults.source;
    return ok;
}
