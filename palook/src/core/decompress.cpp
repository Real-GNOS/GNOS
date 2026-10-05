#include "decompress.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace pal {

namespace {

inline quint16 le16(const uchar *p)
{
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

inline quint32 le32(const uchar *p)
{
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) |
           (quint32(p[3]) << 24);
}

// ---------------------------------------------------------------------------
// YJ1
// ---------------------------------------------------------------------------

// Bit reader at bit granularity across little-endian 16-bit words.
// Faithful port of sdlpal's yj1_get_bits, hardened with an end check.
struct Yj1Bits {
    const uchar *base = nullptr;
    const uchar *end = nullptr;
    unsigned bitptr = 0;
    bool fail = false;

    quint32 get(unsigned count)
    {
        const uchar *temp = base + ((bitptr >> 4) << 1);
        const unsigned bptr = bitptr & 0xf;
        if (temp + 4 > end) {
            fail = true;
            return 0;
        }
        bitptr += count;
        if (count > 16 - bptr) {
            const unsigned adj = count + bptr - 16;
            const unsigned mask = 0xffffu >> bptr;
            return (quint32(((quint32(temp[0]) | (quint32(temp[1]) << 8)) & mask) << adj)) |
                   ((quint32(temp[2]) | (quint32(temp[3]) << 8)) >> (16 - adj));
        }
        return quint32(quint16((quint32(temp[0]) | (quint32(temp[1]) << 8)) << bptr)) >>
               (16 - count);
    }
};

struct Yj1Node {
    uchar value = 0;
    bool leaf = false;
    Yj1Node *left = nullptr;
    Yj1Node *right = nullptr;
};

// The 24-byte block header, accessed by offset to avoid packing questions.
struct Yj1Block {
    const uchar *p = nullptr;
    quint16 uncompLen() const { return le16(p); }
    quint16 compLen() const { return le16(p + 2); }
    quint16 repeatTable(int i) const { return le16(p + 4 + 2 * i); }
    uchar offCodeLen(int i) const { return p[12 + i]; }
    uchar repeatCodeLen(int i) const { return p[16 + i]; }
    uchar codeCountCodeLen(int i) const { return p[19 + i]; }
    uchar codeCountTable(int i) const { return p[22 + i]; }
};

unsigned yj1_get_loop(Yj1Bits &bs, const Yj1Block &h)
{
    if (bs.get(1))
        return h.codeCountTable(0);
    unsigned temp = bs.get(2);
    if (bs.fail)
        return 0;
    if (temp)
        return bs.get(h.codeCountCodeLen(int(temp) - 1));
    return h.codeCountTable(1);
}

unsigned yj1_get_count(Yj1Bits &bs, const Yj1Block &h)
{
    unsigned temp = bs.get(2);
    if (bs.fail)
        return 0;
    if (temp != 0) {
        if (bs.get(1))
            return bs.get(h.repeatCodeLen(int(temp) - 1));
        return h.repeatTable(int(temp));
    }
    return h.repeatTable(0);
}

} // namespace

bool hasYj1Signature(const QByteArray &data)
{
    return data.size() >= 4 &&
           data[0] == 'Y' && data[1] == 'J' && data[2] == '_' && data[3] == '1';
}

