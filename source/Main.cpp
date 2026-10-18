// PedSkills III - port of JuniorDjjr's PedSkills (GTA San Andreas) to GTA III 1.0.
// https://github.com/JuniorDjjr/PedSkills
//
// How GTA III aims (CWeapon::FireInstantHit, 0x55D2E0; see re3 Weapon.cpp): when a ped shoots
// at the entity it points its gun at, the game aims at that entity and moves the end of the
// shot, which lies at the weapon's full range, by a random +-0.2 * (100 - accuracy) on x and y
// and +-0.1 * (100 - accuracy) on z. So 100 is a perfect shot, and values above 100 are NOT
// better: the game doesn't clamp them, so 120 misses like 80 and 255 is the worst possible.
// If the ped's arm IK hasn't locked onto the target yet, the shot goes straight out of the
// hand and accuracy isn't used at all.
//
// Like the SA mod, this hooks the one place that reads the shooter's accuracy and works out
// the accuracy for that shot. The per-ped value itself is only changed at spawn (cop and ped
// constructors) and when a mission script sets it.
//
// The hooks are plain jumps to small naked stubs (as in the SA mod), installed once the game
// starts its scripts, after every other ASI has loaded. An earlier build used SafetyHook mid
// hooks installed while the ASIs were still loading, which made other mods crash at startup.

#include <plugin.h>
#include <CPed.h>
#include <CVehicle.h>
#include <CWeaponInfo.h>
#include <CTimer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

#include <Windows.h>

using namespace plugin;

