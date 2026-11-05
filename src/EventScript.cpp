#include "EventScript.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <tuple>

namespace es {

// ---------------------------------------------------------------------------------
// Opcode table (operand width + kind). Kinds: 0 none, 'u' unsigned, 's' signed,
// 'p' relocated pointer, 'f' raw u32 (int or float bits), 'd' f64, 'j' relative jump
// (from the operand start). Source: EternalSonataReprise docs/script-vm-notes.md §8,
// identical on the PS3 interpreter (sub_3B0B18).
struct OpInfo { uint8_t width; char kind; };
static OpInfo OpTable(uint8_t op) {
    switch (op) {
    case 0x01: case 0x05: return { 1,'u' };
    case 0x02: case 0x06: return { 2,'u' };
    case 0x03: return { 4,'f' };
    case 0x04: return { 8,'d' };
    case 0x07: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
    case 0x7a: case 0x7d: case 0x89: return { 4,'p' };
    case 0x08: return { 4,'s' };
    case 0x09: return { 1,'s' };
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: return { 4,'s' };
    case 0x16: case 0x17: case 0x18: case 0x19: case 0x1a: case 0x1b: return { 1,'s' };
    case 0x26: return { 4,'u' };
    case 0x27: return { 1,'u' };
    case 0x4c: case 0x4d: case 0x52: case 0x55: case 0x5a: return { 1,'u' };
    case 0x4e: case 0x4f: case 0x50: case 0x51: case 0x53:
    case 0x56: case 0x57: case 0x58: case 0x59: case 0x5b: return { 1,'s' };
    case 0x54: case 0x5c: return { 4,'s' };
    case 0x74: case 0x75: case 0x76: return { 4,'j' };
    case 0x77: case 0x78: case 0x79: return { 1,'j' };
    case 0x7b: case 0x7f: case 0x83: case 0x87: return { 4,'u' };
    case 0x80: case 0x84: case 0x88: return { 1,'u' };
    default: return { 0,0 };
    }
}

uint32_t Image::U32(uint32_t o) const {
    const auto& d = *m_File;
    if (o + 4 > d.size()) return 0;
    return ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) | ((uint32_t)d[o + 2] << 8) | d[o + 3];
}
uint16_t Image::U16(uint32_t o) const {
    const auto& d = *m_File;
    if (o + 2 > d.size()) return 0;
    return (uint16_t)((d[o] << 8) | d[o + 1]);
}
float Image::F32(uint32_t o) const { uint32_t u = U32(o); float f; std::memcpy(&f, &u, 4); return f; }
std::string Image::CStr(uint32_t o, size_t max) const {
    std::string s; const auto& d = *m_File;
    for (size_t i = 0; i < max && o + i < d.size() && d[o + i]; i++) s += (char)d[o + i];
    return s;
}
std::string Image::Tag(uint32_t o) const {
    const auto& d = *m_File;
    if (o + 4 > d.size()) return "";
    return std::string((const char*)&d[o], 4);
}
std::string Image::ModelName(uint32_t o) const {
    std::string t = Tag(o);
    if (t == "NMDL") return CStr(o + 0x10, 16);
    if (t != "NOBJ") return "";
    uint32_t sz = U32(o + 4), p = o + 8;
    while (p + 8 <= o + sz) {
        std::string ct = Tag(p); uint32_t cs = U32(p + 4);
        if (ct == "NMDL") return CStr(p + 0x10, 16);
        if (cs < 8) break;
        p += cs; while (p & 3) p++;
    }
    return "";
}

