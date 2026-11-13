/*
 * zaxis/Bmx/Src/com/menu/gui/guitoggl.c
 * Original: C:\zaxis\Bmx\src\com\menu\gui\guitoggl.c
 * Evidence: [V] path string 0x0055A2AC referenced by 13 functions (their asserts; DM-056 created the
 *           two handlers 0x0043A840 / 0x0043AED0, reached only through thunks, and the setters
 *           0x0043B660 / 0x0043BD90). [I] 0x00439F80 / 0x00439FA1 (the generic element handler) by
 *           the pipeline's split pass (its one caller vote is guitoggl.c): no literal ties it to a file,
 *           and it could as well be an object of its own between guisfile.c and guitoggl.c.
 * 14 functions, VA 0x00439f80-0x0043be61.
 * DM-056 (draft). Non-matching C (AD-8). Types: gui.h. Notes: docs/re/frontend.md.
 *
 * guiElementHandler is the type handler of GUI_TYPE_SPRITE (1) and 5 and the "base class" every
 * other handler (button, toggle, the frontend's custom handlers) calls to draw an element.
 * 0x00439FA1 is not a real function: it is the draw case of guiElementHandler (after its jump at
 * 0x00439F99), which Ghidra's analysis had made a function of its own. DM-064's `nofunc` row in
 * decomp/symbols/symbols.csv removes it from Ghidra (ApplySymbols gives the code back to 0x00439F80); the C keeps
 * that case as the helper guiElementDraw, which has no VA of its own.
 *
 * The toggle element (GUI_TYPE_TOGGLE): a button with a second text, one option of "a|b|c",
 * drawn after the element and cycled with left / right (guiToggleHandler) or up / down
 * (guiToggleVHandler). [V] No shipped screen uses it (all EDEFs are type 1).
 */
#include <string.h>
#include "decomp.h"
#include "zmem.h"
#include "gui.h"

void guiDrawMarkup(const char *s, int16_t x, int16_t y);

#define GUITOGGL_SRC "C:\\zaxis\\Bmx\\src\\com\\menu\\gui\\guitoggl.c"

/* VA 0x00439f80, 1780 bytes, [I] row, status draft */
int guiElementHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    if (msg == GUI_MSG_UPDATE)
        return 1;
    if (msg != GUI_MSG_DRAW_ELEM)
        return 0;
    return guiElementDraw(msg, e, a, b);
}

/* The draw case of guiElementHandler (code 0x00439FA1-0x0043A66A, inside 0x00439F80; not a function
 * in the exe, DM-064). */
