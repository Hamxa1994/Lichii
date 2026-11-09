// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace Common {

/// Running totals of the frames the renderer presented: the ones the game rendered and the ones
/// frame generation synthesised between them. Each consumer (the log, the status bar) keeps its
/// own previous totals and turns the difference into rates.
class FrameGenStats {
public:
    struct Totals {
        u64 original{};
        u64 generated{};
    };

    /// One game frame was presented together with 'generated' synthesised frames before it.
    static void Record(u64 generated) noexcept {
        original_frames.fetch_add(1, std::memory_order_relaxed);
        generated_frames.fetch_add(generated, std::memory_order_relaxed);
    }

    [[nodiscard]] static Totals Get() noexcept {
        return {original_frames.load(std::memory_order_relaxed),
                generated_frames.load(std::memory_order_relaxed)};
    }

private:
    static inline std::atomic<u64> original_frames{};
    static inline std::atomic<u64> generated_frames{};
};

} // namespace Common