bool Image::Load(const std::vector<uint8_t>& file) {
    m_File = &file; valid = false;
    if (file.size() < 0x18) return false;
    if ((U32(0) & ~1u) != 0x180) return false;
    if (U32(0x0C) != file.size()) return false;
    m_ImageEnd = 0x18 + U32(0x10);
    m_RelocBase = m_ImageEnd + U32(0x14);
    if (m_RelocBase + 8 > file.size()) return false;
    uint32_t o = m_RelocBase;
    auto list = [&](std::vector<uint32_t>& out) {
        uint32_t n = U32(o); o += 4;
        if (o + 4ull * n > file.size()) return false;
        out.resize(n);
        for (uint32_t i = 0; i < n; i++) out[i] = U32(o + 4 * i);
        o += 4 * n; return true;
        };
    if (!list(m_RelA) || !list(m_RelB)) return false;
    for (int t = 0; t < 4; t++) {
        uint32_t n = U32(o); o += 4;
        if (o + 8ull * n > file.size()) return false;
        m_Tables[t].resize(n);
        for (uint32_t i = 0; i < n; i++) m_Tables[t][i] = { U32(o + 8 * i), U32(o + 8 * i + 4) };
        o += 8 * n;
    }
    m_IsA.assign(m_ImageEnd + 4, false); m_IsB.assign(m_ImageEnd + 4, false);
    for (uint32_t s : m_RelA) if (s < m_IsA.size()) m_IsA[s] = true;
    for (uint32_t s : m_RelB) if (s < m_IsB.size()) m_IsB[s] = true;
    m_NativeAt.clear(); m_ImportAt.clear();
    for (auto& e : m_Tables[0]) m_NativeAt[e.second] = e.first;
    for (auto& e : m_Tables[1]) m_ImportAt[e.second] = e.first;
    valid = true;
    return true;
}

Ptr Image::ResolvePtr(uint32_t site) const {
    Ptr p; uint32_t v = U32(site);
    auto n = m_NativeAt.find(site);
    if (n != m_NativeAt.end()) { p.kind = PtrKind::Native; p.value = n->second; return p; }
    auto i = m_ImportAt.find(site);
    if (i != m_ImportAt.end()) { p.kind = PtrKind::Import; p.value = i->second; return p; }
    if (site < m_IsB.size() && m_IsB[site]) { p.kind = PtrKind::Bulk; p.value = v; p.fileoff = m_ImageEnd + v; return p; }
    if (site < m_IsA.size() && m_IsA[site]) { p.kind = PtrKind::Image; p.value = v; p.fileoff = v; return p; }
    p.kind = PtrKind::Raw; p.value = v; p.fileoff = v; return p;
}

uint32_t Image::ExportOffset(uint32_t sym) const {
    for (auto& e : m_Tables[2]) if (e.first == sym) return e.second;
    return 0;
}

static Val NumVal(double v, uint32_t raw) {
    Val r; r.kind = Val::Num; r.num = v; r.raw = raw; r.has_lead = true; r.lead = v; return r;
}