int guiElementDraw(uint32_t msg, GuiElement *e, uint32_t selected, uint32_t b)
{
    /* Draw the sprite part (frame animation, scale, pulse / highlight tint) and then the text
     * part (one line per '\n', aligned in the w x h box; text starting with '!' is markup). */
    ZprimAsset *spr;
    int16_t x, y, w, h, tx, ty;
    int scaled = 0, idx;
    uint16_t sf;
    char *s, *line, *nl;
    uint32_t saveArg24, saveClut38, saveClut34;

    x = (int16_t)(e->slideX + e->x);
    y = (int16_t)(e->slideY + e->y);
    w = e->w;
    h = e->h;
    spr = (selected && e->selSprite != NULL) ? e->selSprite : e->sprite;
    if (spr != NULL) {
        w = (int16_t)zprimGetSpriteWidth(spr, e->frame);
        h = (int16_t)zprimGetSpriteHeight(spr, e->frame);
    }
    if (e->flags & GUIF_SCALED) {
        if (e->flags & GUIF_WIDE) {
            w = (int16_t)(w * 160 / 100);
        } else {
            w = (int16_t)(w * e->scaleX);
            h = (int16_t)(h * e->scaleY);
        }
        zprimSetScale(e->scaleX, e->scaleY);
        scaled = 1;
    } else if (e->flags & GUIF_WIDE) {
        w = (int16_t)(w * 160 / 100);
        zprimSetScale(1.0f, 1.0f);
        scaled = 1;
    }
    if (e->flags & GUIF_CENTRE_X)
        x = (int16_t)(x - w / 2);
    if (e->flags & GUIF_CENTRE_Y)
        y = (int16_t)(y - h / 2);

    /* ---- sprite ---- */
    sf = e->spriteFlags;
    if (!(sf & GUIS_NOSPRITE)) {
        if (sf & GUIS_ANIM) {
            idx = e->animIndex;
            if (e->animTimer < e->animTimes[idx]) {
                e->animTimer++;
            } else if (!(sf & GUIS_BACKWARDS)) {
                e->animIndex = (int16_t)(idx + 1);
                if (e->animIndex >= (int)e->animCount) {
                    if (!(sf & GUIS_LOOP)) {
                        e->animIndex = (int16_t)(e->animCount - 1);        /* stop on the last frame */
                        e->spriteFlags = sf & ~GUIS_ANIM;
                        e->animTimer = 0;
                    } else if (!(sf & GUIS_PINGPONG)) {
                        e->animIndex = 0;
                        e->animTimer = 0;
                    } else {
                        e->animIndex = (int16_t)(idx - 1);
                        e->spriteFlags = sf | GUIS_BACKWARDS;
                        e->animTimer = 0;
                    }
                } else {
                    e->animTimer = 0;
                }
            } else {
                e->animIndex = (int16_t)(idx - 1);
                if (e->animIndex < 0) {
                    if (!(sf & GUIS_LOOP)) {
                        e->animIndex = (int16_t)(e->animCount - 1);        /* sic: the last frame */
                        e->spriteFlags = sf & ~GUIS_ANIM;
                        e->animTimer = 0;
                    } else if (!(sf & GUIS_PINGPONG)) {
                        e->animTimer = 0;
                        e->animIndex = (int16_t)(e->animCount - 1);
                    } else {
                        e->animIndex = 1;
                        e->spriteFlags = sf & ~GUIS_BACKWARDS;
                        e->animTimer = 0;
                    }
                } else {
                    e->animTimer = 0;
                }
            }
            e->frame = e->animFrames[e->animIndex];
        }
        zprimSetAttr(ZPRIM_ATTR_SPR_MODE3, (e->spriteFlags & GUIS_MODE3) != 0);
        if (e->spriteFlags & GUIS_MODE1) {
            zprimSetAttr(ZPRIM_ATTR_SPR_MODE1, 1);
            zprimSetAttr(ZPRIM_ATTR_SPR_ARG24, e->spriteParam);
        } else {
            zprimSetAttr(ZPRIM_ATTR_SPR_MODE1, 0);
        }
        zprimSetAttr(ZPRIM_ATTR_SPR_ARG30, e->palette);
        zprimSetSprite((selected && e->selSprite != NULL) ? e->selSprite : e->sprite);
        if (!guiHelpActive() && (e->flags & GUIF_PULSE)) {
            zprimSetRGB(0xDE, 0xDE, 0x9C, ZPRIM_ATTR_CLUT38);     /* pale yellow glow */
            zprimSetAttr(ZPRIM_ATTR_CLUT34, g_guiPulse);
        }
        if (e->flags & GUIF_HIGHLIGHT) {
            zprimSetAttr(ZPRIM_ATTR_CLUT38, e->highlight);
            zprimSetAttr(ZPRIM_ATTR_CLUT34, e->alpha);
        }
        zprimDrawSprite(e->frame, (int16_t)(e->spriteX + x), (int16_t)(e->spriteY + y));
        if (scaled)
            zprimResetScale();
        if (e->flags & (GUIF_PULSE | GUIF_HIGHLIGHT))
            zprimSetAttr(ZPRIM_ATTR_CLUT34, 0);
    }

    /* ---- text ---- */
    if ((e->textFlags & GUIT_NOTEXT) || e->text == 0)
        return 1;
    s = (char *)zmemDMMLock(e->text);
    for (line = s; *line != '\0'; ) {
        nl = strchr(line, '\n');
        if (nl != NULL)
            *nl = '\0';
        zprimSetAttr(ZPRIM_ATTR_FONT_MODE3, (e->textFlags & GUIT_MODE3) != 0);
        if (e->textFlags & GUIT_RGB) {
            zprimSetAttr(ZPRIM_ATTR_FONT_MODE2, 1);
            zprimSetRGB(e->rgb[0], e->rgb[1], e->rgb[2], ZPRIM_ATTR_FONT_ARG48);
        } else {
            zprimSetAttr(ZPRIM_ATTR_FONT_MODE2, 0);
        }
        saveArg24 = zprimGetAttr(ZPRIM_ATTR_SPR_ARG24);
        saveClut38 = zprimGetAttr(ZPRIM_ATTR_CLUT38);
        saveClut34 = zprimGetAttr(ZPRIM_ATTR_CLUT34);
        if (e->textFlags & GUIT_HIGHLIGHT) {
            zprimSetAttr(ZPRIM_ATTR_FONT_MODE1, 1);
            zprimSetAttr(ZPRIM_ATTR_SPR_ARG24, e->spriteParam);
            zprimSetAttr(ZPRIM_ATTR_CLUT38, e->highlight);
            zprimSetAttr(ZPRIM_ATTR_CLUT34, e->alpha);
        } else {
            zprimSetAttr(ZPRIM_ATTR_FONT_MODE1, 0);
        }
        zprimSetAttr(ZPRIM_ATTR_FONT_ARG40, e->textPalette);
        zprimSetAttr(ZPRIM_ATTR_SPACING, (uint32_t)(int)e->spacing);
        tx = e->textX;
        ty = e->textY;
        zprimSetFont(e->font);
        if (*line == '!') {
            zprimSetSprite(e->sprite);
            zprimSetAttr(ZPRIM_ATTR_SPR_MODE3, 1);
            guiDrawMarkup(line, (int16_t)(tx + x), (int16_t)(ty + y));
        } else {
            /* the alignment measures 's' (the text from its first line up to the end of this
             * one), not 'line': on the second and later lines centred / right-aligned text is
             * placed by the width of all lines so far (a quirk of the original) */
            if (e->textFlags & GUIT_CENTRE_X) {
                if (!(e->textFlags & GUIT_FIXED))
                    tx = (int16_t)(tx + (w - zprimGetTextWidth(e->font, (uint8_t *)s)) / 2);
                else
                    tx = (int16_t)(tx + ((w - (int)strlen(s) * e->spacing) >> 1));
            }
            if (e->textFlags & GUIT_CENTRE_Y)
                ty = (int16_t)(ty + (h - zprimGetFontHeight(e->font)) / 2);
            if (e->textFlags & GUIT_RIGHT) {
                if (!(e->textFlags & GUIT_FIXED))
                    tx = (int16_t)(tx + w - zprimGetTextWidth(e->font, (uint8_t *)s));
                else
                    tx = (int16_t)(tx + w - (int16_t)strlen(s) * e->spacing);
            }
            if (!(e->textFlags & GUIT_FIXED))
                zprimDrawText((uint8_t *)line, tx + x, (int16_t)(ty + y));
            else
                zprimDrawTextFixed((uint8_t *)line, tx + x, (int16_t)(ty + y));
        }
        zprimSetAttr(ZPRIM_ATTR_SPACING, 0);
        zprimSetAttr(ZPRIM_ATTR_FONT_ARG40, 0);
        zprimSetAttr(ZPRIM_ATTR_SPR_ARG24, saveArg24);
        zprimSetAttr(ZPRIM_ATTR_CLUT38, saveClut38);
        zprimSetAttr(ZPRIM_ATTR_CLUT34, saveClut34);
        /* next line: advance by the font's line height */
        y = (int16_t)(y + 1 + zprimGetFontHeight(e->font));
        if (nl == NULL) {
            line = strchr(line, '\0');
        } else {
            *nl = '\n';
            line = nl + 1;
        }
    }
    zmemDMMUnlock(e->text);
    return 1;
}

