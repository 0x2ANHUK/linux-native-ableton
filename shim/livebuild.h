// Which Live binary this process is: the shim's address-based fixes are made
// for exact builds and choose their addresses (or stay off) by this.
#pragma once
#include <cstdint>

enum class LiveBuild { Unknown, Live12_4_7, Live11_3_43 };

// From the GNU build ID of the main program (its PT_NOTE), read once.
LiveBuild liveBuild();

// The address for the running build: a12 on Live 12.4.7, a11 on Live 11.3.43,
// 0 (feature off) otherwise or when that build has no such address.
inline uintptr_t liveAddress(uintptr_t a12, uintptr_t a11)
{
    switch (liveBuild()) {
    case LiveBuild::Live12_4_7: return a12;
    case LiveBuild::Live11_3_43: return a11;
    default: return 0;
    }
}
