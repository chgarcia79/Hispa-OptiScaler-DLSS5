#include "pch.h"
#include "XeSS_UE_Dx12_Internal.h"

#include <detours/detours.h>
#include <scanner/scanner.h>

namespace XeSSUnreal::Detail
{
namespace
{
constexpr uint32_t VolatileRegisters = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11);

template <typename Visitor> void WalkInstructions(uintptr_t start, uintptr_t end, int maxCount, Visitor&& visit)
{
    auto address = start;
    for (int i = 0; i < maxCount && address < end; i++)
    {
        auto next = (uintptr_t) DetourCopyInstruction(nullptr, nullptr, (PVOID) address, nullptr, nullptr);
        if (next <= address || !visit(address, next))
            return;
        address = next;
    }
}

uintptr_t CallTarget(uintptr_t address, uintptr_t next)
{
    auto code = (uint8_t*) address;
    if (code[0] == 0xE8)
        return next + *(int32_t*) (code + 1);
    if (code[0] == 0x48)
        code++;
    if (code[0] == 0xFF && code[1] == 0x15)
    {
        uintptr_t target = 0;
        SafeRead(next + *(int32_t*) (code + 2), target);
        return target;
    }
    return 0;
}

void TrackViewRegisters(uintptr_t address, uint32_t& viewRegisters, int32_t& loadOffset)
{
    auto code = (uint8_t*) address;
    uint8_t rex = 0;
    if ((code[0] & 0xF0) == 0x40) { rex = code[0]; code++; }
    auto movups = code[0] == 0x0F && code[1] == 0x10;
    auto opcode = code[0];
    if (!movups && opcode != 0x8B && opcode != 0x89 && opcode != 0x8D)
        return;
    auto modrm = movups ? code[2] : code[1];
    auto displacement = movups ? code + 3 : code + 2;
    auto mod = modrm >> 6;
    auto reg = ((modrm >> 3) & 7) | ((rex & 4) << 1);
    auto rm = (modrm & 7) | ((rex & 1) << 3);
    auto wide = (rex & 8) != 0;
    if (opcode == 0x8D) { viewRegisters &= ~(1u << reg); return; }
    if (mod == 3 && !movups)
    {
        if (!wide) return;
        auto dst = opcode == 0x8B ? reg : rm;
        auto src = opcode == 0x8B ? rm : reg;
        if (viewRegisters & (1u << src)) viewRegisters |= 1u << dst; else viewRegisters &= ~(1u << dst);
        return;
    }
    if (opcode == 0x89) return;
    if (mod == 2 && (rm & 7) != 4 && (viewRegisters & (1u << rm)) && (movups || wide) && loadOffset == 0)
        loadOffset = *(int32_t*) displacement;
    if (!movups)
        viewRegisters &= ~(1u << reg);
}

uintptr_t CallAfterName(uintptr_t nameReference, bool& nameArgument)
{
    uintptr_t target = 0;
    bool fifth = false, seventh = false;
    WalkInstructions(nameReference, nameReference + 0x40, 16,
                     [&](uintptr_t address, uintptr_t next)
                     {
                         auto code = (uint8_t*) address;
                         if ((code[0] == 0x48 || code[0] == 0x4C) && code[1] == 0x89 && (code[2] & 0xC7) == 0x44 &&
                             code[3] == 0x24 && code[4] == 0x20)
                             fifth = true;
                         if (code[0] == 0xC6 && code[1] == 0x44 && code[2] == 0x24 && code[3] == 0x30 && code[4] == 1)
                             seventh = true;
                         if (code[0] == 0xE8)
                             target = CallTarget(address, next);
                         return target == 0;
                     });
    nameArgument = fifth && seventh;
    return target;
}

void FindViewSizeAccess(uintptr_t function, uintptr_t nameReference, uintptr_t& getSecondaryViewRectSize,
                        int32_t& viewRectOffset)
{
    constexpr uint32_t R9 = 1 << 9, Rcx = 1 << 1;
    auto walk = [&](uintptr_t start, uintptr_t end, int maxCount)
    {
        uint32_t viewRegisters = R9;
        WalkInstructions(start, end, maxCount,
                         [&](uintptr_t address, uintptr_t next)
                         {
                             if (auto target = CallTarget(address, next); target != 0)
                             {
                                 if (getSecondaryViewRectSize == 0 && (viewRegisters & Rcx))
                                     getSecondaryViewRectSize = target;
                                 viewRegisters &= ~VolatileRegisters;
                                 return true;
                             }
                             TrackViewRegisters(address, viewRegisters, viewRectOffset);
                             return true;
                         });
    };
    walk(function, nameReference, 64);
    if (viewRectOffset != 0) return;
    bool nameArgument = false;
    if (auto helper = CallAfterName(nameReference, nameArgument); helper != 0)
        walk(helper, helper + 0x400, 128);
}

bool StoresThroughRdx(uintptr_t function)
{
    constexpr uint8_t Rdx = 2;
    bool stores = false;
    WalkInstructions(function, function + 0x200, 48,
                     [&](uintptr_t address, uintptr_t)
                     {
                         auto code = (uint8_t*) address;
                         while (*code == 0x66 || *code == 0xF2 || *code == 0xF3) code++;
                         uint8_t rex = 0;
                         if ((*code & 0xF0) == 0x40) rex = *code++;
                         auto twoByte = code[0] == 0x0F;
                         auto opcode = twoByte ? code[1] : code[0];
                         auto modrm = twoByte ? code[2] : code[1];
                         auto mod = modrm >> 6;
                         auto reg = ((modrm >> 3) & 7) | ((rex & 4) << 1);
                         auto rm = (modrm & 7) | ((rex & 1) << 3);
                         if (!twoByte && (opcode == 0xE8 || opcode == 0xC3 || (opcode == 0xFF && ((modrm >> 3) & 7) == 2)))
                             return false;
                         auto store = twoByte ? (opcode == 0x11 || opcode == 0x29 || opcode == 0x7F)
                                              : (opcode == 0x88 || opcode == 0x89 || opcode == 0xC6 || opcode == 0xC7);
                         if (store && mod != 3 && rm == Rdx) { stores = true; return false; }
                         if (!twoByte && (((opcode == 0x8B || opcode == 0x8D) && reg == Rdx) ||
                                          (opcode == 0xBA && (rex & 1) == 0) ||
                                          ((opcode == 0x31 || opcode == 0x33) && mod == 3 && reg == Rdx)))
                             return false;
                         return true;
                     });
    return stores;
}
} // namespace

