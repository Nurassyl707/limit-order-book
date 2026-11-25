#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// Open-addressing hash map OrderId -> uint32_t slot index.
// Linear probing + backward-shift deletion (no tombstones), power-of-two
// capacity, load factor <= 0.5. Key 0 is the empty marker, so OrderId 0 is
// invalid by design. Far more cache-friendly than std::unordered_map.
class IdMap {
public:
    explicit IdMap(std::size_t initial_capacity = 1024) {
        std::size_t cap = 16;
        while (cap < initial_capacity * 2) cap <<= 1;
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
    }

    std::size_t size() const noexcept { return size_; }

    bool insert(OrderId key, std::uint32_t val) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        std::size_t i = home(key);
        while (slots_[i].key != 0) {
            if (slots_[i].key == key) return false;
            i = (i + 1) & mask_;
        }
        slots_[i].key = key;
        slots_[i].val = val;
        ++size_;
        return true;
    }

    // Returns pointer to value or nullptr.
    const std::uint32_t* find(OrderId key) const noexcept {
        std::size_t i = home(key);
        while (slots_[i].key != 0) {
            if (slots_[i].key == key) return &slots_[i].val;
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    bool erase(OrderId key) noexcept {
        std::size_t i = home(key);
        while (slots_[i].key != key) {
            if (slots_[i].key == 0) return false;
            i = (i + 1) & mask_;
        }
        // Backward-shift deletion keeps probe chains intact.
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (slots_[j].key == 0) break;
            const std::size_t k = home(slots_[j].key);
            const bool in_gap = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (in_gap) continue;
            slots_[i] = slots_[j];
            i = j;
        }
        slots_[i].key = 0;
        --size_;
        return true;
    }

private:
    struct Slot {
        OrderId       key = 0;
        std::uint32_t val = 0;
    };

    std::size_t home(OrderId key) const noexcept {
        // Fibonacci hashing: good dispersion even for sequential ids.
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> 20) & mask_;
    }

    void grow() {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(old.size() * 2, Slot{});
        mask_ = slots_.size() - 1;
        size_ = 0;
        for (const Slot& s : old)
            if (s.key != 0) insert(s.key, s.val);
    }

    std::vector<Slot> slots_;
    std::size_t       mask_ = 0;
    std::size_t       size_ = 0;
};

}  // namespace lob
