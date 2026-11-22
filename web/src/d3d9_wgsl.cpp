// Direct3D 9 shader bytecode in WGSL (d3d9_wgsl.h).
//
// The token stream (the DirectX SDK's d3d9types.h, D3DSHADER_*): a version token, then instructions until the end token.
// An instruction is an opcode token (its length in tokens in bits 24..27, from Shader Model 2 on), a destination token,
// a predicate token when it is predicated, then its sources; a source read through a register (relative addressing) is
// followed by a token naming that register. Comments carry the constant table (CTAB), whose arrays relative addressing
// reads.
//
// Each register is a module variable. An instruction computes only the components its destination's write mask selects
// (the sources' swizzles narrowed to them), or a scalar for all of them. Chrome keeps about a megabyte per module, which
// grows with the number of statements, not the length of each.
#include "d3d9_wgsl.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <set>

namespace bo1gpu
{
enum RegType : uint8_t
{
    REG_TEMP = 0,
    REG_INPUT = 1,
    REG_CONST = 2,
    REG_ADDR = 3, // vertex shaders; pixel shaders: REG_TEXTURE
    REG_TEXTURE = 3,
    REG_RASTOUT = 4,
    REG_ATTROUT = 5,
    REG_OUTPUT = 6, // vs_3_0; vs_2_0: texture coordinate outputs
    REG_CONSTINT = 7,
    REG_COLOROUT = 8,
    REG_DEPTHOUT = 9,
    REG_SAMPLER = 10,
    REG_CONST2 = 11,
    REG_CONST3 = 12,
    REG_CONST4 = 13,
    REG_CONSTBOOL = 14,
    REG_LOOP = 15,
    REG_MISCTYPE = 17,
    REG_LABEL = 18,
    REG_PREDICATE = 19,
};

enum Opcode : uint16_t
{
    OP_NOP = 0,
    OP_MOV,
    OP_ADD,
    OP_SUB,
    OP_MAD,
    OP_MUL,
    OP_RCP,
    OP_RSQ,
    OP_DP3,
    OP_DP4,
    OP_MIN,
    OP_MAX,
    OP_SLT,
    OP_SGE,
    OP_EXP,
    OP_LOG,
    OP_LIT,
    OP_DST,
    OP_LRP,
    OP_FRC,
    OP_M4x4,
    OP_M4x3,
    OP_M3x4,
    OP_M3x3,
    OP_M3x2,
    OP_CALL,
    OP_CALLNZ,
    OP_LOOP,
    OP_RET,
    OP_ENDLOOP,
    OP_LABEL,
    OP_DCL,
    OP_POW,
    OP_CRS,
    OP_SGN,
    OP_ABS,
    OP_NRM,
    OP_SINCOS,
    OP_REP,
    OP_ENDREP,
    OP_IF,
    OP_IFC,
    OP_ELSE,
    OP_ENDIF,
    OP_BREAK,
    OP_BREAKC,
    OP_MOVA,
    OP_DEFB,
    OP_DEFI,
    OP_TEXCOORD = 64,
    OP_TEXKILL,
    OP_TEX,
    OP_EXPP = 78,
    OP_LOGP,
    OP_CND,
    OP_DEF,
    OP_CMP = 88,
    OP_BEM,
    OP_DP2ADD,
    OP_DSX,
    OP_DSY,
    OP_TEXLDD,
    OP_SETP,
    OP_TEXLDL,
    OP_BREAKP,
    OP_PHASE = 0xFFFD,
    OP_COMMENT = 0xFFFE,
    OP_END = 0xFFFF,
};

enum
{
    USAGE_POSITION = 0,
    USAGE_TEXCOORD = 5,
    USAGE_COLOR = 10,
};

struct Src
{
    uint8_t type;
    uint16_t num;
    uint8_t swz; // 2 bits a component
    uint8_t mod;
    uint8_t rel; // read through a0 / aL
    uint8_t relType;
    uint8_t relComp;
};

struct Dst
{
    uint8_t type;
    uint16_t num;
    uint8_t mask;
    uint8_t mod; // 1 saturate
    uint8_t rel;
};

struct ShaderInstr
{
    uint16_t op;
    uint8_t ctrl;
    uint8_t pred;
    uint8_t hasDst;
    uint8_t nsrc;
    Dst dst;
    Src predSrc;
    Src src[4];
    uint32_t lit[4];
};

// a register a declaration names, and what it means
struct Decl
{
    uint8_t type;
    uint16_t num;
    uint8_t mask;
    uint8_t usage, index;
};

// what ShaderEmit needs besides the instructions
struct Parsed
{
    std::vector<ShaderInstr> instrs;
    std::vector<Decl> decls;
    std::set<int> temps;
    std::map<int, int> floatSlot;         // float constant register -> slot in c
    std::map<int, std::vector<float>> defF; // def c#
    std::map<int, std::vector<int>> defI;   // defi i#
    std::map<int, bool> defB;               // defb b#
    std::set<int> labels;
    int samplerDim[16] = {};
    bool samplerDeclared[16] = {};
};

ShaderInfo::~ShaderInfo() { delete (Parsed *)instrs; }

int VaryingLocation(int usage, int index)
{
    if (usage == USAGE_TEXCOORD && index < 10)
        return index;
    if (usage == USAGE_COLOR && index < 2)
        return 10 + index;
    return -1;
}

static bool HasDest(uint16_t op)
{
    switch (op)
    {
    case OP_NOP:
    case OP_CALL:
    case OP_CALLNZ:
    case OP_LOOP:
    case OP_RET:
    case OP_ENDLOOP:
    case OP_LABEL:
    case OP_REP:
    case OP_ENDREP:
    case OP_IF:
    case OP_IFC:
    case OP_ELSE:
    case OP_ENDIF:
    case OP_BREAK:
    case OP_BREAKC:
    case OP_BREAKP:
        return false;
    default:
        return true;
    }
}

static uint8_t RegTypeOf(uint32_t t) { return (uint8_t)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }

static void FoldConst(uint8_t &type, uint16_t &num)
{
    if (type == REG_CONST2)
        num += 2048;
    else if (type == REG_CONST3)
        num += 4096;
    else if (type == REG_CONST4)
        num += 6144;
    else
        return;
    type = REG_CONST;
}

// ============================================================================================================
// parsing
// ============================================================================================================

namespace
{
struct Reader
{
    const uint32_t *t;
    size_t n, at;
    bool ok = true;
    uint32_t Next()
    {
        if (at >= n)
        {
            ok = false;
            return 0;
        }
        return t[at++];
    }
};
} // namespace

static Src ReadSrc(Reader &r, bool sm2)
{
    uint32_t t = r.Next();
    Src s = {};
    s.type = RegTypeOf(t);
    s.num = (uint16_t)(t & 0x7FF);
    s.swz = (uint8_t)((t >> 16) & 0xFF);
    s.mod = (uint8_t)((t >> 24) & 0xF);
    s.rel = (uint8_t)((t >> 13) & 1);
    FoldConst(s.type, s.num);
    if (s.rel)
    {
        if (sm2)
        {
            uint32_t rt = r.Next();
            s.relType = RegTypeOf(rt);
            s.relComp = (uint8_t)((rt >> 16) & 3);
        }
        else
        {
            s.relType = REG_ADDR;
            s.relComp = 0;
        }
    }
    return s;
}

static Dst ReadDst(Reader &r, bool sm3)
{
    uint32_t t = r.Next();
    Dst d = {};
    d.type = RegTypeOf(t);
    d.num = (uint16_t)(t & 0x7FF);
    d.mask = (uint8_t)((t >> 16) & 0xF);
    d.mod = (uint8_t)((t >> 20) & 0xF);
    d.rel = (uint8_t)((t >> 13) & 1);
    FoldConst(d.type, d.num);
    if (d.rel && sm3)
        r.Next(); // o[aL + n]: the register token (not supported, see ShaderParse)
    return d;
}

// the constant table's float4 arrays: [first register, count)
static void ParseCtab(const uint32_t *data, size_t words, std::vector<std::pair<int, int>> &arrays)
{
    if (words < 8 || data[0] != 0x42415443) // 'CTAB'
        return;
    const uint8_t *base = (const uint8_t *)(data + 1);
    size_t bytes = (words - 1) * 4;
    uint32_t count = data[4], info = data[5];
    for (uint32_t i = 0; i < count; ++i)
    {
        size_t at = info + (size_t)i * 20;
        if (at + 20 > bytes)
            break;
        uint16_t set, reg, regs;
        memcpy(&set, base + at + 4, 2);
        memcpy(&reg, base + at + 6, 2);
        memcpy(&regs, base + at + 8, 2);
        if (set == 2) // D3DXRS_FLOAT4
            arrays.push_back({reg, regs});
    }
}

void ShaderParse(const uint32_t *code, size_t words, ShaderInfo &out)
{
    Parsed *p = new Parsed;
    delete (Parsed *)out.instrs;
    out.instrs = (std::vector<ShaderInstr> *)p;
    if (!words)
    {
        out.error = "empty";
        return;
    }
    uint32_t version = code[0];
    out.pixel = (version >> 16) == 0xFFFF;
    out.major = (version >> 8) & 0xFF;
    out.minor = version & 0xFF;
    if (((version >> 16) != 0xFFFF && (version >> 16) != 0xFFFE) || out.major < 2 || out.major > 3)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "version %08x is not shader model 2 or 3", version);
        out.error = buf;
        return;
    }
    bool sm3 = out.major >= 3;
    bool pixel = out.pixel;
    std::vector<std::pair<int, int>> ctabArrays;
    std::set<int> floatsRead, intsRead;
    std::vector<std::pair<int, int>> relRanges; // relative reads of c: [first, end)
    Reader r = {code, words, 1};
    while (r.ok && r.at < words)
    {
        uint32_t tok = r.Next();
        uint16_t op = (uint16_t)(tok & 0xFFFF);
        if (op == OP_COMMENT)
        {
            uint32_t n = (tok >> 16) & 0x7FFF;
            if (r.at + n > words)
                break;
            ParseCtab(code + r.at, n, ctabArrays);
            r.at += n;
            continue;
        }
        if (op == OP_END)
            break;
        uint32_t len = (tok >> 24) & 0xF;
        size_t end = r.at + len;
        ShaderInstr in = {};
        in.op = op;
        in.ctrl = (uint8_t)((tok >> 16) & 0xFF);
        in.pred = (uint8_t)((tok >> 28) & 1);
        if (op == OP_DCL)
        {
            uint32_t usage = r.Next();
            Dst d = ReadDst(r, sm3);
            Decl decl = {d.type, d.num, d.mask, (uint8_t)(usage & 0x1F), (uint8_t)((usage >> 16) & 0xF)};
            if (d.type == REG_SAMPLER && d.num < 16)
            {
                uint32_t type = (usage >> 27) & 0xF;
                p->samplerDim[d.num] = type == 3 ? DIM_CUBE : type == 4 ? DIM_3D : DIM_2D;
                p->samplerDeclared[d.num] = true;
            }
            else
                p->decls.push_back(decl);
        }
        else if (op == OP_DEF || op == OP_DEFI || op == OP_DEFB)
        {
            Dst d = ReadDst(r, sm3);
            if (op == OP_DEF)
            {
                std::vector<float> v(4);
                for (int i = 0; i < 4; ++i)
                {
                    uint32_t bits = r.Next();
                    memcpy(&v[i], &bits, 4);
                }
                p->defF[d.num] = v;
            }
            else if (op == OP_DEFI)
            {
                std::vector<int> v(4);
                for (int i = 0; i < 4; ++i)
                    v[i] = (int)r.Next();
                p->defI[d.num] = v;
            }
            else
                p->defB[d.num] = r.Next() != 0;
        }
        else if (op == OP_PHASE || op == OP_TEXCOORD || (op >= 67 && op <= 77) || (op >= 82 && op <= 87) || op == OP_BEM)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "opcode %u (shader model 1) is not supported", op);
            out.error = buf;
            return;
        }
        else
        {
            if (HasDest(op))
            {
                in.hasDst = 1;
                in.dst = ReadDst(r, sm3);
                if (in.dst.rel)
                {
                    out.error = "relative addressing of a destination is not supported";
                    return;
                }
            }
            if (in.pred)
                in.predSrc = ReadSrc(r, true);
            while (r.ok && r.at < end && in.nsrc < 4)
                in.src[in.nsrc++] = ReadSrc(r, true);
            if (op == OP_LABEL && in.nsrc)
                p->labels.insert(in.src[0].num);
            if (in.hasDst && in.dst.type == REG_TEMP)
                p->temps.insert(in.dst.num);
            if (in.hasDst && in.dst.type == REG_COLOROUT)
                out.colorOutputs |= 1u << (in.dst.num & 3);
            if (in.hasDst && in.dst.type == REG_DEPTHOUT)
                out.writesDepth = true;
            for (int i = 0; i < in.nsrc; ++i)
            {
                const Src &s = in.src[i];
                if (s.type == REG_TEMP)
                    p->temps.insert(s.num);
                else if (s.type == REG_CONST)
                {
                    int count = 1;
                    if (op >= OP_M4x4 && op <= OP_M3x2 && i == 1)
                        count = (op == OP_M4x4 || op == OP_M3x4) ? 4 : op == OP_M3x2 ? 2 : 3;
                    if (s.rel)
                    {
                        // the array of the constant table that holds the register; without one, up to the last register
                        int first = s.num, last = pixel ? 224 : 256;
                        for (auto &a : ctabArrays)
                            if (s.num >= a.first && s.num < a.first + a.second)
                            {
                                first = a.first;
                                last = a.first + a.second;
                            }
                        last = std::max(last, (int)s.num + count);
                        relRanges.push_back({first, last});
                    }
                    else
                        for (int k = 0; k < count; ++k)
                            floatsRead.insert(s.num + k);
                }
                else if (s.type == REG_CONSTINT)
                    intsRead.insert(s.num);
            }
        }
        if (!r.ok)
            break;
        if (r.at != end)
        {
            char buf[80];
            snprintf(buf, sizeof(buf), "opcode %u: %u tokens, not %u", op, (unsigned)(r.at - (end - len)), len);
            out.error = buf;
            return;
        }
        if (op != OP_DCL && op != OP_DEF && op != OP_DEFI && op != OP_DEFB)
            p->instrs.push_back(in);
    }
    if (!r.ok)
    {
        out.error = "truncated";
        return;
    }

    // the float constants: each register read directly, unless the shader defines it, and the arrays read relative
    std::vector<std::pair<int, int>> ranges = relRanges;
    for (int reg : floatsRead)
        if (!p->defF.count(reg))
            ranges.push_back({reg, reg + 1});
    std::sort(ranges.begin(), ranges.end());
    for (auto &rg : ranges)
    {
        if (!out.floatRuns.empty() && rg.first <= out.floatRuns.back().reg + out.floatRuns.back().count)
        {
            ConstRun &last = out.floatRuns.back();
            int end2 = std::max((int)last.reg + last.count, rg.second);
            out.floatSlots += end2 - (last.reg + last.count);
            last.count = (uint16_t)(end2 - last.reg);
        }
        else
        {
            out.floatRuns.push_back({(uint16_t)rg.first, (uint16_t)(rg.second - rg.first), (uint16_t)out.floatSlots});
            out.floatSlots += rg.second - rg.first;
        }
    }
    for (const ConstRun &run : out.floatRuns)
        for (int k = 0; k < run.count; ++k)
        {
            p->floatSlot[run.reg + k] = run.slot + k;
            auto def = p->defF.find(run.reg + k);
            if (def != p->defF.end())
            {
                DefSlot ds;
                ds.slot = (uint16_t)(run.slot + k);
                memcpy(ds.value, def->second.data(), 16);
                out.defSlots.push_back(ds);
            }
        }
    for (int reg : intsRead)
        if (!p->defI.count(reg))
            out.intCount = std::max(out.intCount, (uint32_t)reg + 1);

    // samplers, attributes, varyings
    for (int s = 0; s < 16; ++s)
        if (p->samplerDeclared[s])
            out.samplers.push_back({(uint8_t)s, (uint8_t)p->samplerDim[s]});
    for (const Decl &d : p->decls)
    {
        if (!pixel && d.type == REG_INPUT)
            out.inputs.push_back({d.usage, d.index});
        else if (!pixel && d.type == REG_OUTPUT && sm3)
        {
            int loc = VaryingLocation(d.usage, d.index);
            if (loc >= 0)
                out.varyings |= 1u << loc;
        }
        else if (pixel && d.type == REG_INPUT)
        {
            int loc = sm3 ? VaryingLocation(d.usage, d.index) : VaryingLocation(USAGE_COLOR, d.num);
            if (loc >= 0)
                out.varyings |= 1u << loc;
            else if (sm3)
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "pixel input usage %u index %u is not supported", d.usage, d.index);
                out.error = buf;
            }
        }
        else if (pixel && d.type == REG_TEXTURE)
        {
            int loc = VaryingLocation(USAGE_TEXCOORD, d.num);
            if (loc >= 0)
                out.varyings |= 1u << loc;
        }
    }
    if (!pixel && !sm3)
    {
        // vs_2_0 writes its outputs undeclared: oT#, oD#
        for (const ShaderInstr &in : p->instrs)
        {
            if (!in.hasDst)
                continue;
            int loc = in.dst.type == REG_OUTPUT ? VaryingLocation(USAGE_TEXCOORD, in.dst.num)
                      : in.dst.type == REG_ATTROUT ? VaryingLocation(USAGE_COLOR, in.dst.num)
                                                   : -1;
            if (loc >= 0)
                out.varyings |= 1u << loc;
        }
    }
}

