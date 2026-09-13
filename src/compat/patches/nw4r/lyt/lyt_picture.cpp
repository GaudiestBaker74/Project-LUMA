// =============================================================================
// PC_PORT PATCH of the vendored nw4r/lyt/lyt_picture.cpp (see
// src/compat/patches/README.md).
//
// Change vs. upstream (M9.5.4 hardening): the Picture resource constructor
// indexed the mat1 offset table with `pRes->materialIdx` unconditionally.
// A brlyt whose pic1 block precedes mat1 (null pMaterialList), or whose
// index is out of range (corrupt / mis-swapped data), read a wild offset and
// faulted inside the Material ctor — with no log line, on the scene-init
// worker thread. The patched ctor validates the index against
// MaterialList::materialNum first and leaves the pane without a material
// (DrawSelf already bails on a null mpMaterial). Same guard TextBox got in
// the patched lyt_textBox.cpp.
//
// Change vs. upstream (title widescreen): DrawSelf widens screen-covering
// panes (see the note inside) so full-screen effects such as TitleLogo's
// PicFlash cover a widescreen framebuffer instead of only the 4:3 design area.
//
// Everything else is identical to upstream.
// =============================================================================
#include "nw4r/lyt/layout.h"
#include "nw4r/lyt/picture.h"
#include "nw4r/ut/inlines.h"
#include "platform/Log/Log.h"  // PC_PORT
#include "compat/game/UiAnchoring.h"  // PC_PORT: screen-covering panes (the flash)

#include <set>       // PC_PORT: the once-per-pane report below
#include <string>
#include <cstring>

namespace nw4r {
    namespace lyt {
        NW4R_UT_RTTI_DEF_DERIVED(Picture, Pane);

        Picture::Picture(const res::Picture* pRes, const ResBlockSet& rBlockSet) : Pane(pRes) {
            u8 num = ut::Min< u8 >(pRes->texCoordNum, GX_MAX_TEXCOORD);
            Init(num);

            for (int i = 0; i < 4; i++) {
                mVtxColors[i] = pRes->vtxCols[i];
            }

            if (num > 0 && !mTexCoordAry.IsEmpty()) {
                mTexCoordAry.Copy(reinterpret_cast< const u8* >(pRes) + sizeof(res::Picture), num);
            }

            // PC_PORT (M9.5.4 hardening): validate materialIdx against mat1
            // before indexing the offset table (see the file header).
            if (rBlockSet.pMaterialList == NULL || pRes->materialIdx >= rBlockSet.pMaterialList->materialNum) {
                PL_LOG_WARN("compat.lyt", "Picture '%.16s': material %u out of range (materialNum %u) — no material",
                            pRes->name, static_cast< unsigned >(pRes->materialIdx),
                            rBlockSet.pMaterialList != NULL ? static_cast< unsigned >(rBlockSet.pMaterialList->materialNum) : 0u);
                return;
            }

            void* pMaterialBuf = Layout::AllocMemory(sizeof(Material));

            if (pMaterialBuf != NULL) {
                const u32* const pMatOffsetTbl = detail::ConvertOffsToPtr< u32 >(rBlockSet.pMaterialList, sizeof(res::MaterialList));

                const res::Material* const pResMaterial =
                    detail::ConvertOffsToPtr< res::Material >(rBlockSet.pMaterialList, pMatOffsetTbl[pRes->materialIdx]);

                Material* pMaterial = new (pMaterialBuf) Material(pResMaterial, rBlockSet);

                mpMaterial = pMaterial;
            }
        }

        void Picture::Init(u8 num) {
            if (num > 0) {
                ReserveTexCoord(num);
            }
        }

        void Picture::ReserveTexCoord(u8 num) {
            mTexCoordAry.Reserve(num);
        }

        Picture::~Picture() {
            if (mpMaterial != NULL && !mpMaterial->IsUserAllocated()) {
                mpMaterial->~Material();
                Layout::FreeMemory(mpMaterial);
                mpMaterial = NULL;
            }

            mTexCoordAry.Free();
        }