std::vector<Call> Image::Decompile(uint32_t start, uint32_t end) const {
    std::vector<Call> out;
    const auto& d = *m_File;
    end = std::min<uint32_t>(end, m_ImageEnd);
    std::vector<Val> stack; Val acc;
    uint32_t o = start;
    while (o < end) {
        uint8_t op = d[o]; OpInfo oi = OpTable(op); uint32_t a = o + 1;
        if (a + oi.width > d.size()) break;
        auto pops = [&]() { Val v; if (!stack.empty()) { v = stack.back(); stack.pop_back(); } return v; };
        switch (op) {
        case 0x01: acc = NumVal(d[a], d[a]); break;
        case 0x02: acc = NumVal(U16(a), U16(a)); break;
        case 0x05: acc = NumVal(-(double)d[a], 0); break;
        case 0x06: acc = NumVal(-(double)U16(a), 0); break;
        case 0x03: {
            uint32_t raw = U32(a); float f = F32(a);
            double v = (raw >= 0x10000 && std::isfinite(f) && std::fabs(f) > 1e-6 && std::fabs(f) < 1e7) ? f : (double)raw;
            if (raw == 0) v = 0.0;
            acc = NumVal(v, raw); break;
        }
        case 0x04: { uint64_t u = ((uint64_t)U32(a) << 32) | U32(a + 4); double dv; std::memcpy(&dv, &u, 8); acc = NumVal(dv, 0); break; }
        case 0x07: acc = Val(); acc.kind = Val::PtrV; acc.ptr = ResolvePtr(a); break;
        case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f: {
            Ptr p = ResolvePtr(a); acc = Val(); acc.kind = Val::Expr;
            if (p.kind == PtrKind::Native && p.value >= 500 && p.value < 600) { acc.is_global_deref = true; acc.global_id = p.value; }
            break;
        }
        case 0x81: case 0x82: stack.push_back(acc); break;
        case 0x83: case 0x84: stack.push_back(Val()); break;
        case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: pops(); break;
        case 0x7a: case 0x7d: {
            Call c; c.type = (op == 0x7a) ? Call::CallFn : Call::Native; c.site = o; c.fn = ResolvePtr(a);
            uint32_t nb = 0, n2 = a + 4;
            if (n2 < end) {
                uint8_t op2 = d[n2];
                if (op2 == 0x88) nb = d[n2 + 1];
                else if (op2 == 0x87) nb = U32(n2 + 1);
                else if (op2 == 0x85) nb = 4;
                else if (op2 == 0x86) nb = 8;
            }
            size_t na = std::min<size_t>(nb / 4, stack.size());
            for (size_t i = 0; i < na; i++) c.args.push_back(stack[stack.size() - 1 - i]);
            stack.resize(stack.size() - na);
            out.push_back(c);
            acc = Val(); acc.kind = Val::Ret; acc.ptr = c.fn;
            break;
        }
        case 0x7e: {
            Call c; c.type = Call::Sleep; c.site = o;
            if (acc.kind == Val::Num) c.sleep_frames = acc.num;
            else if (acc.has_lead) c.sleep_ticks = acc.lead;
            out.push_back(c); break;
        }
        case 0x7c: { Call c; c.type = Call::Return; c.site = o; out.push_back(c); break; }
        case 0x74: case 0x77: case 0x75: case 0x78: case 0x76: case 0x79: {
            int32_t rel = (oi.width == 4) ? (int32_t)U32(a) : (int8_t)d[a];
            Call c; c.site = o; c.target = a + rel;
            c.type = (op == 0x74 || op == 0x77) ? Call::Goto : Call::Branch; c.cond = acc;
            out.push_back(c); break;
        }
        default:
            if ((op >= 0x32 && op <= 0x47) || (op >= 0x5d && op <= 0x72)) {
                Val l = pops(); Val r = acc; Val e; e.kind = Val::Expr;
                if (l.has_lead) { e.has_lead = true; e.lead = l.lead; }
                else if (r.has_lead) { e.has_lead = true; e.lead = r.lead; }
                if ((op == 0x5d || op == 0x5e || op == 0x5f) && l.is_global_deref && r.kind == Val::Num) {
                    e.is_cmp_eq_global = true; e.cmp_const = r.num; e.global_id = l.global_id;
                }
                acc = e;
            }
            else if ((op >= 0x48 && op <= 0x4b) || op == 0x73 || (op >= 0x4c && op <= 0x5c) || (op >= 0x10 && op <= 0x21) || op == 0x08 || op == 0x09) {
                Val e; e.kind = Val::Expr; acc = e;
            }
            break;
        }
        o = a + oi.width;
    }
    return out;
}

// ---------------------------------------------------------------------------------
// BTX

std::string BtxString(const Image& img, uint32_t btx, uint32_t id, const std::string& lang) {
    if (img.Tag(btx) != "BTX ") return "";
    uint32_t q = btx + img.U32(btx + 4), n = img.U32(btx + 0x0C);
    std::string want = lang; while (want.size() < 4) want += ' ';
    for (uint32_t i = 0; i < n && i < 16; i++) {
        std::string fc = img.Tag(q);
        uint32_t etab = img.U32(q + 4), nxt = img.U32(q + 8), cnt = img.U32(q + 0x10);
        if (fc == want) {
            for (uint32_t k = 0; k < cnt; k++) {
                uint32_t e = q + etab + 8 * k;
                if (img.U32(e) == id) return img.CStr(q + img.U32(e + 4), 2048);
            }
            return "";
        }
        if (!nxt) break;
        q += nxt;
    }
    return "";
}