// ============================================================================================================
// what stand-in pixel shaders read (d3d9_webgpu.cpp: stand-ins)
// ============================================================================================================

// the components a swizzle reads for x and y, a bit each
static uint8_t SwizzleComps(uint8_t swz) { return (uint8_t)(1u << (swz & 3) | 1u << ((swz >> 2) & 3)); }

// the varying location a pixel shader's input register is, in the components read (comps, a bit each; -1: none):
// ps_3_0 declares them, two inputs sharing a register each in their components; ps_2_x reads texture coordinates in
// t# and colours in v#
static int PixelInputLocation(const Parsed &p, bool sm3, uint8_t type, int num, uint8_t comps)
{
    if (type == REG_TEXTURE)
        return sm3 ? -1 : VaryingLocation(USAGE_TEXCOORD, num);
    if (type != REG_INPUT)
        return -1;
    if (!sm3)
        return VaryingLocation(USAGE_COLOR, num);
    for (const Decl &d : p.decls)
        if (d.type == REG_INPUT && d.num == num && (!d.mask || (d.mask & comps)))
            return VaryingLocation(d.usage, d.index);
    return -1;
}

// the swizzle of a value swizzled by `inner`, then by `outer`: component i is inner's component outer[i]
static uint8_t ComposeSwizzle(uint8_t inner, uint8_t outer)
{
    uint8_t r = 0;
    for (int i = 0; i < 4; ++i)
    {
        int o = (outer >> (2 * i)) & 3;
        r |= (uint8_t)(((inner >> (2 * o)) & 3) << (2 * i));
    }
    return r;
}

