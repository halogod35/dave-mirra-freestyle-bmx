/*
 * _unassigned/gap_rider__guiinput.c
 * Original: (unknown) - between rider.c and Bmx/Src/com/menu/gui/guiinput.c in link order.
 * Evidence: [I] none of these functions references a path string or a literal of a neighbour.
 *           They are GUI code (they call guimain.c and are called by it), so they belong to an
 *           object of Bmx/Src/com/menu/gui/ that links before guiinput.c (the gui objects link in
 *           alphabetical order: guiinput, guimain, guisfile, guitoggl). The help-system stubs
 *           ("GUIMAIN: Failed to init help system") suggest a stubbed-out help module.
 * 15 functions, VA 0x00436520-0x00436c05.
 * DM-056 (draft). Non-matching C (AD-8). Types: gui.h. Notes: docs/re/frontend.md.
 *
 * 6 of the 15 were not functions in the Ghidra project (DM-056 creates them in decomp/symbols/symbols.csv):
 * they are reached through the /INCREMENTAL thunk table only.
 */
#include "decomp.h"
#include "gui.h"

static uint32_t g_guiMarkupTarget;     /* 0x0056D290 element whose text "!B$...$" sets */

/* VA 0x00436520, 462 bytes, unassigned, status draft */
int guiButtonHandler(uint32_t msg, GuiElement *e, uint32_t a, uint32_t b)
{
    /* GUI_TYPE_BUTTON: a sprite/text element that takes the focus. Focus and blur restyle the
     * text with the screen's focus / blur font and colour (SCRN +0x20 / +0x26); up / down move the
     * focus to the element named by link[0] / link[1]. */
    GuiElement *other;
    uint16_t font;
    uint8_t rgb[3];
    int i;

    switch (msg) {
    case GUI_MSG_UPDATE:
        guiElementHandler(msg, e, a, b);
        return 1;
    case GUI_MSG_DRAW_ELEM:
        if (!(e->flags & GUIF_FOCUS))
            guiElementHandler(msg, e, a, b);
        else
            guiElementHandler(GUI_MSG_DRAW_ELEM, e, 1, b);
        return 1;
    case GUI_MSG_FOCUS:
        for (i = 0; (other = guiGetElement(i)) != NULL; i++)
            if (other != e)
                guiSendMessage(GUI_MSG_BLUR, other, 0, 0);
        e->flags |= GUIF_FOCUS;
        guiSetPulse(e, 1);
        font = guiGetFocusFont();
        if (font != 0)
            guiLoadFont(e, font);
        if (!(e->textFlags & GUIT_RGB))
            return 1;
        guiGetFocusRGB(&rgb[0], &rgb[1], &rgb[2]);
        guiSetTextRGB(e, rgb[0], rgb[1], rgb[2]);
        return 1;
    case GUI_MSG_BLUR:
        e->flags &= ~GUIF_FOCUS;
        guiSetPulse(e, 0);
        font = guiGetBlurFont();
        if (font != 0)
            guiLoadFont(e, font);
        if (!(e->textFlags & GUIT_RGB))
            return 1;
        guiGetBlurRGB(&rgb[0], &rgb[1], &rgb[2]);
        guiSetTextRGB(e, rgb[0], rgb[1], rgb[2]);
        return 1;
    case GUI_MSG_UP:
    case GUI_MSG_UP + GUI_MSG_PAD2:
        if (e != NULL && e->link[0] != 0) {
            guiFocus((e->id & 0xFFFFFF00u) | e->link[0]);
            return 1;
        }
        break;
    case GUI_MSG_DOWN:
    case GUI_MSG_DOWN + GUI_MSG_PAD2:
        if (e != NULL && e->link[1] != 0) {
            guiFocus((e->id & 0xFFFFFF00u) | e->link[1]);
            return 1;
        }
        break;
    }
    return 0;
}

/* VA 0x004367c0, 96 bytes, unassigned, status draft */
static const char *guiParseInt(const char *s, int *out)
{
    /* "[ ,]*-?digits[ ,]*": a signed decimal number between blanks / commas */
    int value = 0, sign = 1;

    while (*s == ' ' || *s == ',')
        s++;
    while (*s == '-' || (*s >= '0' && *s <= '9')) {
        if (*s == '-')
            sign = -sign;
        else
            value = value * 10 + (*s - '0');
        s++;
    }
    if (out != NULL)
        *out = sign * value;
    while (*s == ' ' || *s == ',')
        s++;
    return s;
}