// ---------------------------------------------------------------------------------
// Timeline

const char* ObjKindName(ObjKind k) {
    switch (k) {
    case ObjKind::Actor: return "actor"; case ObjKind::Model: return "model";
    case ObjKind::Effect: return "effect"; case ObjKind::Camera: return "camera";
    case ObjKind::Light4: return "light (point)"; case ObjKind::Light5: return "light (directional)";
    case ObjKind::Sound: return "sound"; default: return "?";
    }
}
const char* RecordName(EvRecord::Type t) {
    static const char* n[] = { "pos","rot","target","scale","campar","camera","motion","lookat","lookat_obj",
        "lookat_release","moveto","attach_bone","attach","light_clear","light_link","text","say",
        "effect_cmd","fade_set","fade_in","fade_out","music","delete","node_visible","render_flag","flash","other" };
    return n[(int)t];
}

static double NumOf(const std::vector<Val>& a, size_t i, double def = 0.0) {
    return (i < a.size() && a[i].kind == Val::Num) ? a[i].num : def;
}
static uint32_t RawOf(const std::vector<Val>& a, size_t i) {
    return (i < a.size() && a[i].kind == Val::Num) ? a[i].raw : 0;
}
static uint32_t PtrOf(const std::vector<Val>& a, size_t i) {
    return (i < a.size() && a[i].kind == Val::PtrV && a[i].ptr.kind == PtrKind::Bulk) ? a[i].ptr.fileoff : 0;
}

// Main event function: entry (+0x18) calls a stub, the stub calls the event body.
static uint32_t FindMain(const Image& img) {
    auto entry = img.Decompile(0x18, std::min<uint32_t>(img.ImageEnd(), 0x40));
    for (auto& c : entry) if (c.type == Call::CallFn && c.fn.kind == PtrKind::Image) {
        auto stub = img.Decompile(c.fn.fileoff, img.ImageEnd());
        for (auto& s : stub) {
            if (s.type == Call::CallFn && s.fn.kind == PtrKind::Image) return s.fn.fileoff;
            if (s.type == Call::Return) break;
        }
        return c.fn.fileoff;
    }
    return 0x1e;
}

