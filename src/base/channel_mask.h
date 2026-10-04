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

// A set of channels of any width: bit i selects channel i. A default-constructed mask is unset;
// parse() refuses a mask that selects nothing.
class ChannelMask {
public:
    // Reads decimal of any length or "0x" hex.
    static ChannelMask parse(std::string_view token) {
        token = trim_whitespace(token);

        ChannelMask mask;
        if (token.size() >= 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
            mask.parse_hex(token.substr(2), token);
        else
            mask.parse_decimal(token);

        mask.drop_empty_words();
        if (mask.empty())
            throw ConfigurationError("Trace: channel mask {} selects no channel!", std::string(token));
        return mask;
    }

    bool empty() const { return m_words.empty(); }

    size_t count() const {
        size_t n_channels = 0;
        for (uint64_t word : m_words)
            n_channels += std::popcount(word);
        return n_channels;
    }

    // The mask must not be empty.
    int highest() const { return int(64 * (m_words.size() - 1)) + 63 - std::countl_zero(m_words.back()); }

    // In ascending order.
    std::vector<int> channels() const {
        std::vector<int> selected;
        selected.reserve(count());
        for (size_t i = 0; i < m_words.size(); i++) {
            for (uint64_t bits = m_words[i]; bits != 0; bits &= bits - 1)
                selected.push_back(int(64 * i) + std::countr_zero(bits));
        }
        return selected;
    }

    std::string str() const {
        if (empty())
            return "0x0";
        std::string hex = fmt::format("0x{:x}", m_words.back());
        for (size_t i = m_words.size() - 1; i-- > 0;)
            hex += fmt::format("{:016x}", m_words[i]);
        return hex;
    }

private:
    std::vector<uint64_t> m_words; // word i holds channels 64i..64i+63; the last is nonzero

    static std::string_view trim_whitespace(std::string_view text) {
        while (!text.empty() && std::string_view("\r\n \t").find(text.back()) != std::string_view::npos)
            text.remove_suffix(1);
        return text;
    }

    // -1 for a character that is not a hex digit.
    static int hex_digit_value(char digit) {
        if (digit >= '0' && digit <= '9')
            return digit - '0';
        if (digit >= 'a' && digit <= 'f')
            return digit - 'a' + 10;
        if (digit >= 'A' && digit <= 'F')
            return digit - 'A' + 10;
        return -1;
    }

    [[noreturn]] static void throw_malformed(std::string_view token) {
        throw ConfigurationError("Trace: malformed channel mask {}!", std::string(token));
    }

    void drop_empty_words() {
        while (!m_words.empty() && m_words.back() == 0)
            m_words.pop_back();
    }

    void parse_hex(std::string_view digits, std::string_view token) {
        if (digits.empty())
            throw_malformed(token);
        for (size_t i = 0; i < digits.size(); i++) {
            const int value = hex_digit_value(digits[digits.size() - 1 - i]);
            if (value < 0)
                throw_malformed(token);
            if (i % 16 == 0)
                m_words.push_back(0);
            m_words.back() |= uint64_t(value) << (4 * (i % 16));
        }
    }

    void parse_decimal(std::string_view digits) {
        if (digits.empty())
            throw_malformed(digits);
        for (char digit : digits) {
            if (digit < '0' || digit > '9')
                throw_malformed(digits);
            // mask = mask * 10 + digit, with the carry between words held in 128 bits.
            unsigned __int128 carry = digit - '0';
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