void PixelSamplerCoords(const ShaderInfo &info, SamplerCoord coords[16])
{
    for (int s = 0; s < 16; ++s)
        coords[s] = {VARYING_COUNT, 0};
    if (!info.pixel || !info.instrs || !info.error.empty())
        return;
    const Parsed &p = *(const Parsed *)info.instrs;
    bool sm3 = info.major >= 3;
    struct Origin
    {
        int loc;
        uint8_t swz;
    };
    std::map<int, Origin> temps; // a temp register's x and y come from this varying's components
    auto origin = [&](const Src &s, Origin &o) {
        int loc = PixelInputLocation(p, sm3, s.type, s.num, SwizzleComps(s.swz));
        if (loc >= 0)
        {
            o = {loc, s.swz};
            return true;
        }
        if (s.type == REG_TEMP)
        {
            auto it = temps.find(s.num);
            if (it != temps.end())
            {
                o = {it->second.loc, ComposeSwizzle(it->second.swz, s.swz)};
                return true;
            }
        }
        return false;
    };
    for (const ShaderInstr &in : p.instrs)
    {
        if ((in.op == OP_TEX || in.op == OP_TEXLDL || in.op == OP_TEXLDD) && in.nsrc >= 2 && in.src[1].type == REG_SAMPLER && in.src[1].num < 16)
        {
            Origin o;
            if (coords[in.src[1].num].loc == VARYING_COUNT && origin(in.src[0], o))
                coords[in.src[1].num] = {(uint8_t)o.loc, (uint8_t)(o.swz & 0xF)};
        }
        if (!in.hasDst || in.dst.type != REG_TEMP || !(in.dst.mask & 3))
            continue; // x and y stay what they were
        // a copy, or an affine function, of one varying: that varying's; anything else loses the origin
        bool simple = in.op == OP_MOV || in.op == OP_ADD || in.op == OP_SUB || in.op == OP_MAD || in.op == OP_MUL || in.op == OP_FRC;
        Origin o = {-1, 0};
        bool ambiguous = false;
        if (simple)
            for (int i = 0; i < in.nsrc; ++i)
            {
                Origin oi;
                if (!origin(in.src[i], oi))
                    continue;
                if (o.loc < 0)
                    o = oi;
                else if (oi.loc != o.loc)
                    ambiguous = true;
            }
        if (simple && o.loc >= 0 && !ambiguous)
            temps[in.dst.num] = o;
        else
            temps.erase(in.dst.num);
    }
}

uint32_t VertexVaryingsFrom(const ShaderInfo &info, int constReg, int inputUsage)
{
    if (info.pixel || !info.instrs || !info.error.empty() || (constReg < 0 && inputUsage < 0))
        return 0;
    const Parsed &p = *(const Parsed *)info.instrs;
    bool sm3 = info.major >= 3;
    std::set<int> temps; // the temp registers holding a value derived from the source
    uint32_t out = 0;
    auto from = [&](const Src &s, int count) {
        if (s.type == REG_CONST)
            return constReg >= 0 && constReg >= s.num && constReg < s.num + count;
        if (s.type == REG_INPUT)
        {
            for (const Decl &d : p.decls)
                if (d.type == REG_INPUT && d.num == s.num)
                    return inputUsage >= 0 && d.usage == inputUsage;
            return false;
        }
        return s.type == REG_TEMP && temps.count(s.num) > 0;
    };
    for (int pass = 0; pass < 2; ++pass) // twice, for loops
        for (const ShaderInstr &in : p.instrs)
        {
            if (!in.hasDst)
                continue;
            bool derived = false;
            for (int i = 0; i < in.nsrc; ++i)
            {
                int count = 1;
                if (in.op >= OP_M4x4 && in.op <= OP_M3x2 && i == 1)
                    count = (in.op == OP_M4x4 || in.op == OP_M3x4) ? 4 : in.op == OP_M3x2 ? 2 : 3;
                derived = derived || from(in.src[i], count);
            }
            if (in.dst.type == REG_TEMP)
            {
                if (derived)
                    temps.insert(in.dst.num);
                else if (in.dst.mask == 0xF)
                    temps.erase(in.dst.num);
                continue;
            }
            if (!derived)
                continue;
            int loc = -1;
            if (in.dst.type == REG_OUTPUT && sm3)
            {
                // two outputs may share a register, each in its components
                uint8_t mask = in.dst.mask ? in.dst.mask : 0xF;
                for (const Decl &d : p.decls)
                    if (d.type == REG_OUTPUT && d.num == in.dst.num && (!d.mask || (d.mask & mask)))
                    {
                        int l = VaryingLocation(d.usage, d.index);
                        if (l >= 0)
                            out |= 1u << l;
                    }
            }
            else if (in.dst.type == REG_OUTPUT)
                loc = VaryingLocation(USAGE_TEXCOORD, in.dst.num);
            else if (in.dst.type == REG_ATTROUT)
                loc = VaryingLocation(USAGE_COLOR, in.dst.num);
            if (loc >= 0)
                out |= 1u << loc;
        }
    return out;
}

// ============================================================================================================
// WGSL
// ============================================================================================================