bool HookModule(HMODULE module)
{
    uintptr_t name = 0;
    for (auto textureName : { L"Upscaled Velocity Texture", L"XeSS::FlattenedVelocity", L"XeSS::UpscaledVelocity" })
    {
        name = scanner::FindWideString(module, textureName);
        if (name != 0) break;
    }
    if (name == 0) return false;
    auto reference = scanner::FindRipReference(module, name);
    if (reference == 0) return false;
    auto function = scanner::GetFunctionStart(reference);
    if (function == 0) return false;
    if (StoresThroughRdx(function))
    {
        bool nameArgument = false;
        auto flattening = CallAfterName(reference, nameArgument);
        if (flattening == 0 || !nameArgument) return false;
        o_FlattenVelocity = (PFN_FlattenVelocity) flattening;
        g_xessVelocityName = (const wchar_t*) name;
        DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) o_FlattenVelocity, hkFlattenVelocity);
        DetourTransactionCommit();
        LOG_INFO("Hooked UE XeSS velocity flattening at {:X}", flattening);
        return true;
    }
    uintptr_t getSecondary = 0;
    int32_t viewRectOffset = 0;
    FindViewSizeAccess(function, reference, getSecondary, viewRectOffset);
    o_AddVelocityFlatteningXeSSPass = (PFN_AddVelocityFlatteningXeSSPass) function;
    if (getSecondary != 0 && viewRectOffset > 0)
    {
        o_GetSecondaryViewRectSize = (PFN_GetSecondaryViewRectSize) getSecondary;
        g_viewRectOffset = viewRectOffset;
    }
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_AddVelocityFlatteningXeSSPass, hkAddVelocityFlatteningXeSSPass);
    if (o_GetSecondaryViewRectSize != nullptr)
        DetourAttach(&(PVOID&) o_GetSecondaryViewRectSize, hkGetSecondaryViewRectSize);
    DetourTransactionCommit();
    LOG_INFO("Hooked UE XeSS AddVelocityFlatteningXeSSPass at {:X}", function);
    return true;
}
} // namespace XeSSUnreal::Detail