int yj1Decompress(const QByteArray &srcArr, QByteArray &dst, int maxOut)
{
    // Padding lets the bit reader safely look a few bytes ahead.
    const int pad = 64;
    std::vector<uchar> srcBuf(size_t(srcArr.size()) + pad, 0);
    std::memcpy(srcBuf.data(), srcArr.constData(), size_t(srcArr.size()));
    const uchar *src = srcBuf.data();
    const uchar *srcEnd = srcBuf.data() + srcArr.size();
    const uchar *bufEnd = srcBuf.data() + srcBuf.size();

    if (srcArr.size() < 16)
        return -1;
    if (le32(src) != 0x315f4a59u) // 'YJ_1'
        return -1;
    const quint32 uncompressedLength = le32(src + 4);
    const quint16 blockCount = le16(src + 12);
    const uchar treeLength = src[15];
    if (uncompressedLength == 0 || int(uncompressedLength) > maxOut)
        return -1;

    // ---- Huffman tree ----------------------------------------------------
    const unsigned tree_len = unsigned(treeLength) * 2;
    if (16u + tree_len > size_t(srcArr.size()))
        return -1;

    std::vector<Yj1Node> root(std::max<size_t>(tree_len, 2) + 1);
    Yj1Bits treeBits{src + 16 + tree_len, bufEnd, 0, false};
    root[0].leaf = false;
    root[0].left = &root[1];
    root[0].right = &root[2];
    for (unsigned i = 1; i <= tree_len; ++i) {
        const bool leaf = !treeBits.get(1);
        const uchar value = src[15 + i];
        if (treeBits.fail)
            return -1;
        root[i].leaf = leaf;
        root[i].value = value;
        if (leaf) {
            root[i].left = root[i].right = nullptr;
        } else {
            const size_t li = (size_t(value) << 1) + 1;
            const size_t ri = li + 1;
            if (ri >= root.size())
                return -1;
            root[i].left = &root[li];
            root[i].right = &root[ri];
        }
    }

    src += 16 + tree_len +
           (((tree_len & 0xf) ? (tree_len >> 4) + 1 : (tree_len >> 4)) << 1);

    // ---- Blocks ----------------------------------------------------------
    QByteArray out(int(uncompressedLength), Qt::Uninitialized);
    uchar *dstBase = reinterpret_cast<uchar *>(out.data());
    uchar *dstPtr = dstBase;
    uchar *dstEnd = dstBase + uncompressedLength;
    bool fail = false;

    auto put = [&](uchar v) -> bool {
        if (dstPtr >= dstEnd)
            return false;
        *dstPtr++ = v;
        return true;
    };

    for (quint16 bi = 0; bi < blockCount && !fail; ++bi) {
        if (src + 24 > srcEnd) {
            fail = true;
            break;
        }
        Yj1Block blk{src};
        const uchar *blkBase = blk.p;
        src += 4;
        if (blk.compLen() == 0) {
            // Uncompressed block.
            unsigned hul = blk.uncompLen();
            if (src + hul > srcEnd) {
                fail = true;
                break;
            }
            while (hul--) {
                if (!put(*src++)) {
                    fail = true;
                    break;
                }
            }
            continue;
        }
        if (blk.compLen() < 24) {
            fail = true;
            break;
        }
        src += 20;

        Yj1Bits bs{src, bufEnd, 0, false};
        for (;;) {
            if (bs.fail) {
                fail = true;
                break;
            }
            unsigned loop = yj1_get_loop(bs, blk);
            if (bs.fail || loop == 0)
                break;
            while (loop--) {
                const Yj1Node *node = &root[0];
                unsigned steps = 0;
                while (!node->leaf) {
                    node = bs.get(1) ? node->right : node->left;
                    if (bs.fail || !node || ++steps > root.size()) {
                        fail = true;
                        break;
                    }
                }
                if (fail || !put(node->value)) {
                    fail = true;
                    break;
                }
            }
            if (fail)
                break;

            loop = yj1_get_loop(bs, blk);
            if (bs.fail || loop == 0)
                break;
            while (loop--) {
                unsigned count = yj1_get_count(bs, blk);
                unsigned pos = bs.get(2);
                if (bs.fail)
                    break;
                pos = bs.get(blk.offCodeLen(int(pos)));
                if (bs.fail) {
                    fail = true;
                    break;
                }
                if (pos > unsigned(dstPtr - dstBase)) {
                    fail = true; // reference before start of output
                    break;
                }
                while (count--) {
                    if (!put(*(dstPtr - pos))) {
                        fail = true;
                        break;
                    }
                }
                if (fail)
                    break;
            }
            if (fail)
                break;
        }
        if (fail)
            break;
        src = blkBase + blk.compLen();
    }

    if (fail)
        return -1;
    const qint64 written = dstPtr - dstBase;
    if (written != qint64(uncompressedLength))
        return -1;
    dst = out;
    return int(written);
}

// ---------------------------------------------------------------------------
// YJ2
// ---------------------------------------------------------------------------

