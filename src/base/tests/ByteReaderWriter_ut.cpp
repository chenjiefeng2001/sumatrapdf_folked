/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/ByteReaderWriter.h"

// must be last due to assert() over-write
#include "base/UtAssert.h"

#define ABC "abc"
void ByteOrderTests() {
    {
        const u8 data[] = {1, 2, 3, 4, 5, 6, 7, 8};
        ByteReader r(data, dimofi(data));
        utassert(r.CanRead(0, 8));
        utassert(r.CanRead(8, 0));
        utassert(!r.CanRead(0, -1));
        utassert(!r.CanRead(-1, 1));
        utassert(!r.CanRead(INT_MAX, INT_MAX));
        utassert(!r.CanRead(1, 8));
        utassert(r.UInt16LE(6) == 0x0807);
        utassert(r.UInt32BE(4) == 0x05060708);
        utassert(r.UInt64LE(0) == 0x0807060504030201ULL);
        for (int off : {-1, 8, INT_MAX - 1, INT_MAX}) {
            utassert(r.UInt8(off) == 0);
            utassert(r.UInt16LE(off) == 0);
            utassert(r.UInt16BE(off) == 0);
            utassert(r.UInt32LE(off) == 0);
            utassert(r.UInt32BE(off) == 0);
            utassert(r.UInt64LE(off) == 0);
            utassert(r.UInt64BE(off) == 0);
        }
        ByteReader empty(nullptr, 0);
        utassert(!empty.CanRead(0, 1));
        utassert(empty.UInt64LE(0) == 0);
    }

    u8 d1[] = {0x00, 0x01,
               0x00,                               // to skip
               0x01, 0x00, 0xff, 0xfe, 0x00, 0x00, // to skip
               0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xfe, 0x02, 0x00, 'a', 'b', 'c'};

    {
        u16 vu16;
        u32 vu32;
        char b[3];
        ByteReader d(d1, sizeof(d1));
        utassert(0 == d.Offset());
        vu16 = d.UInt16LE();
        utassert(2 == d.Offset());
        utassert(vu16 == 0x100);
        d.Skip(1);
        utassert(3 == d.Offset());
        vu16 = d.UInt16LE();
        utassert(5 == d.Offset());
        utassert(vu16 == 0x1);
        vu16 = d.UInt16LE();
        utassert(7 == d.Offset());
        utassert(vu16 == 0xfeff);
        d.Skip(2);
        utassert(9 == d.Offset());
        d.Unskip(4);
        utassert(5 == d.Offset());
        vu32 = d.UInt32LE();
        utassert(vu32 == 0xfeff);

        vu32 = d.UInt32LE();
        utassert(13 == d.Offset());
        utassert(vu32 == 0x1000000);
        vu32 = d.UInt32LE();
        utassert(17 == d.Offset());
        utassert(vu32 == 1);
        vu32 = d.UInt32LE();
        utassert(21 == d.Offset());
        utassert(vu32 == 0xfeffffff);

        vu16 = d.UInt16LE();
        utassert(vu16 == 0x02);
        utassert(23 == d.Offset());

        d.Bytes(b, 3);
        utassert(MemEq(ABC, b, 3));
        utassert(26 == d.Offset());
    }

    {
        u16 vu16;
        u32 vu32;
        char b[3];
        ByteReader d(d1, sizeof(d1));
        vu16 = d.UInt16BE();
        utassert(vu16 == 1);
        d.Skip(1);
        vu16 = d.UInt16BE();
        utassert(vu16 == 0x100);
        vu16 = d.UInt16BE();
        utassert(vu16 == 0xfffe);
        d.Skip(2);

        vu32 = d.UInt32BE();
        utassert(vu32 == 1);
        vu32 = d.UInt32BE();
        utassert(vu32 == 0x1000000);
        vu32 = d.UInt32BE();
        utassert(vu32 == 0xfffffffe);

        vu16 = d.UInt16BE();
        utassert(vu16 == 0x200);
        d.Bytes(b, 3);
        utassert(MemEq(ABC, b, 3));
        utassert(26 == d.Offset());
    }

    {
        i16 v16;
        i32 v32;
        char b[3];
        ByteReader d(d1, sizeof(d1));
        v16 = d.Int16LE();
        utassert(v16 == 0x100);
        d.Skip(1);
        v16 = d.Int16LE();
        utassert(v16 == 0x1);
        v16 = d.Int16LE();
        utassert(v16 == -257);
        d.Skip(2);

        v32 = d.Int32LE();
        utassert(v32 == 0x1000000);
        v32 = d.Int32LE();
        utassert(v32 == 1);
        v32 = d.Int32LE();
        utassert(v32 == -16777217);

        v16 = d.Int16LE();
        utassert(v16 == 0x2);
        d.Bytes(b, 3);
        utassert(MemEq(ABC, b, 3));
        utassert(26 == d.Offset());
    }

    {
        i16 v16;
        i32 v32;
        char b[3];
        ByteReader d(d1, sizeof(d1));
        v16 = d.Int16BE();
        utassert(v16 == 0x1);
        d.Skip(1);
        v16 = d.Int16BE();
        utassert(v16 == 0x100);
        v16 = d.Int16BE();
        utassert(v16 == -2);
        d.Skip(2);

        v32 = d.Int32BE();
        utassert(v32 == 1);
        v32 = d.Int32BE();
        utassert(v32 == 0x1000000);
        v32 = d.Int32BE();
        utassert(v32 == -2);

        v16 = d.Int16BE();
        utassert(v16 == 0x200);
        d.Bytes(b, 3);
        utassert(MemEq(ABC, b, 3));
        utassert(26 == d.Offset());
    }
}

#undef ABC