        void Picture::Append(const TexMap& rTexMap) {
            if (mpMaterial->GetTextureNum() >= mpMaterial->GetTextureCap() || mpMaterial->GetTextureNum() >= mpMaterial->GetTexCoordGenCap()) {
                return;
            }

            u8 idx = mpMaterial->GetTextureNum();

            mpMaterial->SetTextureNum(idx + 1);
            mpMaterial->SetTexture(idx, rTexMap);

            mpMaterial->SetTexCoordGenNum(mpMaterial->GetTextureNum());
            mpMaterial->SetTexCoordGen(idx, TexCoordGen());

            SetTexCoordNum(mpMaterial->GetTextureNum());

            if (mSize == Size(0.0f, 0.0f) && mpMaterial->GetTextureNum() == 1) {
                mSize = detail::GetTextureSize(mpMaterial, 0);
            }
        }

        void Picture::SetTexCoordNum(u8 num) {
            mTexCoordAry.SetSize(num);
        }

        const ut::Color Picture::GetVtxColor(u32 idx) const {
            return mVtxColors[idx];
        }

        void Picture::SetVtxColor(u32 idx, ut::Color color) {
            mVtxColors[idx] = color;
        }

        u8 Picture::GetVtxColorElement(u32 idx) const {
            return detail::GetVtxColorElement(mVtxColors, idx);
        }

        void Picture::SetVtxColorElement(u32 idx, u8 value) {
            detail::SetVtxColorElement(mVtxColors, idx, value);
        }

        void Picture::DrawSelf(const DrawInfo& rInfo) {
            if (mpMaterial == NULL) {
                return;
            }

            LoadMtx(rInfo);

            bool useVtxColor = mpMaterial->SetupGX(detail::IsModulateVertexColor(mVtxColors, mGlbAlpha), mGlbAlpha);

            detail::SetVertexFormat(useVtxColor, mTexCoordAry.GetSize());

            // PC_PORT (title widescreen): a pane that covers the whole design
            // area is a screen-covering EFFECT (TitleLogo's PicFlash: an 8x8
            // texture stretched over the screen and faded by the logo's
            // "Appear" animation). The console's design space is 4:3, so drawn
            // at its authored size the flash covered only the middle 960 px of
            // a 1280-wide frame and the sides kept the unflashed scene — a hard
            // vertical cut at the 4:3 edges. Widen it symmetrically about its
            // centre (the screen centre) until it covers the framebuffer; the
            // same "extend the sides, keep the centre" rule the background
            // follows. Ordinary panes are untouched (factor 1).
            math::VEC2 basePt = GetVtxPos();
            Size size = mSize;

            const f32 widen = compat::ui::screenCoveringPaneScaleX(mSize.width, mSize.height);

            if (widen > 1.0f) {
                const f32 extra = mSize.width * (widen - 1.0f);
                basePt.x -= extra * 0.5f;
                size.width += extra;

                // Reported once per pane: this is a screen-covering effect
                // being stretched over a widescreen frame, worth seeing in the
                // log (and the place to look if a pane is widened by mistake).
                static std::set< std::string > sReported;
                std::string name(mName, strnlen(mName, sizeof(mName)));
                if (sReported.insert(name).second) {
                    PL_LOG_INFO("compat.lyt", "screen-covering pane '%s': %.0fx%.0f design units widened x%.3f to cover the framebuffer",
                                name.c_str(), static_cast< double >(mSize.width), static_cast< double >(mSize.height),
                                static_cast< double >(widen));
                }
            }

            detail::DrawQuad(basePt, size, mTexCoordAry.GetSize(), mTexCoordAry.GetArray(), useVtxColor ? mVtxColors : NULL, mGlbAlpha);
        }

    };  // namespace lyt
};  // namespace nw4r
