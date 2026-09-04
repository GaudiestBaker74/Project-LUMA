// =============================================================================
// PC_PORT PATCH of the vendored nw4r/lyt/lyt_textBox.cpp (see
// src/compat/patches/README.md).
//
// Change vs. upstream: the TextBox resource constructor read the embedded
// text as `wchar_t` straight out of the brlyt image. On the console wchar_t
// is 2 bytes and the text is UTF-16 big-endian; on the host wchar_t is
// 4 bytes (Linux) / 2 bytes (Windows) and little-endian, so the direct read
// yields garbage and the buffer length math (textBufBytes / sizeof(wchar_t))
// is wrong. The patched ctor interprets the resource fields as BE u16 and
// widens the characters into a host wchar_t staging buffer before handing
// them to the (unchanged, host-native) SetString().
//
// Change vs. upstream (M9.5.3c): DrawSelf was decompiled only up to the GX
// setup; the patched version appends the actual text print (cursor to the
// aligned rect origin + TextWriterBase::Print), following the nw4r reference
// in homebuttonLib's lyt_textBox.cpp. See the comment in DrawSelf.
//
// Everything else is identical to upstream.
// =============================================================================
#include "nw4r/lyt/layout.h"
#include "platform/Log/Log.h"
#include "nw4r/ut/RuntimeTypeInfo.h"
#include "nw4r/lyt/resourceAccessor.h"
#include "nw4r/lyt/textBox.h"
#include "nw4r/ut/Font.h"
#include "nw4r/ut/ResFont.h"
#include "nw4r/ut/WideTextWriter.h"
#include <cstdio>
#include <vector>
#include <cwchar>

namespace nw4r {
    namespace lyt {

        // PC_PORT: petari omitted the TextBox RTTI definition (the other pane
        // classes have theirs: lyt_bounding/lyt_picture/lyt_window). TextBox
        // declares it with the older NW4R_UT_RUNTIME_TYPEINFO macro, so the
        // matching *_DEFINITION macro (const definition) is the right one.
        NW4R_UT_RUNTIME_TYPEINFO_DEFINITION(TextBox, Pane);

        namespace {
            inline u8 ClampColor(s16 colVal) {
                return u8(colVal < 0 ? 0 : (colVal > 255 ? 255 : colVal));
            }

            ut::Color GetColor(const GXColorS10& src) {
                GXColor dst;
                dst.r = ClampColor(src.r);
                dst.g = ClampColor(src.g);
                dst.b = ClampColor(src.b);
                dst.a = ClampColor(src.a);
                return ut::Color(dst);
            }
        };  // namespace

        TextBox::TextBox(const res::TextBox* pBlock, const ResBlockSet& resBlockSet) : Pane(pBlock) {
            // PC_PORT (M9.5.2): on-disk text metrics are in console wchar_t
            // units (2 bytes), not host wchar_t units.
            u16 allocStrBufLen = static_cast< u16 >(pBlock->textBufBytes / 2);
            if (allocStrBufLen > 0) {
                allocStrBufLen -= 1;
            }

            Init(allocStrBufLen);

            if (pBlock->textStrBytes >= 2 && mTextBuf) {
                const u16* const pBlockText = detail::ConvertOffsToPtr< u16 >(pBlock, pBlock->textStrOffset);
                const u16 resStrLen = static_cast< u16 >(pBlock->textStrBytes / 2 - 1);

                // UTF-16BE -> host wchar_t staging buffer.
                std::vector< wchar_t > hostText(resStrLen);
                for (u16 i = 0; i < resStrLen; ++i) {
                    const u16 unit = pBlockText[i];
                    hostText[i] = static_cast< wchar_t >(static_cast< u16 >((unit >> 8) | (unit << 8)));
                }
                SetString(hostText.data(), 0, resStrLen);
            }

            for (int i = 0; i < 2; ++i) {
                mTextColors[i] = pBlock->textCols[i];
            }

            mFontSize = pBlock->fontSize;
            mTextPosition = pBlock->textPosition;
            mBits.textAlignment = pBlock->textAlignment;
            mCharSpace = pBlock->charSpace;
            mLineSpace = pBlock->lineSpace;

            const res::Font* const fonts = detail::ConvertOffsToPtr< res::Font >(resBlockSet.pFontList, sizeof(*resBlockSet.pFontList));
            // PC_PORT (M9.5.3c hardening): same validation as lyt_material.cpp —
            // a corrupt fnl1 (bad fontIdx or nameStrOffset) produced a wild
            // pointer here (SIGSEGV in GetFont/GetResource). Degrade to an empty
            // name: the lookups return null and the textbox stays without a
            // font (it draws nothing) instead of crashing the build.
            const char* fontName = "";
            if (resBlockSet.pFontList != nullptr) {
                const u32 fontListNum = resBlockSet.pFontList->fontNum;
                const u32 fontListSize = resBlockSet.pFontList->blockHeader.size;
                if (fontListSize >= sizeof(*resBlockSet.pFontList) &&
                    pBlock->fontIdx < fontListNum &&
                    fonts[pBlock->fontIdx].nameStrOffset <
                        fontListSize - sizeof(*resBlockSet.pFontList)) {
                    fontName = detail::ConvertOffsToPtr< char >(fonts, fonts[pBlock->fontIdx].nameStrOffset);
                } else {
                    PL_LOG_WARN("compat.lyt",
                                "TextBox '%.16s': font %u out of range (fontNum %u, block size %u) — no font",
                                pBlock->name, static_cast< unsigned >(pBlock->fontIdx),
                                static_cast< unsigned >(fontListNum),
                                static_cast< unsigned >(fontListSize));
                }
            }

            if (ut::Font* pFont = resBlockSet.pResAccessor->GetFont(fontName)) {
                mpFont = pFont;
            } else if (void* fontRes = resBlockSet.pResAccessor->GetResource('font', fontName, 0)) {
                ut::ResFont* pResFont = Layout::NewObj< ut::ResFont >();
                pResFont->SetResource(fontRes);

                mpFont = pResFont;
                mBits.bAllocFont = true;
            }

            const u32* const matOffsTbl = detail::ConvertOffsToPtr< u32 >(resBlockSet.pMaterialList, sizeof(*resBlockSet.pMaterialList));
            const res::Material* const pResMaterial =
                detail::ConvertOffsToPtr< res::Material >(resBlockSet.pMaterialList, matOffsTbl[pBlock->materialIdx]);
            mpMaterial = Layout::NewObj< Material >(pResMaterial, resBlockSet);
        }

