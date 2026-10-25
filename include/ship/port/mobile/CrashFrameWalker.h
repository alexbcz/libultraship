#pragma once

#include <cstddef>
#include <cstdint>

namespace Ship::CrashDiagnostics {

// AArch64 frame records contain the caller frame pointer and signed return PC.
// The reader must fail safely for inaccessible memory; never dereference an
// untrusted frame pointer directly in a signal handler.
template <typename Read, typename Emit>
size_t WalkFrames(uintptr_t frame, uintptr_t stackLow, uintptr_t stackHigh, Read read, Emit emit) {
    size_t count = 0;
    while (count < 64 && stackHigh >= stackLow && stackHigh - stackLow >= 16 &&
           frame >= stackLow && frame <= stackHigh - 16 && (frame & 15) == 0) {
        uintptr_t record[2];
        if (!read(frame, record, sizeof(record))) {
            break;
        }
        emit(record[1]);
        ++count;
        if (record[0] <= frame) {
            break;
        }
        frame = record[0];
    }
    return count;
}

} // namespace Ship::CrashDiagnostics