namespace
{
const char kComp[] = "xyzw";

std::string Num(int n) { return std::to_string(n); }

std::string Float(float f)
{
    if (f != f)
        return "bitcast<f32>(0x7fc00000u)";
    if (f > 3.4e38f)
        return "3.4e38";
    if (f < -3.4e38f)
        return "-3.4e38";
    char buf[48];
    snprintf(buf, sizeof(buf), "%.9g", f);
    std::string s(buf);
    if (s.find_first_of(".e") == std::string::npos)
        s += ".0";
    return s;
}

std::string Swizzle(uint8_t swz, int n)
{
    if (n == 4 && swz == 0xE4)
        return "";
    std::string s = ".";
    for (int i = 0; i < n; ++i)
        s += kComp[(swz >> (2 * i)) & 3];
    return s;
}

std::string MaskVec(uint8_t mask)
{
    std::string s = "vec4<bool>(";
    for (int i = 0; i < 4; ++i)
        s += std::string(i ? ", " : "") + ((mask >> i) & 1 ? "true" : "false");
    return s + ")";
}

const char *CmpOp(uint8_t ctrl)
{
    switch (ctrl)
    {
    case 1:
        return ">";
    case 2:
        return "==";
    case 3:
        return ">=";
    case 4:
        return "<";
    case 5:
        return "!=";
    case 6:
        return "<=";
    default:
        return nullptr;
    }
}

enum ValKind
{
    VAL_FLOAT,
    VAL_INT,
    VAL_BOOL,
};

// the components a write mask selects, in order
struct Mask
{
    int c[4];
    int n;
};

Mask MaskOf(uint8_t mask)
{
    Mask m = {{0, 0, 0, 0}, 0};
    for (int i = 0; i < 4; ++i)
        if (mask >> i & 1)
            m.c[m.n++] = i;
    return m;
}

std::string VecType(int n, ValKind k)
{
    const char *scalar = k == VAL_FLOAT ? "f32" : k == VAL_INT ? "i32" : "bool";
    const char *suffix = k == VAL_FLOAT ? "f" : k == VAL_INT ? "i" : "<bool>";
    return n == 1 ? scalar : "vec" + Num(n) + suffix;
}

// a scalar in n components
std::string Splat(const std::string &e, int n, ValKind k = VAL_FLOAT) { return n == 1 ? e : VecType(n, k) + "(" + e + ")"; }

// the masked components of a vector `width` wide: its swizzle (none for all of them)
std::string Narrow(const Mask &m, int width = 4)
{
    std::string swz;
    bool identity = m.n == width;
    for (int i = 0; i < m.n; ++i)
    {
        swz += kComp[m.c[i]];
        identity = identity && m.c[i] == i;
    }
    return identity ? "" : "." + swz;
}

struct Emitter
{
    const ShaderInfo &info;
    const Parsed &p;
    uint32_t shadowMask;
    bool byInstance; // a vertex shader's constants are read from the arena, where the draw's first instance says (ShaderEmit)
    std::string error;
    std::string *body = nullptr;
    std::string mainBody, subs;
    int indent = 1;
    int depth = 0; // flow control nesting in the current function
    int loopIds = 0;
    std::vector<std::string> loopStep; // ENDLOOP: the aL step and the saved aL
    bool inSub = false;
    bool usesLit = false;
    bool usesRecip = false; // rcp, rsq, nrm
    bool usesVpos = false, usesVface = false;
    std::set<std::string> privates; // "name: type"

    Emitter(const ShaderInfo &i, const Parsed &pp, uint32_t shadow, bool instance) : info(i), p(pp), shadowMask(shadow), byInstance(instance) {}

    void Line(const std::string &s)
    {
        body->append((size_t)indent * 4, ' ');
        *body += s;
        *body += '\n';
    }

    void Fail(const std::string &what)
    {
        if (error.empty())
            error = what;
    }

    bool Vertex() const { return !info.pixel; }
    bool Sm3() const { return info.major >= 3; }

    void Private(const std::string &decl) { privates.insert(decl); }

    // ----- constants -----

    // The shader's block: the fix, the bools, the float constants' slots and the int constants, 16 bytes each. It is the
    // uniform U, or, read by the draw's instance, the vec4u's of the arena from bo1_at on.
    std::string Arena(uint32_t at) { return "bo1_arena[bo1_at" + (at ? " + " + Num((int)at) + "u" : std::string()) + "]"; }
    std::string Fix() { return byInstance ? "bitcast<vec4f>(" + Arena(0) + ")" : "U.bo1_fix"; }
    std::string Bools() { return byInstance ? Arena(1) + ".x" : "U.bo1_bools.x"; }
    std::string FloatConst(int slot) { return byInstance ? "bitcast<vec4f>(" + Arena(2 + slot) + ")" : "U.c[" + Num(slot) + "]"; }
    // one read relative, at an i32: past the shader's slots it reads the last, as an index past a uniform's array does
    std::string FloatConstAt(const std::string &index)
    {
        if (!byInstance)
            return "U.c[" + index + "]";
        return "bitcast<vec4f>(bo1_arena[bo1_at + 2u + min(u32(" + index + "), " + Num((int)info.floatSlots - 1) + "u)])";
    }
    std::string IntConst4(int n) { return byInstance ? "bitcast<vec4i>(" + Arena(2 + info.floatSlots + n) + ")" : "U.i[" + Num(n) + "]"; }

    // ----- sources -----

    std::string RelIndex(const Src &a)
    {
        std::string reg;
        if (a.relType == REG_LOOP)
        {
            reg = "aL";
            Private("aL: i32");
        }
        else
        {
            reg = std::string("a0.") + kComp[a.relComp & 3];
            Private("a0: vec4i");
        }
        auto slot = p.floatSlot.find(a.num);
        int base = slot != p.floatSlot.end() ? slot->second : 0;
        return reg + " + " + Num(base);
    }

    // a source register's vec4 before its swizzle; matrix rows add `row`
    std::string Reg(const Src &a, int row = 0)
    {
        int n = a.num + row;
        switch (a.type)
        {
        case REG_TEMP:
            Private("r" + Num(n) + ": vec4f");
            return "r" + Num(n);
        case REG_INPUT:
            Private("v" + Num(n) + ": vec4f");
            return "v" + Num(n);
        case REG_CONST: {
            if (a.rel)
                return FloatConstAt(RelIndex(a) + (row ? " + " + Num(row) : ""));
            if (p.defF.count(n))
                return "k" + Num(n);
            auto slot = p.floatSlot.find(n);
            if (slot == p.floatSlot.end())
            {
                Fail("constant c" + Num(n) + " has no slot");
                return "vec4f(0.0)";
            }
            return FloatConst(slot->second);
        }
        case REG_TEXTURE: // REG_ADDR
            if (Vertex())
            {
                Private("a0: vec4i");
                return "vec4f(a0)";
            }
            Private("t" + Num(n) + ": vec4f");
            return "t" + Num(n);
        case REG_CONSTINT:
            return p.defI.count(n) ? "vec4f(ki" + Num(n) + ")" : "vec4f(" + IntConst4(n) + ")";
        case REG_MISCTYPE:
            if (n == 0)
            {
                usesVpos = true;
                Private("vpos: vec4f");
                return "vpos";
            }
            usesVface = true;
            Private("vface: vec4f");
            return "vface";
        case REG_LOOP:
            Private("aL: i32");
            return "vec4f(f32(aL))";
        default:
            Fail("source register type " + Num(a.type) + " is not supported");
            return "vec4f(0.0)";
        }
    }

    std::string Modify(const Src &a, const std::string &v)
    {
        switch (a.mod)
        {
        case 0:
            return v;
        case 1:
            return "(-" + v + ")";
        case 2:
            return "(" + v + " - 0.5)";
        case 3:
            return "(0.5 - " + v + ")";
        case 4:
            return "(" + v + " * 2.0 - 1.0)";
        case 5:
            return "(1.0 - " + v + " * 2.0)";
        case 6:
            return "(1.0 - " + v + ")";
        case 7:
            return "(" + v + " * 2.0)";
        case 8:
            return "(" + v + " * -2.0)";
        case 11:
            return "abs(" + v + ")";
        case 12:
            return "(-abs(" + v + "))";
        default:
            Fail("source modifier " + Num(a.mod) + " is not supported");
            return v;
        }
    }

    // components `comps` of a source (indexes into its swizzle), swizzled and modified; one is a scalar
    std::string Pick(const Src &a, const int *comps, int n, int row = 0)
    {
        std::string swz;
        bool identity = n == 4;
        for (int i = 0; i < n; ++i)
        {
            int c = (a.swz >> (2 * comps[i])) & 3;
            swz += kComp[c];
            identity = identity && c == i;
        }
        return Modify(a, Reg(a, row) + (identity ? "" : "." + swz));
    }

    // the first n components of a source (n == 1: a scalar)
    std::string S(const Src &a, int n = 4, int row = 0)
    {
        static const int first[4] = {0, 1, 2, 3};
        return Pick(a, first, n, row);
    }

