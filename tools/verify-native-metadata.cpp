// SPDX-License-Identifier: MIT
// Inspect the linked PE/PDB without loading kernel code. In particular, reject
// a serialized KSMULTIPLE_ITEM used where PortCls expects KSATTRIBUTE_LIST.
#include <windows.h>
#include <dbghelp.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

class Image {
    std::vector<unsigned char> bytes_;
    IMAGE_NT_HEADERS64 nt_{};
    std::vector<IMAGE_SECTION_HEADER> sections_;
    template<class T> T file(size_t offset) const {
        if (offset > bytes_.size() || sizeof(T) > bytes_.size() - offset)
            throw std::runtime_error("PE read outside file");
        T value; std::memcpy(&value, bytes_.data() + offset, sizeof(value)); return value;
    }
public:
    explicit Image(const char *path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open driver image");
        bytes_.assign(std::istreambuf_iterator<char>(input), {});
        auto dos = file<IMAGE_DOS_HEADER>(0);
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
            throw std::runtime_error("invalid DOS header");
        nt_ = file<IMAGE_NT_HEADERS64>(dos.e_lfanew);
        if (nt_.Signature != IMAGE_NT_SIGNATURE || nt_.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt_.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            throw std::runtime_error("expected an x64 PE image");
        size_t table = dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt_.FileHeader.SizeOfOptionalHeader;
        for (unsigned i = 0; i < nt_.FileHeader.NumberOfSections; ++i)
            sections_.push_back(file<IMAGE_SECTION_HEADER>(table + i * sizeof(IMAGE_SECTION_HEADER)));
    }
    template<class T> T rva(uint64_t address) const {
        if (address > UINT32_MAX) throw std::runtime_error("invalid RVA");
        for (const auto &section : sections_) {
            if (address >= section.VirtualAddress) {
                uint64_t delta = address - section.VirtualAddress;
                if (delta <= section.SizeOfRawData && sizeof(T) <= section.SizeOfRawData - delta)
                    return file<T>(section.PointerToRawData + static_cast<size_t>(delta));
            }
        }
        throw std::runtime_error("metadata pointer outside initialized PE sections");
    }
    template<class T> T va(uint64_t address) const {
        if (address < nt_.OptionalHeader.ImageBase) throw std::runtime_error("invalid preferred-image pointer");
        return rva<T>(address - nt_.OptionalHeader.ImageBase);
    }
    void check(uint64_t table, size_t count) const {
        if (!count || count > 128) throw std::runtime_error("invalid data-range table size");
        unsigned ranges = 0, attributes = 0;
        for (size_t i = 0; i < count; ++i) {
            uint64_t range = rva<uint64_t>(table + i * 8);
            uint32_t size = va<uint32_t>(range), flags = va<uint32_t>(range + 4);
            if (size < 64 || size > 4096 || (flags & ~2u))
                throw std::runtime_error("invalid data range header");
            ++ranges;
            if (!(flags & 2)) continue;
            if (++i == count) throw std::runtime_error("missing KSATTRIBUTE_LIST");
            uint64_t list = rva<uint64_t>(table + i * 8);
            uint32_t number = va<uint32_t>(list);
            if (!number || number > 8)
                throw std::runtime_error("invalid KSATTRIBUTE_LIST.Count (serialized list used as a descriptor?)");
            uint64_t pointers = va<uint64_t>(list + 8);
            for (uint32_t n = 0; n < number; ++n) {
                uint64_t attribute = va<uint64_t>(pointers + n * 8);
                uint32_t attributeSize = va<uint32_t>(attribute);
                if (attributeSize < 24 || attributeSize > 4096)
                    throw std::runtime_error("invalid KSATTRIBUTE size");
                (void)va<unsigned char>(attribute + attributeSize - 1);
                ++attributes;
            }
        }
        if (!ranges || !attributes) throw std::runtime_error("expected mode-aware stream ranges");
        std::printf("Native metadata PASS: %u ranges, %u attribute references; no kernel code loaded.\n", ranges, attributes);
    }
};

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    HANDLE process = GetCurrentProcess();
    bool initialized = false;
    try {
        Image image(argv[1]);
        std::string path(argv[1]);
        size_t slash = path.find_last_of("/\\");
        std::string directory = slash == std::string::npos ? "." : path.substr(0, slash);
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
        if (!SymInitialize(process, directory.c_str(), FALSE)) throw std::runtime_error("SymInitialize failed");
        initialized = true;
        DWORD64 base = SymLoadModuleEx(process, nullptr, argv[1], "oa2dpaudio", 0, 0, nullptr, 0);
        if (!base) throw std::runtime_error("cannot load PE/PDB symbols");
        alignas(SYMBOL_INFO) unsigned char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = MAX_SYM_NAME;
        if (!SymFromName(process, "oa2dpaudio!Oa2dpPinDataRangePointersStream", symbol))
            throw std::runtime_error("stream table symbol missing; use the matching private PDB");
        ULONG64 length = symbol->Size;
        if (!length && !SymGetTypeInfo(process, base, symbol->TypeIndex, TI_GET_LENGTH, &length))
            throw std::runtime_error("stream table length unavailable");
        if (length % 8) throw std::runtime_error("invalid x64 pointer-table length");
        image.check(symbol->Address - base, static_cast<size_t>(length / 8));
        SymCleanup(process);
        return 0;
    } catch (const std::exception &error) {
        if (initialized) SymCleanup(process);
        std::fprintf(stderr, "Native metadata FAIL: %s\n", error.what());
        return 1;
    }
}
