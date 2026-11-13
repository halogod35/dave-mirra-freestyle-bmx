/*
 * NgEngine/Zge/ztoken.c
 * Original: unknown (no path string; name invented in the original style from the "ZTOKEN assert
 * failed" message)
 * Evidence: [I] new file. The functions share the globals 0x00675380-0x006763CC (between
 * zpanorama.c's 0x0067537C and zFont.c's 0x006763D0) and their literals 0x00563560-0x00563704 sit
 * between the "Panorama" literal and zFont.c's path string. The zScene pass named seven of them (ztokenOpen ..
 * ztokenPrintLocation, the zScene.c / zscnpts.c readers); the ZGE object pass created the eleven in the holes.
 * 18 functions, VA 0x004a1120-0x004a2615.
 * Draft. Non-matching C (AD-8). Types: ztools.h. Description: docs/re/zge.md 3.
 *
 * A whitespace / comma separated tokenizer for the engine's text scripts (Scene.Txt) with a small
 * preprocessor:
 *   ; and // comment to the end of the line, / * ... * / (as separate tokens) nest;
 *   "quoted strings" keep spaces and are never treated as keywords;
 *   tokens are upper-cased (unless ztokenSetCapitalize(0));
 *   #INCLUDE file, #INCLUDE_PATH dir, #DEFINE name value, $$name value (define), $$$file (include),
 *   $$~ (forget all symbols); #ifndef name and #endif are read and ignored (no conditionals);
 *   $name is replaced by its value; $* $+ $- $/ a b evaluate to the integer result ("%d"); numbers
 *   are decimal, 0x.. or $.. hex;
 *   with ztokenSetHashComments(1) a token starting with # skips its line.
 * Includes nest 3 deep (4 file records).
 */
#include "decomp.h"
#include "ztools.h"

int sscanf(const char *s, const char *fmt, ...);   /* CRT 0x004A6391 */

ZtokenFile  g_ztokenFiles[ZTOKEN_MAX_DEPTH];        /* 0x00675380 */
ZtokenFile *g_ztokenCur;                            /* 0x00675FB0 */
int         g_ztokenDepth;                          /* 0x00675FB4 0 = the main file */
int         g_ztokenVerbose;                        /* 0x00675FB8 */
int         g_ztokenCapitalize;                     /* 0x00675FBC */
int         g_ztokenHashComments;                   /* 0x00675FC0 */
int         g_ztokenPreprocess;                     /* 0x00675FC4 */
char        g_ztokenSymbols[ZTOKEN_SYMBOL_SPACE];   /* 0x00675FC8 "$NAME@value$NAME@value..." */
uint32_t    g_ztokenSymbolsUsed;                    /* 0x006763C8 */
int         g_ztokenQuoted;                         /* 0x006763CC */

#define IS_SEPARATOR(c) ((c) == ' ' || (c) == '\t' || (c) == '\r' || (c) == '\n' || (c) == ',')

/* Push a file record: copy the parent's (includePath is inherited), open "<includePath><name>". */
static int ztokenPushInclude(const char *name, char *path)
{
    ZtokenFile *parent = g_ztokenCur;

    g_ztokenCur++;
    g_ztokenDepth++;
    zstringMemCopy(g_ztokenCur, parent, sizeof(ZtokenFile));
    zstringPrintf(path, "%s%s", g_ztokenCur->includePath, name);
    g_ztokenCur->file = zfileOpen(path, 1, 0);
    if (g_ztokenCur->file == 0) {
        g_pfnDebugPrintf("Can't open include file: %s\n", path);
        g_ztokenDepth--;
        g_ztokenCur--;
        return 0;
    }
    g_ztokenCur->pos = 0;
    g_ztokenCur->line[0] = 0;
    g_ztokenCur->lineNo = 0;
    zstringCopy(g_ztokenCur->path, path);
    if (g_ztokenVerbose)
        g_pfnDebugPrintf("\n*** Opened File %s ***\n\n", path);
    return 1;
}

/* Next line of the current file; at the end of an include go back to the parent. 0 = end of all. */
static int ztokenNextLine(void)
{
    for (;;) {
        g_ztokenCur->lineNo++;
        g_ztokenCur->line[0] = 0;
        g_ztokenCur->pos = 0;
        if (zfileGets((zfileHandle *)g_ztokenCur->file, g_ztokenCur->line, 0xFF))
            return 1;
        if (g_ztokenDepth == 0)
            return 0;
        ztokenClose();                      /* inlined */
        g_ztokenDepth--;
        g_ztokenCur--;
        if (g_ztokenCur->line[g_ztokenCur->pos] != 0)
            return 1;                       /* continue the parent's line */
    }
}