/* ================================================================== the toggle */

static GuiToggleData *lockData(GuiElement *e, int line)
{
    GuiToggleData *d = (GuiToggleData *)zmemDMMLock(e->data);

    if (d == NULL)
        zAssertFail("GUITOGGL: Failed to lock toggle data\n", GUITOGGL_SRC, line, NULL);
    return d;
}

/* the toggle's own text, below the element part (lines, alignment as guiElementDraw) */
static void drawToggleText(GuiElement *e, uint32_t selected, int dataLine, int textLine, int paletteFirst)
{
    GuiToggleData *d;
    ZprimAsset *spr;
    int16_t x, y, w, h, tx, ty;
    char *s, *line, *nl;

    x = (int16_t)(e->slideX + e->x);
    y = (int16_t)(e->slideY + e->y);
    w = e->w;
    h = e->h;
    spr = (selected && e->selSprite != NULL) ? e->selSprite : e->sprite;
    if (spr != NULL) {
        w = (int16_t)zprimGetSpriteWidth(spr, e->frame);
        h = (int16_t)zprimGetSpriteHeight(spr, e->frame);
    }
    if (e->flags & GUIF_SCALED) {
        if (e->flags & GUIF_WIDE) {
            w = (int16_t)(w * 160 / 100);
        } else {
            w = (int16_t)(w * e->scaleX);
            h = (int16_t)(h * e->scaleY);
        }
        zprimSetScale(e->scaleX, e->scaleY);
    } else if (e->flags & GUIF_WIDE) {
        w = (int16_t)(w * 160 / 100);
        zprimSetScale(1.0f, 1.0f);
    }
    if (e->flags & GUIF_CENTRE_X)
        x = (int16_t)(x - w / 2);
    if (e->flags & GUIF_CENTRE_Y)
        y = (int16_t)(y - h / 2);
    if (e->data == 0)
        return;
    d = lockData(e, dataLine);
    if (!(d->flags & GUIT_NOTEXT) && d->text != 0) {
        s = (char *)zmemDMMLock(d->text);
        if (s == NULL)
            zAssertFail("GUITOGGL: Failed to lock toggle text\n", GUITOGGL_SRC, textLine, NULL);
        for (line = s; *line != '\0'; ) {
            nl = strchr(line, '\n');
            if (nl != NULL)
                *nl = '\0';
            zprimSetAttr(ZPRIM_ATTR_FONT_MODE3, (d->flags & GUIT_MODE3) != 0);
            if (d->flags & GUIT_RGB) {
                zprimSetAttr(ZPRIM_ATTR_FONT_MODE2, 1);
                zprimSetRGB(d->rgb[0], d->rgb[1], d->rgb[2], ZPRIM_ATTR_FONT_ARG48);
            } else {
                zprimSetAttr(ZPRIM_ATTR_FONT_MODE2, 0);
            }
            if (paletteFirst) {                 /* the two handlers set these in either order */
                zprimSetAttr(ZPRIM_ATTR_FONT_ARG40, d->palette);
                zprimSetAttr(ZPRIM_ATTR_SPACING, (uint32_t)(int)d->spacing);
            } else {
                zprimSetAttr(ZPRIM_ATTR_SPACING, (uint32_t)(int)d->spacing);
                zprimSetAttr(ZPRIM_ATTR_FONT_ARG40, d->palette);
            }
            tx = d->textX;
            ty = d->textY;
            if (d->flags & GUIT_CENTRE_X)
                tx = (int16_t)(tx + (w - zprimGetTextWidth(d->font, (uint8_t *)s)) / 2);
            if (d->flags & GUIT_CENTRE_Y)
                ty = (int16_t)(ty + (h - zprimGetFontHeight(d->font)) / 2);
            if (d->flags & GUIT_RIGHT)
                tx = (int16_t)(tx + w - zprimGetTextWidth(d->font, (uint8_t *)s));
            zprimSetFont(d->font);
            zprimDrawText((uint8_t *)line, tx + x, (int16_t)(ty + y));
            zprimSetAttr(ZPRIM_ATTR_SPACING, 0);
            zprimSetAttr(ZPRIM_ATTR_FONT_ARG40, 0);
            y = (int16_t)(y + 1 + zprimGetFontHeight(d->font));
            if (nl == NULL) {
                line = strchr(line, '\0');
            } else {
                *nl = '\n';
                line = nl + 1;
            }
        }
        zmemDMMUnlock(d->text);
    }
    zmemDMMUnlock(e->data);
}

