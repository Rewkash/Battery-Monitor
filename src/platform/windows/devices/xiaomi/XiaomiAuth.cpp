#include "platform/windows/devices/xiaomi/XiaomiAuth.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>

namespace battery_monitor {

namespace {

constexpr int kXiaomiAuthPattern = 0x9999;

constexpr std::array<std::uint8_t, 16> kXiaomiAuthSeq = {
    0x11, 0x22, 0x33, 0x33, 0x22, 0x11, 0x11, 0x22,
    0x33, 0x33, 0x22, 0x11, 0x11, 0x22, 0x33, 0x33};

constexpr std::array<std::array<int, 16>, 16> kXiaomiAuthCoefficients = {{
    {{2, 1, 1, 1, 4, 2, 1, 1, 2, 2, 4, 2, 4, 4, 16, 8}},
    {{2, 1, 1, 1, 4, 2, 1, 1, 1, 1, 2, 1, 2, 2, 8, 4}},
    {{1, 1, 4, 2, 2, 2, 4, 2, 16, 8, 4, 4, 2, 1, 1, 1}},
    {{1, 1, 4, 2, 1, 1, 2, 1, 8, 4, 2, 2, 2, 1, 1, 1}},
    {{16, 8, 2, 2, 4, 2, 4, 4, 1, 1, 4, 2, 1, 1, 2, 1}},
    {{8, 4, 1, 1, 2, 1, 2, 2, 1, 1, 4, 2, 1, 1, 2, 1}},
    {{2, 2, 4, 2, 4, 4, 16, 8, 2, 1, 1, 1, 4, 2, 1, 1}},
    {{1, 1, 2, 1, 2, 2, 8, 4, 2, 1, 1, 1, 4, 2, 1, 1}},
    {{4, 2, 4, 4, 16, 8, 2, 2, 1, 1, 2, 1, 1, 1, 4, 2}},
    {{2, 1, 2, 2, 8, 4, 1, 1, 1, 1, 2, 1, 1, 1, 4, 2}},
    {{4, 4, 16, 8, 1, 1, 2, 1, 4, 2, 1, 1, 4, 2, 2, 2}},
    {{2, 2, 8, 4, 1, 1, 2, 1, 4, 2, 1, 1, 2, 1, 1, 1}},
    {{1, 1, 2, 1, 1, 1, 4, 2, 4, 4, 16, 8, 2, 2, 4, 2}},
    {{1, 1, 2, 1, 1, 1, 4, 2, 2, 2, 8, 4, 1, 1, 2, 1}},
    {{4, 2, 1, 1, 2, 1, 1, 1, 4, 2, 2, 2, 16, 8, 4, 4}},
    {{4, 2, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 8, 4, 2, 2}},
}};

std::uint32_t ModPow(std::uint32_t base, std::uint32_t exponent, std::uint32_t modulo) {
    std::uint64_t result = 1;
    std::uint64_t value = base % modulo;
    std::uint32_t exp = exponent;

    while (exp > 0U) {
        if ((exp & 1U) != 0U) {
            result = (result * value) % modulo;
        }
        value = (value * value) % modulo;
        exp >>= 1U;
    }

    return static_cast<std::uint32_t>(result);
}

std::array<std::array<std::uint8_t, 16>, 16> BuildXiaomiBiasMatrix() {
    std::array<std::array<std::uint8_t, 16>, 16> bias{};
    for (std::size_t i = 0; i < 16; ++i) {
        for (std::size_t j = 0; j < 16; ++j) {
            const std::uint32_t exponent = static_cast<std::uint32_t>(17U * (i + 2U) + (j + 1U));
            const std::uint32_t inner = ModPow(45U, exponent, 257U);
            const std::uint32_t outer = ModPow(45U, inner, 257U);
            bias[i][j] = static_cast<std::uint8_t>(outer == 256U ? 0U : outer);
        }
    }
    return bias;
}

std::array<std::uint8_t, 256> BuildXiaomiExpTable() {
    std::array<std::uint8_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (i == 128U) {
            table[i] = 0;
            continue;
        }
        table[i] = static_cast<std::uint8_t>(ModPow(45U, static_cast<std::uint32_t>(i), 257U));
    }
    return table;
}

std::array<std::uint8_t, 256> BuildXiaomiLogTable() {
    std::array<std::uint8_t, 256> table{};
    table[0] = 128U;
    for (std::uint32_t i = 1; i < 256U; ++i) {
        const std::uint32_t mod_exp = ModPow(45U, i, 257U);
        if (mod_exp != 256U) {
            table[mod_exp] = static_cast<std::uint8_t>(i);
        }
    }
    return table;
}

std::array<std::array<std::uint8_t, 16>, 17> BuildXiaomiKeySchedule(std::array<std::uint8_t, 16> key_init) {
    static const auto bias_matrix = BuildXiaomiBiasMatrix();

    key_init[15] ^= 0x06U;

    std::array<std::array<std::uint8_t, 16>, 17> keys{};
    keys[0] = key_init;

    std::array<std::uint8_t, 17> reg{};
    std::uint8_t xor_sum = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        reg[i] = key_init[i];
        xor_sum ^= key_init[i];
    }
    reg[16] = xor_sum;