/* A number token: 0x.. / 0X.. / $.. hex, else decimal. */
static int ztokenParseInt(const char *tok, int *value)
{
    if (zstringCompareN(tok, "0x", 2) == 0 || zstringCompareN(tok, "0X", 2) == 0)
        return sscanf(tok + 2, "%X", value);
    if (zstringCompareN(tok, "$", 1) == 0)
        return sscanf(tok + 1, "%X", value);
    return sscanf(tok, "%d", value);
}

/* VA 0x004a1120, 183 bytes, [I] row, status draft */
int ztokenOpen(char *path)
{
    g_ztokenDepth = 0;
    g_ztokenCur = &g_ztokenFiles[0];
    g_ztokenVerbose = 0;
    g_ztokenCapitalize = 1;
    g_ztokenHashComments = 0;
    g_ztokenPreprocess = 1;
    g_ztokenFiles[0].includePath[0] = 0;
    g_ztokenSymbolsUsed = 0;
    g_ztokenCur->file = zfileOpen(path, 1, 0);
    if (g_ztokenCur->file == 0)
        return 0;
    g_ztokenCur->pos = 0;
    g_ztokenCur->line[0] = 0;
    g_ztokenCur->lineNo = 0;
    zstringCopy(g_ztokenCur->path, path);
    if (g_ztokenVerbose)
        g_pfnDebugPrintf("\n*** Opened File %s ***\n\n", path);
    return 1;
}

/* VA 0x004a11e0, 63 bytes, [I] row, status draft */
int ztokenClose(void)
{
    g_ztokenCur->lineNo = -1;
    zfileClose((zfileHandle *)g_ztokenCur->file, 0);
    if (g_ztokenVerbose)
        g_pfnDebugPrintf("\n*** Closed File ***\n\n");
    return 1;
}

