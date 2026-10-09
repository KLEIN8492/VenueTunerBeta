#include "core/SystemState.h"
#include "dsp/FilterDesign.h"
#include <cmath>

namespace vt {

void SystemState::ensureDefaults(int numIn, int numOut)
{
    if (!sources.empty() || !outputs.empty()) return;
    for (int i = 0; i < std::min(numIn, 2); ++i)
        sources.push_back({ "Input " + juce::String(i + 1), "program", i });
    const char* names[] = { "Main L", "Main R" };
    for (int o = 0; o < std::min(numOut, 2); ++o) {
        OutputConfig c;
        c.name = numOut == 1 ? "Main" : names[o];
        c.channel = o;
        c.mix.assign(sources.size(), 0.0f); // unrouted: route explicitly (a built-in mic must not hit the PA)
        outputs.push_back(c);
    }
}

void SystemState::removeSource(int index)
{
    if (index < 0 || index >= int(sources.size())) return;
    sources.erase(sources.begin() + index);
    for (auto& o : outputs)
        if (index < int(o.mix.size())) o.mix.erase(o.mix.begin() + index);
    auto fix = [index](int& ref) { ref = ref == index ? -1 : (ref > index ? ref - 1 : ref); };
    fix(measure.micSource);
    fix(measure.refSource);
    if (measure.ref == "source" && measure.refSource < 0) measure.ref = "generator";
}

void SystemState::removeOutput(int index)
{
    if (index < 0 || index >= int(outputs.size())) return;
    outputs.erase(outputs.begin() + index);
    for (auto& g : measure.groups) {
        std::vector<int> kept;
        for (int o : g.outputs)
            if (o != index) kept.push_back(o > index ? o - 1 : o);
        g.outputs = kept;
    }
    std::vector<int> t;
    for (int x : gen.targets)
        if (x != index) t.push_back(x > index ? x - 1 : x);
    gen.targets = t;
}

// ---------------------------------------------------------------- serialisation
namespace {

juce::var obj() { return juce::var(new juce::DynamicObject()); }

template <class T>
juce::var arr(const std::vector<T>& v)
{
    juce::Array<juce::var> a;
    for (auto& x : v) a.add(x);
    return a;
}

juce::var outputToVar(const OutputConfig& o)
{
    auto v = obj();
    auto* d = v.getDynamicObject();
    d->setProperty("name", o.name);
    d->setProperty("type", o.type);
    d->setProperty("colour", o.colour);
    d->setProperty("id", o.id);
    d->setProperty("channel", o.channel);
    d->setProperty("gainDb", o.gainDb);
    d->setProperty("polarityInvert", o.polarityInvert);
    d->setProperty("delayMs", o.delayMs);
    d->setProperty("mute", o.mute);
    d->setProperty("limiterOn", o.limiterOn);
    d->setProperty("limThreshDb", o.limThreshDb);
    d->setProperty("limAttackMs", o.limAttackMs);
    d->setProperty("limReleaseMs", o.limReleaseMs);
    d->setProperty("mix", arr(o.mix));

    juce::Array<juce::var> peq, ap;
    for (auto& b : o.peq) {
        auto x = obj();
        x.getDynamicObject()->setProperty("on", b.on);
        x.getDynamicObject()->setProperty("type", b.type);
        x.getDynamicObject()->setProperty("freq", b.freq);
        x.getDynamicObject()->setProperty("gainDb", b.gainDb);
        x.getDynamicObject()->setProperty("q", b.q);
        peq.add(x);
    }
    for (auto& b : o.ap) {
        auto x = obj();
        x.getDynamicObject()->setProperty("on", b.on);
        x.getDynamicObject()->setProperty("order", b.order);
        x.getDynamicObject()->setProperty("freq", b.freq);
        x.getDynamicObject()->setProperty("q", b.q);
        ap.add(x);
    }
    d->setProperty("peq", peq);
    d->setProperty("ap", ap);
    for (auto [key, xo] : { std::pair { "hp", &o.hp }, std::pair { "lp", &o.lp } }) {
        auto x = obj();
        x.getDynamicObject()->setProperty("type", xo->type);
        x.getDynamicObject()->setProperty("freq", xo->freq);
        d->setProperty(key, x);
    }
    auto fir = obj();
    fir.getDynamicObject()->setProperty("on", o.fir.on);
    fir.getDynamicObject()->setProperty("path", o.fir.path);
    fir.getDynamicObject()->setProperty("taps", o.fir.taps);
    fir.getDynamicObject()->setProperty("latencySamples", o.fir.latencySamples);
    d->setProperty("fir", fir);
    return v;
}

const juce::StringArray kPeqTypes { "bell", "lowshelf", "highshelf", "notch", "lowpass", "highpass" };
const juce::StringArray kXoTypes { "off", "bw6", "bw12", "bw18", "bw24", "bw36", "bw48", "lr12", "lr24", "lr48" };

// Strict field reader. Missing fields keep their default; present fields must have
// the right type and range.
struct Reader
{
    const juce::var& v;
    juce::String ctx;
    juce::String& err;