    // component i of a source
    std::string Comp(const Src &a, int i) { return Pick(a, &i, 1); }

    // the components of a source that the destination's mask writes
    std::string SM(const Src &a, const Mask &m, int row = 0) { return Pick(a, m.c, m.n, row); }

    // a bool condition: b#, or p0.c (if, callnz, breakp, predicates)
    std::string Cond(const Src &a)
    {
        std::string c;
        if (a.type == REG_CONSTBOOL)
        {
            auto def = p.defB.find(a.num);
            c = def != p.defB.end() ? (def->second ? "true" : "false") : "((" + Bools() + " >> " + Num(a.num) + "u) & 1u) != 0u";
        }
        else if (a.type == REG_PREDICATE)
        {
            Private("p0: vec4<bool>");
            c = std::string("p0.") + kComp[a.swz & 3];
        }
        else
        {
            Fail("condition register type " + Num(a.type) + " is not supported");
            return "false";
        }
        return a.mod == 13 ? "!(" + c + ")" : "(" + c + ")";
    }

    // an int constant's vec4i (loop, rep)
    std::string IntConst(const Src &a)
    {
        if (a.type != REG_CONSTINT)
        {
            Fail("loop count register type " + Num(a.type) + " is not supported");
            return "vec4i(0)";
        }
        auto def = p.defI.find(a.num);
        if (def != p.defI.end())
            return "vec4i(" + Num(def->second[0]) + ", " + Num(def->second[1]) + ", " + Num(def->second[2]) + ", " + Num(def->second[3]) + ")";
        return IntConst4(a.num);
    }

    // ----- destinations -----

    std::string DstReg(const Dst &d, ValKind &kind, bool &scalarReg)
    {
        kind = VAL_FLOAT;
        scalarReg = false;
        int n = d.num;
        switch (d.type)
        {
        case REG_TEMP:
            Private("r" + Num(n) + ": vec4f");
            return "r" + Num(n);
        case REG_ADDR:
            if (Vertex())
            {
                kind = VAL_INT;
                Private("a0: vec4i");
                return "a0";
            }
            Private("t" + Num(n) + ": vec4f");
            return "t" + Num(n);
        case REG_OUTPUT:
            Private("o" + Num(n) + ": vec4f");
            return "o" + Num(n);
        case REG_RASTOUT:
            Private("rast" + Num(n) + ": vec4f");
            return "rast" + Num(n);
        case REG_ATTROUT:
            Private("oD" + Num(n) + ": vec4f");
            return "oD" + Num(n);
        case REG_COLOROUT:
            Private("oC" + Num(n) + ": vec4f");
            return "oC" + Num(n);
        case REG_DEPTHOUT:
            Private("oDepth: f32");
            scalarReg = true;
            return "oDepth";
        case REG_PREDICATE:
            kind = VAL_BOOL;
            Private("p0: vec4<bool>");
            return "p0";
        default:
            Fail("destination register type " + Num(d.type) + " is not supported");
            return "";
        }
    }

    Mask DestMask(const ShaderInstr &in) { return MaskOf(in.dst.type == REG_DEPTHOUT ? 1 : in.dst.mask); }

    // Write an expression of the destination's components (scalar: one for all of them), through its modifiers. Only
    // those components are computed; the register's others stay.
    void Assign(const ShaderInstr &in, std::string e, bool scalar, ValKind valKind = VAL_FLOAT)
    {
        ValKind kind;
        bool scalarReg;
        std::string reg = DstReg(in.dst, kind, scalarReg);
        if (reg.empty())
            return;
        if (kind != valKind)
        {
            Fail("mismatched destination");
            return;
        }
        Mask m = DestMask(in);
        if (!m.n)
            return;
        if (scalar)
            e = Splat(e, m.n, kind);
        if ((in.dst.mod & 1) && kind == VAL_FLOAT)
            e = "saturate(" + e + ")";
        if (scalarReg)
        {
            Line(reg + " = " + e + ";");
            return;
        }
        std::string type = VecType(4, kind);
        if (in.pred)
        {
            // predicated: only the components whose predicate is true
            Line("{");
            ++indent;
            Line("let t = " + e + ";");
            Line(reg + " = select(" + reg + ", " + Merge(reg, m, "t", type) + ", " + MaskVec(in.dst.mask) + " & " + PredVec(in.predSrc) + ");");
            --indent;
            Line("}");
        }
        else if (m.n == 4)
            Line(reg + " = " + e + ";");
        else if (m.n == 1)
            Line(reg + "." + kComp[m.c[0]] + " = " + e + ";");
        else if (m.c[m.n - 1] - m.c[0] == m.n - 1)
        {
            // a run of components, between the register's others
            int first = m.c[0], end = first + m.n;
            std::string v = first ? reg + "." + std::string(kComp, first) + ", " : "";
            v += e;
            if (end < 4)
                v += ", " + reg + "." + std::string(kComp + end, 4 - end);
            Line(reg + " = " + type + "(" + v + ");");
        }
        else
        {
            Line("{");
            ++indent;
            Line("let t = " + e + ";");
            Line(reg + " = " + Merge(reg, m, "t", type) + ";");
            --indent;
            Line("}");
        }
    }

    // the register, its masked components from t
    std::string Merge(const std::string &reg, const Mask &m, const std::string &t, const std::string &type)
    {
        std::string v = type + "(";
        for (int c = 0, k = 0; c < 4; ++c)
        {
            if (c)
                v += ", ";
            if (k < m.n && m.c[k] == c)
            {
                v += m.n == 1 ? t : t + "." + kComp[k];
                ++k;
            }
            else
                v += reg + "." + kComp[c];
        }
        return v + ")";
    }

    std::string PredVec(const Src &a)
    {
        Private("p0: vec4<bool>");
        std::string v = "p0" + Swizzle(a.swz, 4);
        return a.mod == 13 ? "!" + v : v;
    }

    // ----- textures -----

    // a texture instruction's result, in the destination's components
    std::string Sample(const ShaderInstr &in, const Mask &m)
    {
        const Src &c = in.src[0], &smp = in.src[1];
        int s = smp.num;
        if (s >= 16)
        {
            Fail("sampler " + Num(s));
            return Splat("0.0", m.n);
        }
        int dim = p.samplerDim[s];
        bool vtx = Vertex();
        std::string tex = (vtx ? "vtex" : "tex") + Num(s), sam = (vtx ? "vsmp" : "smp") + Num(s);
        std::string coord = S(c, dim == DIM_2D ? 2 : 3), w = Comp(c, 3);
        bool proj = in.op == OP_TEX && in.ctrl == 1;
        if (!vtx && dim == DIM_2D && (shadowMask >> s & 1))
        {
            // hardware shadow maps: the depth comparison with z, the result in every component
            std::string z = Comp(c, 2);
            return Splat(proj ? "textureSampleCompareLevel(" + tex + ", " + sam + ", " + coord + " / " + w + ", " + z + " / " + w + ")"
                              : "textureSampleCompareLevel(" + tex + ", " + sam + ", " + coord + ", " + z + ")",
                m.n);
        }
        std::string r;
        if (in.op == OP_TEXLDL)
            r = "textureSampleLevel(" + tex + ", " + sam + ", " + coord + ", " + w + ")";
        else if (in.op == OP_TEXLDD)
        {
            int n = dim == DIM_2D ? 2 : 3;
            r = "textureSampleGrad(" + tex + ", " + sam + ", " + coord + ", " + S(in.src[2], n) + ", " + S(in.src[3], n) + ")";
        }
        else if (vtx)
            r = "textureSampleLevel(" + tex + ", " + sam + ", " + coord + ", 0.0)";
        else if (proj) // texldp
            r = "textureSample(" + tex + ", " + sam + ", " + coord + " / " + w + ")";
        else if (in.ctrl == 2) // texldb
            r = "textureSampleBias(" + tex + ", " + sam + ", " + coord + ", " + w + ")";
        else
            r = "textureSample(" + tex + ", " + sam + ", " + coord + ")";
        // the sampler's swizzle, then the destination's components
        std::string swz;
        bool identity = m.n == 4;
        for (int i = 0; i < m.n; ++i)
        {
            int k = (smp.swz >> (2 * m.c[i])) & 3;
            swz += kComp[k];
            identity = identity && k == i;
        }
        return identity ? r : r + "." + swz;
    }

    // ----- instructions -----