static void cyclePrev(GuiElement *e)
{
    uint16_t i = guiToggleGetIndex(e);

    guiToggleSelect(e, (uint16_t)(i == 0 ? guiToggleGetCount(e) - 1 : i - 1));
}

static void cycleNext(GuiElement *e)
{
    uint16_t i = guiToggleGetIndex(e);

    guiToggleSelect(e, (uint16_t)(i == (uint16_t)(guiToggleGetCount(e) - 1) ? 0 : i + 1));
}

/* VA 0x0043a840, 1276 bytes, [V] direct, status draft */
int guiToggleHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    switch (msg) {
    case GUI_MSG_UPDATE:
        guiElementHandler(msg, e, a, b);
        return 1;
    case GUI_MSG_DRAW_ELEM:
        break;
    case GUI_MSG_FOCUS:
    case GUI_MSG_BLUR:
        guiButtonHandler(msg, e, a, b);
        return 1;
    case GUI_MSG_UP:
    case GUI_MSG_DOWN:
    case GUI_MSG_UP + GUI_MSG_PAD2:
    case GUI_MSG_DOWN + GUI_MSG_PAD2:
        guiButtonHandler(msg, e, a, b);
        return 0;
    case GUI_MSG_LEFT:
    case GUI_MSG_LEFT + GUI_MSG_PAD2:
        cyclePrev(e);
        return 0;
    case GUI_MSG_RIGHT:
    case GUI_MSG_RIGHT + GUI_MSG_PAD2:
        cycleNext(e);
        return 0;
    default:
        return 0;
    }
    if (e->flags & GUIF_FOCUS)
        guiElementHandler(GUI_MSG_DRAW_ELEM, e, 1, b);
    else
        guiElementHandler(msg, e, a, b);
    drawToggleText(e, a, 0x85, 0x8A, 1);
    return 1;
}