    for (std::size_t key_idx = 1; key_idx < 17; ++key_idx) {
        for (auto& value : reg) {
            value = static_cast<std::uint8_t>(((value & 0xFFU) >> 5U) | ((value & 0xFFU) << 3U));
        }

        std::array<std::uint8_t, 16> key_i{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t reg_idx = (key_idx + i) % 17U;
            key_i[i] = static_cast<std::uint8_t>(reg[reg_idx] + bias_matrix[key_idx - 1U][i]);
        }
        keys[key_idx] = key_i;
    }

    return keys;
}

}  // namespace

std::array<std::uint8_t, 16> ComputeXiaomiChallengeResponse(const std::array<std::uint8_t, 16>& challenge) {
    static const auto exp_tab = BuildXiaomiExpTable();
    static const auto log_tab = BuildXiaomiLogTable();

    auto keys = BuildXiaomiKeySchedule(challenge);
    std::array<std::uint8_t, 16> ciphertext = kXiaomiAuthSeq;
    const std::array<std::uint8_t, 16> plaintext = kXiaomiAuthSeq;

    for (int round = 0; round < 8; ++round) {
        if (round == 2) {
            for (std::size_t i = 0; i < 16; ++i) {
                if (((1 << static_cast<int>(i)) & kXiaomiAuthPattern) != 0) {
                    ciphertext[i] ^= plaintext[i];
                } else {
                    ciphertext[i] = static_cast<std::uint8_t>(ciphertext[i] + plaintext[i]);
                }
            }
        }

        for (std::size_t i = 0; i < 16; ++i) {
            if (((1 << static_cast<int>(i)) & kXiaomiAuthPattern) != 0) {
                ciphertext[i] ^= keys[round * 2][i];
            } else {
                ciphertext[i] = static_cast<std::uint8_t>(ciphertext[i] + keys[round * 2][i]);
            }
        }

        for (std::size_t i = 0; i < 16; ++i) {
            if (((1 << static_cast<int>(i)) & kXiaomiAuthPattern) != 0) {
                ciphertext[i] = exp_tab[ciphertext[i]];
            } else {
                ciphertext[i] = log_tab[ciphertext[i]];
            }
        }

        for (std::size_t i = 0; i < 16; ++i) {
            if (((1 << static_cast<int>(i)) & kXiaomiAuthPattern) != 0) {
                ciphertext[i] = static_cast<std::uint8_t>(keys[round * 2 + 1][i] + ciphertext[i]);
            } else {
                ciphertext[i] = static_cast<std::uint8_t>(keys[round * 2 + 1][i] ^ ciphertext[i]);
            }
        }

        const auto copy = ciphertext;
        for (std::size_t i = 0; i < 16; ++i) {
            std::uint8_t sum = 0;
            for (std::size_t j = 0; j < 16; ++j) {
                const int product = kXiaomiAuthCoefficients[i][j] * static_cast<int>(copy[j]);
                sum = static_cast<std::uint8_t>(sum + product);
            }
            ciphertext[i] = sum;
        }
    }

    for (std::size_t i = 0; i < 16; ++i) {
        if (((1 << static_cast<int>(i)) & kXiaomiAuthPattern) != 0) {
            ciphertext[i] = static_cast<std::uint8_t>(keys[16][i] ^ ciphertext[i]);
        } else {
            ciphertext[i] = static_cast<std::uint8_t>(keys[16][i] + ciphertext[i]);
        }
    }

    return ciphertext;
}

std::array<std::uint8_t, 16> GenerateRandomChallenge() {
    std::array<std::uint8_t, 16> challenge{};
    std::random_device random;
    for (auto& value : challenge) {
        value = static_cast<std::uint8_t>(random() & 0xFFU);
    }
    return challenge;
}

}  // namespace battery_monitor