/* VA 0x004a1220, 3694 bytes, [I] row, status draft */
int ztokenNext(char *buf, int size)
{
    int commentDepth = 0, prevDepth;
    int includeNext = 0;                  /* the previous token was #INCLUDE */
    int quoted, n;
    char c, *p;
    char value[0x80];
    char path[0x100];
    int a, b;

    for (;;) {
        /* skip separators, reading lines as needed */
        for (;;) {
            while (IS_SEPARATOR(g_ztokenCur->line[g_ztokenCur->pos]))
                g_ztokenCur->pos++;
            if (g_ztokenCur->line[g_ztokenCur->pos] != 0)
                break;
            if (!ztokenNextLine())
                return 0;
        }

        /* one token */
        n = 0;
        quoted = g_ztokenCur->line[g_ztokenCur->pos] == '"';
        if (quoted) {
            g_ztokenQuoted |= 1;
            g_ztokenCur->pos++;
            while ((c = g_ztokenCur->line[g_ztokenCur->pos]) != '"' && c != 0) {
                buf[n++] = c;
                g_ztokenCur->pos++;
                if (n >= size - 1)
                    return 0;
            }
            if (g_ztokenCur->line[g_ztokenCur->pos] != '"')
                return 0;                   /* unterminated string */
            g_ztokenCur->pos++;
        } else {
            g_ztokenQuoted &= ~1;
            while (!IS_SEPARATOR(c = g_ztokenCur->line[g_ztokenCur->pos])) {
                /* [V] the exe has no NUL test here (lines end with a newline); the C stops at the NUL */
                if (c == 0)
                    break;
                buf[n++] = c;
                g_ztokenCur->pos++;
                if (n == size - 1)
                    return 0;
            }
        }
        buf[n] = 0;

        /* line comments */
        if (!quoted && (buf[0] == ';' || zstringCompareN(buf, "////", 2) == 0)) {
            if (!ztokenNextLine())
                return 0;
            continue;
        }
        if (g_ztokenHashComments && !quoted && buf[0] == '#') {
            g_ztokenCur->lineNo++;
            g_ztokenCur->line[0] = 0;
            if (!zfileGets((zfileHandle *)g_ztokenCur->file, g_ztokenCur->line, 0xFF))
                return 0;                   /* (no return to the parent file here) */
            g_ztokenCur->pos = 0;
            continue;
        }

        /* block comments; the delimiters must be tokens of their own */
        prevDepth = commentDepth;
        if (!quoted) {
            if (zstringCompare(buf, "/*") == 0)
                commentDepth++;
            else if (zstringCompare(buf, "*/") == 0)
                commentDepth--;
        }
        if (commentDepth > 0)
            continue;
        if (commentDepth < 0)
            return 0;
        if (prevDepth == 1)
            continue;                       /* the closing delimiter itself */

        if (g_ztokenCapitalize) {
            for (p = buf; *p; p++)
                if (*p >= 'a' && *p <= 'z')
                    *p -= 0x20;
        }
        if (g_ztokenVerbose) {
            g_pfnDebugPrintf("\t");
            g_pfnDebugPrintf(buf);
            g_pfnDebugPrintf(";");
        }

        if (zstringCompareNoCase(buf, "#INCLUDE") == 0 && g_ztokenPreprocess) {
            includeNext = 1;
            continue;
        }
        if (includeNext) {
            /* this token is the file name */
            if (g_ztokenDepth == 3) {
                g_pfnDebugPrintf("**ERROR** Maximum include nesting reached!\n");
            } else if (ztokenPushInclude(buf, path)) {
                includeNext = 0;
                continue;
            }
            buf[0] = 0;
            ztokenPrintLocation();          /* inlined */
            return 0;
        }

        if (zstringCompareNoCase(buf, "#INCLUDE_PATH") == 0) {
            if (g_ztokenPreprocess && !ztokenNext(g_ztokenCur->includePath, 0x100))
                g_ztokenCur->includePath[0] = 0;
            continue;
        }
        if (zstringCompareNoCase(buf, "#ifndef") == 0) {
            if (!ztokenNext(buf + 1, 0x80)) /* the symbol is read and ignored */
                return 0;
            continue;
        }
        if (zstringCompareNoCase(buf, "#endif") == 0)
            continue;
        if (zstringCompareNoCase(buf, "#DEFINE") == 0) {
            if (!ztokenNext(buf + 1, 0x80))
                return 0;
            buf[0] = '$';
            if (!ztokenNext(value, 0x80))
                return 0;
            if (g_ztokenPreprocess)
                ztokenDefine(buf, value);
            continue;
        }

        if (buf[0] != '$')
            break;                          /* an ordinary token */

        switch (buf[1]) {
        case '$':
            if (buf[2] == '~') {            /* $$~ forget every symbol */
                g_ztokenSymbolsUsed = 0;
            } else if (buf[2] == '$') {     /* $$$file include */
                if (g_ztokenDepth == 3) {
                    g_pfnDebugPrintf("**ERROR** Maximum include nesting reached!\n");
                } else if (ztokenPushInclude(buf + 3, path)) {
                    buf[0] = 0;
                    return 0;               /* [V] returns "no token" after a successful include */
                }
            } else {                        /* $$name value */
                ztokenNext(value, 0x80);
                ztokenDefine(buf + 1, value);
            }
            continue;
        case '*':
        case '+':
        case '-':
        case '/':
            if (ztokenNext(value, 0x80))
                ztokenParseInt(value, &a);
            if (ztokenNext(value, 0x80))
                ztokenParseInt(value, &b);
            switch (buf[1]) {
            case '*': a = a * b; break;
            case '+': a = a + b; break;
            case '-': a = a - b; break;
            default:  a = a / b; break;
            }
            zstringPrintf(buf, "%d", a);
            break;
        default:                            /* $name */
            if (!ztokenLookup(buf)) {
                g_pfnDebugPrintf("Symbol not found: '%s'\n", buf);
                ztokenNext(buf, size);
            }
            break;
        }
        break;
    }
    if (g_ztokenVerbose)
        g_pfnDebugPrintf("\n");
    return 1;
}

/* VA 0x004a20c0, 470 bytes, [I] row, status draft */
int ztokenDefine(const char *name, const char *value)
{
    char key[0x40];
    uint32_t at, i, end, oldLen, newLen;
    int n;

    for (n = 0; name[n]; n++)
        key[n] = name[n];
    key[n] = 0;
    at = ztokenLookup(key);
    if (at == 0) {
        /* append "$name@value"; a '$' or '@' inside either is refused as "out of space" */
        i = g_ztokenSymbolsUsed;
        if (i >= ZTOKEN_SYMBOL_SPACE)
            goto full;
        g_ztokenSymbols[i++] = '$';
        for (name++; *name; name++, i++) {
            if (i >= ZTOKEN_SYMBOL_SPACE || *name == '$' || *name == '@')
                goto full;
            g_ztokenSymbols[i] = *name;
        }
        if (i >= ZTOKEN_SYMBOL_SPACE)
            goto full;
        g_ztokenSymbols[i++] = '@';
        for (; *value; value++, i++) {
            if (i >= ZTOKEN_SYMBOL_SPACE || *value == '$' || *value == '@')
                goto full;
            g_ztokenSymbols[i] = *value;
        }
        if (i >= ZTOKEN_SYMBOL_SPACE)
            goto full;
        g_ztokenSymbolsUsed = i;
        return 1;
full:
        g_pfnDebugPrintf("Out of symbol space\n");
        return 0;
    }

    /* replace the value in place, moving the rest of the table */
    for (i = at; g_ztokenSymbols[i] != '@'; i++)
        ;
    i++;
    for (end = i; end < g_ztokenSymbolsUsed && g_ztokenSymbols[end] != '$'; end++)
        ;
    oldLen = end - i;
    newLen = zstringLength(value);
    if (newLen != oldLen) {
        zstringMemMove(&g_ztokenSymbols[i + newLen], &g_ztokenSymbols[end], g_ztokenSymbolsUsed - end);
        g_ztokenSymbolsUsed = g_ztokenSymbolsUsed - oldLen + newLen;
    }
    zstringMemCopy(&g_ztokenSymbols[i], value, newLen);
    g_pfnDebugPrintf("Replacing symbol:%s\n", name);
    return 1;
}