EventTimeline BuildTimeline(const Image& img, const std::string& lang) {
    EventTimeline tl;
    if (!img.valid) return tl;
    tl.main_fn = FindMain(img);
    auto calls = img.Decompile(tl.main_fn, img.ImageEnd());
    double t = 0.0;
    auto obj = [&](double id) -> EvObject& { uint16_t i = (uint16_t)id; EvObject& o = tl.objects[i]; o.id = i; return o; };
    auto rec = [&](EvRecord::Type ty, double id) -> EvRecord& {
        tl.records.emplace_back(); EvRecord& r = tl.records.back(); r.type = ty; r.t = t; r.obj = (uint16_t)id; return r; };
    for (auto& c : calls) {
        if (c.type == Call::Return) break;
        if (c.type == Call::Sleep) {
            if (c.sleep_ticks >= 0) t += c.sleep_ticks / 300.0;
            else if (c.sleep_frames >= 0) t += c.sleep_frames / 60.0;
            continue;
        }
        if (c.type != Call::CallFn && c.type != Call::Native) continue;
        const auto& a = c.args;
        const bool imp = c.fn.kind == PtrKind::Import, nat = c.fn.kind == PtrKind::Native;
        const uint32_t id = c.fn.value;
        if (imp) {
            switch (id) {
            case 20043: { EvObject& o = obj(NumOf(a, 0)); o.kind = ObjKind::Actor; o.char_id = (int)NumOf(a, 1); o.t_create = t; break; }
            case 20042: case 20049: case 20053: {
                EvObject& o = obj(NumOf(a, 0)); o.ref = PtrOf(a, 1); o.tag = img.Tag(o.ref); o.t_create = t;
                o.kind = (o.tag == "Mefc") ? ObjKind::Effect : ObjKind::Model; o.name = img.ModelName(o.ref); break;
            }
            case 20044: obj(NumOf(a, 0)).kind = ObjKind::Camera; obj(NumOf(a, 0)).t_create = t; break;
            case 20045: obj(NumOf(a, 0)).kind = ObjKind::Light4; obj(NumOf(a, 0)).t_create = t; break;
            case 20046: obj(NumOf(a, 0)).kind = ObjKind::Light5; obj(NumOf(a, 0)).t_create = t; break;
            case 20047: obj(NumOf(a, 0)).kind = ObjKind::Sound; obj(NumOf(a, 0)).t_create = t; break;
            case 20048: rec(EvRecord::Delete, NumOf(a, 0)); break;
            case 15: case 16: case 20: case 19: {
                EvRecord& r = rec(id == 15 ? EvRecord::Pos : id == 16 ? EvRecord::Rot : id == 20 ? EvRecord::Target : EvRecord::Scale, NumOf(a, 0));
                for (int k = 0; k < 3; k++) r.v[k] = (float)NumOf(a, 1 + k);
                break;
            }
            case 20055: {
                EvRecord& p = rec(EvRecord::Pos, NumOf(a, 0)); for (int k = 0; k < 3; k++) p.v[k] = (float)NumOf(a, 1 + k);
                EvRecord& r = rec(EvRecord::Rot, NumOf(a, 0)); for (int k = 0; k < 3; k++) r.v[k] = (float)NumOf(a, 4 + k);
                EvRecord& s = rec(EvRecord::Scale, NumOf(a, 0)); for (int k = 0; k < 3; k++) s.v[k] = (float)NumOf(a, 7 + k, 1.0);
                break;
            }
            case 20056: case 20057: {
                EvObject& o = obj(NumOf(a, 0));
                for (int k = 0; k < 3; k++) { o.light_pos[k] = (float)NumOf(a, 1 + k); o.light_rot[k] = (float)NumOf(a, 4 + k); }
                if (id == 20056) o.light_color = RawOf(a, 7);
                break;
            }
            case 20060: case 20061: {
                EvRecord& r = rec(EvRecord::Motion, NumOf(a, 0));
                r.nmtn = (id == 20060) ? PtrOf(a, 1) : 0;
                r.flags = (uint32_t)NumOf(a, 2); r.blend_ticks = (int)NumOf(a, 3);
                r.start_ticks = (int)NumOf(a, 4); r.speed = (float)NumOf(a, 5, 1.0);
                if (r.nmtn) r.str = img.CStr(r.nmtn + 0x10, 16);
                else { r.str = "model motion #" + std::to_string((int)NumOf(a, 1)); }
                break;
            }
            case 20062: { EvRecord& r = rec(EvRecord::LookAt, NumOf(a, 0)); for (int k = 0; k < 3; k++) r.v[k] = (float)NumOf(a, 1 + k); r.flags = (uint32_t)NumOf(a, 4); r.speed = (float)NumOf(a, 5); break; }
            case 20063: { EvRecord& r = rec(EvRecord::LookAtObj, NumOf(a, 0)); r.other = (uint16_t)NumOf(a, 1); break; }
            case 105: { EvRecord& r = rec(EvRecord::LookAtRelease, NumOf(a, 0)); r.speed = (float)NumOf(a, 2); break; }
            case 76: { EvRecord& r = rec(EvRecord::MoveTo, NumOf(a, 0)); for (int k = 0; k < 3; k++) r.v[k] = (float)NumOf(a, 1 + k); r.flags = (uint32_t)NumOf(a, 4); r.blend = (float)NumOf(a, 5); break; }
            case 20058: { EvRecord& r = rec(EvRecord::AttachBone, NumOf(a, 0)); r.other = (uint16_t)NumOf(a, 1); r.idx = (int)NumOf(a, 3); for (int k = 0; k < 6; k++) r.v[k] = (float)NumOf(a, 4 + k); break; }
            case 20054: rec(EvRecord::LightClear, NumOf(a, 0)); break;
            case 20067: rec(EvRecord::Say, NumOf(a, 0)); break;
            case 11: { EvRecord& r = rec(EvRecord::FadeIn, 0); r.blend = (float)NumOf(a, 0); break; }
            case 12: { EvRecord& r = rec(EvRecord::FadeSet, 0); r.blend = (float)NumOf(a, 0); r.color = RawOf(a, 1); break; }
            case 14: { EvRecord& r = rec(EvRecord::FadeOut, 0); r.blend = (float)NumOf(a, 0); break; }
            // sym20038(layer, seconds, ARGB, x) -> N1120(layer, -1 = from current, ARGB,
            // seconds): screen-layer fade (layer 4 is the black fade of sym11/12/14).
            // The slideshow calls it on layer 1 just before each picture change.
            case 20038: { EvRecord& r = rec(EvRecord::Flash, 0); r.idx = (int)NumOf(a, 0); r.blend = (float)NumOf(a, 1); r.color = RawOf(a, 2); break; }
            case 20065: case 20004: case 20005: case 20007: case 20008: break;  // voice banks / event begin+end
            default: tl.unresolved["sym" + std::to_string(id)]++; break;
            }
        }
        else if (nat) {
            switch (id) {
            case 1023: { EvRecord& r = rec(EvRecord::CamActivate, NumOf(a, 0)); r.blend = (float)NumOf(a, 1); r.curve = (float)NumOf(a, 2); break; }
            case 1025: { EvRecord& r = rec(EvRecord::CamParam, NumOf(a, 0)); r.idx = (int)NumOf(a, 1); r.v[0] = (float)NumOf(a, 2); break; }
            case 1055: { EvRecord& r = rec(EvRecord::Attach, NumOf(a, 0)); r.other = (uint16_t)NumOf(a, 1); break; }
            case 1098: { EvRecord& r = rec(EvRecord::LightLink, NumOf(a, 0)); r.other = (uint16_t)NumOf(a, 1); break; }
            case 1054: { EvRecord& r = rec(EvRecord::EffectCmd, NumOf(a, 0)); r.str = img.CStr(PtrOf(a, 1), 8); break; }
            case 1074: { EvRecord& r = rec(EvRecord::NodeVisible, NumOf(a, 0)); r.str = img.CStr(PtrOf(a, 1), 16); r.flags = (uint32_t)NumOf(a, 2); break; }
            case 219: {
                EvRecord& r = rec(EvRecord::Text, 0); r.text_id = (int)NumOf(a, 1);
                r.str = BtxString(img, PtrOf(a, 0), (uint32_t)r.text_id, lang); break;
            }
            case 2000: { EvRecord& r = rec(EvRecord::Music, 0); r.str = img.CStr(PtrOf(a, 0), 16); break; }
            case 115: break;  // frame rate (only feeds the sleep conversion)
            // Object flag setters. 1028(obj, b): b != 0 sets flags 0x08001000 (hides a
            // slideshow image), b == 0 clears them; 1097(obj, n): 2D draw priority.
            case 1079: case 1095: case 1097: case 1050: case 1028: case 1099: {
                EvRecord& r = rec(EvRecord::RenderFlag, NumOf(a, 0)); r.str = "N" + std::to_string(id);
                r.idx = (int)id; r.v[0] = (float)NumOf(a, 1); r.v[1] = (float)NumOf(a, 2); break;
            }
            default: tl.unresolved["N" + std::to_string(id)]++; break;
            }
        }
    }
    tl.duration = t;
    tl.valid = true;
    return tl;
}