/* VA 0x0043aed0, 44 bytes, [I] row, status draft */
int guiToggleVHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    /* the same with the axes swapped: up / down cycle, left / right move the focus. Not used. */
    switch (msg) {
    case GUI_MSG_UPDATE:
        guiElementHandler(msg, e, a, b);
        return 1;
    case GUI_MSG_DRAW_ELEM:
        break;
    case GUI_MSG_FOCUS:
    case GUI_MSG_BLUR:
        guiButtonHandler(msg, e, a, b);
        return 1;
    case GUI_MSG_UP:
    case GUI_MSG_UP + GUI_MSG_PAD2:
        cyclePrev(e);
        return 0;
    case GUI_MSG_DOWN:
    case GUI_MSG_DOWN + GUI_MSG_PAD2:
        cycleNext(e);
        return 0;
    case GUI_MSG_LEFT:
    case GUI_MSG_RIGHT:
    case GUI_MSG_LEFT + GUI_MSG_PAD2:
    case GUI_MSG_RIGHT + GUI_MSG_PAD2:
        guiButtonHandler(msg, e, a, b);
        return 0;
    default:
        return 0;
    }
    if (e->flags & GUIF_FOCUS)
        guiElementHandler(GUI_MSG_DRAW_ELEM, e, 1, b);
    else
        guiElementHandler(msg, e, a, b);
    drawToggleText(e, a, 0x170, 0x175, 0);
    return 1;
}

/* VA 0x0043b560, 82 bytes, [V] direct, status draft */
void guiToggleSetFlags(GuiElement *e, uint16_t flags)
{
    GuiToggleData *d;

    if (e->data == 0)
        return;
    d = lockData(e, 0x20B);
    d->flags = flags;
    zmemDMMUnlock(e->data);
}

/* VA 0x0043b5e0, 91 bytes, [V] direct, status draft */
void guiToggleSetTextPos(GuiElement *e, int16_t x, int16_t y)
{
    GuiToggleData *d;

    if (e->data == 0)
        return;
    d = lockData(e, 0x21C);
    d->textX = x;
    d->textY = y;
    zmemDMMUnlock(e->data);
}

/* VA 0x0043b660, 85 bytes, [I] row, status draft */
void guiToggleSetPalette(GuiElement *e, uint32_t palette)
{
    GuiToggleData *d;

    if (e->data == 0)
        return;
    d = lockData(e, 0x22E);
    d->palette = palette & 0xFFFF;
    zmemDMMUnlock(e->data);
}

/* VA 0x0043b6e0, 122 bytes, [V] direct, status draft */
void guiToggleLoadFont(GuiElement *e, uint32_t id)
{
    ZprimAsset *font = zprimLoadAsset(id);
    GuiToggleData *d;

    if (font == NULL) {
        g_pfnDebugPrintf("GUITOGGL: Error - failed to load font %04.4X\n", id & 0xFFFF);
        return;
    }
    if (e->data == 0)
        return;
    d = lockData(e, 0x245);
    d->font = font;
    zmemDMMUnlock(e->data);
}

/* VA 0x0043b780, 94 bytes, [V] direct, status draft */
void guiToggleSetRGB(GuiElement *e, uint8_t r, uint8_t g, uint8_t b)
{
    GuiToggleData *d;

    if (e->data == 0)
        return;
    d = lockData(e, 0x256);
    d->rgb[0] = r;
    d->rgb[1] = g;
    d->rgb[2] = b;
    zmemDMMUnlock(e->data);
}

/* VA 0x0043b800, 82 bytes, [V] direct, status draft */
void guiToggleSetSpacing(GuiElement *e, int16_t spacing)
{
    GuiToggleData *d;

    if (e->data == 0)
        return;
    d = lockData(e, 0x269);
    d->spacing = spacing;
    zmemDMMUnlock(e->data);
}

