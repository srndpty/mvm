#pragma once

// 診断用: MLT の module (affine・qtcrop) が libmlt から呼ぶ property 取得関数を、process
// 内で計時する。
//
// MLT 7.36.1 の affine は毎 frame mlt_properties_get(properties, "rect") を呼び、animation が付いた
// property ではこれが animation 全体を文字列へ直す (mlt_animation_serialize_cut_tf)。この関数は
// libmlt の中で呼ばれるので直接は計れない。代わりに、module 側の import 表 (IAT) にある
// mlt_properties_get などの番地を計時用の関数へ差し替え、呼び出しの回数と時間を数える。
// MLT を作り直さず、DLL の file も変えない。remove() で元の番地へ戻す。
//
// 時間は QueryPerformanceCounter の差の合計 (呼び出した thread ごとの経過時間の和)。mutex
// の待ちを含む。 MLT のヘッダは使わない (型は ABI が同じ自前の定義で受ける)。

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mvm::test {

struct MltRectAbi {
    double x, y, w, h, o;
};

struct MltPropertyProbeCell {
    std::atomic<long long> calls{0};
    std::atomic<long long> ticks{0};
};

struct MltPropertyProbe {
    enum Function { Get = 0, AnimGetRect, AnimGetDouble, FunctionCount };

    enum Name { Rect = 0, Rotate, Shear, Other, NameCount };

    using Cell = MltPropertyProbeCell;

    struct Snapshot {
        long long calls[FunctionCount][NameCount]{};
        long long ticks[FunctionCount][NameCount]{};
    };

    static constexpr const char* kFunctionNames[FunctionCount] = {
        "mlt_properties_get", "mlt_properties_anim_get_rect", "mlt_properties_anim_get_double"};
    static constexpr const char* kNameLabels[NameCount] = {"rect", "fix_rotate_x", "fix_shear_x",
                                                           "other"};
    static constexpr int kThreadCapacity = 64;

    inline static Cell cells[FunctionCount][NameCount];
    inline static std::atomic<long> threads[kThreadCapacity];
    inline static void* originals[FunctionCount] = {};

    struct Patch {
        void** slot;
        void* original;
    };

    inline static std::vector<Patch> patches;

    static Name classify(const char* name) {
        if (!name)
            return Other;
        // "rect" と "transition.rect" (filter として使う affine) をまとめる。
        const std::size_t length = std::strlen(name);
        if (length >= 4 && std::strcmp(name + length - 4, "rect") == 0)
            return Rect;
        if (length >= 12 && std::strcmp(name + length - 12, "fix_rotate_x") == 0)
            return Rotate;
        if (length >= 11 && std::strcmp(name + length - 11, "fix_shear_x") == 0)
            return Shear;
        return Other;
    }

    static long long now() {
        LARGE_INTEGER value;
        QueryPerformanceCounter(&value);
        return value.QuadPart;
    }

    static void record(Function function, const char* name, long long began) {
        auto& cell = cells[function][classify(name)];
        cell.calls.fetch_add(1, std::memory_order_relaxed);
        cell.ticks.fetch_add(now() - began, std::memory_order_relaxed);
        const long thread = static_cast<long>(GetCurrentThreadId());
        for (auto& slot : threads) {
            long seen = slot.load(std::memory_order_relaxed);
            if (seen == thread)
                break;
            if (seen == 0 && slot.compare_exchange_strong(seen, thread))
                break;
            if (seen == thread)
                break;
        }
    }

    static char* hookGet(void* properties, const char* name) {
        const long long began = now();
        char* value =
            reinterpret_cast<char* (*)(void*, const char*)>(originals[Get])(properties, name);
        record(Get, name, began);
        return value;
    }

    static MltRectAbi hookAnimGetRect(void* properties, const char* name, int position,
                                      int length) {
        const long long began = now();
        const MltRectAbi value = reinterpret_cast<MltRectAbi (*)(void*, const char*, int, int)>(
            originals[AnimGetRect])(properties, name, position, length);
        record(AnimGetRect, name, began);
        return value;
    }

    static double hookAnimGetDouble(void* properties, const char* name, int position, int length) {
        const long long began = now();
        const double value = reinterpret_cast<double (*)(void*, const char*, int, int)>(
            originals[AnimGetDouble])(properties, name, position, length);
        record(AnimGetDouble, name, began);
        return value;
    }

    // module (例: "libmltplus.dll") の import 表のうち、libmlt-7.dll から取っている上の 3 関数を
    // 差し替える。差し替えた数を返す。module が読み込まれていなければ 0。
    static int install(const wchar_t* module) {
        const HMODULE core = GetModuleHandleW(L"libmlt-7.dll");
        const HMODULE target = GetModuleHandleW(module);
        if (!core || !target)
            return 0;
        void* hooks[FunctionCount] = {reinterpret_cast<void*>(&hookGet),
                                      reinterpret_cast<void*>(&hookAnimGetRect),
                                      reinterpret_cast<void*>(&hookAnimGetDouble)};
        for (int function = 0; function < FunctionCount; ++function) {
            void* address = reinterpret_cast<void*>(GetProcAddress(core, kFunctionNames[function]));
            if (!address)
                return 0;
            originals[function] = address;
        }
        const auto* base = reinterpret_cast<const std::uint8_t*>(target);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (directory.VirtualAddress == 0)
            return 0;
        int installed = 0;
        for (auto* descriptor =
                 reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
             descriptor->Name != 0; ++descriptor) {
            if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), "libmlt-7.dll") !=
                0)
                continue;
            for (auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(const_cast<std::uint8_t*>(base) +
                                                                   descriptor->FirstThunk);
                 thunk->u1.Function != 0; ++thunk) {
                void** slot = reinterpret_cast<void**>(&thunk->u1.Function);
                for (int function = 0; function < FunctionCount; ++function) {
                    if (*slot != originals[function])
                        continue;
                    DWORD protection = 0;
                    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection))
                        continue;
                    patches.push_back({slot, *slot});
                    *slot = hooks[function];
                    VirtualProtect(slot, sizeof(void*), protection, &protection);
                    ++installed;
                }
            }
        }
        return installed;
    }

    // 差し替えた番地を元へ戻す。戻した後の値が元の番地と一致することを確かめる。
    static bool remove() {
        bool restored = true;
        for (const auto& patch : patches) {
            DWORD protection = 0;
            if (!VirtualProtect(patch.slot, sizeof(void*), PAGE_READWRITE, &protection)) {
                restored = false;
                continue;
            }
            *patch.slot = patch.original;
            VirtualProtect(patch.slot, sizeof(void*), protection, &protection);
            restored = restored && *patch.slot == patch.original;
        }
        patches.clear();
        return restored;
    }

    static Snapshot snapshot() {
        Snapshot value;
        for (int function = 0; function < FunctionCount; ++function)
            for (int name = 0; name < NameCount; ++name) {
                value.calls[function][name] = cells[function][name].calls.load();
                value.ticks[function][name] = cells[function][name].ticks.load();
            }
        return value;
    }

    static int threadCount() {
        int count = 0;
        for (const auto& slot : threads)
            count += slot.load() != 0;
        return count;
    }

    static double seconds(long long ticks) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return static_cast<double>(ticks) / static_cast<double>(frequency.QuadPart);
    }
};

} // namespace mvm::test