namespace {

const uchar yj2_data1[0x100] = {
    0x3f, 0x0b, 0x17, 0x03, 0x2f, 0x0a, 0x16, 0x00, 0x2e, 0x09, 0x15, 0x02, 0x2d, 0x01, 0x08, 0x00,
    0x3e, 0x07, 0x14, 0x03, 0x2c, 0x06, 0x13, 0x00, 0x2b, 0x05, 0x12, 0x02, 0x2a, 0x01, 0x04, 0x00,
    0x3d, 0x0b, 0x11, 0x03, 0x29, 0x0a, 0x10, 0x00, 0x28, 0x09, 0x0f, 0x02, 0x27, 0x01, 0x08, 0x00,
    0x3c, 0x07, 0x0e, 0x03, 0x26, 0x06, 0x0d, 0x00, 0x25, 0x05, 0x0c, 0x02, 0x24, 0x01, 0x04, 0x00,
    0x3b, 0x0b, 0x17, 0x03, 0x23, 0x0a, 0x16, 0x00, 0x22, 0x09, 0x15, 0x02, 0x21, 0x01, 0x08, 0x00,
    0x3a, 0x07, 0x14, 0x03, 0x20, 0x06, 0x13, 0x00, 0x1f, 0x05, 0x12, 0x02, 0x1e, 0x01, 0x04, 0x00,
    0x39, 0x0b, 0x11, 0x03, 0x1d, 0x0a, 0x10, 0x00, 0x1c, 0x09, 0x0f, 0x02, 0x1b, 0x01, 0x08, 0x00,
    0x38, 0x07, 0x0e, 0x03, 0x1a, 0x06, 0x0d, 0x00, 0x19, 0x05, 0x0c, 0x02, 0x18, 0x01, 0x04, 0x00,
    0x37, 0x0b, 0x17, 0x03, 0x2f, 0x0a, 0x16, 0x00, 0x2e, 0x09, 0x15, 0x02, 0x2d, 0x01, 0x08, 0x00,
    0x36, 0x07, 0x14, 0x03, 0x2c, 0x06, 0x13, 0x00, 0x2b, 0x05, 0x12, 0x02, 0x2a, 0x01, 0x04, 0x00,
    0x35, 0x0b, 0x11, 0x03, 0x29, 0x0a, 0x10, 0x00, 0x28, 0x09, 0x0f, 0x02, 0x27, 0x01, 0x08, 0x00,
    0x34, 0x07, 0x0e, 0x03, 0x26, 0x06, 0x0d, 0x00, 0x25, 0x05, 0x0c, 0x02, 0x24, 0x01, 0x04, 0x00,
    0x33, 0x0b, 0x17, 0x03, 0x23, 0x0a, 0x16, 0x00, 0x22, 0x09, 0x15, 0x02, 0x21, 0x01, 0x08, 0x00,
    0x32, 0x07, 0x14, 0x03, 0x20, 0x06, 0x13, 0x00, 0x1f, 0x05, 0x12, 0x02, 0x1e, 0x01, 0x04, 0x00,
    0x31, 0x0b, 0x11, 0x03, 0x1d, 0x0a, 0x10, 0x00, 0x1c, 0x09, 0x0f, 0x02, 0x1b, 0x01, 0x08, 0x00,
    0x30, 0x07, 0x0e, 0x03, 0x1a, 0x06, 0x0d, 0x00, 0x19, 0x05, 0x0c, 0x02, 0x18, 0x01, 0x04, 0x00
};

const uchar yj2_data2[0x10] = {
    0x08, 0x05, 0x06, 0x04, 0x07, 0x05, 0x06, 0x03, 0x07, 0x05, 0x06, 0x04, 0x07, 0x04, 0x05, 0x03
};

struct Yj2Node {
    quint16 value = 0;
    quint16 weight = 0;
    Yj2Node *parent = nullptr;
    Yj2Node *left = nullptr;
    Yj2Node *right = nullptr;
};

struct Yj2Tree {
    std::vector<Yj2Node *> list; // 321 entries
    std::vector<Yj2Node> node;   // 641 entries
    Yj2Node *end() { return node.data() + node.size(); }
};

void yj2_build_tree(Yj2Tree &t)
{
    t.list.assign(321, nullptr);
    t.node.assign(641, Yj2Node{});
    Yj2Node *node = t.node.data();
    for (int i = 0; i <= 0x140; i++)
        t.list[i] = node + i;
    for (int i = 0; i <= 0x280; i++) {
        node[i].value = quint16(i);
        node[i].weight = 1;
    }
    node[0x280].parent = node + 0x280;
    int i = 0;
    for (int ptr = 0x141; ptr <= 0x280; i += 2, ptr++) {
        node[ptr].left = node + i;
        node[ptr].right = node + i + 1;
        node[i].parent = node[i + 1].parent = node + ptr;
        node[ptr].weight = node[i].weight + node[i + 1].weight;
    }
}

void yj2_adjust_tree(Yj2Tree &t, unsigned short value)
{
    Yj2Node *node = t.list[value];
    if (!node)
        return;
    Yj2Node *nodeBegin = t.node.data();
    Yj2Node *nodeEnd = t.end();
    while (node->value != 0x280) {
        Yj2Node *temp = node + 1;
        while (temp < nodeEnd && node->weight == temp->weight)
            temp++;
        temp--;
        if (temp != node && temp >= nodeBegin) {
            Yj2Node *tmp1 = node->parent;
            node->parent = temp->parent;
            temp->parent = tmp1;
            if (node->value > 0x140) {
                if (node->left) node->left->parent = temp;
                if (node->right) node->right->parent = temp;
            } else {
                t.list[node->value] = temp;
            }
            if (temp->value > 0x140) {
                if (temp->left) temp->left->parent = node;
                if (temp->right) temp->right->parent = node;
            } else {
                t.list[temp->value] = node;
            }
            Yj2Node tmp = *node;
            *node = *temp;
            *temp = tmp;
            node = temp;
        }
        node->weight++;
        if (!node->parent)
            break;
        node = node->parent;
    }
    node->weight++;
}

} // namespace