        void TextBox::Init(u16 allocStrLen) {
            mTextBuf = 0;
            mTextBufBytes = 0;
            mTextLen = 0;
            mpFont = 0;
            mFontSize = Size(0, 0);
            SetTextPositionH(1);
            SetTextPositionV(1);
            mLineSpace = 0;
            mCharSpace = 0;
            mpTagProcessor = 0;
            memset(&mBits, 0, sizeof(mBits));

            if (allocStrLen > 0) {
                AllocStringBuffer(allocStrLen);
            }
        }

        TextBox::~TextBox() {
            SetFont(nullptr);

            if (mpMaterial != NULL && !mpMaterial->IsUserAllocated()) {
                mpMaterial->~Material();
                Layout::FreeMemory(mpMaterial);
                mpMaterial = NULL;
            }

            FreeStringBuffer();
        }

        const ut::Color TextBox::GetVtxColor(u32 idx) const {
            return GetTextColor(idx / 2);
        }

        void TextBox::SetVtxColor(u32 idx, ut::Color color) {
            SetTextColor(idx / 2, color);
        }

        void TextBox::SetTextColor(u32 type, ut::Color value) {
            mTextColors[type] = value;
        }

        u8 TextBox::GetVtxColorElement(u32 idx) const {
            return reinterpret_cast< const u8* >(&mTextColors[idx / (4 * 2)])[idx % 4];
        }

        void TextBox::SetVtxColorElement(u32 idx, u8 value) {
            reinterpret_cast< u8* >(&mTextColors[idx / (4 * 2)])[idx % 4] = value;
        }

        const ut::Rect TextBox::GetTextDrawRect() const {
            ut::WideTextWriter writer;

            return GetTextDrawRect(&writer);
        }

        const ut::Rect TextBox::GetTextDrawRect(const DrawInfo&) const {
            return GetTextDrawRect();
        }

        void TextBox::DrawSelf(const DrawInfo& drawInfo) {
            if (!mTextBuf || !mpFont || !mpMaterial) {
                return;
            }

            LoadMtx(drawInfo);

            ut::WideTextWriter writer;

            ut::Rect textRect = GetTextDrawRect(&writer);

            ut::Color topCol = detail::MultipleAlpha(mTextColors[0], mGlbAlpha);
            ut::Color btmCol = detail::MultipleAlpha(mTextColors[1], mGlbAlpha);
            writer.SetGradationMode(topCol != btmCol ? ut::CharWriter::GRADMODE_V : ut::CharWriter::GRADMODE_NONE);
            writer.SetTextColor(topCol, btmCol);
            ut::Color minCol = GetColor(mpMaterial->GetTevColor(0));
            ut::Color maxCol = GetColor(mpMaterial->GetTevColor(1));
            writer.SetColorMapping(minCol, maxCol);
            writer.SetupGX();

            GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);

