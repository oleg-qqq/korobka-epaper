// Маленький генератор QR-кодов: байтовый режим, версии 1-10, коррекция M.
// Этого хватает для строки Wi-Fi до ~200 байт. Алгоритм по стандарту
// ISO/IEC 18004 (как в известной библиотеке qrcodegen Nayuki).
#ifndef __QR_CODE_H__
#define __QR_CODE_H__

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define QR_MAX_VERSION 10
#define QR_MAX_SIZE (QR_MAX_VERSION * 4 + 17)   // 57 модулей

class QrCode {
public:
    int size = 0;   // 0, если закодировать не получилось

    bool Get(int x, int y) const {
        return x >= 0 && y >= 0 && x < size && y < size && mod_[y][x] != 0;
    }

    // Кодирует строку. true при успехе.
    bool Encode(const char* text) {
        size = 0;
        const int len = (int)strlen(text);
        // коррекция M: число кодовых слов коррекции на блок и число блоков
        static const int8_t kEccPerBlock[QR_MAX_VERSION + 1] = { -1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 };
        static const int8_t kNumBlocks[QR_MAX_VERSION + 1]   = { -1,  1,  1,  1,  2,  2,  4,  4,  4,  5,  5 };

        int ver = 0;
        for (int v = 1; v <= QR_MAX_VERSION; v++) {
            int cap_bits = DataCodewords(v, kEccPerBlock[v], kNumBlocks[v]) * 8;
            int cc_bits = v <= 9 ? 8 : 16;
            if (4 + cc_bits + len * 8 <= cap_bits) { ver = v; break; }
        }
        if (ver == 0) return false;

        const int ecc_len = kEccPerBlock[ver];
        const int blocks = kNumBlocks[ver];
        const int data_cw = DataCodewords(ver, ecc_len, blocks);

        // 1. Данные: режим "байты", длина, сами байты, окончание и заполнение.
        uint8_t data[400];
        memset(data, 0, sizeof(data));
        int bit = 0;
        auto put = [&](uint32_t val, int n) {
            for (int i = n - 1; i >= 0; i--, bit++) {
                if ((val >> i) & 1) data[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
            }
        };
        put(0x4, 4);
        put((uint32_t)len, ver <= 9 ? 8 : 16);
        for (int i = 0; i < len; i++) put((uint8_t)text[i], 8);
        int cap = data_cw * 8;
        int term = cap - bit < 4 ? cap - bit : 4;
        put(0, term);
        put(0, (8 - bit % 8) % 8);
        for (uint8_t pad = 0xEC; bit < cap; pad ^= 0xEC ^ 0x11) put(pad, 8);

        // 2. Коррекция ошибок по блокам и перемешивание.
        uint8_t all[400];
        int all_len = AddEccAndInterleave(data, ver, ecc_len, blocks, all);

        // 3. Служебные узоры, данные, лучшая маска.
        size = ver * 4 + 17;
        memset(mod_, 0, sizeof(mod_));
        memset(func_, 0, sizeof(func_));
        DrawFunctionPatterns(ver);
        DrawCodewords(all, all_len);

        int best_mask = 0;
        long best_penalty = -1;
        for (int m = 0; m < 8; m++) {
            ApplyMask(m);
            DrawFormatBits(m);
            long p = Penalty();
            if (best_penalty < 0 || p < best_penalty) { best_penalty = p; best_mask = m; }
            ApplyMask(m);   // XOR ещё раз отменяет маску
        }
        ApplyMask(best_mask);
        DrawFormatBits(best_mask);
        return true;
    }

private:
    uint8_t mod_[QR_MAX_SIZE][QR_MAX_SIZE];
    uint8_t func_[QR_MAX_SIZE][QR_MAX_SIZE];

    static int RawDataModules(int ver) {
        int r = (16 * ver + 128) * ver + 64;
        if (ver >= 2) {
            int na = ver / 7 + 2;
            r -= (25 * na - 10) * na - 55;
            if (ver >= 7) r -= 36;
        }
        return r;
    }

    static int DataCodewords(int ver, int ecc_len, int blocks) {
        return RawDataModules(ver) / 8 - ecc_len * blocks;
    }

    // ---- поле Галуа GF(256), многочлен 0x11D ----
    static uint8_t GfMul(uint8_t x, uint8_t y) {
        int z = 0;
        for (int i = 7; i >= 0; i--) {
            z = (z << 1) ^ ((z >> 7) * 0x11D);
            z ^= ((y >> i) & 1) * x;
        }
        return (uint8_t)z;
    }

    static void RsDivisor(int degree, uint8_t* result) {
        memset(result, 0, (size_t)degree);
        result[degree - 1] = 1;
        uint8_t root = 1;
        for (int i = 0; i < degree; i++) {
            for (int j = 0; j < degree; j++) {
                result[j] = GfMul(result[j], root);
                if (j + 1 < degree) result[j] ^= result[j + 1];
            }
            root = GfMul(root, 0x02);
        }
    }

    static void RsRemainder(const uint8_t* data, int len, const uint8_t* gen, int degree, uint8_t* out) {
        memset(out, 0, (size_t)degree);
        for (int i = 0; i < len; i++) {
            uint8_t factor = data[i] ^ out[0];
            memmove(out, out + 1, (size_t)(degree - 1));
            out[degree - 1] = 0;
            for (int j = 0; j < degree; j++) out[j] ^= GfMul(gen[j], factor);
        }
    }

    static int AddEccAndInterleave(const uint8_t* data, int ver, int ecc_len, int blocks, uint8_t* out) {
        int raw_cw = RawDataModules(ver) / 8;
        int short_blocks = blocks - raw_cw % blocks;
        int short_len = raw_cw / blocks;
        uint8_t gen[32];
        RsDivisor(ecc_len, gen);

        uint8_t blk[8][160];
        int k = 0;
        for (int i = 0; i < blocks; i++) {
            int dlen = short_len - ecc_len + (i < short_blocks ? 0 : 1);
            uint8_t* b = blk[i];
            memcpy(b, data + k, (size_t)dlen);
            uint8_t ecc[32];
            RsRemainder(data + k, dlen, gen, ecc_len, ecc);
            k += dlen;
            if (i < short_blocks) {
                b[dlen] = 0;   // пустое место, чтобы все блоки были одной длины
                memcpy(b + dlen + 1, ecc, (size_t)ecc_len);
            } else {
                memcpy(b + dlen, ecc, (size_t)ecc_len);
            }
        }
        int n = 0;
        for (int i = 0; i < short_len + 1; i++) {
            for (int j = 0; j < blocks; j++) {
                if (i != short_len - ecc_len || j >= short_blocks) out[n++] = blk[j][i];
            }
        }
        return n;
    }

    // ---- служебные узоры ----
    void SetFunc(int x, int y, bool dark) {
        mod_[y][x] = dark ? 1 : 0;
        func_[y][x] = 1;
    }

    void DrawFinder(int cx, int cy) {
        for (int dy = -4; dy <= 4; dy++) {
            for (int dx = -4; dx <= 4; dx++) {
                int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= size || y >= size) continue;
                int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
                SetFunc(x, y, dist != 2 && dist != 4);
            }
        }
    }

