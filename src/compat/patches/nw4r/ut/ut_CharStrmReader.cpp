// =============================================================================
// PC_PORT PATCH of the vendored nw4r/ut/ut_CharStrmReader.cpp (M9.5.4 v7).
//
// Change: ReadNextCharUTF16 reads one host `wchar_t` per character instead of
// one `u16`.
//
// The console's wchar_t is 2 bytes, so "UTF-16 stream" and "wchar_t string"
// were the same thing. The game (TextBox::SetString, the message system, the
// tag helpers) always hands wchar_t strings to TextWriterBase<wchar_t>; on
// Linux wchar_t is 4 bytes, so stepping the stream by u16 read every
// character followed by a bogus 0 code (harmless for plain text, but it put
// the tag processor's `str` pointer in the middle of a wchar_t whenever a
// 0x1A message tag appeared). On Windows/MSVC wchar_t is 2 bytes and this
// patch is a no-op. The byte-oriented readers (UTF-8, CP1252, SJIS) are
// unchanged: they read `char` streams.
// =============================================================================
#include "nw4r/ut/CharStrmReader.h"

namespace nw4r {
    namespace ut {

        namespace {
            inline bool IsSJISLeadByte(u8 c) {
                return ((0x81 <= c) && (c < 0xA0)) || (0xE0 <= c);
            }
        };  // namespace

        u16 CharStrmReader::ReadNextCharUTF8() {
            u16 code;
            if ((GetChar< u8 >() & 0x80) == 0) {
                code = GetChar< u8 >();
                StepStrm< u8 >();
            } else if ((GetChar< u8 >() & 0xE0) == 0xC0) {
                code = static_cast< u16 >(((GetChar< u8 >(0) & 0x1F) << 6) | ((GetChar< u8 >(1) & 0x3F)));
                StepStrm< u8 >(2);
            } else {
                code = static_cast< u16 >(((GetChar< u8 >(0) & 0x1F) << 12) | ((GetChar< u8 >(1) & 0x3F) << 6) | ((GetChar< u8 >(2) & 0x3F)));
                StepStrm< u8 >(3);
            }

            return code;
        }

        u16 CharStrmReader::ReadNextCharUTF16() {
            // PC_PORT: one host wchar_t per character (see the file banner).
            u16 code = static_cast< u16 >(GetChar< wchar_t >());
            StepStrm< wchar_t >();
            return code;
        }

        u16 CharStrmReader::ReadNextCharCP1252() {
            u16 code = GetChar< u8 >();
            StepStrm< u8 >();
            return code;
        }

        u16 CharStrmReader::ReadNextCharSJIS() {
            u16 code;

            if (IsSJISLeadByte(GetChar< u8 >())) {
                code = static_cast< u16 >((GetChar< u8 >(0) << 8) | GetChar< u8 >(1));
                StepStrm< u8 >(2);
            } else {
                code = GetChar< u8 >();
                StepStrm< u8 >();
            }

            return code;
        }
    };  // namespace ut
};  // namespace nw4r