            // PC_PORT (M9.5.3c): petari's decomp stops before the actual
            // print. The nw4r reference (homebuttonLib's lyt_textBox.cpp, the
            // same library family) positions the pen at the aligned text
            // origin and then prints the string. Its per-line CalcLineStrNum
            // loop relies on a width-limited CalcLineRectImpl overload petari
            // does not have; our TextWriterBase::PrintImpl walks the whole
            // stream (explicit '\n' tags wrap lines), so the single Print is
            // the faithful subset — width-limit wrapping lands if a shipped
            // layout needs it.
            writer.SetCursor(textRect.left, textRect.top);
            writer.Print(mTextBuf, mTextLen);
        }

        void TextBox::SetFont(const ut::Font* pFont) {
            if (mBits.bAllocFont) {
                mpFont->~Font();
                Layout::FreeMemory(const_cast< ut::Font* >(mpFont));
                mBits.bAllocFont = false;
            }

            mpFont = pFont;

            if (mpFont != NULL) {
                SetFontSize(Size(static_cast< f32 >(mpFont->GetWidth()), static_cast< f32 >(mpFont->GetHeight())));
            } else {
                SetFontSize(Size(0.0f, 0.0f));
            }
        }

        const ut::Rect TextBox::GetTextDrawRect(ut::WideTextWriter* pWriter) const {
            ut::Rect rect;

            pWriter->SetCursor(0.0f, 0.0f);

            pWriter->SetFont(*mpFont);
            pWriter->SetFontSize(mFontSize.width, mFontSize.height);

            pWriter->SetLineSpace(mLineSpace);
            pWriter->SetCharSpace(mCharSpace);

            pWriter->SetWidthLimit(mSize.width);
            pWriter->SetDrawFlag(MakeDrawFlag());

            if (mpTagProcessor != NULL) {
                pWriter->SetTagProcessor(mpTagProcessor);
            }

            pWriter->CalcStringRect(&rect, mTextBuf, mTextLen);

            math::VEC2 base = GetVtxPos();
            rect.MoveTo(base.x + (mSize.width - rect.GetWidth()) * GetTextMagH(), base.y + (mSize.height - rect.GetHeight()) * GetTextMagV());

            return rect;
        }

        u16 TextBox::GetStringBufferLength() const {
            if (mTextBufBytes == 0) {
                return 0;
            }

            return mTextBufBytes / sizeof(wchar_t) - 1;
        }

        void TextBox::AllocStringBuffer(u16 len) {
            if (len == 0) {
                return;
            }

            u16 chars = len + 1;
            u16 bytes = chars * sizeof(wchar_t);

            if (bytes > mTextBufBytes) {
                FreeStringBuffer();
                mTextBuf = static_cast< wchar_t* >(Layout::AllocMemory(bytes));

                if (mTextBuf != NULL) {
                    mTextBufBytes = bytes;
                }
            }
        }

        void TextBox::FreeStringBuffer() {
            if (mTextBuf == NULL) {
                return;
            }

            Layout::FreeMemory(mTextBuf);
            mTextBuf = NULL;
            mTextBufBytes = 0;
        }

        u16 TextBox::SetString(const wchar_t* pStr, u16 pos) {
            return SetString(pStr, pos, wcslen(pStr));
        }

        u16 TextBox::SetString(const wchar_t* pStr, u16 pos, u16 len) {
            if (mTextBuf == NULL) {
                return 0;
            }

            const u16 maxlen = GetStringBufferLength();
            if (pos >= maxlen) {
                return 0;
            }

            const u16 chars = ut::Min< u16 >(len, maxlen - pos);
            memcpy(&mTextBuf[pos], pStr, chars * sizeof(wchar_t));

            mTextLen = pos + chars;
            mTextBuf[mTextLen] = L'\0';

            return chars;
        }

        f32 TextBox::GetTextMagH() const {
            f32 mag = 0.0f;

            switch (GetTextPositionH()) {
            default:
            case 0: {
                mag = 0.0f;
                break;
            }

            case 1: {
                mag = 0.5f;
                break;
            }

            case 2: {
                mag = 1.0f;
                break;
            }
            }

            return mag;
        }

        f32 TextBox::GetTextMagV() const {
            f32 mag = 0.0f;

            switch (GetTextPositionV()) {
            default:
            case 0: {
                mag = 0.0f;
                break;
            }

            case 1: {
                mag = 0.5f;
                break;
            }

            case 2: {
                mag = 1.0f;
                break;
            }
            }

            return mag;
        }

        u32 TextBox::MakeDrawFlag() const {
            u32 flag = 0;

            switch (GetTextPositionH()) {
            case 1: {
                flag |= 1;
                break;
            }

            case 2: {
                flag |= 2;
                break;
            }

            default: {
                break;
            }
            }

            return flag;
        }

        f32 TextBox::GetTextAlignMag() const {
            switch (GetTextAlignment()) {
            case 0:
            default:
                return GetTextMagH();
            case 1:
                return 0;
            case 2:
                return 0.5f;
            case 3:
                return 1.0f;
            }
        }
    };  // namespace lyt
};  // namespace nw4r