    // m4x4, m4x3, m3x4, m3x3, m3x2: component i is source 0 dotted with row i (the registers from source 1's on)
    void Matrix(const ShaderInstr &in, int cols, int rows)
    {
        Mask m = DestMask(in);
        std::string parts;
        for (int i = 0; i < m.n; ++i)
        {
            int row = m.c[i];
            parts += std::string(i ? ", " : "") + (row >= rows ? "0.0" : "dot(" + S(in.src[0], cols) + ", " + S(in.src[1], cols, row) + ")");
        }
        Assign(in, m.n == 1 ? parts : VecType(m.n, VAL_FLOAT) + "(" + parts + ")", false);
    }

    void Instr(const ShaderInstr &in)
    {
        const Src *s = in.src;
        Mask m = DestMask(in);
        switch (in.op)
        {
        case OP_NOP:
            break;
        case OP_MOV:
            Assign(in, SM(s[0], m), false);
            break;
        case OP_ADD:
            Assign(in, SM(s[0], m) + " + " + SM(s[1], m), false);
            break;
        case OP_SUB:
            Assign(in, SM(s[0], m) + " - " + SM(s[1], m), false);
            break;
        case OP_MUL:
            Assign(in, SM(s[0], m) + " * " + SM(s[1], m), false);
            break;
        case OP_MAD:
            Assign(in, SM(s[0], m) + " * " + SM(s[1], m) + " + " + SM(s[2], m), false);
            break;
        case OP_RCP:
            usesRecip = true;
            Assign(in, "bo1_rcp(" + Comp(s[0], 0) + ")", true);
            break;
        case OP_RSQ:
            usesRecip = true;
            Assign(in, "bo1_rsq(" + Comp(s[0], 0) + ")", true);
            break;
        case OP_DP3:
            Assign(in, "dot(" + S(s[0], 3) + ", " + S(s[1], 3) + ")", true);
            break;
        case OP_DP4:
            Assign(in, "dot(" + S(s[0]) + ", " + S(s[1]) + ")", true);
            break;
        case OP_MIN:
            Assign(in, "min(" + SM(s[0], m) + ", " + SM(s[1], m) + ")", false);
            break;
        case OP_MAX:
            Assign(in, "max(" + SM(s[0], m) + ", " + SM(s[1], m) + ")", false);
            break;
        case OP_SLT:
            Assign(in, "select(" + Splat("0.0", m.n) + ", " + Splat("1.0", m.n) + ", " + SM(s[0], m) + " < " + SM(s[1], m) + ")", false);
            break;
        case OP_SGE:
            Assign(in, "select(" + Splat("0.0", m.n) + ", " + Splat("1.0", m.n) + ", " + SM(s[0], m) + " >= " + SM(s[1], m) + ")", false);
            break;
        case OP_EXP:
        case OP_EXPP:
            Assign(in, "exp2(" + Comp(s[0], 0) + ")", true);
            break;
        case OP_LOG:
        case OP_LOGP:
            Assign(in, "log2(abs(" + Comp(s[0], 0) + "))", true);
            break;
        case OP_LIT:
            usesLit = true;
            Assign(in, "bo1_lit(" + S(s[0]) + ")" + Narrow(m), false);
            break;
        case OP_DST:
            Assign(in, "vec4f(1.0, " + Comp(s[0], 1) + " * " + Comp(s[1], 1) + ", " + Comp(s[0], 2) + ", " + Comp(s[1], 3) + ")" + Narrow(m), false);
            break;
        case OP_LRP:
            Assign(in, "mix(" + SM(s[2], m) + ", " + SM(s[1], m) + ", " + SM(s[0], m) + ")", false);
            break;
        case OP_FRC:
            Assign(in, "fract(" + SM(s[0], m) + ")", false);
            break;
        case OP_M4x4:
            Matrix(in, 4, 4);
            break;
        case OP_M4x3:
            Matrix(in, 4, 3);
            break;
        case OP_M3x4:
            Matrix(in, 3, 4);
            break;
        case OP_M3x3:
            Matrix(in, 3, 3);
            break;
        case OP_M3x2:
            Matrix(in, 3, 2);
            break;
        case OP_POW:
            Assign(in, "pow(abs(" + Comp(s[0], 0) + "), " + Comp(s[1], 0) + ")", true);
            break;
        case OP_CRS:
            if (in.dst.mask & 8)
                Assign(in, "vec4f(cross(" + S(s[0], 3) + ", " + S(s[1], 3) + "), 0.0)" + Narrow(m), false);
            else
                Assign(in, "cross(" + S(s[0], 3) + ", " + S(s[1], 3) + ")" + Narrow(m, 3), false);
            break;
        case OP_SGN:
            Assign(in, "sign(" + SM(s[0], m) + ")", false);
            break;
        case OP_ABS:
            Assign(in, "abs(" + SM(s[0], m) + ")", false);
            break;
        case OP_NRM:
            // every component over the length of xyz (0 stays 0)
            usesRecip = true;
            Assign(in, SM(s[0], m) + " * bo1_rsq(dot(" + S(s[0], 3) + ", " + S(s[0], 3) + "))", false);
            break;
        case OP_SINCOS: {
            std::string a = Comp(s[0], 0), parts;
            for (int i = 0; i < m.n; ++i)
                parts += std::string(i ? ", " : "") + (m.c[i] == 0 ? "cos(" + a + ")" : m.c[i] == 1 ? "sin(" + a + ")" : "0.0");
            Assign(in, m.n == 1 ? parts : VecType(m.n, VAL_FLOAT) + "(" + parts + ")", false);
            break;
        }
        case OP_MOVA:
            Assign(in, VecType(m.n, VAL_INT) + "(round(" + SM(s[0], m) + "))", false, VAL_INT);
            break;
        case OP_CMP:
            Assign(in, "select(" + SM(s[2], m) + ", " + SM(s[1], m) + ", " + SM(s[0], m) + " >= " + Splat("0.0", m.n) + ")", false);
            break;
        case OP_CND:
            Assign(in, "select(" + SM(s[2], m) + ", " + SM(s[1], m) + ", " + SM(s[0], m) + " > " + Splat("0.5", m.n) + ")", false);
            break;
        case OP_DP2ADD:
            Assign(in, "dot(" + S(s[0], 2) + ", " + S(s[1], 2) + ") + " + Comp(s[2], 0), true);
            break;
        case OP_DSX:
            Assign(in, "dpdx(" + SM(s[0], m) + ")", false);
            break;
        case OP_DSY:
            Assign(in, "dpdy(" + SM(s[0], m) + ")", false);
            break;
        case OP_SETP: {
            const char *op = CmpOp(in.ctrl);
            if (!op)
            {
                Fail("setp comparison");
                break;
            }
            Assign(in, SM(s[0], m) + " " + op + " " + SM(s[1], m), false, VAL_BOOL);
            break;
        }
        case OP_TEXKILL: {
            // the components the "destination" names, each tested (shader model 2 and up: all four, as written)
            ValKind kind;
            bool scalarReg;
            std::string reg = DstReg(in.dst, kind, scalarReg);
            std::string test;
            for (int c = 0; c < 4; ++c)
                if (in.dst.mask >> c & 1)
                    test += std::string(test.empty() ? "" : " || ") + reg + "." + kComp[c] + " < 0.0";
            if (!test.empty())
                Line("if (" + test + ") { discard; }");
            break;
        }
        case OP_TEX:
        case OP_TEXLDL:
        case OP_TEXLDD:
            if (in.nsrc < 2)
            {
                Fail("texld without a sampler");
                break;
            }
            Assign(in, Sample(in, m), false);
            break;

        // flow control
        case OP_IF:
            Line("if " + Cond(s[0]) + " {");
            ++indent;
            ++depth;
            break;
        case OP_IFC: {
            const char *op = CmpOp(in.ctrl);
            if (!op)
            {
                Fail("ifc comparison");
                break;
            }
            Line("if (" + Comp(s[0], 0) + " " + op + " " + Comp(s[1], 0) + ") {");
            ++indent;
            ++depth;
            break;
        }
        case OP_ELSE:
            --indent;
            Line("} else {");
            ++indent;
            break;
        case OP_ENDIF:
        case OP_ENDREP:
            --indent;
            --depth;
            Line("}");
            break;
        case OP_REP: {
            std::string id = "rep" + Num(loopIds++);
            Line("for (var " + id + ": i32 = 0; " + id + " < min(" + IntConst(s[0]) + ".x, 255); " + id + "++) {");
            ++indent;
            ++depth;
            break;
        }
        case OP_LOOP: {
            // loop aL, i#: i#.x times, aL from i#.y in steps of i#.z
            Private("aL: i32");
            std::string id = Num(loopIds++), ic = IntConst(s[1]);
            Line("{");
            ++indent;
            Line("let saved" + id + " = aL;");
            Line("let loop" + id + " = " + ic + ";");
            Line("aL = loop" + id + ".y;");
            Line("for (var n" + id + ": i32 = 0; n" + id + " < min(loop" + id + ".x, 255); n" + id + "++) {");
            ++indent;
            ++depth;
            loopStep.push_back(id);
            break;
        }
        case OP_ENDLOOP: {
            if (loopStep.empty())
            {
                Fail("endloop without loop");
                break;
            }
            std::string id = loopStep.back();
            loopStep.pop_back();
            Line("aL += loop" + id + ".z;");
            --indent;
            --depth;
            Line("}");
            Line("aL = saved" + id + ";");
            --indent;
            Line("}");
            break;
        }
        case OP_BREAK:
            Line("break;");
            break;
        case OP_BREAKC: {
            const char *op = CmpOp(in.ctrl);
            if (!op)
            {
                Fail("breakc comparison");
                break;
            }
            Line("if (" + Comp(s[0], 0) + " " + op + " " + Comp(s[1], 0) + ") { break; }");
            break;
        }
        case OP_BREAKP:
            Line("if " + Cond(s[0]) + " { break; }");
            break;
        case OP_CALL:
            Line("sub" + Num(s[0].num) + "();");
            break;
        case OP_CALLNZ:
            Line("if " + Cond(s[1]) + " { sub" + Num(s[0].num) + "(); }");
            break;
        case OP_LABEL:
            if (inSub || depth)
            {
                Fail("label inside a function");
                break;
            }
            inSub = true;
            body = &subs;
            Line("");
            indent = 0;
            Line("fn sub" + Num(s[0].num) + "() {");
            indent = 1;
            break;
        case OP_RET:
            if (depth)
                Line("return;");
            else if (inSub)
            {
                indent = 0;
                Line("}");
                indent = 1;
                inSub = false;
            }
            else
                body = &subs; // the end of the main function: what follows are subroutines
            break;
        default:
            Fail("opcode " + Num(in.op) + " is not supported");
            break;
        }
    }
};
} // namespace