    bool fail(const juce::String& key, const juce::String& why)
    {
        err = ctx + key + ": " + why;
        return false;
    }

    bool num(const char* key, float& out, double lo, double hi)
    {
        double d = out;
        if (!num(key, d, lo, hi)) return false;
        out = float(d);
        return true;
    }
    bool num(const char* key, double& out, double lo, double hi)
    {
        if (!v.hasProperty(key)) return true;
        const auto& x = v[key];
        if (!(x.isDouble() || x.isInt() || x.isInt64())) return fail(key, "expected a number");
        const double d = double(x);
        if (!std::isfinite(d) || d < lo || d > hi)
            return fail(key, "out of range [" + juce::String(lo) + ", " + juce::String(hi) + "]");
        out = d;
        return true;
    }
    bool integer(const char* key, int& out, int lo, int hi)
    {
        double d = out;
        if (!num(key, d, lo, hi)) return false;
        if (d != std::floor(d)) return fail(key, "expected an integer");
        out = int(d);
        return true;
    }
    bool boolean(const char* key, bool& out)
    {
        if (!v.hasProperty(key)) return true;
        if (!v[key].isBool()) return fail(key, "expected true/false");
        out = bool(v[key]);
        return true;
    }
    bool str(const char* key, juce::String& out, juce::StringArray allowed = {})
    {
        if (!v.hasProperty(key)) return true;
        if (!v[key].isString()) return fail(key, "expected a string");
        const auto s = v[key].toString();
        if (!allowed.isEmpty() && !allowed.contains(s)) return fail(key, "must be one of " + allowed.joinIntoString(", "));
        out = s;
        return true;
    }
    template <class T>
    bool list(const char* key, std::vector<T>& out, double lo, double hi)
    {
        if (!v.hasProperty(key)) return true;
        const auto* a = v[key].getArray();
        if (a == nullptr) return fail(key, "expected an array");
        std::vector<T> tmp;
        for (auto& x : *a) {
            if (!(x.isDouble() || x.isInt() || x.isInt64())) return fail(key, "expected numbers");
            const double d = double(x);
            if (!std::isfinite(d) || d < lo || d > hi) return fail(key, "value out of range");
            tmp.push_back(T(d));
        }
        out = std::move(tmp);
        return true;
    }
};

// Array of objects under `key`, each read by `readOne`, at most `maxCount`.
template <class T, class F>
bool objects(const juce::var& v, const char* key, std::vector<T>& out, int maxCount, const juce::String& ctx,
             juce::String& err, F readOne)
{
    if (!v.hasProperty(key)) return true;
    const auto* a = v[key].getArray();
    if (a == nullptr) { err = ctx + key + ": expected an array"; return false; }
    if (a->size() > maxCount) { err = ctx + key + ": at most " + juce::String(maxCount) + " allowed"; return false; }
    std::vector<T> tmp(size_t(a->size()));
    for (int i = 0; i < a->size(); ++i) {
        if (!(*a)[i].isObject()) { err = ctx + key + "/" + juce::String(i) + ": expected an object"; return false; }
        Reader r { (*a)[i], ctx + key + "/" + juce::String(i) + "/", err };
        if (!readOne(r, tmp[size_t(i)])) return false;
    }
    out = std::move(tmp);
    return true;
}

bool xo(const juce::var& v, const char* key, XoConfig& x, const juce::String& ctx, juce::String& err)
{
    if (!v.hasProperty(key)) return true;
    Reader r { v[key], ctx + key + "/", err };
    return r.str("type", x.type, kXoTypes) && r.num("freq", x.freq, 10, 24000);
}

bool fir(const juce::var& v, FirConfig& f, const juce::String& ctx, juce::String& err)
{
    if (!v.hasProperty("fir")) return true;
    Reader r { v["fir"], ctx + "fir/", err };
    return r.boolean("on", f.on) && r.str("path", f.path) && r.integer("taps", f.taps, 0, 1 << 20)
        && r.integer("latencySamples", f.latencySamples, 0, 1 << 20);
}

bool outputFromVar(const juce::var& v, OutputConfig& o, const juce::String& ctx, juce::String& err)
{
    if (!v.isObject()) { err = ctx + "expected an object"; return false; }
    Reader r { v, ctx, err };
    return r.str("name", o.name) && r.str("type", o.type) && r.str("colour", o.colour)
        && r.integer("id", o.id, 0, 1 << 30)
        && r.integer("channel", o.channel, -1, kMaxCh - 1)
        && r.num("gainDb", o.gainDb, -80, 12) && r.boolean("polarityInvert", o.polarityInvert)
        && r.num("delayMs", o.delayMs, 0, 1000) && r.boolean("mute", o.mute)
        && r.boolean("limiterOn", o.limiterOn) && r.num("limThreshDb", o.limThreshDb, -40, 0)
        && r.num("limAttackMs", o.limAttackMs, 0, 100) && r.num("limReleaseMs", o.limReleaseMs, 1, 5000)
        && r.list("mix", o.mix, 0.0, 4.0)
        && objects(v, "peq", o.peq, kMaxPeq, ctx, err, [&](Reader& b, PeqBand& p) {
               return b.boolean("on", p.on) && b.str("type", p.type, kPeqTypes) && b.num("freq", p.freq, 10, 24000)
                   && b.num("gainDb", p.gainDb, -30, 15) && b.num("q", p.q, 0.1, 30);
           })
        && objects(v, "ap", o.ap, kMaxAp, ctx, err, [&](Reader& b, ApBand& p) {
               return b.boolean("on", p.on) && b.integer("order", p.order, 1, 2) && b.num("freq", p.freq, 10, 24000)
                   && b.num("q", p.q, 0.1, 10);
           })
        && xo(v, "hp", o.hp, ctx, err) && xo(v, "lp", o.lp, ctx, err)
        && fir(v, o.fir, ctx, err);
}

// Pre-patch files (milestones 1–2): `inputs` with roles, outputs = physical channels,
// mixes indexed by physical input, measurement by physical input.
void migrateLegacy(const juce::var& v, SystemState& s)
{
    std::vector<int> map; // old input index -> source index
    if (const auto* a = v["inputs"].getArray())
        for (int i = 0; i < a->size(); ++i) {
            const auto role = (*a)[i]["role"].toString();
            // Keep inputs that were in use: routed somewhere, renamed, or a mic/loopback.
            bool routed = false;
            if (const auto* outs = v["outputs"].getArray())
                for (auto& o : *outs)
                    if (const auto* m = o["mix"].getArray(); m && i < m->size() && double((*m)[i]) > 0) routed = true;
            const bool used = role == "mic" || role == "loopback" || routed || (*a)[i]["name"].toString() != "In " + juce::String(i + 1);
            if (role == "none" || i >= kMaxCh || !used) { map.push_back(-1); continue; }
            map.push_back(int(s.sources.size()));
            s.sources.push_back({ (*a)[i]["name"].toString(), role == "console" ? "program" : role, i });
        }
    std::vector<OutputConfig> kept;
    for (size_t o = 0; o < s.outputs.size(); ++o) {
        auto out = s.outputs[o];
        out.channel = int(o);
        std::vector<float> mix(s.sources.size(), 0.0f);
        bool routed = false;
        for (size_t i = 0; i < out.mix.size() && i < map.size(); ++i)
            if (map[i] >= 0) { mix[size_t(map[i])] = out.mix[i]; routed |= out.mix[i] > 0; }
        out.mix = mix;
        // Keep only outputs that were actually in use: the old layout had one per channel.
        const OutputConfig untouched;
        const bool used = routed || out.name != "Out " + juce::String(int(o) + 1) || !out.peq.empty() || !out.ap.empty()
                       || out.hp.type != "off" || out.lp.type != "off" || out.fir.on || out.gainDb != untouched.gainDb
                       || out.delayMs != untouched.delayMs || out.polarityInvert;
        if (used) kept.push_back(out);
    }
    s.outputs = kept;
    s.gen.targets.clear();
    const auto& m = v["measure"];
    auto mapped = [&](const juce::var& x) { const int i = x.isVoid() ? -1 : int(x); return i >= 0 && i < int(map.size()) ? map[size_t(i)] : -1; };
    s.measure.micSource = mapped(m["micInput"]);
    if (m["ref"].toString() == "input") {
        s.measure.refSource = mapped(m["refInput"]);
        s.measure.ref = s.measure.refSource >= 0 ? "source" : "generator";
    }
}

} // namespace

juce::var sourcesToVar(const std::vector<SourceConfig>& src)
{
    juce::Array<juce::var> a;
    for (auto& x : src) {
        auto v = obj();
        v.getDynamicObject()->setProperty("name", x.name);
        v.getDynamicObject()->setProperty("type", x.type);
        v.getDynamicObject()->setProperty("channel", x.channel);
        a.add(v);
    }
    return a;
}

bool sourcesFromVar(const juce::var& v, std::vector<SourceConfig>& out, juce::String& err)
{
    auto holder = obj();
    holder.getDynamicObject()->setProperty("sources", v);
    return objects(holder, "sources", out, kMaxSources, "", err, [](Reader& r, SourceConfig& x) {
        return r.str("name", x.name) && r.str("type", x.type, { "program", "mic", "loopback" })
            && r.integer("channel", x.channel, -1, kMaxCh - 1);
    });
}

juce::var outputsToVar(const std::vector<OutputConfig>& outs)
{
    juce::Array<juce::var> a;
    for (auto& o : outs) a.add(outputToVar(o));
    return a;
}

juce::var toVar(const SystemState& s)
{
    auto root = obj();
    auto* r = root.getDynamicObject();

    auto dev = obj();
    dev.getDynamicObject()->setProperty("inputDevice", s.device.inputDevice);
    dev.getDynamicObject()->setProperty("outputDevice", s.device.outputDevice);
    dev.getDynamicObject()->setProperty("sampleRate", s.device.sampleRate);
    dev.getDynamicObject()->setProperty("bufferSize", s.device.bufferSize);
    r->setProperty("device", dev);

    r->setProperty("sources", sourcesToVar(s.sources));
    r->setProperty("outputs", outputsToVar(s.outputs));

    auto g = obj();
    g.getDynamicObject()->setProperty("on", s.gen.on);
    g.getDynamicObject()->setProperty("signal", s.gen.signal);
    g.getDynamicObject()->setProperty("levelDb", s.gen.levelDb);
    g.getDynamicObject()->setProperty("targets", arr(s.gen.targets));
    r->setProperty("gen", g);

    auto m = obj();
    m.getDynamicObject()->setProperty("micSource", s.measure.micSource);
    m.getDynamicObject()->setProperty("ref", s.measure.ref);
    m.getDynamicObject()->setProperty("refSource", s.measure.refSource);
    m.getDynamicObject()->setProperty("averaging", s.measure.averaging);
    m.getDynamicObject()->setProperty("smoothing", s.measure.smoothing);
    m.getDynamicObject()->setProperty("multiWindow", s.measure.multiWindow);
    juce::Array<juce::var> pos, groups;
    for (auto& p : s.measure.positions) pos.add(p);
    for (auto& g : s.measure.groups) {
        auto x = obj();
        x.getDynamicObject()->setProperty("name", g.name);
        x.getDynamicObject()->setProperty("outputs", arr(g.outputs));
        groups.add(x);
    }
    m.getDynamicObject()->setProperty("positions", pos);
    m.getDynamicObject()->setProperty("groups", groups);
    r->setProperty("measure", m);

    auto c = obj();
    auto lat = obj();
    for (auto& [k, v] : s.cal.ioLatency) lat.getDynamicObject()->setProperty(k, v);
    c.getDynamicObject()->setProperty("ioLatency", lat);
    auto spl = obj();
    for (auto& [k, v] : s.cal.splOffset) spl.getDynamicObject()->setProperty(k, v);
    c.getDynamicObject()->setProperty("splOffset", spl);
    c.getDynamicObject()->setProperty("micCalPath", s.cal.micCalPath);
    r->setProperty("cal", c);
    return root;
}

bool outputsFromVar(const juce::var& v, std::vector<OutputConfig>& out, juce::String& err)
{
    const auto* a = v.getArray();
    if (a == nullptr) { err = "outputs: expected an array"; return false; }
    if (a->size() > kMaxOutputs) { err = "outputs: at most " + juce::String(kMaxOutputs); return false; }
    std::vector<OutputConfig> tmp(size_t(a->size()));
    for (int i = 0; i < a->size(); ++i)
        if (!outputFromVar((*a)[i], tmp[size_t(i)], "outputs/" + juce::String(i) + "/", err)) return false;
    out = std::move(tmp);
    return true;
}

bool fromVar(const juce::var& v, SystemState& out, juce::String& err)
{
    if (!v.isObject()) { err = "state: expected an object"; return false; }
    SystemState s;

    if (v.hasProperty("device")) {
        Reader r { v["device"], "device/", err };
        if (!(r.str("inputDevice", s.device.inputDevice) && r.str("outputDevice", s.device.outputDevice)
              && r.num("sampleRate", s.device.sampleRate, 22050, 192000)
              && r.integer("bufferSize", s.device.bufferSize, 16, 4096))) return false;
    }
    if (v.hasProperty("sources") && !sourcesFromVar(v["sources"], s.sources, err)) return false;
    if (v.hasProperty("outputs") && !outputsFromVar(v["outputs"], s.outputs, err)) return false;
    const bool legacy = v.hasProperty("inputs") && !v.hasProperty("sources");
    if (legacy) migrateLegacy(v, s);
    if (v.hasProperty("gen")) {
        Reader r { v["gen"], "gen/", err };
        if (!(r.boolean("on", s.gen.on) && r.str("signal", s.gen.signal, { "pink" })
              && r.num("levelDb", s.gen.levelDb, -80, kGeneratorMaxDb)
              && r.list("targets", s.gen.targets, 0, kMaxCh - 1))) return false;
    }
    if (v.hasProperty("measure")) {
        Reader r { v["measure"], "measure/", err };
        if (!legacy && !(r.integer("micSource", s.measure.micSource, -1, kMaxSources - 1)
              && r.str("ref", s.measure.ref, { "generator", "source" })
              && r.integer("refSource", s.measure.refSource, -1, kMaxSources - 1))) return false;
        if (!(r.integer("averaging", s.measure.averaging, 1, 256)
              && r.integer("smoothing", s.measure.smoothing, 0, 48)
              && r.boolean("multiWindow", s.measure.multiWindow))) return false;
        if (const auto* pa = v["measure"]["positions"].getArray()) {
            if (pa->size() > 64) { err = "measure/positions: at most 64"; return false; }
            for (auto& p : *pa) s.measure.positions.push_back(p.toString());
        }
        if (!objects(v["measure"], "groups", s.measure.groups, 64, "measure/", err, [](Reader& g, MeasureGroup& x) {
                return g.str("name", x.name) && g.list("outputs", x.outputs, 0, kMaxOutputs - 1);
            })) return false;
    }
    if (v.hasProperty("cal")) {
        const auto& c = v["cal"];
        Reader r { c, "cal/", err };
        if (!r.str("micCalPath", s.cal.micCalPath)) return false;
        if (auto* lat = c["ioLatency"].getDynamicObject())
            for (auto& p : lat->getProperties()) {
                if (!(p.value.isDouble() || p.value.isInt())) { err = "cal/ioLatency: expected numbers"; return false; }
                s.cal.ioLatency[p.name.toString()] = double(p.value);
            }
        if (auto* spl = c["splOffset"].getDynamicObject())
            for (auto& p : spl->getProperties()) {
                if (!(p.value.isDouble() || p.value.isInt())) { err = "cal/splOffset: expected numbers"; return false; }
                s.cal.splOffset[p.name.toString()] = double(p.value);
            }
    }
    // Cross-checks: mix rows match the source list; references exist; one output per channel.
    for (auto& o : s.outputs) o.mix.resize(s.sources.size(), 0.0f);
    if (s.measure.micSource >= int(s.sources.size())) { err = "measure/micSource: no such source"; return false; }
    if (s.measure.refSource >= int(s.sources.size())) { err = "measure/refSource: no such source"; return false; }
    for (size_t a = 0; a < s.outputs.size(); ++a)
        for (size_t b = a + 1; b < s.outputs.size(); ++b)
            if (s.outputs[a].channel >= 0 && s.outputs[a].channel == s.outputs[b].channel) {
                err = "outputs/" + juce::String(int(b)) + "/channel: physical output " + juce::String(s.outputs[b].channel + 1)
                    + " is already used by \"" + s.outputs[a].name + "\"";
                return false;
            }
    // Every output has a unique id (new, migrated or duplicated outputs get one here).
    int nextId = 1;
    for (auto& o : s.outputs) nextId = std::max(nextId, o.id + 1);
    for (size_t a = 0; a < s.outputs.size(); ++a) {
        bool dup = s.outputs[a].id <= 0;
        for (size_t b = 0; b < a && !dup; ++b) dup = s.outputs[b].id == s.outputs[a].id;
        if (dup) s.outputs[a].id = nextId++;
    }
    for (auto& g : s.measure.groups) { // drop members that don't exist (any more)
        std::vector<int> kept;
        for (int o : g.outputs)
            if (o < int(s.outputs.size())) kept.push_back(o);
        g.outputs = kept;
    }
    std::vector<int> targets;
    for (int t : s.gen.targets)
        if (t < int(s.outputs.size())) targets.push_back(t);
    s.gen.targets = targets;
    out = std::move(s);
    return true;
}

bool applyPatch(SystemState& s, const juce::String& path, const juce::var& value, juce::String& err)
{
    auto root = toVar(s);
    auto keys = juce::StringArray::fromTokens(path, "/", "");
    keys.removeEmptyStrings();
    if (keys.isEmpty()) { err = "empty path"; return false; }

    // Walk to the parent container; every step must already exist.
    juce::var node = root;
    for (int i = 0; i < keys.size() - 1; ++i) {
        const auto& k = keys[i];
        if (auto* a = node.getArray()) {
            const int idx = k.getIntValue();
            if (!k.containsOnly("0123456789") || idx >= a->size()) { err = path + ": no such element"; return false; }
            node = (*a)[idx];
        } else if (node.isObject() && node.hasProperty(k)) {
            node = node[k.toRawUTF8()];
        } else { err = path + ": no such field"; return false; }
    }
    const auto& last = keys[keys.size() - 1];
    if (auto* a = node.getArray()) {
        const int idx = last.getIntValue();
        if (!last.containsOnly("0123456789") || idx >= a->size()) { err = path + ": no such element"; return false; }
        a->set(idx, value);
    } else if (auto* d = node.getDynamicObject()) {
        // ioLatency is a free-form map; everything else must exist already.
        const bool isMap = keys.size() >= 2 && keys[keys.size() - 2] == "ioLatency";
        if (!d->hasProperty(last) && !isMap) { err = path + ": no such field"; return false; }
        const auto& old = d->getProperty(last);
        const bool numOld = old.isDouble() || old.isInt() || old.isInt64();
        const bool numNew = value.isDouble() || value.isInt() || value.isInt64();
        if (!isMap && !(numOld && numNew) && !(old.isBool() && value.isBool())
            && !(old.isString() && value.isString()) && !(old.isArray() && value.isArray())
            && !(old.isObject() && value.isObject())) {
            err = path + ": wrong type";
            return false;
        }
        d->setProperty(last, value);
    } else { err = path + ": not a container"; return false; }

    return fromVar(root, s, err); // validates everything; s untouched on failure
}

Params toParams(const SystemState& s, double sampleRate)
{
    Params p;
    p.numOut = int(s.outputs.size());
    for (size_t o = 0; o < s.outputs.size(); ++o) {
        const auto& c = s.outputs[o];
        auto& q = p.out[o];
        q.gainDb = c.gainDb;
        q.polarityInvert = c.polarityInvert;
        q.delayMs = c.delayMs;
        q.mute = c.mute;
        q.limiterOn = c.limiterOn;
        q.limThreshDb = c.limThreshDb;
        q.limAttackMs = c.limAttackMs;
        q.limReleaseMs = c.limReleaseMs;
        q.channel = c.channel;
        q.slot = int(o); // default 1:1; the dispatcher maps stable ids to slots
        // Resolve the source mix to physical input gains. Only program sources can feed
        // an output: measurement mics and loopbacks never reach the PA.
        for (size_t i = 0; i < c.mix.size() && i < s.sources.size(); ++i) {
            const auto& src = s.sources[i];
            if (src.type == "program" && src.channel >= 0 && src.channel < kMaxCh) q.mix[src.channel] += c.mix[i];
        }
        designOutputIir(c, sampleRate, q.iir);
        q.firOn = c.fir.on && c.fir.taps > 0;
        q.firLoaded = c.fir.taps > 0;
    }
    for (const auto& src : s.sources)
        if (src.type == "program" && src.channel >= 0 && src.channel < kMaxCh) p.consoleInputs |= (1ull << src.channel);
    p.gen.on = s.gen.on;
    p.gen.levelDb = s.gen.levelDb;
    for (int t : s.gen.targets)
        if (t >= 0 && t < kMaxCh) p.gen.targets |= (1ull << t);
    auto channelOf = [&](int src) { return src >= 0 && src < int(s.sources.size()) ? s.sources[size_t(src)].channel : -1; };
    p.measure.micInput = channelOf(s.measure.micSource);
    p.measure.ref = s.measure.ref == "source" ? RefSource::PhysicalInput : RefSource::Generator;
    p.measure.refInput = channelOf(s.measure.refSource);
    return p;
}

} // namespace vt