std::vector<std::string> VerifyLib(const Image& lib) {
    struct Exp { uint32_t sym; uint32_t native; };
    static const Exp checks[] = {
        {15,1036},{16,1037},{20,1046},{20042,1062},{20043,1141},{20043,1062},{20044,1062},
        {20045,1062},{20046,1062},{20048,1063},{20054,1098},{20060,1113},{11,1120},{12,1120} };
    std::vector<std::string> bad;
    for (const Exp& e : checks) {
        uint32_t off = lib.ExportOffset(e.sym);
        if (!off) { bad.push_back("lib.e: export sym" + std::to_string(e.sym) + " missing"); continue; }
        // Scan to the next export: bodies may return early (sym20060 bails out on a null motion).
        uint32_t end = lib.ImageEnd();
        for (auto& x : lib.Table(2)) if (x.second > off && x.second < end) end = x.second;
        bool ok = false;
        for (auto& c : lib.Decompile(off, end))
            if (c.type == Call::Native && c.fn.value == e.native) { ok = true; break; }
        if (!ok) bad.push_back("lib.e: sym" + std::to_string(e.sym) + " no longer calls N" + std::to_string(e.native));
    }
    return bad;
}

// ---------------------------------------------------------------------------------
// Map setup

static void CollectSpawns(const Image& map, uint32_t fn, int depth, std::vector<MapObject>& out) {
    auto calls = map.Decompile(fn, map.ImageEnd());
    MapObject* cur = nullptr;
    for (auto& c : calls) {
        if (c.type == Call::Return) break;
        if (c.type != Call::CallFn) continue;
        if (c.fn.kind == PtrKind::Image) {
            if (depth < 3) CollectSpawns(map, c.fn.fileoff, depth + 1, out);
            cur = nullptr; continue;
        }
        if (c.fn.kind != PtrKind::Import) continue;
        uint32_t id = c.fn.value;
        if (id == 20053 || id == 20049 || id == 20042) {
            MapObject m; m.ref = PtrOf(c.args, 1); m.tag = map.Tag(m.ref); m.name = map.ModelName(m.ref);
            out.push_back(m); cur = &out.back();
        }
        else if (cur && (id == 15 || id == 16 || id == 19)) {
            float* dst = id == 15 ? cur->pos : id == 16 ? cur->rot : cur->scale;
            for (int k = 0; k < 3; k++) dst[k] = (float)NumOf(c.args, 1 + k, id == 19 ? 1.0 : 0.0);
        }
    }
}