static const char *kSrgbEncode = "fn bo1_srgb(c: vec3f) -> vec3f {\n"
                                 "    let s = saturate(c);\n"
                                 "    return select(1.055 * pow(s, vec3f(1.0 / 2.4)) - 0.055, s * 12.92, s < vec3f(0.0031308));\n"
                                 "}\n";

static const char *kLit = "fn bo1_lit(s: vec4f) -> vec4f {\n"
                          "    let n = max(s.x, 0.0);\n"
                          "    let h = max(s.y, 0.0);\n"
                          "    let p = clamp(s.w, -127.9961, 127.9961);\n"
                          "    return vec4f(1.0, n, select(0.0, pow(h, p), s.x > 0.0 && s.y > 0.0), 1.0);\n"
                          "}\n";

// rcp, rsq and nrm of 0. Direct3D 9's hardware multiplies 0 by anything to 0, infinity included, and its shaders count
// on it; in WGSL, 0 times infinity is NaN. So 0's reciprocals are the largest float here, as DXVK makes them
// (d3d9.floatEmulation). Call of the Dead's frost (sw4_2d_frost) offsets its lookup of the scene along its frost's
// gradient, normalized with rsq, and where there is no frost the gradient is 0: the offset was NaN, and the whole
// screen showed one texel of the scene, the frost over it.
static const char *kRecip = "fn bo1_rcp(x: f32) -> f32 {\n"
                            "    return select(1.0 / x, 3.4028234e38, x == 0.0);\n"
                            "}\n"
                            "fn bo1_rsq(x: f32) -> f32 {\n"
                            "    return select(inverseSqrt(abs(x)), 3.4028234e38, x == 0.0);\n"
                            "}\n";

