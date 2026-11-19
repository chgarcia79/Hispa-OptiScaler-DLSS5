#include "pch.h"
#include "scanner.h"
#include <proxies/KernelBase_Proxy.h>

struct SectionRange
{
    BYTE *start, *end;
};

// Executable sections, or the initialized data ones
std::vector<SectionRange> GetSections(HMODULE hMod, bool executable = true)
{
    std::vector<SectionRange> secs;

    if (hMod == nullptr)
        return secs;

    BYTE* base = reinterpret_cast<BYTE*>(hMod);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto first = IMAGE_FIRST_SECTION(nt);

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        auto& s = first[i];

        auto isExecutable = (s.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;

        if (executable ? isExecutable : !isExecutable && (s.Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA))
        {
            BYTE* start = base + s.VirtualAddress;
            BYTE* end = start + s.Misc.VirtualSize;
            secs.push_back({ start, end });
        }
    }

    return secs;
}

uintptr_t FindPattern(uintptr_t startAddress, uintptr_t maxSize, const char* mask)
{
    std::vector<std::pair<uint8_t, bool>> pattern;

    for (size_t i = 0; i < strlen(mask);)
    {
        if (mask[i] != '?')
        {
            pattern.emplace_back(static_cast<uint8_t>(strtoul(&mask[i], nullptr, 16)), false);
            i += 3;
        }
        else
        {
            pattern.emplace_back(0x00, true);
            i += 2;
        }
    }

    const auto dataStart = reinterpret_cast<const uint8_t*>(startAddress);
    const auto dataEnd = dataStart + maxSize + 1;

    auto sig = std::search(dataStart, dataEnd, pattern.begin(), pattern.end(),
                           [](uint8_t currentByte, std::pair<uint8_t, bool> Pattern)
                           { return Pattern.second || (currentByte == Pattern.first); });

    if (sig == dataEnd)
        return NULL;

    return std::distance(dataStart, sig) + startAddress;
}

uintptr_t scanner::GetAddress(const std::wstring_view moduleName, const std::string_view pattern, ptrdiff_t offset,
                              uintptr_t startAddress)
{
    uintptr_t address = NULL;
    auto module = GetModuleHandle(moduleName.data());

    if (module == nullptr)
        return NULL;

    auto sections = GetSections(module);

    if (startAddress != 0)
    {
        for (size_t i = 0; i < sections.size(); i++)
        {
            auto section = &sections[i];

            if (((uintptr_t) section->start < startAddress && (uintptr_t) section->end > startAddress))
            {
                address = FindPattern(startAddress, (uintptr_t) section->end - startAddress, pattern.data());

                if (address != NULL)
                    break;
            }
            else if ((uintptr_t) section->start > startAddress)
            {
                address = FindPattern((uintptr_t) section->start, (uintptr_t) section->end - (uintptr_t) section->start,
                                      pattern.data());

                if (address != NULL)
                    break;
            }
        }
    }
    else
    {
        for (size_t i = 0; i < sections.size(); i++)
        {
            auto section = &sections[i];
            address = FindPattern((uintptr_t) section->start, (uintptr_t) section->end - (uintptr_t) section->start,
                                  pattern.data());

            if (address != NULL)
                break;
        }
    }

    if (address != NULL)
    {
        return (address + offset);
    }
    else
    {
        return NULL;
    }
}

uintptr_t scanner::GetAddress(HMODULE module, const std::string_view pattern, ptrdiff_t offset, uintptr_t startAddress)
{
    if (module == nullptr)
        return NULL;

    uintptr_t address = NULL;
    auto sections = GetSections(module);

    if (startAddress != 0)
    {
        for (size_t i = 0; i < sections.size(); i++)
        {
            auto section = &sections[i];

            if (((uintptr_t) section->start < startAddress && (uintptr_t) section->end > startAddress))
            {
                address = FindPattern(startAddress, (uintptr_t) section->end - startAddress, pattern.data());

                if (address != NULL)
                    break;
            }
            else if ((uintptr_t) section->start > startAddress)
            {
                address = FindPattern((uintptr_t) section->start, (uintptr_t) section->end - (uintptr_t) section->start,
                                      pattern.data());

                if (address != NULL)
                    break;
            }
        }
    }
    else
    {
        for (size_t i = 0; i < sections.size(); i++)
        {
            auto section = &sections[i];
            address = FindPattern((uintptr_t) section->start, (uintptr_t) section->end - (uintptr_t) section->start,
                                  pattern.data());

            if (address != NULL)
                break;
        }
    }

    if (address != NULL)
    {
        return (address + offset);
    }
    else
    {
        return NULL;
    }
}

uintptr_t scanner::GetOffsetFromInstruction(const std::wstring_view moduleName, const std::string_view pattern,
                                            ptrdiff_t offset)
{
    auto module = GetModuleHandle(moduleName.data());

    if (module == nullptr)
        return NULL;

    uintptr_t address = NULL;

    auto sections = GetSections(module);

    for (size_t i = 0; i < sections.size(); i++)
    {
        auto section = &sections[i];
        address = FindPattern((uintptr_t) section->start, (uintptr_t) section->end - (uintptr_t) section->start,
                              pattern.data());

        if (address != NULL)
            break;
    }

    if (address != NULL)
    {
        auto reloffset = *reinterpret_cast<int32_t*>(address + offset) + sizeof(int32_t);
        return (address + offset + reloffset);
    }
    else
    {
        return NULL;
    }
}

uintptr_t scanner::FindWideString(HMODULE module, std::wstring_view text)
{
    auto needle = reinterpret_cast<const BYTE*>(text.data());
    auto needleSize = (text.size() + 1) * sizeof(wchar_t);

    for (auto& section : GetSections(module, false))
    {
        auto found = std::search(section.start, section.end, needle, needle + needleSize);
        if (found != section.end)
            return reinterpret_cast<uintptr_t>(found);
    }

    return 0;
}

uintptr_t scanner::FindRipReference(HMODULE module, uintptr_t target)
{
    for (auto& section : GetSections(module))
    {
        for (auto p = section.start; p + 7 <= section.end; p++)
        {
            if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D || (p[2] & 0xC7) != 0x05)
                continue;

            if (reinterpret_cast<uintptr_t>(p + 7) + *reinterpret_cast<int32_t*>(p + 3) == target)
                return reinterpret_cast<uintptr_t>(p);
        }
    }

    return 0;
}

uintptr_t scanner::GetFunctionStart(uintptr_t address)
{
    DWORD64 imageBase = 0;
    auto function = RtlLookupFunctionEntry(address, &imageBase, nullptr);
    if (function == nullptr)
        return 0;

    return imageBase + function->BeginAddress;
}