MapSetup ExtractMapSetup(const Image& map, uint32_t event_no, const std::string& map_stem) {
    MapSetup ms;
    if (!map.valid) return ms;
    auto calls = map.Decompile(0x18, map.ImageEnd());
    for (size_t i = 0; i < calls.size(); i++) {
        const Call& c = calls[i];
        if (c.type != Call::Branch || !c.cond.is_cmp_eq_global || (uint32_t)c.cond.cmp_const != event_no) continue;
        ms.found_branch = true; ms.branch_site = c.site;
        // Taken when the comparison holds: the fall-through block up to the jump target.
        for (size_t j = i + 1; j < calls.size() && calls[j].site < c.target; j++)
            if (calls[j].type == Call::CallFn && calls[j].fn.kind == PtrKind::Image)
                CollectSpawns(map, calls[j].fn.fileoff, 1, ms.objects);
        break;
    }
    std::string stem; for (char ch : map_stem) stem += (char)tolower((unsigned char)ch);
    const auto& d = map.Data();
    for (uint32_t p = map.ImageEnd(); p + 8 < map.RelocBase(); p += 4) {
        if (d[p] != 'N' || d[p + 1] != 'O' || d[p + 2] != 'B' || d[p + 3] != 'J') continue;
        std::string n = map.ModelName(p), lo;
        for (char ch : n) lo += (char)tolower((unsigned char)ch);
        if (lo == stem) { ms.default_map = p; break; }
    }
    return ms;
}

// ---------------------------------------------------------------------------------
// Event -> map index

std::string MapStemFromPath(const std::string& path) {
    size_t sl = path.find_last_of("/\\");
    std::string s = sl == std::string::npos ? path : path.substr(sl + 1);
    size_t dot = s.find('.'); if (dot != std::string::npos) s = s.substr(0, dot);
    for (char& c : s) c = (char)tolower((unsigned char)c);
    if (s.rfind("cfdata_", 0) == 0) s = s.substr(7);   // flattened dumps: "cfdata_agn01.e"
    return s;
}