std::string ShaderEmit(const ShaderInfo &info, uint32_t shadowMask, uint32_t extraVaryings, uint32_t targetMask, bool byInstance,
    uint32_t *overrides)
{
    *overrides = 0;
    if (!info.error.empty() || !info.instrs)
        return "";
    const Parsed &p = *(const Parsed *)info.instrs;
    Emitter e(info, p, shadowMask, byInstance && !info.pixel);
    e.body = &e.mainBody;
    for (const ShaderInstr &in : p.instrs)
        e.Instr(in);
    if (e.inSub || e.depth)
        e.Fail("unbalanced flow control");
    if (!e.error.empty())
    {
        ((ShaderInfo &)info).error = e.error;
        return "";
    }
    bool pixel = info.pixel, sm3 = info.major >= 3;

    // the entry point: inputs into their registers, the shader, its outputs out
    std::string entry;
    if (!pixel)
    {
        // invariant: a surface drawn twice with different shaders, such as the depth prepass's and the lit pass's after
        // it, whose LESSEQUAL test passes only where it comes out at the same depth, gets the same position from both
        // where they compute it with the same instructions, as Direct3D 9 gave; WGSL leaves that to the compiler
        // otherwise, which can contract a multiply and an add into one in one shader and not in the other
        std::string in, out = "struct VsOut {\n    @builtin(position) @invariant bo1_pos: vec4f,\n";
        entry = "@vertex\nfn main(";
        std::string body, pos = "vec4f(0.0)";
        int k = 0;
        for (const Decl &d : p.decls)
            if (d.type == REG_INPUT)
            {
                in += "    @location(" + Num(k) + ") attr" + Num(k) + ": vec4f,\n";
                e.Private("v" + Num(d.num) + ": vec4f");
                body += "    v" + Num(d.num) + " = bo1_in(vin.attr" + Num(k) + ", " + Num(k) + "u);\n";
                ++k;
            }
        if (k)
            entry += "vin: VsIn";
        if (e.byInstance)
        {
            // the instance counts the arena's blocks of this shader's size: a draw's first is its block's place
            // (d3d9_webgpu.cpp: FirstInstance), and draws alike, their blocks in a row, are instances of one draw
            entry += std::string(k ? ", " : "") + "@builtin(instance_index) bo1_instance: u32";
            body = "    bo1_at = bo1_instance * " + Num((int)(info.UniformBytes() / 16)) + "u;\n" + body;
        }
        entry += ") -> VsOut {\n" + body + "    shader_main();\n    var res: VsOut;\n";
        uint32_t written = 0;
        if (sm3)
        {
            for (const Decl &d : p.decls)
            {
                if (d.type != REG_OUTPUT)
                    continue;
                e.Private("o" + Num(d.num) + ": vec4f");
                if (d.usage == USAGE_POSITION && d.index == 0)
                    pos = "o" + Num(d.num);
                int loc = VaryingLocation(d.usage, d.index);
                if (loc >= 0 && !(written >> loc & 1))
                {
                    written |= 1u << loc;
                    entry += "    res.vary" + Num(loc) + " = o" + Num(d.num) + ";\n";
                }
            }
        }
        else
        {
            e.Private("rast0: vec4f");
            pos = "rast0";
            for (int loc = 0; loc < VARYING_COUNT; ++loc)
                if (info.varyings >> loc & 1)
                {
                    std::string reg = loc < 10 ? "o" + Num(loc) : "oD" + Num(loc - 10);
                    e.Private(reg + ": vec4f");
                    written |= 1u << loc;
                    // vs_2_0 colors are clamped
                    entry += "    res.vary" + Num(loc) + " = " + (loc < 10 ? reg : "saturate(" + reg + ")") + ";\n";
                }
        }
        for (int loc = 0; loc < VARYING_COUNT; ++loc)
            if (extraVaryings >> loc & 1 && !(written >> loc & 1))
            {
                written |= 1u << loc;
                entry += "    res.vary" + Num(loc) + " = vec4f(0.0);\n";
            }
        for (int loc = 0; loc < VARYING_COUNT; ++loc)
            if (written >> loc & 1)
                out += "    @location(" + Num(loc) + ") vary" + Num(loc) + ": vec4f,\n";
        entry += "    let p = " + pos + ";\n";
        entry += "    res.bo1_pos = vec4f(p.xy + " + e.Fix() + ".xy * p.w, p.zw);\n    return res;\n}\n";
        std::string head;
        if (k)
            *overrides |= OVERRIDE_INPUTS;
        if (k)
            head += "struct VsIn {\n" + in + "}\n\n"
                    "override bo1_inColor: u32 = 0u;\n"
                    "override bo1_inUbyte: u32 = 0u;\n\n"
                    "fn bo1_in(v: vec4f, k: u32) -> vec4f {\n"
                    "    var r = v;\n"
                    "    if (((bo1_inColor >> k) & 1u) != 0u) { r = r.zyxw; }\n"
                    "    if (((bo1_inUbyte >> k) & 1u) != 0u) { r = round(r * 255.0); }\n"
                    "    return r;\n"
                    "}\n\n";
        entry = head + out + "}\n\n" + entry;
    }
    else
    {
        std::string in, body;
        uint32_t read = 0;
        for (const Decl &d : p.decls)
        {
            int loc = -1;
            std::string reg;
            if (d.type == REG_INPUT)
            {
                loc = sm3 ? VaryingLocation(d.usage, d.index) : VaryingLocation(USAGE_COLOR, d.num);
                reg = "v" + Num(d.num);
            }
            else if (d.type == REG_TEXTURE)
            {
                loc = VaryingLocation(USAGE_TEXCOORD, d.num);
                reg = "t" + Num(d.num);
            }
            if (loc < 0)
                continue;
            e.Private(reg + ": vec4f");
            read |= 1u << loc;
            std::string v = "pin.vary" + Num(loc);
            if (!sm3 && d.type == REG_INPUT)
                v = "saturate(" + v + ")"; // ps_2_0 colors are clamped
            // a register two inputs share (packed): each its components
            int sharing = 0;
            for (const Decl &o : p.decls)
                sharing += o.type == d.type && o.num == d.num;
            uint8_t mask = d.mask ? d.mask : 0xF;
            if (mask == 0xF || sharing == 1)
                body += "    " + reg + " = " + v + ";\n";
            else
                body += "    " + reg + " = select(" + reg + ", " + v + ", " + MaskVec(mask) + ");\n";
        }
        for (int loc = 0; loc < VARYING_COUNT; ++loc)
            if (read >> loc & 1)
                in += "    @location(" + Num(loc) + ") vary" + Num(loc) + ": vec4f,\n";
        if (e.usesVpos)
        {
            in += "    @builtin(position) bo1_fragPos: vec4f,\n";
            // D3D9's vPos: the pixel's whole coordinates
            body += "    vpos = vec4f(floor(pin.bo1_fragPos.xy), 0.0, 1.0);\n";
        }
        if (e.usesVface)
        {
            in += "    @builtin(front_facing) bo1_front: bool,\n";
            body += "    vface = vec4f(select(-1.0, 1.0, pin.bo1_front));\n";
        }
        std::string out;
        uint32_t outputs = info.colorOutputs & targetMask;
        for (int i = 0; i < 4; ++i)
            if (info.colorOutputs >> i & 1)
                e.Private("oC" + Num(i) + ": vec4f");
        for (int i = 0; i < 4; ++i)
            if (outputs >> i & 1)
                out += "    @location(" + Num(i) + ") c" + Num(i) + ": vec4f,\n";
        if (info.writesDepth)
        {
            out += "    @builtin(frag_depth) bo1_depth: f32,\n";
            e.Private("oDepth: f32");
        }
        std::string head;
        if (!in.empty())
            head += "struct PsIn {\n" + in + "}\n\n";
        if (!out.empty())
            head += "struct PsOut {\n" + out + "}\n\n";
        if (outputs)
        {
            *overrides |= OVERRIDE_CLAMP;
            head += "override bo1_clamp: u32 = 0u;\n";
        }
        if (info.colorOutputs & 1)
        {
            *overrides |= OVERRIDE_ALPHA;
            head += "override bo1_alphaFunc: u32 = 8u;\noverride bo1_srgbWrite: bool = false;\n\n" + std::string(kSrgbEncode);
        }
        entry = head + "\n@fragment\nfn main(" + (in.empty() ? "" : "pin: PsIn") + ")" + (out.empty() ? "" : " -> PsOut") + " {\n" + body +
                "    shader_main();\n";
        if (info.colorOutputs & 1)
        {
            // the alpha test (D3DRS_ALPHAFUNC; 8 is D3DCMP_ALWAYS) and sRGB writes, on the first render target
            entry += "    if (bo1_alphaFunc != 8u) {\n"
                     "        let a = oC0.a;\n"
                     "        let aref = U.bo1_fix.z;\n"
                     "        var keep = false;\n"
                     "        switch bo1_alphaFunc {\n"
                     "            case 2u: { keep = a < aref; }\n"
                     "            case 3u: { keep = a == aref; }\n"
                     "            case 4u: { keep = a <= aref; }\n"
                     "            case 5u: { keep = a > aref; }\n"
                     "            case 6u: { keep = a != aref; }\n"
                     "            case 7u: { keep = a >= aref; }\n"
                     "            default: { keep = false; }\n"
                     "        }\n"
                     "        if (!keep) { discard; }\n"
                     "    }\n"
                     "    if (bo1_srgbWrite) { oC0 = vec4f(bo1_srgb(oC0.rgb), oC0.a); }\n";
        }
        if (!out.empty())
        {
            entry += "    var res: PsOut;\n";
            for (int i = 0; i < 4; ++i)
                if (outputs >> i & 1)
                    entry += "    res.c" + Num(i) + " = select(oC" + Num(i) + ", saturate(oC" + Num(i) + "), ((bo1_clamp >> " + Num(i) + "u) & 1u) != 0u);\n";
            if (info.writesDepth)
                entry += "    res.bo1_depth = oDepth;\n";
            entry += "    return res;\n";
        }
        entry += "}\n";
    }

    // the module
    std::string m = "diagnostic(off, derivative_uniformity);\n\n";
    if (e.byInstance)
        m += "@group(0) @binding(0) var<storage, read> bo1_arena: array<vec4u>;\nvar<private> bo1_at: u32;\n";
    else
    {
        m += "struct Consts {\n    bo1_fix: vec4f,\n    bo1_bools: vec4u,\n";
        if (info.floatSlots)
            m += "    c: array<vec4f, " + Num(info.floatSlots) + ">,\n";
        if (info.intCount)
            m += "    i: array<vec4i, " + Num(info.intCount) + ">,\n";
        m += "}\n\n@group(0) @binding(" + std::string(pixel ? "1" : "0") + ") var<uniform> U: Consts;\n";
    }
    for (const ShaderInfo::Sampler &s : info.samplers)
    {
        int b = pixel ? s.stage : 32 + s.stage;
        std::string name = pixel ? Num(s.stage) : "v" + Num(s.stage);
        bool shadow = pixel && s.dim == DIM_2D && (shadowMask >> s.stage & 1);
        const char *type = shadow ? "texture_depth_2d" : s.dim == DIM_CUBE ? "texture_cube<f32>" : s.dim == DIM_3D ? "texture_3d<f32>" : "texture_2d<f32>";
        m += "@group(1) @binding(" + Num(b) + ") var " + (pixel ? "tex" : "vtex") + Num(s.stage) + ": " + type + ";\n";
        m += "@group(1) @binding(" + Num(b + 4 * (pixel ? 4 : 1)) + ") var " + (pixel ? "smp" : "vsmp") + Num(s.stage) + ": " +
             (shadow ? "sampler_comparison" : "sampler") + ";\n";
    }
    m += "\n";
    for (auto &def : p.defF)
        m += "const k" + Num(def.first) + " = vec4f(" + Float(def.second[0]) + ", " + Float(def.second[1]) + ", " + Float(def.second[2]) + ", " +
             Float(def.second[3]) + ");\n";
    for (auto &def : p.defI)
        m += "const ki" + Num(def.first) + " = vec4i(" + Num(def.second[0]) + ", " + Num(def.second[1]) + ", " + Num(def.second[2]) + ", " +
             Num(def.second[3]) + ");\n";
    for (const std::string &v : e.privates)
        m += "var<private> " + v + ";\n";
    m += "\n";
    if (e.usesLit)
        m += kLit;
    if (e.usesRecip)
        m += kRecip;
    m += e.subs;
    m += "\nfn shader_main() {\n" + e.mainBody + "}\n\n" + entry;
    return m;
}
} // namespace bo1gpu