/* VA 0x00436840, 370 bytes, unassigned, status draft */
void guiDrawMarkup(const char *s, int16_t x, int16_t y)
{
    /* Element text that starts with '!' is markup (guiElementDraw), a run of commands:
     *   !B$text$     set the text of the element chosen by guiSetMarkupTarget (once)
     *   !S x,y,f     draw frame f of the element's sprite at (x, y) + the element origin
     *   !T x,y$text$ text with '~' for an apostrophe; parsed but not drawn on PC
     *   !!           nothing
     * Anything else ends the run. */
    char buf[200];
    char *d;
    int px, py, frame;
    GuiElement *e;

    while (*s == '!') {
        switch (s[1]) {
        case 'B':
        case 'b':
            if (s[2] != '$')
                return;
            for (s += 3, d = buf; *s != '$' && *s != '!' && *s != '\0'; s++)
                *d++ = *s;
            *d = '\0';
            if (*s != '$')
                return;
            s++;
            if (g_guiMarkupTarget != 0) {
                e = guiFindElement(g_guiMarkupTarget);
                if (e != NULL)
                    guiSetText(e, buf);
                g_guiMarkupTarget = 0;
            }
            break;
        case 'S':
        case 's':
            s = guiParseInt(s + 2, &px);
            s = guiParseInt(s, &py);
            s = guiParseInt(s, &frame);
            zprimDrawSprite((uint32_t)frame, (int16_t)(px + x), (int16_t)(py + y));
            break;
        case 'T':
        case 't':
            s = guiParseInt(s + 2, &px);
            s = guiParseInt(s, &py);
            if (*s != '$')
                return;
            for (s++, d = buf; *s != '$' && *s != '!' && *s != '\0'; s++)
                *d++ = (*s == '~') ? '\'' : *s;
            *d = '\0';
            if (*s != '$')
                return;
            s++;                /* the text is not drawn */
            break;
        case '!':
            s += 2;
            break;
        default:
            return;
        }
    }
}

/* VA 0x00436aa0, 10 bytes, unassigned, status draft */
void guiSetMarkupTarget(uint32_t id)
{
    g_guiMarkupTarget = id;
}

/* ---- the help system: stubs on PC ---- */

/* VA 0x00436ac0, 3 bytes, unassigned, status draft */
int guiHelpInit(void)
{
    return 0;           /* guiInitializeMenuMode: nonzero = "Failed to init help system" */
}

/* VA 0x00436ae0, 3 bytes, unassigned, status draft */
int guiNullHandler(void)
{
    return 0;           /* type handler of GUI_TYPE_NULL */
}

/* VA 0x00436b00, 3 bytes, unassigned, status draft */
int guiHelpActive(void)
{
    return 0;           /* guiUpdate: while help is up the pad goes to guiHelpInput */
}

/* VA 0x00436b20, 3 bytes, unassigned, status draft */
int guiHelpQuery(void)
{
    return 0;           /* not called */
}

/* VA 0x00436b40, 1 bytes, unassigned, status draft */
void guiHelpShutdown(void)
{
}

/* VA 0x00436b60, 1 bytes, unassigned, status draft */
void guiHelpReset(void)
{
    /* guiLoadScreenNow */
}

/* VA 0x00436b80, 1 bytes, unassigned, status draft */
void guiHelpStub(void)
{
}

/* VA 0x00436ba0, 1 bytes, unassigned, status draft */
void guiHelpInput(uint32_t msg)
{
    (void)msg;
}

/* VA 0x00436bc0, 1 bytes, unassigned, status draft */
void guiHelpUpdate(void)
{
}

/* VA 0x00436be0, 1 bytes, unassigned, status draft */
void guiHelpDraw(void)
{
}

/* VA 0x00436c00, 6 bytes, unassigned, status draft */
int guiHelpEnabled(void)
{
    return 1;           /* not called */
}
