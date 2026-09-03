// =============================================================================
// PC_PORT PATCH of the vendored nw4r/lyt/lyt_texMap.cpp (see
// src/compat/patches/README.md).
//
// Change vs. upstream: TexMap::ReplaceImage(TPLPalette*, u32) detected an
// unbound palette with `reinterpret_cast<u32>(p->descriptorArray) <
// 0x80000000` (a PPC32 address-space test) and bound it in place with
// TPLBind's 32-bit offset arithmetic. Neither compiles nor works on an
// LP64 host: TPL blobs are big-endian and their pointer fields are 4-byte
// file offsets. The patched version routes the blob through
// Platform::CompatGx::tplToHost (compat/gx/TplHost), which converts it to a
// native struct tree (cached per source blob).
//
// Everything else is identical to upstream.
// =============================================================================
#include "nw4r/lyt/common.h"
#include "compat/gx/TplHost.h"
#include "platform/Log/Log.h"
#include "nw4r/lyt/texMap.h"
#include "revolution/gx/GXEnum.h"
#include "revolution/gx/GXGet.h"
#include "revolution/gx/GXStruct.h"
#include "revolution/tpl.h"

namespace nw4r {
    namespace lyt {
        void TexMap::Get(_GXTexObj* pTexObj) const {
            if (detail::IsCITexelFormat(GetTexelFormat())) {
                u32 tlutName = GXGetTexObjTlut(pTexObj);
                GXInitTexObjCI(pTexObj, mImage, mWidth, mHeight, GXCITexFmt(GetTexelFormat()), GetWrapModeS(), GetWrapModeT(), IsMipMap(), tlutName);
            } else {
                GXInitTexObj(pTexObj, mImage, mWidth, mHeight, GetTexelFormat(), GetWrapModeS(), GetWrapModeT(), IsMipMap());
            }

            GXInitTexObjLOD(pTexObj, GetMinFilter(), GetMagFilter(), GetMinLOD(), GetMaxLOD(), GetLODBias(), IsBiasClampEnable(), IsEdgeLODEnable(),
                            GetAnisotropy());
        }

        void TexMap::Get(_GXTlutObj* pTlutObj) const {
            GXInitTlutObj(pTlutObj, GetPalette(), GetPaletteFormat(), GetPaletteEntryNum());
        }

        void TexMap::Set(const GXTexObj& texObj) {
            void* image;
            u16 width, height;
            GXTexFmt format;
            GXTexWrapMode wrapS, wrapT;
            GXBool mipmap;

            GXGetTexObjAll(&texObj, &image, &width, &height, &format, &wrapS, &wrapT, &mipmap);

            mImage = image;
            SetSize(width, height);
            mBits.textureFormat = format;
            SetWrapMode(wrapS, wrapT);
            SetMipMap(mipmap);

            GXTexFilter minFilter, magFilter;
            f32 minLOD, maxLOD, lodBias;
            GXBool biasCLampEnable, edgeLODEnable;
            GXAnisotropy aniso;
            GXGetTexObjLODAll(&texObj, &minFilter, &magFilter, &minLOD, &maxLOD, &lodBias, &biasCLampEnable, &edgeLODEnable, &aniso);

            SetFilter(minFilter, magFilter);
            SetLOD(minLOD, maxLOD);
            SetLODBias(lodBias);
            SetBiasClampEnable(biasCLampEnable);
            SetEdgeLODEnable(edgeLODEnable);
            mBits.anisotropy = aniso;
        }

        void TexMap::ReplaceImage(const TPLDescriptor* pTPLDesc) {
            const TPLHeader& header = *pTPLDesc->textureHeader;
            mImage = header.data;
            SetSize(header.width, header.height);
            SetTexelFormat(GXTexFmt(header.format));

            if (const TPLClutHeader* const pClut = pTPLDesc->CLUTHeader) {
                SetPalette(pClut->data);
                SetPaletteFormat(pClut->format);
                SetPaletteEntryNum(pClut->numEntries);
            } else {
                SetPalette(nullptr);
                SetPaletteFormat(GXTlutFmt(0));
                SetPaletteEntryNum(0);
            }
        }

        void TexMap::ReplaceImage(TPLPalette* p, u32 id) {
            // PC_PORT (M9.5.2): the console bound the big-endian palette in
            // place (TPLBind, 32-bit offset arithmetic + a PPC address-space
            // test). The host converts it to a native struct tree instead;
            // the conversion is cached per source blob, so animation-driven
            // ReplaceImage calls are cheap.
            if (p == nullptr) {
                return;
            }

            // PC_PORT: breadcrumb for the first real-brlyt builds.
            PL_LOG_INFO("compat.lyt", "TexMap::ReplaceImage: palette %p id %u ...", p,
                        static_cast< unsigned >(id));

            TPLPalettePtr pHost = Platform::CompatGx::tplToHost(p);
            if (pHost == nullptr) {
                return;
            }

            const TPLDescriptor* pDesc = TPLGet(pHost, id);
            if (pDesc == nullptr || pDesc->textureHeader == nullptr) {
                PL_LOG_WARN("compat.lyt", "TexMap::ReplaceImage: descriptor %u has no texture header",
                            static_cast< unsigned >(id));
                return;
            }

            ReplaceImage(pDesc);

            PL_LOG_INFO("compat.lyt", "TexMap::ReplaceImage: done (%u x %u, fmt %u)",
                        static_cast< unsigned >(mWidth), static_cast< unsigned >(mHeight),
                        static_cast< unsigned >(GetTexelFormat()));
        }
    };  // namespace lyt
};  // namespace nw4r