int yj2Decompress(const QByteArray &srcArr, QByteArray &dst, int maxOut)
{
    if (srcArr.size() < 5)
        return -1;
    const quint32 length = le32(reinterpret_cast<const uchar *>(srcArr.constData()));
    if (length == 0 || int(length) > maxOut)
        return -1;

    const int pad = 64;
    std::vector<uchar> srcBuf(size_t(srcArr.size()) + pad, 0);
    std::memcpy(srcBuf.data(), srcArr.constData() + 4, size_t(srcArr.size()) - 4);
    const uchar *src = srcBuf.data();
    const size_t srcLen = size_t(srcArr.size()) - 4 + pad;

    Yj2Tree tree;
    yj2_build_tree(tree);

    QByteArray out(int(length), Qt::Uninitialized);
    uchar *dstBase = reinterpret_cast<uchar *>(out.data());
    uchar *dstPtr = dstBase;
    uchar *dstEnd = dstBase + length;

    unsigned ptr = 0; // bit position
    bool fail = false;

    auto bitAt = [&](unsigned pos) -> unsigned {
        if ((pos >> 3) >= srcLen) {
            fail = true;
            return 0;
        }
        return (src[pos >> 3] >> (pos & 7)) & 1u;
    };

    for (;;) {
        if (fail)
            break;
        Yj2Node *node = &tree.node[0x280];
        while (node->value > 0x140) {
            node = bitAt(ptr) ? node->right : node->left;
            ptr++;
            if (fail || !node) {
                fail = true;
                break;
            }
        }
        if (fail)
            break;
        const unsigned short val = node->value;

        if (tree.node[0x280].weight == 0x8000) {
            for (int i = 0; i < 0x141; i++)
                if (tree.list[i]->weight & 1)
                    yj2_adjust_tree(tree, static_cast<unsigned short>(i));
            for (int i = 0; i <= 0x280; i++)
                tree.node[i].weight >>= 1;
        }
        yj2_adjust_tree(tree, val);

        if (val > 0xff) {
            unsigned i;
            unsigned temp = 0;
            for (i = 0; i < 8; i++, ptr++)
                temp |= bitAt(ptr) << i;
            if (fail)
                break;
            const unsigned tmp = temp & 0xff;
            for (; i < yj2_data2[tmp & 0xf] + 6; i++, ptr++)
                temp |= bitAt(ptr) << i;
            if (fail)
                break;
            temp >>= yj2_data2[tmp & 0xf];
            const unsigned pos = (temp & 0x3f) | (unsigned(yj2_data1[tmp]) << 6);
            if (pos == 0xfff)
                break;
            if (pos + 1 > unsigned(dstPtr - dstBase)) {
                fail = true;
                break;
            }
            const uchar *pre = dstPtr - pos - 1;
            const unsigned n = val - 0xfd;
            for (i = 0; i < n; i++) {
                if (dstPtr >= dstEnd) {
                    fail = true;
                    break;
                }
                *dstPtr++ = *pre++;
            }
            if (fail)
                break;
        } else {
            if (dstPtr >= dstEnd) {
                fail = true;
                break;
            }
            *dstPtr++ = uchar(val);
        }
    }

    if (fail)
        return -1;
    if (qint64(dstPtr - dstBase) != qint64(length))
        return -1;
    dst = out;
    return int(length);
}

// ---------------------------------------------------------------------------

QByteArray autoDecompress(const QByteArray &src, int maxOut, QString *method)
{
    if (hasYj1Signature(src)) {
        QByteArray out;
        if (yj1Decompress(src, out, maxOut) >= 0) {
            if (method) *method = QStringLiteral("YJ1");
            return out;
        }
        if (method) *method = QStringLiteral("YJ1 (failed)");
        return {};
    }

    // YJ2 has no signature: its first DWORD is the output length.  Try it
    // only when the declared length looks plausible, then let the caller
    // sanity-check the result against the expected layout.
    if (src.size() >= 8) {
        const quint32 len = le32(reinterpret_cast<const uchar *>(src.constData()));
        if (len > 0 && int(len) <= maxOut &&
            len < quint32(src.size()) * 64u + 4096u) {
            QByteArray out;
            if (yj2Decompress(src, out, maxOut) >= 0) {
                if (method) *method = QStringLiteral("YJ2");
                return out;
            }
        }
    }
    return {};
}

} // namespace pal
