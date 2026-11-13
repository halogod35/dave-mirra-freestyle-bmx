/*
 * zaxis/Bmx/Src/com/menu/gui/guisfile.c
 * Original: (no path string; the name follows its "GUISFILE:" messages, DM-056)
 * Evidence: [V] the "GUISFILE:" literals 0x0055A168-0x0055A218 (with "EDEF" 0x0055A1DC and "SCRN"
 *           0x0055A210) follow guimain.c's last literal 0x0055A128 and precede guitoggl.c's
 *           0x0055A24C in .data; the only code that references them is 0x00439A00, which sits
 *           between guimain.c and guitoggl.c (link order = literal order). DM-040 had it in guimain.c
 *           (split). The one "GUISFILE:" message inside guimain.c (0x00559F2C, guiLoadScreenNow) is in
 *           guimain.c's own literal run.
 * 1 function, VA 0x00439a00-0x00439e5b.
 * DM-056 (draft). Non-matching C (AD-8). Types: gui.h. Format: docs/formats/ZAL.md 3.2,
 * tools/dmbmx/formats/scrn.py (tests/test_scrn_*.py check the data against this reader).
 */
#include <string.h>
#include "decomp.h"
#include "gui.h"

void guiSetAnimCount(GuiElement *e, uint16_t count);

/* VA 0x00439a00, 1116 bytes, [I] row, status draft */
int guiLoadScreenFile(uint32_t id)
{
    /* The SCRN asset 'id' (group 0xE000 | n is loaded by guiLoadScreenNow) -> screen style and
     * one element per EDEF. Text offsets point into the STRS block (from its "STRS" tag). */
    ZprimAsset *asset;
    GuiScrnHeader *hdr;
    const GuiEdef *ed;
    const char *strs;
    GuiElement *e, *focus;
    int i;

    asset = zprimLoadAsset(id);
    if (asset == NULL) {
        g_pfnDebugPrintf("GUISFILE: Error - failed to load screen %x\n", id & 0xFFFF);
        return 1;
    }
    hdr = (GuiScrnHeader *)zprimLockAsset(asset);
    if (memcmp(hdr->tag, "SCRN", 4) != 0) {
        g_pfnDebugPrintf("GUISFILE: Error - bad screen %04.4X\n", id & 0xFFFF);
        zprimUnlockAsset(asset);
        return 1;
    }
    if (hdr->backdrop != 0)
        guiLoadBackdrop(hdr->backdrop);
    guiSetFocusFont(hdr->focusFont);
    guiSetFocusRGB(hdr->focusRGB[0], hdr->focusRGB[1], hdr->focusRGB[2]);
    guiSetBlurFont(hdr->blurFont);
    guiSetBlurRGB(hdr->blurRGB[0], hdr->blurRGB[1], hdr->blurRGB[2]);
    strs = (const char *)hdr + hdr->strsOffset;

    focus = NULL;
    if (hdr->edefCount != 0) {
        ed = (const GuiEdef *)((const uint8_t *)hdr + hdr->edefOffset);
        for (i = 0; i < hdr->edefCount; i++, ed++) {
            if (memcmp(ed->tag, "EDEF", 4) != 0) {
                g_pfnDebugPrintf("GUISFILE: Error - bad screen element def %04.4X\n", id & 0xFFFF);
                zprimUnlockAsset(asset);
                return 1;
            }
            e = guiCreateElement(ed->type, ed->priority, ed->id);
            if (e == NULL) {
                g_pfnDebugPrintf("GUISFILE: Error - failed to allocate element\n");
                zprimUnlockAsset(asset);
                return 1;
            }
            if (ed->flags & GUIF_DEFAULT)
                focus = e;
            guiSetFlags(e, ed->flags);
            guiSetLink(e, 0, ed->link[0]);
            guiSetLink(e, 1, ed->link[1]);
            guiSetLink(e, 2, ed->link[2]);
            guiSetLink(e, 3, ed->link[3]);
            guiSetSlide(e, ed->slideX, ed->slideY);
            guiSetPos(e, ed->x, ed->y);
            if (ed->flags & GUIF_SCALED)
                guiSetScale(e, ed->scaleX * (1.0f / 256.0f), ed->scaleY * (1.0f / 256.0f));
            guiSetSize(e, ed->w, ed->h);

            if (!(ed->spriteFlags & 2)) {
                guiSetSpriteFlags(e, ed->spriteFlags);
                if (ed->sprite != 0)
                    guiLoadSprite(e, ed->sprite);
                if (ed->selSprite != 0)
                    guiLoadSelSprite(e, ed->selSprite);
                guiSetSpriteOffset(e, ed->spriteX, ed->spriteY);
                guiSetFrame(e, ed->frame);
                if (ed->spriteFlags & GUIS_ANIM) {
                    guiSetAnimCount(e, ed->animCount);
                    guiSetAnim(e, ed->animCount, ed->animFrames, ed->animTimes);
                }
                if (ed->spriteFlags & GUIS_MODE1)
                    guiSetSpriteParam(e, ed->spriteParam);
            } else {
                guiSetSpriteFlags(e, 2);
            }

            if (!(ed->textFlags & 2)) {
                guiSetTextFlags(e, ed->textFlags);
                guiSetSpacing(e, ed->spacing);
                if (ed->font != 0)
                    guiLoadFont(e, ed->font);
                guiSetTextPos(e, ed->textX, ed->textY);
                if (ed->text != 0)
                    guiSetText(e, strs + ed->text);
                guiSetTextRGB(e, ed->textRGB[0], ed->textRGB[1], ed->textRGB[2]);
            } else {
                guiSetTextFlags(e, 2);
            }

            if (ed->type == GUI_TYPE_TOGGLE && !(ed->toggleFlags & 2)) {
                guiToggleSetFlags(e, ed->toggleFlags);
                guiToggleSetSpacing(e, ed->toggleSpacing);
                if (ed->toggleFont != 0)
                    guiToggleLoadFont(e, ed->toggleFont);
                guiToggleSetTextPos(e, ed->toggleX, ed->toggleY);
                if (ed->toggleText != 0)
                    guiToggleSetOptions(e, strs + ed->toggleText);
                guiToggleSetRGB(e, ed->toggleRGB[0], ed->toggleRGB[1], ed->toggleRGB[2]);
            } else {
                guiToggleSetFlags(e, 2);
            }
        }
        if (focus != NULL)
            guiSendMessage(GUI_MSG_FOCUS, focus, 0, 0);
    }
    zprimUnlockAsset(asset);
    return 0;
}