/* VA 0x0043b880, 405 bytes, [V] direct, status draft */
void guiToggleSetOptions(GuiElement *e, const char *options)
{
    GuiToggleData *d;
    char *buf;
    uint16_t index;
    int same;

    if (e->data != 0) {
        d = lockData(e, 0x27D);
        options = guiTextLookup(options);
        if (d->options != 0) {
            buf = (char *)zmemDMMLock(d->options);
            same = strcmp(options, buf) == 0;
            zmemDMMUnlock(d->options);
            if (same) {
                zmemDMMUnlock(e->data);
                return;
            }
        }
        if (d->options != 0) {
            zmemDMMDealloc(d->options);
            d->options = 0;
        }
        d->options = zmemDMMAlloc((uint32_t)strlen(options) + 1);
        if (d->options == 0)
            zAssertFail("GUITOGGL: Error - failed to malloc text string\n", GUITOGGL_SRC, 0x29F, NULL);
        buf = (char *)zmemDMMLock(d->options);
        if (buf == NULL)
            zAssertFail("GUITOGGL: Failed to lock toggle text\n", GUITOGGL_SRC, 0x2A6, NULL);
        strcpy(buf, options);
        zmemDMMUnlock(d->options);
        index = d->index;
        zmemDMMUnlock(e->data);
        guiToggleSelect(e, index);
    }
    guiSendMessage(GUI_MSG_TEXT, e, (uint32_t)options, 0);
}

/* VA 0x0043ba90, 512 bytes, [V] direct, status draft */
void guiToggleSelect(GuiElement *e, uint16_t index)
{
    /* option 'index' of "a|b|c" becomes the toggle's text */
    GuiToggleData *d;
    char buf[200], *o = buf, *cur;
    const char *p;
    int n, same;

    if (e != NULL && e->data != 0) {
        d = lockData(e, 0x2C6);
        d->index = index;
        if (d->options != 0) {
            p = (const char *)zmemDMMLock(d->options);
            n = 0;
            if (index != 0) {
                do {
                    if ((*p != '|' && *p != '\0') || (n++, *p != '\0'))
                        p++;
                } while (n < index);
            }
            while (*p != '|' && *p != '\0')
                *o++ = *p++;
            zmemDMMUnlock(d->options);
        }
        *o = '\0';
        if (d->text != 0) {
            cur = (char *)zmemDMMLock(d->text);
            same = strcmp(buf, cur) == 0;
            zmemDMMUnlock(d->text);
            if (same) {
                zmemDMMUnlock(e->data);
                return;
            }
        }
        if (d->text != 0) {
            zmemDMMDealloc(d->text);
            d->text = 0;
        }
        d->text = zmemDMMAlloc((uint32_t)strlen(buf) + 1);
        if (d->text == 0)
            zAssertFail("GUITOGGL: Error - failed to malloc text string\n", GUITOGGL_SRC, 0x2F9, NULL);
        cur = (char *)zmemDMMLock(d->text);
        if (cur == NULL)
            zAssertFail("GUITOGGL: Failed to lock toggle text\n", GUITOGGL_SRC, 0x2FE, NULL);
        strcpy(cur, buf);
        zmemDMMUnlock(d->text);
        zmemDMMUnlock(e->data);
    }
    guiSendMessage(GUI_MSG_TEXT, e, (uint32_t)buf, 0);
}

/* VA 0x0043bd10, 82 bytes, [V] direct, status draft */
uint16_t guiToggleGetIndex(GuiElement *e)
{
    GuiToggleData *d;
    uint16_t index;

    if (e->data == 0)
        return 0;
    d = lockData(e, 0x313);
    index = d->index;
    zmemDMMUnlock(e->data);
    return index;
}

/* VA 0x0043bd90, 82 bytes, [I] row, status draft */
void guiToggleSetCount(GuiElement *e, uint16_t count)
{
    GuiToggleData *d;

    if (e->data == 0)
        return;
    d = lockData(e, 0x326);
    d->count = count;
    zmemDMMUnlock(e->data);
}

/* VA 0x0043be10, 82 bytes, [V] direct, status draft */
uint16_t guiToggleGetCount(GuiElement *e)
{
    GuiToggleData *d;
    uint16_t count;

    if (e->data == 0)
        return 0;
    d = lockData(e, 0x338);
    count = d->count;
    zmemDMMUnlock(e->data);
    return count;
}