/* VA 0x004a22a0, 139 bytes, [I] row, status draft */
int ztokenLookup(char *name)
{
    uint32_t i, j;
    int n;

    for (i = 0; i < g_ztokenSymbolsUsed; i++) {
        if (g_ztokenSymbols[i] != '$')
            continue;
        for (n = 0, j = i; j < g_ztokenSymbolsUsed && g_ztokenSymbols[j] == name[n]; j++, n++)
            ;
        if (name[n] == 0 && g_ztokenSymbols[i + n] == '@') {
            /* found: overwrite name with the value (from after the '@' to the next '$') */
            j = i + n + 1;
            for (n = 0; j < g_ztokenSymbolsUsed && g_ztokenSymbols[j] != '$'; j++)
                name[n++] = g_ztokenSymbols[j];
            name[n] = 0;
            return (int)i + 1;
        }
    }
    return 0;
}

/* VA 0x004a2330, 186 bytes, [I] row, status draft */
int ztokenReadInt(int *value)
{
    char tok[0x80];

    if (!ztokenNext(tok, sizeof(tok)))
        return 0;
    return ztokenParseInt(tok, value) == 1;
}

/* VA 0x004a23f0, 74 bytes, [I] row, status draft */
int ztokenReadFloat(float *value)
{
    char tok[0x80];

    if (!ztokenNext(tok, sizeof(tok)))
        return 0;
    return sscanf(tok, "%f", value) == 1;
}

/* VA 0x004a2440, 25 bytes, [I] row, status draft */
int ztokenReadString(char *buf, int size)
{
    return ztokenNext(buf, size) != 0;
}

/* VA 0x004a2460, 126 bytes, [I] row, status draft */
int ztokenExpect(char *token)
{
    char tok[0x80];
    char *p;

    for (p = token; *p; p++)            /* upper-cases the caller's string in place */
        if (*p >= 'a' && *p <= 'z')
            *p -= 0x20;
    if (!ztokenNext(tok, sizeof(tok)))
        return 0;
    if (zstringCompare(tok, token) == 0)
        return 1;
    g_pfnDebugPrintf("ZTOKEN assert failed : found '%s', expecting '%s'\n", tok, token);
    return 0;
}

/* VA 0x004a24e0, 12 bytes, [I] row, status draft */
int ztokenGetLineNumber(void)
{
    return g_ztokenCur->lineNo;
}

/* VA 0x004a24f0, 93 bytes, [I] row, status draft */
void ztokenPrintLocation(void)
{
    int d;

    for (d = g_ztokenDepth; d >= 0; d--) {
        if (d != g_ztokenDepth)
            g_pfnDebugPrintf("Included from:");
        g_pfnDebugPrintf("File:%s,  Line:%d\n", g_ztokenFiles[d].path, g_ztokenFiles[d].lineNo);
    }
}

/* VA 0x004a2550, 22 bytes, [I] row, status draft */
char *ztokenGetLineRest(void)
{
    return &g_ztokenCur->line[g_ztokenCur->pos];
}

/* VA 0x004a2570, 23 bytes, [I] row, status draft */
void ztokenSkipLine(void)
{
    g_ztokenCur->line[g_ztokenCur->pos] = 0;
}

/* VA 0x004a2590, 10 bytes, [I] row, status draft */
void ztokenSetVerbose(int on)
{
    g_ztokenVerbose = on;
}

/* VA 0x004a25a0, 73 bytes, [I] row, status draft */
void ztokenSetCapitalize(int on)
{
    if (g_ztokenVerbose)
        g_pfnDebugPrintf(on ? "\n*** Set Capitalize TRUE ***\n\n" : "\n*** Set Capitalize FALSE ***\n\n");
    g_ztokenCapitalize = on;
}

/* VA 0x004a25f0, 10 bytes, [I] row, status draft */
void ztokenSetHashComments(int on)
{
    g_ztokenHashComments = on;
}

/* VA 0x004a2600, 10 bytes, [I] row, status draft */
void ztokenSetPreprocess(int on)
{
    g_ztokenPreprocess = on;
}

/* VA 0x004a2610, 6 bytes, [I] row, status draft */
int ztokenGetPreprocess(void)
{
    return g_ztokenPreprocess;
}