namespace {

// GTA III 1.0 addresses, from the game's own code.
namespace addr {
    // CWeapon::FireInstantHit, ped shooting at m_pPointGunAt:
    constexpr uintptr_t ReadIkFlags = 0x55D550;   // mov eax, [ebp+214h]  (CPedIK flags, bit 0 = gun pointed)
    constexpr uintptr_t ReadAccuracy = 0x55D55F;  // movzx eax, byte [ebp+49Bh]  (m_nWeaponAccuracy)
    // Opcode 02E2 SET_CHAR_ACCURACY: mov [eax+49Bh], dl
    constexpr uintptr_t ScriptSetAccuracy = 0x446CA5;
    // Immediate operands of "mov byte [eax+49Bh], N" / "mov dword [eax+2C4h], N" (instruction + 6).
    constexpr uintptr_t PedAccuracy = 0x4C5048 + 6;   // CPed::CPed: 60
    constexpr uintptr_t CopAccuracy = 0x4C122E + 6;   // CCopPed::CCopPed, street cop: 60
    constexpr uintptr_t SwatAccuracy = 0x4C128D + 6;  // SWAT: 68
    constexpr uintptr_t FbiAccuracy = 0x4C12EC + 6;   // FBI: 76
    constexpr uintptr_t ArmyAccuracy = 0x4C1355 + 6;  // army: 84
    constexpr uintptr_t CopArmour = 0x4C1215 + 6;     // 0.0
    constexpr uintptr_t SwatArmour = 0x4C1274 + 6;    // 50.0
    constexpr uintptr_t FbiArmour = 0x4C12D3 + 6;     // 100.0
    constexpr uintptr_t ArmyArmour = 0x4C133C + 6;    // 100.0
}

struct Settings {
    // [General]
    float playerAccuracyMult = 1.0f;
    float accuracyMult = 1.0f;
    float accuracyVelocityFactor = 1.0f;
    float accuracyDistanceFactor = 1.0f;
    float accuracyOnVehicleFactor = 1.0f;
    float weaponRangeMult = 1.0f;
    bool alwaysAimAtTarget = true;
    float maxAimAngle = 60.0f;
    bool debugLog = false;
    // [Default skills], -1 = keep the game's value
    int weaponAccuracy = -1;
    int copsWeaponAccuracy = -1;
    int swatWeaponAccuracy = -1;
    int fbiWeaponAccuracy = -1;
    int armyWeaponAccuracy = -1;
    float copsArmour = -1.0f;
    float swatArmour = -1.0f;
    float fbiArmour = -1.0f;
    float armyArmour = -1.0f;
    float scriptAccuracyMult = 1.0f;
} gSet;

std::string gDir;   // folder of this .asi, with trailing slash

std::string TrimStr(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string LowerStr(std::string s) {
    for (char &c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

void Log(const char *fmt, ...) {
    if (!gSet.debugLog)
        return;
    FILE *f = nullptr;
    if (fopen_s(&f, (gDir + "PedSkills.III.log").c_str(), "a") != 0 || !f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void FindDir() {
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&FindDir), &mod);
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(mod, buf, MAX_PATH);
    std::string p(buf, n < MAX_PATH ? n : 0);
    size_t pos = p.find_last_of("\\/");
    gDir = pos == std::string::npos ? std::string() : p.substr(0, pos + 1);
}

void LoadIni() {
    std::ifstream file(gDir + "PedSkills.III.ini");
    if (!file.is_open())
        return;
    std::map<std::string, std::string> kv;
    std::string line, section;
    while (std::getline(file, line)) {
        std::string t = TrimStr(line);
        if (t.empty() || t[0] == ';' || t[0] == '#')
            continue;
        if (t[0] == '[' && t.back() == ']') {
            section = LowerStr(TrimStr(t.substr(1, t.size() - 2)));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos)
            continue;
        std::string val = TrimStr(t.substr(eq + 1));
        size_t semi = val.find(';');   // allow "Key = 1.0 ; comment"
        if (semi != std::string::npos)
            val = TrimStr(val.substr(0, semi));
        kv[section + '\x1f' + LowerStr(TrimStr(t.substr(0, eq)))] = val;
    }
    auto get = [&](const char *sec, const char *key) -> const std::string * {
        auto it = kv.find(std::string(sec) + '\x1f' + key);
        return it == kv.end() || it->second.empty() ? nullptr : &it->second;
    };
    auto f = [&](const char *sec, const char *key, float &out) {
        if (auto v = get(sec, key)) out = strtof(v->c_str(), nullptr);
    };
    auto i = [&](const char *sec, const char *key, int &out) {
        if (auto v = get(sec, key)) out = static_cast<int>(strtol(v->c_str(), nullptr, 10));
    };
    auto b = [&](const char *sec, const char *key, bool &out) {
        if (auto v = get(sec, key)) out = strtol(v->c_str(), nullptr, 10) != 0;
    };
    f("general", "playeraccuracymult", gSet.playerAccuracyMult);
    f("general", "accuracymult", gSet.accuracyMult);
    f("general", "accuracyvelocityfactor", gSet.accuracyVelocityFactor);
    f("general", "accuracydistancefactor", gSet.accuracyDistanceFactor);
    f("general", "accuracyonvehiclefactor", gSet.accuracyOnVehicleFactor);
    f("general", "weaponrangemult", gSet.weaponRangeMult);
    b("general", "alwaysaimattarget", gSet.alwaysAimAtTarget);
    f("general", "maxaimangle", gSet.maxAimAngle);
    b("general", "debuglog", gSet.debugLog);
    i("default skills", "weaponaccuracy", gSet.weaponAccuracy);
    i("default skills", "copsweaponaccuracy", gSet.copsWeaponAccuracy);
    i("default skills", "swatweaponaccuracy", gSet.swatWeaponAccuracy);
    i("default skills", "fbiweaponaccuracy", gSet.fbiWeaponAccuracy);
    i("default skills", "armyweaponaccuracy", gSet.armyWeaponAccuracy);
    f("default skills", "copsarmour", gSet.copsArmour);
    f("default skills", "swatarmour", gSet.swatArmour);
    f("default skills", "fbiarmour", gSet.fbiArmour);
    f("default skills", "armyarmour", gSet.armyArmour);
    f("default skills", "scriptaccuracymult", gSet.scriptAccuracyMult);
}

int ClampAccuracy(float v) {
    if (!(v > 0.0f))
        return 0;
    return v >= 100.0f ? 100 : static_cast<int>(v + 0.5f);
}

void PatchAccuracy(uintptr_t at, int value) {
    if (value >= 0)
        injector::WriteMemory<uint8_t>(at, static_cast<uint8_t>(ClampAccuracy(static_cast<float>(value))), true);
}

void PatchArmour(uintptr_t at, float value) {
    if (value >= 0.0f)
        injector::WriteMemory<float>(at, value, true);
}

CVector VelocityOf(CEntity *e) {
    if (e->m_nType == ENTITY_TYPE_PED) {
        CPed *ped = static_cast<CPed *>(e);
        if (ped->m_bInVehicle && ped->m_pVehicle)
            return ped->m_pVehicle->m_vecMoveSpeed;
        return ped->m_vecMoveSpeed;
    }
    if (e->m_nType == ENTITY_TYPE_VEHICLE || e->m_nType == ENTITY_TYPE_OBJECT)
        return static_cast<CPhysical *>(e)->m_vecMoveSpeed;
    return CVector(0.0f, 0.0f, 0.0f);
}

// Accuracy (0-100) for one shot by `shooter` at its m_pPointGunAt.
// The SA mod divides accuracy by distance and target-movement factors. In GTA III the miss is
// an angle, so distance already lowers the hit rate on its own; dividing by it as well made
// cops past a few metres far worse than the unmodded game. Here the factors take accuracy
// points away instead, sized so a still target nearby is hit about as often as the base value
// says, and a fast or far one less often.
int ShotAccuracy(CPed *shooter) {
    float base = static_cast<float>(shooter->m_nWeaponAccuracy);
    if (base > 100.0f)
        base = 100.0f;   // a script may have set more; the game would treat 120 like 80
    if (shooter->IsPlayer())
        return ClampAccuracy(base * gSet.playerAccuracyMult);

    float acc = base * gSet.accuracyMult;
    if (acc > 100.0f)
        acc = 100.0f;

    CEntity *target = shooter->m_pPointGunAt;
    if (target) {
        CVector d = target->GetPosition() - shooter->GetPosition();
        float dist = d.Magnitude();

        // Distance: same curve as the SA mod (log2 of 0.3 * metres, 0 up to ~3 m).
        float distTerm = dist * 0.3f > 1.0f ? std::min(log2f(dist * 0.3f), 10.0f) : 0.0f;
        acc -= 5.0f * distTerm * gSet.accuracyDistanceFactor;            // 10 m: -8, 30 m: -16

        // Target movement: how fast it crosses the shooter's view, in degrees per second.
        if (dist > 0.5f) {
            CVector rel = VelocityOf(target) - VelocityOf(shooter);
            CVector dir = d / dist;
            float along = rel.x * dir.x + rel.y * dir.y + rel.z * dir.z;
            CVector cross = rel - dir * along;
            float degPerSec = cross.Magnitude() * 50.0f / dist * 57.29578f;   // move speed is per 1/50 s
            acc -= std::min(degPerSec * 0.5f, 40.0f) * gSet.accuracyVelocityFactor;   // 30 deg/s: -15
        }
    }
    if (shooter->m_bInVehicle)
        acc -= 15.0f * gSet.accuracyOnVehicleFactor;

    // The spread is applied at the end of the weapon's range, so a longer range alone would
    // shrink the miss angle. Scale the inaccuracy with WeaponRangeMult to keep the angle.
    if (gSet.weaponRangeMult > 0.0f && gSet.weaponRangeMult != 1.0f && acc < 100.0f)
        acc = 100.0f - (100.0f - acc) * gSet.weaponRangeMult;
    return ClampAccuracy(acc);
}

// Whether the target is within maxAimAngle of where the shooter faces.
bool FacingTarget(CPed *shooter) {
    CEntity *target = shooter->m_pPointGunAt;
    if (!target)
        return false;
    CVector d = target->GetPosition() - shooter->GetPosition();
    float len = sqrtf(d.x * d.x + d.y * d.y);
    if (len < 0.01f)
        return true;
    CVector &fwd = shooter->GetForward();
    float flen = sqrtf(fwd.x * fwd.x + fwd.y * fwd.y);
    if (flen < 0.01f)
        return false;
    float cosAngle = (d.x * fwd.x + d.y * fwd.y) / (len * flen);
    return cosAngle >= cosf(gSet.maxAimAngle / 57.29578f);
}

float gOriginalRange[14];
bool gRangeSaved = false;

void ApplyWeaponRange() {
    if (gSet.weaponRangeMult == 1.0f)
        return;
    if (!gRangeSaved) {
        if (aWeaponInfo[1].m_fRange <= 0.0f)   // weapon.dat not loaded yet
            return;
        for (int w = 0; w < 14; w++)
            gOriginalRange[w] = aWeaponInfo[w].m_fRange;
        gRangeSaved = true;
    }
    for (int w = 0; w < 14; w++)
        aWeaponInfo[w].m_fRange = gOriginalRange[w] * gSet.weaponRangeMult;
}

// Called from the stubs below (cdecl).
uint32_t __cdecl ShotAccuracyHook(CPed *shooter) {
    int acc = ShotAccuracy(shooter);
    static int lastLog = 0;
    if (gSet.debugLog && CTimer::m_snTimeInMilliseconds - lastLog > 250) {
        lastLog = CTimer::m_snTimeInMilliseconds;
        Log("shot: pedtype %d, base accuracy %d -> %d", static_cast<int>(shooter->m_ePedType),
            static_cast<int>(shooter->m_nWeaponAccuracy), acc);
    }
    return static_cast<uint32_t>(acc);
}

// When the ped's arm hasn't locked onto the target yet, the game fires straight out of the
// hand. With AlwaysAimAtTarget, NPCs facing the target aim at it anyway.
uint32_t __cdecl IkFlagsHook(CPed *shooter) {
    uint32_t flags = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(shooter) + 0x214);
    if (!(flags & 1) && gSet.alwaysAimAtTarget && !shooter->IsPlayer() && FacingTarget(shooter))
        flags |= 1;
    return flags;
}

void __cdecl ScriptAccuracyHook(CPed *ped, uint32_t edx) {
    ped->m_nWeaponAccuracy = static_cast<unsigned char>(ClampAccuracy(static_cast<float>(edx & 0xFF) * gSet.scriptAccuracyMult));
}

// Each stub saves all registers, calls its hook, and returns to the instruction after the one
// it replaced. pushad keeps eax at [esp+28], so writing there sets the eax popad restores.
// None of the replaced instructions' successors read the flags.

// 0x55D550: mov eax, [ebp+214h]  (6 bytes) -> eax = IkFlagsHook(ebp)
void __declspec(naked) IkFlagsStub() {
    __asm {
        pushad
        push ebp
        call IkFlagsHook
        add esp, 4
        mov [esp + 28], eax
        popad
        push 0x55D556
        ret
    }
}

// 0x55D55F: movzx eax, byte [ebp+49Bh]  (7 bytes) -> eax = ShotAccuracyHook(ebp)
void __declspec(naked) ShotAccuracyStub() {
    __asm {
        pushad
        push ebp
        call ShotAccuracyHook
        add esp, 4
        mov [esp + 28], eax
        popad
        push 0x55D566
        ret
    }
}

// 0x446CA5: mov [eax+49Bh], dl  (6 bytes) -> ScriptAccuracyHook(eax, edx)
void __declspec(naked) ScriptAccuracyStub() {
    __asm {
        pushad
        push edx
        push eax
        call ScriptAccuracyHook
        add esp, 8
        popad
        push 0x446CAB
        ret
    }
}

void JumpTo(uintptr_t at, size_t length, void *stub) {
    injector::MakeJMP(at, stub, true);
    if (length > 5)
        injector::MakeNOP(at + 5, length - 5, true);
}

void Install() {
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    PatchAccuracy(addr::PedAccuracy, gSet.weaponAccuracy);
        PatchAccuracy(addr::CopAccuracy, gSet.copsWeaponAccuracy);
        PatchAccuracy(addr::SwatAccuracy, gSet.swatWeaponAccuracy);
        PatchAccuracy(addr::FbiAccuracy, gSet.fbiWeaponAccuracy);
        PatchAccuracy(addr::ArmyAccuracy, gSet.armyWeaponAccuracy);
        PatchArmour(addr::CopArmour, gSet.copsArmour);
        PatchArmour(addr::SwatArmour, gSet.swatArmour);
        PatchArmour(addr::FbiArmour, gSet.fbiArmour);
    PatchArmour(addr::ArmyArmour, gSet.armyArmour);

    JumpTo(addr::ReadAccuracy, 7, &ShotAccuracyStub);
    if (gSet.alwaysAimAtTarget)
        JumpTo(addr::ReadIkFlags, 6, &IkFlagsStub);
    if (gSet.scriptAccuracyMult != 1.0f)
        JumpTo(addr::ScriptSetAccuracy, 6, &ScriptAccuracyStub);
    Log("hooks installed");
}

struct PedSkills {
    PedSkills() {
        FindDir();
        LoadIni();
        Log("PedSkills III loaded");
        // initScriptsEvent runs on every new game and load; Install() only acts the first time.
        Events::initScriptsEvent += [] {
            Install();
            ApplyWeaponRange();
        };
    }
} gPedSkills;

}
