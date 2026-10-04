#ifndef RAMULATOR_BASE_CHANNEL_MASK_H
#define RAMULATOR_BASE_CHANNEL_MASK_H

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/exception.h"

namespace Ramulator {

// A set of channel indices: bit i selects channel i. There is no limit on the width; the words grow
// as the highest selected channel needs. A default-constructed mask is "unset" (the field is absent),
// which is not the same as a mask that selects nothing: parse() refuses those.
class ChannelMask {
public:
    // Reads a decimal number of any length, or a "0x" hex one. Throws ConfigurationError on a
    // malformed token and on zero.
    static ChannelMask parse(std::string_view token) {
        while (!token.empty() && (token.back() == '\r' || token.back() == '\n' || token.back() == ' ' || token.back() == '\t'))
            token.remove_suffix(1);

        ChannelMask mask;
        if (token.size() >= 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
            mask.parse_hex(token.substr(2), token);
        else
            mask.parse_decimal(token);

        mask.trim();
        if (mask.empty())
            throw ConfigurationError("Trace: channel mask {} selects no channel!", std::string(token));
        return mask;
    }

    bool empty() const { return m_words.empty(); }

    size_t count() const {
        size_t n = 0;
        for (uint64_t word : m_words)
            n += std::popcount(word);
        return n;
    }

    // The highest selected channel. The mask must not be empty.
    int highest() const { return int(64 * (m_words.size() - 1)) + 63 - std::countl_zero(m_words.back()); }

    // The selected channels, in ascending order.
    std::vector<int> channels() const {
        std::vector<int> out;
        out.reserve(count());
        for (size_t w = 0; w < m_words.size(); w++) {
            for (uint64_t bits = m_words[w]; bits != 0; bits &= bits - 1)
                out.push_back(int(64 * w) + std::countr_zero(bits));
        }
        return out;
    }

    std::string str() const {
        if (empty())
            return "0x0";
        static const char digits[] = "0123456789abcdef";
        std::string out = "0x";
        bool leading = true;
        for (size_t w = m_words.size(); w-- > 0;) {
            for (int shift = 60; shift >= 0; shift -= 4) {
                const int nibble = int((m_words[w] >> shift) & 0xf);
                if (leading && nibble == 0)
                    continue;
                leading = false;
                out.push_back(digits[nibble]);
            }
        }
        return out;
    }

private:
    std::vector<uint64_t> m_words; // word w holds channels 64w..64w+63; no trailing zero words

    void trim() {
        while (!m_words.empty() && m_words.back() == 0)
            m_words.pop_back();
    }

    void parse_hex(std::string_view digits, std::string_view token) {
        if (digits.empty())
            throw ConfigurationError("Trace: malformed channel mask {}!", std::string(token));
        for (size_t i = 0; i < digits.size(); i++) {
            const char c = digits[digits.size() - 1 - i];
            uint64_t nibble;
            if (c >= '0' && c <= '9')
                nibble = c - '0';
            else if (c >= 'a' && c <= 'f')
                nibble = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                nibble = c - 'A' + 10;
            else
                throw ConfigurationError("Trace: malformed channel mask {}!", std::string(token));
            if (i % 16 == 0)
                m_words.push_back(0);
            m_words.back() |= nibble << (4 * (i % 16));
        }
    }

    void parse_decimal(std::string_view digits) {
        if (digits.empty())
            throw ConfigurationError("Trace: malformed channel mask {}!", std::string(digits));
        for (char c : digits) {
            if (c < '0' || c > '9')
                throw ConfigurationError("Trace: malformed channel mask {}!", std::string(digits));
            unsigned __int128 carry = c - '0';
            for (uint64_t &word : m_words) {
                const unsigned __int128 product = (unsigned __int128)word * 10 + carry;
                word = uint64_t(product);
                carry = product >> 64;
            }
            if (carry != 0)
                m_words.push_back(uint64_t(carry));
        }
    }
};

} // namespace Ramulator

#endif // RAMULATOR_BASE_CHANNEL_MASK_H