    void DrawAlignment(int cx, int cy) {
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
                SetFunc(cx + dx, cy + dy, dist != 1);
            }
        }
    }

    void DrawFunctionPatterns(int ver) {
        for (int i = 0; i < size; i++) {
            SetFunc(6, i, i % 2 == 0);
            SetFunc(i, 6, i % 2 == 0);
        }
        DrawFinder(3, 3);
        DrawFinder(size - 4, 3);
        DrawFinder(3, size - 4);

        if (ver >= 2) {
            int na = ver / 7 + 2;
            int step = (ver * 4 + na * 2 + 1) / (na * 2 - 2) * 2;
            int pos[7];
            pos[0] = 6;
            for (int i = na - 1, p = size - 7; i >= 1; i--, p -= step) pos[i] = p;
            for (int i = 0; i < na; i++) {
                for (int j = 0; j < na; j++) {
                    if ((i == 0 && j == 0) || (i == 0 && j == na - 1) || (i == na - 1 && j == 0)) continue;
                    DrawAlignment(pos[i], pos[j]);
                }
            }
        }

        DrawFormatBits(0);   // пока просто занять место

        if (ver >= 7) {
            int rem = ver;
            for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
            long bits = (long)ver << 12 | rem;
            for (int i = 0; i < 18; i++) {
                bool b = ((bits >> i) & 1) != 0;
                int a = size - 11 + i % 3, c = i / 3;
                SetFunc(a, c, b);
                SetFunc(c, a, b);
            }
        }
    }

    void DrawFormatBits(int mask) {
        int data = 0 << 3 | mask;   // уровень M кодируется как 00
        int rem = data;
        for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        int bits = (data << 10 | rem) ^ 0x5412;
        auto b = [&](int i) { return ((bits >> i) & 1) != 0; };
        for (int i = 0; i <= 5; i++) SetFunc(8, i, b(i));
        SetFunc(8, 7, b(6));
        SetFunc(8, 8, b(7));
        SetFunc(7, 8, b(8));
        for (int i = 9; i < 15; i++) SetFunc(14 - i, 8, b(i));
        for (int i = 0; i < 8; i++) SetFunc(size - 1 - i, 8, b(i));
        for (int i = 8; i < 15; i++) SetFunc(8, size - 15 + i, b(i));
        SetFunc(8, size - 8, true);
    }

    void DrawCodewords(const uint8_t* data, int len) {
        int i = 0;
        for (int right = size - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vert = 0; vert < size; vert++) {
                for (int j = 0; j < 2; j++) {
                    int x = right - j;
                    bool upward = ((right + 1) & 2) == 0;
                    int y = upward ? size - 1 - vert : vert;
                    if (!func_[y][x] && i < len * 8) {
                        mod_[y][x] = ((data[i >> 3] >> (7 - (i & 7))) & 1) ? 1 : 0;
                        i++;
                    }
                }
            }
        }
    }

    void ApplyMask(int mask) {
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                if (func_[y][x]) continue;
                bool inv;
                switch (mask) {
                    case 0: inv = (x + y) % 2 == 0; break;
                    case 1: inv = y % 2 == 0; break;
                    case 2: inv = x % 3 == 0; break;
                    case 3: inv = (x + y) % 3 == 0; break;
                    case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                    case 5: inv = x * y % 2 + x * y % 3 == 0; break;
                    case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
                    default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (inv) mod_[y][x] ^= 1;
            }
        }
    }

    // Штраф маски: чем меньше, тем легче код читается камерой.
    long Penalty() const {
        long p = 0;
        // длинные полосы одного цвета и узоры, похожие на угловые квадраты
        for (int pass = 0; pass < 2; pass++) {
            for (int a = 0; a < size; a++) {
                int run = 0;
                int prev = -1;
                uint32_t hist = 0;   // последние 11 модулей
                for (int b = 0; b < size; b++) {
                    int v = pass == 0 ? mod_[a][b] : mod_[b][a];
                    if (v == prev) {
                        run++;
                        if (run == 5) p += 3;
                        else if (run > 5) p += 1;
                    } else {
                        run = 1;
                        prev = v;
                    }
                    hist = ((hist << 1) | (uint32_t)v) & 0x7FF;
                    if (b >= 10 && (hist == 0x5D0 || hist == 0x05D)) p += 40;
                }
            }
        }
        // квадраты 2x2 одного цвета
        for (int y = 0; y + 1 < size; y++) {
            for (int x = 0; x + 1 < size; x++) {
                int c = mod_[y][x];
                if (c == mod_[y][x + 1] && c == mod_[y + 1][x] && c == mod_[y + 1][x + 1]) p += 3;
            }
        }
        // баланс тёмного и светлого
        int dark = 0;
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) dark += mod_[y][x];
        }
        int total = size * size;
        int k = (abs(dark * 20 - total * 10) + total - 1) / total - 1;
        p += (long)k * 10;
        return p;
    }
};

// Строка для QR-кода Wi-Fi: WIFI:T:WPA;S:имя;P:пароль;;
// Спецсимволы \ ; , : " экранируются обратной чертой.
static inline void QrWifiString(char* out, size_t out_size, const char* ssid, const char* pass, bool open_net) {
    auto esc = [](char* dst, size_t cap, size_t& n, const char* s) {
        for (; *s && n + 2 < cap; s++) {
            if (*s == '\\' || *s == ';' || *s == ',' || *s == ':' || *s == '"') dst[n++] = '\\';
            dst[n++] = *s;
        }
    };
    size_t n = 0;
    auto add = [&](const char* s) { while (*s && n + 1 < out_size) out[n++] = *s++; };
    add(open_net ? "WIFI:T:nopass;S:" : "WIFI:T:WPA;S:");
    esc(out, out_size, n, ssid);
    if (!open_net) {
        add(";P:");
        esc(out, out_size, n, pass);
    }
    add(";;");
    out[n < out_size ? n : out_size - 1] = 0;
}

#endif // __QR_CODE_H__