bool EventNumberFromPath(const std::string& path, uint32_t& event, uint32_t& part) {
    size_t sl = path.find_last_of("/\\");
    std::string b = sl == std::string::npos ? path : path.substr(sl + 1);
    for (char& c : b) c = (char)tolower((unsigned char)c);
    // the last "e" followed by four digits (handles prefixes such as "cfdata_e2010_010.e")
    size_t pos = std::string::npos;
    for (size_t i = 0; i + 4 < b.size(); i++)
        if (b[i] == 'e' && isdigit((unsigned char)b[i + 1]) && isdigit((unsigned char)b[i + 2]) &&
            isdigit((unsigned char)b[i + 3]) && isdigit((unsigned char)b[i + 4]) && (i == 0 || !isalpha((unsigned char)b[i - 1])))
            pos = i;
    if (pos == std::string::npos) return false;
    event = (uint32_t)atoi(b.c_str() + pos + 1);
    part = 0;
    if (pos + 5 < b.size() && b[pos + 5] == '_' && isdigit((unsigned char)b[pos + 6])) part = (uint32_t)atoi(b.c_str() + pos + 6);
    return true;
}

std::vector<EventRef> ScanMapEvents(const std::string& path) {
    std::vector<EventRef> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    auto rd = [&](std::vector<uint8_t>& v, uint32_t off, uint32_t n) {
        if (off + (uint64_t)n > v.size()) return false;
        fseek(f, (long)off, SEEK_SET); return fread(v.data() + off, 1, n, f) == n; };
    uint8_t h[24];
    if (fread(h, 1, 24, f) != 24) { fclose(f); return out; }
    auto be = [](const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; };
    uint32_t total = be(h + 12), imend = 0x18 + be(h + 16), rbase = imend + be(h + 20);
    fseek(f, 0, SEEK_END); long fsz = ftell(f);
    if ((be(h) & ~1u) != 0x180 || (long)total != fsz || rbase > total || imend > rbase) { fclose(f); return out; }
    std::vector<uint8_t> d(total, 0);
    if (!rd(d, 0, imend) || !rd(d, rbase, total - rbase)) { fclose(f); return out; }
    Image img;
    if (!img.Load(d)) { fclose(f); return out; }
    std::string self = MapStemFromPath(path);
    for (const Call& c : img.Decompile(0x18, img.ImageEnd())) {
        if (c.type == Call::Branch && c.cond.is_cmp_eq_global && c.cond.cmp_const > 0 && c.cond.cmp_const < 10000) {
            EventRef r; r.kind = EventRef::Branch; r.event = (uint32_t)c.cond.cmp_const; r.map = self; r.from = self;
            out.push_back(r);
        }
        else if (c.type == Call::CallFn && c.fn.kind == PtrKind::Import && c.fn.value == 20018 && c.args.size() >= 4) {
            EventRef r; r.kind = EventRef::Start; r.from = self;
            r.event = (uint32_t)NumOf(c.args, 2); r.part = (uint32_t)NumOf(c.args, 3);
            if (!r.event) continue;
            uint32_t s = PtrOf(c.args, 0);
            if (s && rd(d, s, 16)) {
                std::string m = img.CStr(s, 16); size_t dot = m.find('.');
                r.map = dot == std::string::npos ? m : m.substr(0, dot);
                for (char& ch : r.map) ch = (char)tolower((unsigned char)ch);
            }
            else r.map = self;
            out.push_back(r);
        }
    }
    fclose(f);
    // one entry per (kind, event, part, map)
    std::sort(out.begin(), out.end(), [](const EventRef& a, const EventRef& b) {
        return std::tie(a.event, a.part, a.map, a.kind) < std::tie(b.event, b.part, b.map, b.kind); });
    out.erase(std::unique(out.begin(), out.end(), [](const EventRef& a, const EventRef& b) {
        return a.event == b.event && a.part == b.part && a.map == b.map && a.kind == b.kind; }), out.end());
    return out;
}

} // namespace es
