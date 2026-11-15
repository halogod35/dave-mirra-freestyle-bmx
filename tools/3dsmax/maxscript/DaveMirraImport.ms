-- DaveMirraImport.ms : Dave Mirra Freestyle BMX importer for 3ds Max 2017+ (pure MAXScript, no Python)
-- Drag this file onto the 3ds Max viewport (or Scripting > Run Script...). A dialog opens with
--   a header: "Real-world size" (default; the scale from the scene's System Unit Setup, game units are metres),
--     Scale, LOD, "Exact normals" (the game's vertex normals, Edit_Normals modifier; slower), Textures folder
--   Models tab: Import ZMD...  riders, bikes, shadows, sfx (.zmd)
--               Animation: pick motion.zal, a motion, a start frame, and load it onto a model in the scene
--               (DM_ImportMotion; by default it starts after the last key, so motions chain back to back)
--   Levels tab: Import Level (.ZSD)...  a level folder: world, props, tags (hidden), scene points, panorama
-- Spec: ../../../docs/formats/SPEC.md. The parsing functions below mirror ../parser/dmparse.py line by line; each
-- one names its Python counterpart in a "mirrors dmparse.<function>" comment. Model data stays in game space; one root
-- Dummy carries the axis change (game Y-up -> Max Z-up) and the rider upright correction (SPEC 2.5).
-- Textures are written to a folder you choose, else %TEMP%\DaveMirraTextures\<model>; never into the game.
--
-- Note for maintainers: the code above the "UI" marker uses a small MAXScript subset (no line
-- continuations, every call argument on one line, negative numbers in parentheses) so that the test
-- suite can execute it with a MAXScript-subset interpreter (tests/maxscript_interp.py).

global DM_VERSION = "1.5"

-- ============================================================================ binary reading (dmparse.Buf)

fn DM_u8 f = (readByte f #unsigned)
fn DM_u16 f = (readShort f #unsigned)
fn DM_u32 f = (readLong f #unsigned)
fn DM_i32 f = (readLong f #signed)
fn DM_f32 f = (readFloat f)
fn DM_seek f pos = (fseek f pos #seek_set)

fn DM_readTag f =
(
    local s = ""
    for i = 1 to 4 do s += bit.intAsChar (readByte f #unsigned)
    s
)

-- read a NUL-terminated string from a fixed-size field (mirrors dmparse.cstr)
fn DM_cstr f n =
(
    local s = ""
    local done = false
    for i = 1 to n do
    (
        local c = readByte f #unsigned
        if c == 0 then done = true
        if not done then s += bit.intAsChar c
    )
    s
)

fn DM_fail msg = (throw ("Dave Mirra import: " + msg))

-- ============================================================================ name hash (dmparse.zhash)

fn DM_zhash name =
(
    local s = toUpper name
    local h = 0
    local last = 0
    for i = 1 to s.count do
    (
        local c = bit.charAsInt s[i]
        if c >= 128 then c -= 256
        h += i * c
        last = c
    )
    -- result as a signed 32-bit integer (MAXScript integers are 32-bit; bone ids are read signed too)
    h + (bit.shift last 16)
)

-- ============================================================================ chunk layers (dmparse.parse_4cc / parse_rw)

struct DM_Chunk (tag, ofs, size, version, children)

fn DM_isContainer4cc tag = (tag == "MODL" or tag == "MLOD" or tag == "MATI" or tag == "FORM" or tag == "SCEN" or tag == "TAGB" or tag == "PDBL" or tag == "SCPT" or tag == "SDPA")

fn DM_isContainerRW t = (t == 0x03 or t == 0x06 or t == 0x07 or t == 0x08 or t == 0x09 or t == 0x0A or t == 0x0B or t == 0x0E or t == 0x0F or t == 0x10 or t == 0x14)

-- mirrors dmparse.parse_4cc
fn DM_parse4cc f start end depth =
(
    if depth > 16 then DM_fail "chunk nesting too deep"
    local out = #()
    local p = start
    while p < end do
    (
        if p + 8 > end then DM_fail "truncated chunk header"
        DM_seek f p
        local tag = DM_readTag f
        local size = DM_u32 f
        if size > end - p - 8 then DM_fail ("chunk " + tag + " runs past its parent")
        local c = DM_Chunk tag:tag ofs:p size:size version:undefined children:#()
        -- a top-level PRBL is a form; the PRBL inside it is the record (SPEC 5.5)
        if (DM_isContainer4cc tag) or (tag == "PRBL" and depth == 0) then c.children = DM_parse4cc f (p + 8) (p + 8 + size) (depth + 1)
        append out c
        p = p + 8 + size
    )
    out
)

-- mirrors dmparse.parse_rw
fn DM_parseRW f start end depth =
(
    if depth > 64 then DM_fail "RW nesting too deep"
    local out = #()
    local p = start
    while p < end do
    (
        if p + 12 > end then DM_fail "truncated RW header"
        DM_seek f p
        local t = DM_u32 f
        local size = DM_u32 f
        local ver = DM_u32 f
        if size > end - p - 12 then DM_fail "RW chunk runs past its parent"
        local c = DM_Chunk tag:t ofs:p size:size version:ver children:#()
        if DM_isContainerRW t then c.children = DM_parseRW f (p + 12) (p + 12 + size) (depth + 1)
        append out c
        p = p + 12 + size
    )
    out
)

fn DM_kid c tag =
(
    local r = undefined
    for k in c.children do (if r == undefined and k.tag == tag then r = k)
    r
)

-- ============================================================================ ZMD (dmparse.read_zmd)

struct DM_Frame (right, up, fwd, pos, parent, boneId)
struct DM_Geometry (flags, numVertices, positions, normals, uvs, colors, triangles, slots, sphere, bones)
struct DM_Atomic (frame, flags, geometry)
struct DM_Lod (mldf, rwVersion, frames, atomics)
struct DM_Mdef (kind, name, nameId, entries)
struct DM_Texture (name, nameId, kind, dataOff, dataLen)
struct DM_Model (lods, mdef, textures, empty)

-- reads are done one per statement (never inside a literal) so the byte order never depends on
-- MAXScript's evaluation order of literal elements
fn DM_readP3 f =
(
    local x = DM_f32 f
    local y = DM_f32 f
    local z = DM_f32 f
    [x, y, z]
)

fn DM_read4u8 f =
(
    local a = DM_u8 f
    local b = DM_u8 f
    local c = DM_u8 f
    local d = DM_u8 f
    #(a, b, c, d)
)

-- mirrors dmparse._read_framelist
fn DM_readFrameList f fl =
(
    local st = fl.children[1]
    DM_seek f (st.ofs + 12)
    local n = DM_u32 f
    local frames = #()
    for i = 1 to n do
    (
        local r = DM_readP3 f
        local u = DM_readP3 f
        local a = DM_readP3 f
        local p = DM_readP3 f
        local parent = DM_i32 f
        DM_u32 f
        append frames (DM_Frame right:r up:u fwd:a pos:p parent:parent boneId:undefined)
    )
    for i = 1 to n do
    (
        local ext = fl.children[i + 1]
        for e in ext.children do
        (
            if e.tag == 0x108 and e.size == 8 then
            (
                DM_seek f (e.ofs + 12)
                frames[i].boneId = DM_i32 f
            )
        )
    )
    frames
)

-- mirrors dmparse._read_material
fn DM_readMaterial f m =
(
    local st = m.children[1]
    DM_seek f (st.ofs + 12 + 4)
    local col = DM_read4u8 f
    local tex = undefined
    for i = 2 to m.children.count do
    (
        local k = m.children[i]
        if k.tag == 0x06 then
        (
            local s = k.children[2]
            DM_seek f (s.ofs + 12)
            tex = DM_cstr f s.size
        )
    )
    #(col, tex)
)

-- mirrors dmparse._read_matlist: one #(color, texture) per slot
fn DM_readMatList f ml =
(
    local mst = ml.children[1]
    DM_seek f (mst.ofs + 12)
    local n = DM_u32 f
    local idx = #()
    for i = 1 to n do append idx (DM_i32 f)
    local mats = #()
    for i = 2 to ml.children.count do append mats (DM_readMaterial f ml.children[i])
    local slots = #()
    local nextMat = 1
    for i in idx do
    (
        if i < 0 then
        (
            append slots mats[nextMat]
            nextMat += 1
        ) else (
            if i >= slots.count then DM_fail "material list: bad reuse index"
            append slots slots[i + 1]
        )
    )
    slots
)

-- mirrors dmparse._read_geometry
fn DM_readGeometry f geo =
(
    local st = geo.children[1]
    local ml = geo.children[2]
    local ex = geo.children[3]
    DM_seek f (st.ofs + 12)
    local flags = DM_u32 f
    local nt = DM_u32 f
    local nv = DM_u32 f
    local nm = DM_u32 f
    DM_seek f (st.ofs + 12 + 28)
    local g = DM_Geometry flags:flags numVertices:nv positions:#() normals:undefined uvs:#() colors:undefined triangles:#() slots:#() sphere:undefined bones:undefined
    if (bit.and flags 0x08) != 0 then
    (
        g.colors = #()
        for i = 1 to nv do append g.colors (DM_read4u8 f)
    )
    local nuv = 0
    if (bit.and flags 0x04) != 0 then nuv = 1
    if (bit.and flags 0x80) != 0 then nuv = 2
    for s = 1 to nuv do
    (
        local uv = #()
        for i = 1 to nv do
        (
            local u = DM_f32 f
            local v = DM_f32 f
            append uv #(u, v)
        )
        append g.uvs uv
    )
    for i = 1 to nt do
    (
        local v0 = DM_u16 f
        local v1 = DM_u16 f
        local mat = DM_u16 f
        local v2 = DM_u16 f
        if v0 >= nv or v1 >= nv or v2 >= nv then DM_fail "triangle index out of range"
        append g.triangles #(v1, v0, v2, mat)
    )
    if nm < 1 then DM_fail "geometry without morph target"
    local sp = DM_readP3 f
    local sr = DM_f32 f
    g.sphere = #(sp.x, sp.y, sp.z, sr)
    local hasV = DM_u32 f
    local hasN = DM_u32 f
    if hasV != 0 then (for i = 1 to nv do append g.positions (DM_readP3 f))
    if hasN != 0 then
    (
        g.normals = #()
        for i = 1 to nv do append g.normals (DM_readP3 f)
    )
    g.slots = DM_readMatList f ml
    for e in ex.children do
    (
        if e.tag == 0x10E then
        (
            g.bones = #()
            DM_seek f (e.ofs + 12)
            for k = 1 to (e.size / 12) do
            (
                local b1 = DM_u32 f
                local b2 = DM_u32 f
                local b3 = DM_i32 f
                append g.bones #(b1, b2, b3)
            )
        )
    )
    g
)

-- mirrors dmparse._read_atomic
fn DM_readAtomic f a =
(
    local st = a.children[1]
    DM_seek f (st.ofs + 12)
    local fr = DM_u32 f
    local fl = DM_u32 f
    DM_Atomic frame:fr flags:fl geometry:(DM_readGeometry f a.children[2])
)

-- mirrors dmparse._read_mlod
fn DM_readMlod f c =
(
    local mldf = DM_kid c "MLDF"
    local mdff = DM_kid c "MDFF"
    DM_seek f (mldf.ofs + 8)
    local m1 = DM_u32 f
    local m2 = DM_u32 f
    local clump = (DM_parseRW f (mdff.ofs + 8) (mdff.ofs + 8 + mdff.size) 0)[1]
    if clump.tag != 0x10 then DM_fail "MDFF does not hold a Clump"
    local lod = DM_Lod mldf:#(m1, m2) rwVersion:clump.version frames:(DM_readFrameList f clump.children[2]) atomics:#()
    for i = 3 to clump.children.count do
    (
        local a = clump.children[i]
        if a.tag == 0x14 then append lod.atomics (DM_readAtomic f a)
    )
    lod
)

-- mirrors dmparse._read_mdef
fn DM_readMdef f c =
(
    DM_seek f (c.ofs + 8)
    local kind = DM_u32 f
    local name = DM_cstr f 16
    local nameId = DM_u32 f
    DM_seek f (c.ofs + 8 + 40)
    local count = DM_u16 f
    DM_u16 f
    local entries = #()
    for i = 1 to count do
    (
        local n = DM_cstr f 16
        local id = DM_i32 f
        append entries #(n, id)
    )
    DM_Mdef kind:kind name:name nameId:nameId entries:entries
)

-- mirrors dmparse._image_length: BMP = bfSize; PNG = up to IEND + CRC
fn DM_imageLength f base size kind =
(
    local n = size
    if kind == "bmp" then
    (
        DM_seek f (base + 2)
        local b = DM_u32 f
        if b <= size then n = b
    )
    if kind == "png" then
    (
        local i = size - 8
        local found = false
        while i >= 4 and not found do
        (
            DM_seek f (base + i)
            if (DM_readTag f) == "IEND" then found = true else i -= 1
        )
        if found then n = i + 8
    )
    n
)

-- mirrors dmparse._read_mati_chunk (the image stays in the file: offset + length)
fn DM_readMati f c =
(
    local madf = DM_kid c "MADF"
    local img = undefined
    for k in c.children do (if k.tag == "TBMP" or k.tag == "TPNG" or k.tag == "TDDS" then img = k)
    DM_seek f (madf.ofs + 8)
    if (DM_u32 f) != 0x48 then DM_fail "MADF: bad header"
    DM_seek f (madf.ofs + 8 + 68)
    local nameId = DM_u32 f
    local name = DM_cstr f (madf.size - 72)
    local kind = "dds"
    if img.tag == "TBMP" then kind = "bmp"
    if img.tag == "TPNG" then kind = "png"
    local dataOfs = img.ofs + 8
    local n = DM_imageLength f dataOfs img.size kind
    DM_Texture name:name nameId:nameId kind:kind dataOff:dataOfs dataLen:n
)

-- a MODL chunk of an open file -> model (mirrors the body of dmparse.read_zmd)
fn DM_readModl f modl =
(
    local z = DM_Model lods:#() mdef:undefined textures:#() empty:false
    for c in modl.children do
    (
        if c.tag == "MLOD" then append z.lods (DM_readMlod f c)
        if c.tag == "MDEF" then z.mdef = DM_readMdef f c
        if c.tag == "MATI" then append z.textures (DM_readMati f c)
    )
    z
)

-- mirrors dmparse.read_zmd
fn DM_readZmd path =
(
    local size = getFileSize path
    if size == 0 then return (DM_Model lods:#() mdef:undefined textures:#() empty:true)
    local f = fopen path "rb"
    if f == undefined then DM_fail ("cannot open " + path)
    if (DM_readTag f) != "MODL" then (fclose f; DM_fail "not a ZMD (no MODL)")
    local z = DM_readModl f (DM_parse4cc f 0 size 0)[1]
    fclose f
    z
)

-- ============================================================================ skeleton (dmparse.skeleton / vertex_frames)

global DM_KNOWN_NAMES = #("Skin", "Hips", "_Abdomen", "Chest", "Chestx", "Shoulders", "_LeftShoulder", "_RightShoulder", "Head", "LeftUpArm", "LeftLowArm", "LeftHand", "RightUpArm", "RightLowArm", "RightHand", "LeftUpLeg", "LeftLowLeg", "LeftFoot", "RightUpLeg", "RightLowLeg", "RightFoot", "Hipsdeck", "BackWheel", "FrontWheel", "Sprocket", "Handlebars", "SeatGrip", "LeftGrip", "RightGrip", "LeftPedal", "RightPedal", "SFXCube", "Cone01", "wheel")

fn DM_hex8 v =
(
    local digits = "0123456789ABCDEF"
    local s = ""
    for i = 7 to 0 by (-1) do s += digits[(bit.and (bit.shift v (-4 * i)) 15) + 1]
    s
)

-- frame names, mirrors dmparse.skeleton naming
fn DM_frameNames z lod =
(
    local lo = z.lods[lod + 1]
    local names = #()
    for i = 1 to lo.frames.count do
    (
        local bid = lo.frames[i].boneId
        local nm = undefined
        if bid == undefined or bid == (-1) then nm = "frame_" + ((i - 1) as string)
        -- MDEF lookup: the LAST entry with this id wins (bike_hires lists wheel2 and wheel1 with one id)
        if nm == undefined and z.mdef != undefined then (for e in z.mdef.entries do (if e[2] == bid then nm = e[1]))
        if nm == undefined then (for k in DM_KNOWN_NAMES do (if nm == undefined and (DM_zhash k) == bid then nm = k))
        if nm == undefined then nm = "bone_0x" + (DM_hex8 bid)
        append names nm
    )
    names
)

-- frame index per vertex for a skinned atomic, mirrors dmparse.vertex_frames
fn DM_vertexFrames z lod ai =
(
    local lo = z.lods[lod + 1]
    local a = lo.atomics[ai + 1]
    local g = a.geometry
    if g.bones == undefined then return undefined
    local out = #()
    for v = 1 to g.numVertices do append out a.frame
    for b in g.bones do
    (
        local fi = undefined
        for i = 1 to lo.frames.count do (if fi == undefined and lo.frames[i].boneId == b[3] then fi = i - 1)
        if b[2] > 0 and fi != undefined then
        (
            local last = b[1] + b[2]
            if last > g.numVertices then last = g.numVertices
            for v = b[1] + 1 to last do out[v] = fi
        )
    )
    out
)

-- local frame matrix (row vectors, like RenderWare), translation scaled
fn DM_frameMatrix fr scale = (matrix3 fr.right fr.up fr.fwd (fr.pos * scale))

-- world matrix of frame i (1-based) of a LOD: local * parent * ... (row vectors, unscaled)
fn DM_frameWorld lo i =
(
    local w = DM_frameMatrix lo.frames[i] 1.0
    local p = lo.frames[i].parent
    while p >= 0 do
    (
        w = w * (DM_frameMatrix lo.frames[p + 1] 1.0)
        p = lo.frames[p + 1].parent
    )
    w
)

-- mirrors dmparse.upright_correction: "rx-90" when a skinned skeleton lies along +Z
fn DM_upright z lod =
(
    local lo = z.lods[lod + 1]
    local skinned = false
    for a in lo.atomics do (if a.geometry.bones != undefined then skinned = true)
    if not skinned then return undefined
    local root = 0
    for i = 1 to lo.frames.count do (if root == 0 and lo.frames[i].parent < 0 then root = i)
    local child = 0
    for i = 1 to lo.frames.count do (if child == 0 and lo.frames[i].parent == root - 1 then child = i)
    if child == 0 then return undefined
    local w = DM_frameWorld lo child
    local upz = w.row2.z
    if upz > 0.9 then return "rx-90"
    if upz < (-0.9) then return "rx+90"
    undefined
)

-- root matrix: [upright] * (game Y-up -> Max Z-up)   (row vectors; mirrors dave_mirra_import.root_transform)
fn DM_rootMatrix z lod upright =
(
    local toMax = matrix3 [1,0,0] [0,0,1] [0,(-1),0] [0,0,0]
    local corr = undefined
    if upright then corr = DM_upright z lod
    if corr == "rx-90" then return ((matrix3 [1,0,0] [0,0,(-1)] [0,1,0] [0,0,0]) * toMax)
    if corr == "rx+90" then return ((matrix3 [1,0,0] [0,0,1] [0,(-1),0] [0,0,0]) * toMax)
    toMax
)

-- ============================================================================ bones aimed at their children

global DM_BONE_MIN = 0.02        -- shortest bone, game units (metres): 2 cm
global DM_BONE_DEFAULT = 0.05    -- a lone bone, and the "game frame orientation" mode

-- per frame #(O, length): O = a constant rotation (matrix3, no translation) in the frame's own space such that
-- the bone transform O * frameWorld has its X axis at the frame's main child; length in game units.
-- world: the frames' world matrices (any uniform scale). Mirrors dave_mirra_import.bone_aim:
-- main child = among the children not at the joint (> 1e-6), the one that best continues the incoming
-- direction (parent joint -> joint, largest cosine); a bone whose parent is a root takes the child with the
-- most descendants instead; ties: the first in file order. A leaf continues its parent's direction at half
-- its length. Lengths are at least DM_BONE_MIN. Z = the local Z made perpendicular to X (local Y when X is
-- within ~25 degrees of Z); Y = Z x X.
fn DM_boneAim lo world =
(
    local n = lo.frames.count
    local kids = #()
    local desc = #()
    for i = 1 to n do
    (
        append kids #()
        append desc 0
    )
    for i = 1 to n do
    (
        local par = lo.frames[i].parent
        if par >= 0 and par < i - 1 then append kids[par + 1] i
    )
    for i = n to 1 by (-1) do
    (
        for c in kids[i] do desc[i] += 1 + desc[c]
    )
    local out = #()
    local aimWorld = #()
    for i = 1 to n do
    (
        local w = world[i]
        local par = 0
        if lo.frames[i].parent >= 0 and lo.frames[i].parent < i - 1 then par = lo.frames[i].parent + 1
        local valid = #()
        for c in kids[i] do (if (length lo.frames[c].pos) > 1e-6 then append valid c)
        local x = [1, 0, 0]
        local len = DM_BONE_DEFAULT
        if valid.count > 0 then
        (
            local best = valid[1]
            local useInc = false
            local inc = [0, 0, 0]
            if par > 0 then
            (
                local gp = lo.frames[par].parent
                inc = w.row4 - world[par].row4
                if gp >= 0 and gp < par - 1 and (length inc) > 1e-9 then useInc = true
            )
            if useInc then
            (
                inc = normalize inc
                local score = undefined
                for c in valid do
                (
                    local sc = dot (normalize (world[c].row4 - w.row4)) inc
                    if score == undefined or sc > score then
                    (
                        best = c
                        score = sc
                    )
                )
            ) else (
                for c in valid do (if desc[c] > desc[best] then best = c)
            )
            len = length lo.frames[best].pos
            x = normalize lo.frames[best].pos
        ) else (
            if par > 0 then
            (
                local dw = aimWorld[par]
                local d1 = dot dw w.row1
                local d2 = dot dw w.row2
                local d3 = dot dw w.row3
                x = normalize [d1, d2, d3]
                len = 0.5 * out[par][2]
            )
        )
        if len < DM_BONE_MIN then len = DM_BONE_MIN
        local hint = [0, 0, 1]
        if (abs x.z) > 0.9 then hint = [0, 1, 0]
        local z = normalize (hint - x * (dot hint x))
        local y = cross z x
        append out #((matrix3 x y z [0, 0, 0]), len)
        append aimWorld (normalize (x.x * w.row1 + x.y * w.row2 + x.z * w.row3))
    )
    out
)

-- ============================================================================ byte arrays (motion.zal is read whole)

-- The whole file as a MAXScript array of byte values 0..255. BinaryReader.ReadBytes returns a .NET
-- byte[], and without asDotNetObject:true MAXScript converts it to an array of integers in one call
-- (much faster than 140,000 readByte calls).
fn DM_readAllBytes path =
(
    local size = getFileSize path
    local fs = dotNetObject "System.IO.FileStream" path (dotNetClass "System.IO.FileMode").Open (dotNetClass "System.IO.FileAccess").Read
    local br = dotNetObject "System.IO.BinaryReader" fs
    local d = br.ReadBytes size
    br.Close()  -- also closes fs
    if d == undefined or d.count != size then DM_fail ("could not read " + path)
    d
)

-- little-endian reads from a byte array at a 0-based offset p (mirrors dmparse.Buf)
fn DM_bu16 d p = (d[p + 1] + 256 * d[p + 2])

fn DM_bs16 d p =
(
    local v = d[p + 1] + 256 * d[p + 2]
    if v >= 32768 then v -= 65536
    v
)

fn DM_bs8 d p =
(
    local v = d[p + 1]
    if v >= 128 then v -= 256
    v
)

-- u32 as a signed 32-bit MAXScript integer (like DM_i32): exact for values below 0x80000000
fn DM_b32 d p = (bit.or (d[p + 1] + 256 * d[p + 2]) (bit.shift (d[p + 3] + 256 * d[p + 4]) 16))

-- ============================================================================ LZO1X (dmparse.lzo1x_decompress, SPEC 6)

-- run length extension: t == 0 -> 255 per zero byte, then base + next byte; returns #(t, ip)
fn DM_lzoRun src ip lim t base =
(
    if t == 0 then
    (
        local more = true
        while more do
        (
            if ip + 1 > lim then DM_fail "LZO: input overrun"
            if src[ip + 1] != 0 then more = false else
            (
                t += 255
                ip += 1
            )
        )
        t += base + src[ip + 1]
        ip += 1
    )
    #(t, ip)
)

-- mirrors dmparse.lzo1x_decompress: decode the n bytes of src (a byte array) at 0-based offset start;
-- returns outLen bytes. ip is a 0-based offset (src[ip + 1] is the byte at ip). The states are those
-- of the Python code: 0 loop, 1 first (after a literal run), 2 match, 3 done (after a match).
-- Integer shifts are written as * and / (all values are non-negative).
fn DM_lzo1x src start n outLen =
(
    local out = #()
    local lim = start + n
    local ip = start
    local t = 0
    local state = 0
    local finished = false
    local dist = 0
    local len = 0
    local s = 0
    local r = undefined
    if ip + 1 > lim then DM_fail "LZO: input overrun"
    if src[ip + 1] > 17 then
    (
        t = src[ip + 1] - 17
        ip += 1
        if ip + t > lim then DM_fail "LZO: input overrun"
        for k = 1 to t do append out src[ip + k]
        ip += t
        state = 1
        if t < 4 then
        (
            if ip + 1 > lim then DM_fail "LZO: input overrun"
            t = src[ip + 1]
            ip += 1
            state = 2
        )
    )
    while not finished do
    (
        if state == 0 then
        (
            if ip + 1 > lim then DM_fail "LZO: input overrun"
            t = src[ip + 1]
            ip += 1
            if t >= 16 then state = 2 else
            (
                r = DM_lzoRun src ip lim t 15
                t = r[1]
                ip = r[2]
                if ip + t + 3 > lim then DM_fail "LZO: input overrun"
                for k = 1 to t + 3 do append out src[ip + k]
                ip += t + 3
                state = 1
            )
        )
        if state == 1 then
        (
            if ip + 1 > lim then DM_fail "LZO: input overrun"
            t = src[ip + 1]
            ip += 1
            if t < 16 then
            (
                if ip + 1 > lim then DM_fail "LZO: input overrun"
                dist = 1 + 2048 + t / 4 + 4 * src[ip + 1]
                ip += 1
                s = out.count - dist
                if s < 0 then DM_fail "LZO: lookbehind overrun"
                for k = 1 to 3 do append out out[s + k]
                state = 3
            ) else state = 2
        )
        if state == 2 then
        (
            len = 0
            if t >= 64 then
            (
                if ip + 1 > lim then DM_fail "LZO: input overrun"
                dist = 1 + (bit.and (t / 4) 7) + 8 * src[ip + 1]
                ip += 1
                len = t / 32 + 1
            ) else if t >= 32 then
            (
                r = DM_lzoRun src ip lim (bit.and t 31) 31
                ip = r[2]
                if ip + 2 > lim then DM_fail "LZO: input overrun"
                dist = 1 + src[ip + 1] / 4 + 64 * src[ip + 2]
                ip += 2
                len = r[1] + 2
            ) else if t >= 16 then
            (
                local hi = 2048 * (bit.and t 8)
                r = DM_lzoRun src ip lim (bit.and t 7) 7
                ip = r[2]
                if ip + 2 > lim then DM_fail "LZO: input overrun"
                dist = hi + src[ip + 1] / 4 + 64 * src[ip + 2]
                ip += 2
                if dist == 0 then
                (
                    -- end of stream
                    if out.count != outLen then DM_fail ("LZO: decoded " + (out.count as string) + " bytes, expected " + (outLen as string))
                    finished = true
                ) else (
                    dist += 16384
                    len = r[1] + 2
                )
            ) else (
                if ip + 1 > lim then DM_fail "LZO: input overrun"
                dist = 1 + t / 4 + 4 * src[ip + 1]
                ip += 1
                len = 2
            )
            if not finished then
            (
                s = out.count - dist
                if s < 0 then DM_fail "LZO: lookbehind overrun"
                -- the match may overlap its own output: copy byte by byte
                for k = 1 to len do append out out[s + k]
                state = 3
            )
        )
        if state == 3 and not finished then
        (
            t = bit.and src[ip - 1] 3
            if t == 0 then state = 0 else
            (
                if ip + t + 1 > lim then DM_fail "LZO: input overrun"
                for k = 1 to t do append out src[ip + k]
                ip += t
                t = src[ip + 1]
                ip += 1
                state = 2
            )
        )
        if out.count > outLen then DM_fail "LZO: output overrun"
    )
    out
)

-- ============================================================================ ZAL (dmparse.read_zal, SPEC 6)

-- one asset: its bytes are data[start + 1 .. start + len]
struct DM_ZalGroup (id, size, data, start, len)

-- mirrors dmparse.read_zal for the n bytes of byte array d at 0-based offset start. headerless: the
-- in-memory form (motion bank) without magic + 12 reserved bytes. Compressed assets are decoded.
fn DM_readZalBytes d start n headerless =
(
    local lim = start + n
    local p = start
    local base = start + 16
    if headerless then base = start else
    (
        if n < 20 or (DM_b32 d p) != 0x2A81511C then DM_fail "not a ZAL (bad magic)"
        p += 16
    )
    if p + 4 > lim then DM_fail "ZAL: truncated header"
    local flags = DM_bu16 d p
    local count = DM_bu16 d (p + 2)
    p += 4
    local per = bit.and flags 0x1FFF
    local ngroups = bit.and count 0x3FFF
    local comp = 2 * (flags / 32768) + count / 32768
    local codec = 1
    if (bit.and count 0x4000) != 0 then codec = 2
    local recLen = 8
    if comp != 0 then recLen = 12
    local groups = #()
    for g = 1 to ngroups do
    (
        if p + 4 > lim then DM_fail "ZAL: truncated group table"
        local gid = DM_b32 d p
        p += 4
        for a = 1 to per do
        (
            if p + recLen > lim then DM_fail "ZAL: truncated group table"
            local ofs = DM_b32 d p
            local size = bit.and (DM_b32 d (p + 4)) 0xFFFFFF
            local stored = size
            if comp != 0 then stored = DM_b32 d (p + 8)
            p += recLen
            local q = base + ofs
            if ofs < 0 or stored < 0 or q + stored > lim then DM_fail ("ZAL asset " + (gid as string) + ": data past end of file")
            if comp != 0 then
            (
                if codec != 2 then DM_fail "ZAL codec 1 is not supported (unused by the game files)"
                append groups (DM_ZalGroup id:gid size:size data:(DM_lzo1x d q stored size) start:0 len:size)
            ) else (
                append groups (DM_ZalGroup id:gid size:size data:d start:q len:stored)
            )
        )
    )
    groups
)

-- ============================================================================ motion bank (dmparse.read_motion_bank, SPEC 8)

global DM_RIDER_CHANNELS = #("Hips", "Chest", "Head", "LeftUpArm", "LeftLowArm", "LeftHand", "RightUpArm", "RightLowArm", "RightHand", "LeftUpLeg", "LeftLowLeg", "LeftFoot", "RightUpLeg", "RightLowLeg", "RightFoot")
global DM_BIKE_CHANNELS = #("Hipsdeck", "Handlebars", "Sprocket", "RightPedal", "LeftPedal", "BackWheel", "FrontWheel", "LeftGrip", "RightGrip", "SeatGrip")

-- sections: #(channels, flags, kind) each; rots: one #(x, y, z) per channel, undefined = not in the record.
-- (dmparse's record 'value' float is skipped: no tool uses it.)
struct DM_Clip (frames, frameMs, wide, sections, inits, records, endTime)
struct DM_MotionRec (section, time, trans, rots)
struct DM_Pose (time, rider, bike, riderTrans, bikeTrans)

-- three values: u8 (8-bit init), s8 (8-bit delta) or s16 (16-bit) at p
fn DM_motionVec d p wide init =
(
    local v = #()
    for i = 0 to 2 do
    (
        if wide then append v (DM_bs16 d (p + 2 * i)) else
        (
            if init then append v d[p + i + 1] else append v (DM_bs8 d (p + i))
        )
    )
    v
)

-- one record at p (the generic layout, the nested rec() of dmparse.read_motion); returns #(record, next p)
fn DM_readMotionRecord d p lim init si sections wide =
(
    local start = p
    if p + 6 > lim then DM_fail "motion: truncated record"
    local mask = DM_b32 d p
    local t = DM_bu16 d (p + 4)
    p += 6
    if (bit.and mask 1) != 0 then p += 6
    local trans = undefined
    if (bit.and mask 2) != 0 then
    (
        trans = #(DM_bs16 d p, DM_bs16 d (p + 2), DM_bs16 d (p + 4))
        p += 6
    )
    local cnt = sections[si][1]
    local vsize = 3
    if wide then vsize = 6
    local rots = #()
    for i = 1 to cnt do append rots undefined
    for i = 1 to cnt do
    (
        -- init records: mask bit 2 means every channel; deltas: bit 2 + i is channel i
        local bitv = 4
        if not init then bitv = bit.shift 4 (i - 1)
        if (bit.and mask bitv) != 0 then
        (
            rots[i] = DM_motionVec d p wide init
            p += vsize
        )
    )
    p += 3 - (bit.and (p - start + 3) 3)
    if p > lim then DM_fail "motion: truncated record"
    local rec = DM_MotionRec section:si time:t trans:trans rots:rots
    #(rec, p)
)

-- mirrors dmparse.read_motion: one clip, the n bytes of byte array d at 0-based offset start
fn DM_readMotion d start n =
(
    local lim = start + n
    if n < 12 then DM_fail "motion: truncated clip"
    local fw = DM_bu16 d start
    local flags = DM_bu16 d (start + 2)
    local clip = DM_Clip frames:(bit.and fw 0x7FFF) frameMs:(DM_b32 d (start + 4)) wide:((bit.and flags 0x8000) != 0) sections:#() inits:#() records:#() endTime:undefined
    local p = start + 12
    for i = 1 to (bit.and flags 0xFF) / 16 do
    (
        if p + 4 > lim then DM_fail "motion: truncated clip"
        append clip.sections #(d[p + 1], d[p + 2], DM_bu16 d (p + 2))
        p += 4
    )
    if (bit.and fw 0x8000) != 0 then p += 100
    local vsize = 3
    if clip.wide then vsize = 6
    local r = undefined
    for si = 1 to clip.sections.count do
    (
        local cnt = clip.sections[si][1]
        if clip.sections[si][3] == 0 then
        (
            -- fixed rider init record: mask, time, h, value, trans, then every channel
            local start2 = p
            if p + 18 + cnt * vsize > lim then DM_fail "motion: truncated clip"
            local t = DM_bu16 d (p + 4)
            local trans = #(DM_bs16 d (p + 12), DM_bs16 d (p + 14), DM_bs16 d (p + 16))
            p += 18
            local rots = #()
            for i = 1 to cnt do
            (
                append rots (DM_motionVec d p clip.wide true)
                p += vsize
            )
            p += 3 - (bit.and (p - start2 + 3) 3)
            append clip.inits (DM_MotionRec section:si time:t trans:trans rots:rots)
        ) else if (bit.and clip.sections[si][2] 2) != 0 then (
            append clip.inits undefined
        ) else (
            r = DM_readMotionRecord d p lim true si clip.sections clip.wide
            append clip.inits r[1]
            p = r[2]
        )
    )
    local more = true
    while more do
    (
        if p + 4 > lim then DM_fail "motion: missing terminator"
        local mask = DM_b32 d p
        if mask == 0 then
        (
            if p + 8 > lim then DM_fail "motion: missing terminator"
            clip.endTime = DM_b32 d (p + 4)
            more = false
        ) else (
            local si = (bit.and (bit.shift mask (-28)) 15) + 1
            if si > clip.sections.count then DM_fail "motion: record for a missing section"
            r = DM_readMotionRecord d p lim false si clip.sections clip.wide
            append clip.records r[1]
            p = r[2]
        )
    )
    clip
)

-- mirrors dmparse.read_motion_bank: #(ids, clips), every clip decoded
fn DM_readMotionBank path =
(
    local outer = DM_readZalBytes (DM_readAllBytes path) 0 (getFileSize path) false
    if outer.count < 1 then DM_fail "motion.zal has no groups"
    local g = outer[1]
    local inner = DM_readZalBytes g.data g.start g.len true
    local ids = #()
    local clips = #()
    for c in inner do
    (
        append ids c.id
        append clips (DM_readMotion c.data c.start c.len)
    )
    #(ids, clips)
)

-- motion.zal decoded once per 3ds Max session: #(path, size, clip groups)
global DM_motionCache = undefined

-- the clip groups of motion.zal (decompressed, not yet decoded), cached
fn DM_motionGroups path =
(
    if path == undefined or not (doesFileExist path) then DM_fail "motion.zal not found"
    local size = getFileSize path
    if DM_motionCache != undefined and DM_motionCache[1] == path and DM_motionCache[2] == size then return DM_motionCache[3]
    local outer = DM_readZalBytes (DM_readAllBytes path) 0 size false
    if outer.count < 1 then DM_fail "motion.zal has no groups"
    local g = outer[1]
    local inner = DM_readZalBytes g.data g.start g.len true
    DM_motionCache = #(path, size, inner)
    inner
)

-- decode clip id m (1..238) from the groups of DM_motionGroups
fn DM_motionClip groups m =
(
    local g = undefined
    for c in groups do (if c.id == m then g = c)
    if g == undefined then DM_fail ("motion id " + (m as string) + " is not in motion.zal")
    DM_readMotion g.data g.start g.len
)

-- (v + 0x8000) & 0xFFFF - 0x8000 (mirrors dmparse._s16)
fn DM_s16 v = ((bit.and (v + 32768) 65535) - 32768)

-- mirrors dmparse.sample_motion: one DM_Pose per frame. Init values are absolute; each frame, records
-- at that time replace per-channel deltas, then every channel adds its current delta (deltas persist).
fn DM_sampleMotion clip =
(
    local st = #()  -- per section: #(kind, rot, drot, trans, dtrans)
    for si = 1 to clip.sections.count do
    (
        local cnt = clip.sections[si][1]
        local r = clip.inits[si]
        local rot = #()
        local drot = #()
        for i = 1 to cnt do
        (
            append rot #(0, 0, 0)
            append drot #(0, 0, 0)
        )
        local trans = #(0.0, 0.0, 0.0)
        if r != undefined then
        (
            for i = 1 to cnt do
            (
                local v = r.rots[i]
                if v != undefined then
                (
                    if clip.wide then rot[i] = #(v[1], v[2], v[3]) else rot[i] = #(256 * v[1], 256 * v[2], 256 * v[3])
                )
            )
            if r.trans != undefined then trans = #(r.trans[1] * 0.125, r.trans[2] * 0.125, r.trans[3] * 0.125)
        )
        append st #(clip.sections[si][3], rot, drot, trans, #(0.0, 0.0, 0.0))
    )
    local root = #(0.0, 0.0, 0.0)
    local poses = #()
    for k = 0 to clip.frames - 1 do
    (
        if k > 0 then
        (
            for r in clip.records do
            (
                if r.time == k * clip.frameMs then
                (
                    local s = st[r.section]
                    if r.trans != undefined then s[5] = #(r.trans[1] / 256.0, r.trans[2] / 256.0, r.trans[3] / 256.0)
                    for i = 1 to r.rots.count do
                    (
                        local v = r.rots[i]
                        if v != undefined then
                        (
                            if clip.wide then s[3][i] = #(v[1], v[2], v[3]) else s[3][i] = #(128 * v[1], 128 * v[2], 128 * v[3])
                        )
                    )
                )
            )
            for s in st do
            (
                for i = 1 to s[2].count do
                (
                    local rr = s[2][i]
                    local dd = s[3][i]
                    for j = 1 to 3 do rr[j] = DM_s16 (rr[j] + dd[j])
                )
                if s[1] == 0 then
                (
                    root[1] += s[5][1]
                    root[3] += s[5][3]
                    s[4][2] += s[5][2]
                ) else (
                    for j = 1 to 3 do s[4][j] += s[5][j]
                )
            )
        )
        local pose = DM_Pose time:(k * clip.frameMs) rider:undefined bike:undefined riderTrans:undefined bikeTrans:undefined
        for s in st do
        (
            local snap = #()
            for rr in s[2] do append snap #(rr[1], rr[2], rr[3])
            if s[1] == 0 then
            (
                pose.rider = snap
                pose.riderTrans = #(s[4][1] + root[1], s[4][2], s[4][3] + root[3])
            ) else (
                pose.bike = snap
                pose.bikeTrans = #(s[4][1], s[4][2], s[4][3])
            )
        )
        append poses pose
    )
    poses
)

-- mirrors dmparse.euler_to_quat: s16 binary angles (65536 = 360 deg) -> #(x, y, z, w) = qz * qy * qx.
-- MAXScript's sin/cos take DEGREES: the half angle v * pi / 65536 rad is v * 180 / 65536 deg.
fn DM_eulerToQuat a =
(
    local hx = a[1] * 180.0 / 65536.0
    local hy = a[2] * 180.0 / 65536.0
    local hz = a[3] * 180.0 / 65536.0
    local cx = cos hx
    local sx = sin hx
    local cy = cos hy
    local sy = sin hy
    local cz = cos hz
    local sz = sin hz
    #(sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz)
)

-- mirrors dmparse.quat_mul (Hamilton product, #(x, y, z, w)); plain arrays, not MAXScript quats
fn DM_quatMul a b =
(
    local x = a[4] * b[1] + a[1] * b[4] + a[2] * b[3] - a[3] * b[2]
    local y = a[4] * b[2] - a[1] * b[3] + a[2] * b[4] + a[3] * b[1]
    local z = a[4] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[4]
    local w = a[4] * b[4] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3]
    #(x, y, z, w)
)

global DM_QZP = DM_eulerToQuat #(0, 0, 16384)
global DM_QZM = DM_eulerToQuat #(0, 0, (-16384))

-- mirrors dmparse.channel_rotation: Euler -> quaternion, the arm table (SPEC 8.4), then the
-- engine -> model conversion (x, y, z, w) -> (x, -y, -z, w)
fn DM_channelRotation name e =
(
    local q = DM_eulerToQuat e
    if name == "LeftUpArm" then q = DM_quatMul q DM_QZP
    if name == "LeftLowArm" or name == "LeftHand" then q = DM_quatMul (DM_quatMul DM_QZM q) DM_QZP
    if name == "RightUpArm" then q = DM_quatMul q DM_QZM
    if name == "RightLowArm" or name == "RightHand" then q = DM_quatMul (DM_quatMul DM_QZP q) DM_QZM
    #(q[1], (-q[2]), (-q[3]), q[4])
)

-- quaternion #(x, y, z, w) + translation -> row-vector matrix3 (mirrors dave_mirra_import.quat_rows:
-- the transpose of the column rotation matrix). Built from numbers, not MAXScript's quat type.
fn DM_quatMatrix q pos =
(
    local x = q[1]
    local y = q[2]
    local z = q[3]
    local w = q[4]
    local r1 = [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)]
    local r2 = [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)]
    local r3 = [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]
    matrix3 r1 r2 r3 pos
)

-- "1,8, 15" -> #(1, 8, 15): the dialog's Motion ids field
fn DM_parseMotionIds s =
(
    local ids = #()
    for w in (filterString s ",") do
    (
        local v = trimLeft (trimRight w " \t") " \t"
        if v != "" then
        (
            local ok = v.count <= 9
            for i = 1 to v.count do (if (findString "0123456789" v[i]) == undefined then ok = false)
            if not ok then DM_fail ("bad motion id '" + v + "' (use numbers like 1,8,15)")
            append ids (v as integer)
        )
    )
    ids
)

-- motion.zal next to the model, else undefined (the dialog then asks for it)
fn DM_findMotionZal zmdPath =
(
    local p = (getFilenamePath zmdPath) + "motion.zal"
    if doesFileExist p then p else undefined
)

-- ============================================================================ scene building

fn DM_insideGame path =
(
    local p = path
    local found = false
    local guard = 0
    while not found and p != "" and guard < 64 do
    (
        if doesFileExist ((trimRight p "\\") + "\\NgBMX.exe") then found = true
        local parent = getFilenamePath (trimRight p "\\")
        if parent == p then p = "" else p = parent
        guard += 1
    )
    found
)

fn DM_safeName s =
(
    local ok = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
    local out = ""
    for i = 1 to s.count do (if (findString ok s[i]) != undefined then out += s[i] else out += "_")
    if out == "" then out = "tex"
    out
)

fn DM_textureFolder srcPath texDir =
(
    local d = texDir
    if d == undefined or d == "" then d = (getDir #temp) + "\\DaveMirraTextures\\" + (getFilenameFile srcPath)
    if DM_insideGame d then DM_fail ("texture folder " + d + " is inside the game install; choose another folder")
    makeDir d all:true
    d
)

-- copy each embedded image out of the model file; returns #(#(UPPERNAME, path, isPng), ...)
-- (.NET byte copy: a MAXScript readByte/writeByte loop would take seconds per 200 KB texture)
fn DM_writeTextures srcPath textures folder =
(
    local out = #()
    -- NB: dotNetObject "System.Byte[]" n does NOT make a byte array (it reaches .NET as null:
    -- "Buffer cannot be null. Parameter name: array"). Let BinaryReader.ReadBytes allocate it, and
    -- keep the result a .NET byte[] (asDotNetObject:true) instead of a converted MAXScript array.
    local src = dotNetObject "System.IO.FileStream" srcPath (dotNetClass "System.IO.FileMode").Open (dotNetClass "System.IO.FileAccess").Read
    local br = dotNetObject "System.IO.BinaryReader" src
    for t in textures do
    (
        local path = folder + "\\" + (DM_safeName t.name) + "." + t.kind
        src.Seek t.dataOff (dotNetClass "System.IO.SeekOrigin").Begin
        local buf = br.ReadBytes t.dataLen asDotNetObject:true
        if buf == undefined or buf.Length != t.dataLen then
        (
            br.Close()
            DM_fail ("could not read texture " + t.name + " (" + (t.dataLen as string) + " bytes at " + (t.dataOff as string) + ") from " + srcPath)
        )
        local fs = dotNetObject "System.IO.FileStream" path (dotNetClass "System.IO.FileMode").Create
        fs.Write buf 0 t.dataLen
        fs.Close()
        append out #(toUpper t.name, path, t.kind == "png")
    )
    br.Close()  -- also closes src
    out
)

fn DM_findTex texfiles name =
(
    local r = undefined
    if name != undefined then (for t in texfiles do (if r == undefined and t[1] == (toUpper name) then r = t))
    r
)

-- Multimaterial, one Standard sub-material per RW material slot (face material ID = slot + 1)
fn DM_buildMaterial name slots texfiles =
(
    local mm = multimaterial numsubs:slots.count
    mm.name = name
    for i = 1 to slots.count do
    (
        local s = slots[i]
        local texName = s[2]
        local label = texName
        if label == undefined then label = "untextured_" + ((i - 1) as string)
        local m = standardMaterial name:label
        m.diffuse = color s[1][1] s[1][2] s[1][3]
        local t = DM_findTex texfiles texName
        if t != undefined then
        (
            m.diffuseMap = bitmaptexture filename:t[2]
            if t[3] then
            (
                local op = bitmaptexture filename:t[2]
                op.monoOutput = 1
                m.opacityMap = op
            )
            showTextureMap m true
        )
        mm.materialList[i] = m
    )
    mm
)

-- editable mesh in the part's own space (mirrors dave_mirra_import.build_mesh)
fn DM_buildMesh name g scale =
(
    local verts = #()
    for p in g.positions do append verts (p * scale)
    local faces = #()
    local ids = #()
    for t in g.triangles do
    (
        append faces [t[1] + 1, t[2] + 1, t[3] + 1]
        append ids (t[4] + 1)
    )
    local node = mesh vertices:verts faces:faces materialIDs:ids
    node.name = name
    for ch = 1 to g.uvs.count do
    (
        local uv = g.uvs[ch]
        meshop.setMapSupport node ch true
        meshop.setNumMapVerts node ch uv.count
        for i = 1 to uv.count do meshop.setMapVert node ch i [uv[i][1], 1.0 - uv[i][2], 0.0]
        for i = 1 to faces.count do meshop.setMapFace node ch i faces[i]
    )
    if g.colors != undefined then
    (
        meshop.setMapSupport node 0 true
        meshop.setNumMapVerts node 0 g.colors.count
        for i = 1 to g.colors.count do meshop.setMapVert node 0 i [g.colors[i][1] / 255.0, g.colors[i][2] / 255.0, g.colors[i][3] / 255.0]
        for i = 1 to faces.count do meshop.setMapFace node 0 i faces[i]
        meshop.setMapSupport node (-2) true
        meshop.setNumMapVerts node (-2) g.colors.count
        for i = 1 to g.colors.count do meshop.setMapVert node (-2) i [g.colors[i][4] / 255.0, g.colors[i][4] / 255.0, g.colors[i][4] / 255.0]
        for i = 1 to faces.count do meshop.setMapFace node (-2) i faces[i]
    )
    for i = 1 to faces.count do setFaceSmoothGroup node i 1
    update node
    node
)

-- the game normal scaled to length 1, or undefined when missing or zero-length (mirrors
-- dave_mirra_import.unit_normal; level sectors store s8/127 normals and a few are [0,0,0])
fn DM_unitNormal n =
(
    if n == undefined then return undefined
    local l = length n
    if l < 1e-6 then return undefined
    n / l
)

-- "Flip normals to faces" (mirrors dave_mirra_import.flip_normals_to_faces): a vertex normal flips when its dot
-- product with the sum of its faces' area-weighted CCW normals ((b - a) x (c - a)) is negative. Faces that
-- disagree: the larger area wins; a zero sum keeps the normal; undefined stays undefined. Returns a new array.
fn DM_flipNormalsToFaces positions normals triangles =
(
    local acc = #()
    for i = 1 to positions.count do append acc [0, 0, 0]
    for t in triangles do
    (
        local a = positions[t[1] + 1]
        local fv = cross (positions[t[2] + 1] - a) (positions[t[3] + 1] - a)
        for c = 1 to 3 do acc[t[c] + 1] = acc[t[c] + 1] + fv
    )
    local out = #()
    for i = 1 to normals.count do
    (
        local n = normals[i]
        if n != undefined and (dot n acc[i]) < 0 then n = n * (-1.0)
        append out n
    )
    out
)

-- exact game normals through an Edit_Normals modifier (mirrors dave_mirra_import.set_explicit_normals).
-- g.normals: one per vertex (undefined = none); g.triangles: the faces in face order (0-based vertices).
-- Edit_Normals keeps one normal per vertex and smoothing group: GetNormalID per face corner, then
-- SetNormal + SetNormalExplicit once per new id. Vertices without a usable normal keep the
-- smoothing-group normal. The methods need the modifier current in the Modify panel (one
-- setCurrentObject per mesh); undo and redraw are off while the ids are set.
-- Returns the modifier, or undefined when no vertex has a usable normal.
-- flip:true runs DM_flipNormalsToFaces on g's normals first.
fn DM_setExplicitNormals node g flip:false =
(
    if g.normals == undefined then return undefined
    local normals = g.normals
    if flip then normals = DM_flipNormalsToFaces g.positions g.normals g.triangles
    local unit = #()
    local usable = false
    for n in normals do
    (
        local u = DM_unitNormal n
        if u != undefined then usable = true
        append unit u
    )
    if not usable then return undefined
    local en = Edit_Normals()
    addModifier node en
    select node
    max modify mode
    modPanel.setCurrentObject en
    with undo off
    (
        with redraw off
        (
            local done = #()
            local nn = en.GetNumNormals()
            for i = 1 to nn do append done false
            for fi = 1 to g.triangles.count do
            (
                local t = g.triangles[fi]
                for c = 1 to 3 do
                (
                    local un = unit[t[c] + 1]
                    if un != undefined then
                    (
                        local nid = en.GetNormalID fi c
                        if not done[nid] then
                        (
                            done[nid] = true
                            en.SetNormal nid un
                            en.SetNormalExplicit nid explicit:true
                        )
                    )
                )
            )
        )
    )
    en
)

-- Skin: every frame is a bone, each vertex 100% on its one bone
fn DM_skinMesh node bones vframes =
(
    local sk = skin()
    addModifier node sk
    select node
    max modify mode
    modPanel.setCurrentObject sk
    for i = 1 to bones.count do
    (
        local upd = 0
        if i == bones.count then upd = 1
        skinOps.addBone sk bones[i] upd
    )
    classOf node
    for v = 1 to vframes.count do skinOps.ReplaceVertexWeights sk v (vframes[v] + 1) 1.0
    sk
)

-- index (1-based) of the LAST frame called nm, else 0 (a Python dict {name: index} keeps the last)
fn DM_nameIndex names nm =
(
    local r = 0
    for i = 1 to names.count do (if names[i] == nm then r = i)
    r
)

-- frame index (1-based, 0 = none) of each channel of a section kind: 0 rider, else bike
fn DM_channelFrames clip names rider =
(
    local idx = #()
    for sec in clip.sections do
    (
        if (sec[3] == 0) == rider then
        (
            local table = DM_RIDER_CHANNELS
            if not rider then table = DM_BIKE_CHANNELS
            local n = sec[1]
            if n > table.count then n = table.count
            idx = #()
            for c = 1 to n do append idx (DM_nameIndex names table[c])
        )
    )
    idx
)

-- key one motion clip on the frame nodes, one key per game frame (Max frame start + k = game frame k).
-- Channel frames get the game's rotation with their bind translation; the others keep their bind.
-- Returns the number of keys set. Mirrors dave_mirra_import.key_motion.
-- offsets: per node the constant bone rotation O (DM_boneAim) put in front of every key, or undefined.
fn DM_keyMotion clip lo names frameNodes rootM scale offsets start:0 =
(
    local chanR = DM_channelFrames clip names true
    local chanB = DM_channelFrames clip names false
    local n = 0
    local poses = DM_sampleMotion clip
    for k = 1 to poses.count do
    (
        local pose = poses[k]
        local loc = #()
        for fr in lo.frames do append loc (DM_frameMatrix fr scale)
        if pose.rider != undefined then
        (
            for c = 1 to chanR.count do
            (
                local fi = chanR[c]
                if fi > 0 and c <= pose.rider.count then loc[fi] = DM_quatMatrix (DM_channelRotation DM_RIDER_CHANNELS[c] pose.rider[c]) (lo.frames[fi].pos * scale)
            )
        )
        if pose.bike != undefined then
        (
            for c = 1 to chanB.count do
            (
                local fi = chanB[c]
                if fi > 0 and c <= pose.bike.count then loc[fi] = DM_quatMatrix (DM_channelRotation DM_BIKE_CHANNELS[c] pose.bike[c]) (lo.frames[fi].pos * scale)
            )
        )
        local world = #()
        for i = 1 to lo.frames.count do
        (
            local m = loc[i]
            local par = lo.frames[i].parent
            if par >= 0 and par < i - 1 then m = m * world[par + 1]
            append world m
        )
        animate on
        (
            at time (start + k - 1)
            (
                for i = 1 to frameNodes.count do
                (
                    local m = world[i] * rootM
                    if offsets != undefined then m = offsets[i] * m
                    frameNodes[i].transform = m
                    n += 1
                )
            )
        )
    )
    n
)

-- refuse an empty model or a missing LOD
fn DM_checkModel z path lod =
(
    if z.empty then DM_fail ((filenameFromPath path) + " is an empty (0-byte) model; nothing to import")
    if lod < 0 or lod >= z.lods.count then DM_fail ("LOD " + (lod as string) + " does not exist")
)

-- the frames of a model as the import builds them, shared by the model import and DM_ImportMotion so that
-- both key exactly the same matrices: #(lod, names, world, skinned, aim, offsets).
-- world: each frame's game-space world matrix (scaled); aim: DM_boneAim per frame for a rider with
-- boneAim, else undefined; offsets: the aim rotations O put in front of every bone transform, else undefined.
fn DM_modelRig z lod scale boneAim =
(
    local lo = z.lods[lod + 1]
    local names = DM_frameNames z lod
    local world = #()
    for i = 1 to lo.frames.count do
    (
        local fr = lo.frames[i]
        local m = DM_frameMatrix fr scale
        if fr.parent >= 0 and fr.parent < i - 1 then m = m * world[fr.parent + 1]
        append world m
    )
    local skinned = false
    for a in lo.atomics do (if a.geometry.bones != undefined then skinned = true)
    local aim = undefined
    local offsets = undefined
    if skinned and boneAim then
    (
        aim = DM_boneAim lo world
        offsets = #()
        for a in aim do append offsets a[1]
    )
    #(lo, names, world, skinned, aim, offsets)
)

-- user properties on the model's root dummy, read back by DM_ImportMotion: the model file, LOD, scale (9
-- significant digits, the whole 32-bit float), the bone kind ("aim": bones aimed at their children, so the keys
-- carry the offsets O; "frame": game frame orientation, or a bike's dummies) and whether the root carries the
-- rider upright correction ("1") or not ("0"). Each frame node gets "dmbmx_frame" = its frame index (from 0).
fn DM_tagModelRoot root path lod scale aimed uprightRoot =
(
    setUserProp root "dmbmx_zmd" path
    setUserProp root "dmbmx_lod" (lod as string)
    setUserProp root "dmbmx_scale" (formattedPrint (scale as float) format:".9g")
    setUserProp root "dmbmx_bones" (if aimed then "aim" else "frame")
    setUserProp root "dmbmx_upright" (if uprightRoot then "1" else "0")
)

-- import a .zmd; returns #(root, frameNodes, meshNodes, textureFolder).
-- exactNormals:true puts the game normals on the meshes (Edit_Normals).
-- boneAim:false keeps the bones along their game frames' X axes (the "game frame orientation" choice);
-- flipNormals:true (with exactNormals) applies DM_flipNormalsToFaces.
fn DM_ImportZmd path scale lod upright texDir exactNormals:false boneAim:true flipNormals:false =
(
    local r = DM_ImportZmdMotion path scale lod upright texDir undefined #() exactNormals:exactNormals boneAim:boneAim flipNormals:flipNormals
    #(r[1], r[2], r[3], r[4])
)

-- import a .zmd and key motion clips from motionPath (motion.zal) on it: motionIds e.g. #(1, 8, 15),
-- #() for none. Each clip is keyed from frame 0 (a later clip overwrites an earlier one's keys).
-- Returns #(root, frameNodes, meshNodes, textureFolder, keyCount). Mirrors dave_mirra_import.import_zmd.
-- exactNormals:true puts the game normals on the meshes (Edit_Normals).
-- boneAim:true (default): rider bones point at their main child (DM_boneAim); the joints, the skin and the
-- keys are the same as with boneAim:false. flipNormals:true (with exactNormals): DM_flipNormalsToFaces.
fn DM_ImportZmdMotion path scale lod upright texDir motionPath motionIds exactNormals:false boneAim:true flipNormals:false =
(
    local z = DM_readZmd path
    DM_checkModel z path lod
    local folder = DM_textureFolder path texDir
    local texfiles = DM_writeTextures path z.textures folder
    local clips = #()
    if motionIds.count > 0 then
    (
        local groups = DM_motionGroups motionPath
        for m in motionIds do append clips (DM_motionClip groups m)
    )
    local model = getFilenameFile path
    if z.mdef != undefined and z.mdef.name != "" then model = z.mdef.name
    local rig = DM_modelRig z lod scale boneAim
    local lo = rig[1]
    local names = rig[2]
    local world = rig[3]
    local skinned = rig[4]
    local aim = rig[5]
    local offsets = rig[6]
    -- animated riders get no upright correction: the Hips channel stands them up (SPEC 8.4)
    local uprightRoot = upright and clips.count == 0
    local rootM = DM_rootMatrix z lod uprightRoot
    local root = dummy name:model boxsize:[0.1 * scale, 0.1 * scale, 0.1 * scale]    -- 10 cm helper icon
    root.transform = rootM
    DM_tagModelRoot root path lod scale (offsets != undefined) uprightRoot
    local frameNodes = #()
    for i = 1 to lo.frames.count do
    (
        local wm = world[i] * rootM
        local node = undefined
        if skinned and aim != undefined then
        (
            wm = aim[i][1] * wm
            local p = wm.row4
            node = BoneSys.createBone p (p + wm.row1 * (aim[i][2] * scale)) wm.row3
            node.width = 0.02 * scale
            node.height = 0.02 * scale
            node.length = aim[i][2] * scale
        ) else if skinned then (
            local p = wm.row4
            node = BoneSys.createBone p (p + [DM_BONE_DEFAULT * scale, 0, 0]) [0, 0, 1]
            node.width = 0.02 * scale
            node.height = 0.02 * scale
            node.length = DM_BONE_DEFAULT * scale
        ) else (
            node = dummy name:names[i] boxsize:[0.03 * scale, 0.03 * scale, 0.03 * scale]
        )
        node.name = names[i]
        node.transform = wm
        local par = lo.frames[i].parent
        if par >= 0 and par < i - 1 then node.parent = frameNodes[par + 1] else node.parent = root
        setUserProp node "dmbmx_frame" ((i - 1) as string)
        append frameNodes node
    )
    local meshNodes = #()
    for ai = 1 to lo.atomics.count do
    (
        local a = lo.atomics[ai]
        local g = a.geometry
        local node = DM_buildMesh (names[a.frame + 1] + "_mesh" + ((ai - 1) as string)) g scale
        if exactNormals then DM_setExplicitNormals node g flip:flipNormals
        node.material = DM_buildMaterial (model + "_atomic" + ((ai - 1) as string)) g.slots texfiles
        if g.bones != undefined then
        (
            node.transform = rootM
            node.parent = root
            DM_skinMesh node frameNodes (DM_vertexFrames z lod (ai - 1))
        ) else (
            node.transform = world[a.frame + 1] * rootM
            node.parent = frameNodes[a.frame + 1]
        )
        append meshNodes node
    )
    local keys = 0
    for c in clips do keys += DM_keyMotion c lo names frameNodes rootM scale offsets
    #(root, frameNodes, meshNodes, folder, keys)
)

-- ============================================================================ a motion onto a model in the scene
-- Mirrors dave_mirra_import.list_motions / import_motion. The model import tags its root and frame nodes
-- (DM_tagModelRoot); DM_ImportMotion reads the model file again from there and keys the clip exactly like
-- DM_ImportZmdMotion, from a start frame on, so clips can be chained back to back on one timeline.

-- what plays each motion id in the game (motion.zal stores no names; see docs ANIM.md sections 7 and 8)
global DM_MOTION_STATES = #("not used by the game", "coast (standing still)", "not used by the game", "second motion slot (physics only)", "coast / landing", "not used by the game", "not used by the game", "pedal", "not used by the game", "skid (start)", "skid (hold)", "not used by the game", "crouch (start)", "crouch (hold)", "take-off (hop)", "not used by the game", "turnaround", "turnaround (button held)", "manual (enter)", "manual (hold)", "manual (exit)", "landing variant", "air", "air idle")
global DM_AIR_TRICKS = #("turndown", "can-can", "tabletop", "backflip", "superman", "candy bar", "tailwhip", "front flip", "decade", "no footed can can", "un-turndown", "double tailwhip", "can-can lookdown", "candy bar lookdown", "tobaggan", "superman indian")
global DM_MODIFIERS = #("barspin", "one footer", "one hander", "x-up", "x-down", "suicide no hander", "seat grab", "no footer", "no hander", "nothing", "one hand one footer", "one hand x-up", "rocket air", "double peg grab", "one handed seat grab", "no footed seat grab", "one footed seat grab", "double seat grab", "switch hander", "no footed x-up", "one footed x-up", "no footed one hander", "no handed one footer", "no handed barspin", "barspin to barspin back", "no footed barspin", "one footed barspin", "rocket x-up", "crotch rocket", "rocket one hander", "rocket no hander", "rocket barspin", "rocket one footer", "indian", "switch footer", "x-up seat grab", "barspin seat grab", "busdriver")
global DM_TRICK_ROLES = #("start", "hold", "release")
global DM_GRIND_ROLES = #("enter", "hold", "exit")
global DM_GRINDS = #("double peg grind", "smith grind", "feeble grind", "toothpick grind", "icepick grind", "lip slide", "luc-e grind", "crooked grind", "sprocket grind", "double peg stall", "toothpick stall", "icepick stall", "nosepick", "tail tap", "disaster", "fast plant", "wall tap")

-- a motion id's description, "" when the id is not one of the game's 238 (mirrors motion_name)
fn DM_motionName m =
(
    if m >= 1 and m <= 24 then return DM_MOTION_STATES[m]
    if m >= 25 and m <= 72 then return (DM_AIR_TRICKS[(m - 25) / 3 + 1] + " (" + DM_TRICK_ROLES[(m - 25) - 3 * ((m - 25) / 3) + 1] + ")")
    if m >= 73 and m <= 186 then return (DM_MODIFIERS[(m - 73) / 3 + 1] + " modifier (" + DM_TRICK_ROLES[(m - 73) - 3 * ((m - 73) / 3) + 1] + ")")
    if m >= 187 and m <= 237 then return (DM_GRINDS[(m - 187) / 3 + 1] + " (" + DM_GRIND_ROLES[(m - 187) - 3 * ((m - 187) / 3) + 1] + ")")
    if m == 238 then return "not used by the game"
    ""
)

-- every motion of a motion archive, in archive order: #(#(id, frames, description), ...). Mirrors list_motions.
fn DM_ListMotions zalPath =
(
    local out = #()
    for g in (DM_motionGroups zalPath) do
    (
        local fr = 0
        if g.len >= 2 then fr = bit.and (DM_bu16 g.data g.start) 32767
        append out #(g.id, fr, DM_motionName g.id)
    )
    out
)

-- one DM_ListMotions entry as the dialog lists it: "25: turndown (start), 13 frames" (mirrors motion_label)
fn DM_motionLabel e =
(
    local s = (e[1] as string) + ": "
    if e[3] != "" then s += e[3] + ", "
    s + (e[2] as string) + " frames"
)

-- the root dummy of the imported model a node belongs to (the node itself or the ancestor that carries
-- "dmbmx_zmd"), else undefined
fn DM_modelRoot node =
(
    local n = node
    while n != undefined and (getUserProp n "dmbmx_zmd") == undefined do n = n.parent
    n
)

-- the model to animate: the one target belongs to (any of its nodes), else the one the selection belongs to,
-- else the only one in the scene. Refuses none or several.
fn DM_findTarget target =
(
    if target != undefined then
    (
        local r = DM_modelRoot target
        if r == undefined then DM_fail (target.name + " is not part of a model imported by this importer (version 1.5 or later)")
        return r
    )
    local roots = #()
    for n in selection do
    (
        local r = DM_modelRoot n
        if r != undefined and (findItem roots r) == 0 then append roots r
    )
    if roots.count > 1 then DM_fail "the selection belongs to several imported models: select one bone (or the root) of the model to animate"
    if roots.count == 1 then return roots[1]
    for n in objects do (if (getUserProp n "dmbmx_zmd") != undefined then append roots n)
    if roots.count == 0 then DM_fail "no imported rider or bike in the scene: import a model first (Import ZMD, version 1.5 or later)"
    if roots.count > 1 then DM_fail ("there are " + (roots.count as string) + " imported models in the scene: select one bone (or the root) of the model to animate")
    roots[1]
)

-- the frame nodes of the model under root, by their "dmbmx_frame" index: count nodes, every index once
fn DM_rigNodes root count =
(
    local out = #()
    for i = 1 to count do append out undefined
    for n in objects do
    (
        local v = getUserProp n "dmbmx_frame"
        if v != undefined and (DM_modelRoot n) == root then
        (
            local i = (v as integer) + 1
            if i < 1 or i > count then DM_fail (root.name + ": " + n.name + " is frame " + (v as string) + ", but the model file has " + (count as string) + " frames (a different model file?)")
            if out[i] != undefined then DM_fail (root.name + ": " + out[i].name + " and " + n.name + " are both frame " + (v as string))
            out[i] = n
        )
    )
    for i = 1 to count do (if out[i] == undefined then DM_fail (root.name + ": frame " + ((i - 1) as string) + " of the model is not in the scene (a deleted bone?)"))
    out
)

-- two transforms equal to float precision (Max stores a node's transform as position / rotation / scale, so
-- reading it back can differ in the last bits): rows within 1e-5, positions within 1e-5 game metres
fn DM_sameTransform a b scale =
(
    local ra = #(a.row1, a.row2, a.row3, a.row4)
    local rb = #(b.row1, b.row2, b.row3, b.row4)
    local ok = true
    for r = 1 to 4 do
    (
        local d = ra[r] - rb[r]
        local tol = 1e-5
        if r == 4 then tol = 1e-5 * scale
        if (abs d.x) > tol or (abs d.y) > tol or (abs d.z) > tol then ok = false
    )
    ok
)

-- the latest key time (in frames) of a controller and its sub-controllers, undefined when none has keys
fn DM_lastKeyTime c =
(
    if c == undefined then return undefined
    local r = undefined
    local n = numKeys c
    if n > 0 then r = (getKeyTime c n).frame
    for i = 1 to c.numSubs do
    (
        local t = DM_lastKeyTime c[i].controller
        if t != undefined and (r == undefined or t > r) then r = t
    )
    r
)

-- the frame after the last key of the model under root (any of its frame nodes), 0 when it has no keys
fn DM_nextFreeFrame root =
(
    local last = undefined
    for n in objects do
    (
        if (getUserProp n "dmbmx_frame") != undefined and (DM_modelRoot n) == root then
        (
            local t = DM_lastKeyTime n.controller
            if t != undefined and (last == undefined or t > last) then last = t
        )
    )
    if last == undefined then return 0
    ((floor last) as integer) + 1
)

-- grow the scene's animation range to cover frames a..b (it never shrinks); true when it changed
fn DM_growRange a b =
(
    local s = animationRange.start.frame
    local e = animationRange.end.frame
    local grow = false
    if a < s then (s = a; grow = true)
    if b > e then (e = b; grow = true)
    if grow then animationRange = interval s e
    grow
)

-- key motion motionId of the motion archive zalPath (motion.zal) onto a model in the scene.
-- target: any node of the model (a bone or its root); undefined: the selection's model, else the only one.
-- startFrame: the Max frame of the clip's first frame; undefined: the frame after the model's last key (0 when
-- it has none), so calls chain clips back to back. zmd: the model file, when it has moved since the import.
-- The keys are DM_ImportZmdMotion's for the same model, LOD, scale, bones and clip, shifted by startFrame. They
-- are world transforms: the root keeps its own transform (with a rider's upright correction) and the keys
-- follow it when it has been moved since the import. The animation range grows to cover them.
-- Returns #(root, startFrame, lastFrame, keyCount). Mirrors dave_mirra_import.import_motion.
fn DM_ImportMotion zalPath motionId startFrame:undefined target:undefined zmd:undefined =
(
    local root = DM_findTarget target
    local path = zmd
    if path == undefined then path = getUserProp root "dmbmx_zmd"
    if not (doesFileExist path) then DM_fail ("the model file of " + root.name + " is not at " + path + " (pass zmd:)")
    local lod = (getUserProp root "dmbmx_lod") as integer
    local scale = (getUserProp root "dmbmx_scale") as float
    local aimed = (getUserProp root "dmbmx_bones") == "aim"
    local uprightRoot = ((getUserProp root "dmbmx_upright") as integer) == 1
    local z = DM_readZmd path
    DM_checkModel z path lod
    local rig = DM_modelRig z lod scale aimed
    local lo = rig[1]
    local names = rig[2]
    if aimed and rig[6] == undefined then DM_fail (root.name + ": " + (filenameFromPath path) + " has no skinned rider (a different model file?)")
    local frameNodes = DM_rigNodes root lo.frames.count
    local clip = DM_motionClip (DM_motionGroups zalPath) motionId
    local used = 0
    for i in (DM_channelFrames clip names true) do (if i > 0 then used += 1)
    for i in (DM_channelFrames clip names false) do (if i > 0 then used += 1)
    if used == 0 then DM_fail ("motion " + (motionId as string) + " animates riders and bikes, and " + root.name + " has none of their bones")
    -- DM_ImportZmdMotion's root (no upright correction), moved the way this root has moved since the import
    local rootM = DM_rootMatrix z lod false
    local imported = DM_rootMatrix z lod uprightRoot
    local cur = root.transform
    if not (DM_sameTransform cur imported scale) then rootM = rootM * (inverse imported) * cur
    local start = startFrame
    if start == undefined then start = DM_nextFreeFrame root
    start = start as integer
    local keys = DM_keyMotion clip lo names frameNodes rootM scale rig[6] start:start
    local last = start + keys / frameNodes.count - 1
    DM_growRange start last
    #(root, start, last, keys)
)

-- ============================================================================ levels (dmparse.read_level, SPEC 5)

struct DM_Sector (vertices, normals, uvs, colors, triangles)
struct DM_World (origin, flags, slots, sectors)
struct DM_PropDef (id, name, model)
struct DM_Placed (id, ptype, name, m9, pos)
struct DM_Level (name, files, textures, matPath, world, propDefs, props, points, panorama, tags)

-- mirrors dmparse.read_zsd: #(#(order, size, name), ...)
fn DM_readZsd path =
(
    local size = getFileSize path
    local f = fopen path "rb"
    if (DM_readTag f) != "SCEN" then (fclose f; DM_fail "not a level ZSD (no SCEN)")
    local scen = (DM_parse4cc f 0 size 0)[1]
    local files = #()
    for c in scen.children do
    (
        if c.tag == "SCFI" then
        (
            DM_seek f (c.ofs + 8)
            local order = DM_u8 f
            DM_seek f (c.ofs + 12)
            local sz = DM_u32 f
            append files #(order, sz, DM_cstr f (c.size - 8))
        )
    )
    fclose f
    files
)

-- mirrors dmparse.read_mat_sd
fn DM_readMatSd path =
(
    local size = getFileSize path
    local f = fopen path "rb"
    local out = #()
    for c in (DM_parse4cc f 0 size 0) do (if c.tag == "MATI" then append out (DM_readMati f c))
    fclose f
    out
)

-- mirrors dmparse._read_sector
fn DM_readSector f st flags =
(
    DM_seek f (st.ofs + 12)
    local base = DM_u32 f
    local nt = DM_u32 f
    local nv = DM_u32 f
    if nv > 256 then DM_fail "sector with more than 256 vertices"
    DM_seek f (st.ofs + 12 + 44)
    local sec = DM_Sector vertices:#() normals:undefined uvs:undefined colors:undefined triangles:#()
    for i = 1 to nv do append sec.vertices (DM_readP3 f)
    if (bit.and flags 0x10) != 0 then
    (
        sec.normals = #()
        for i = 1 to nv do
        (
            local x = readByte f #signed
            local y = readByte f #signed
            local z = readByte f #signed
            readByte f #signed
            append sec.normals [x / 127.0, y / 127.0, z / 127.0]
        )
    )
    if (bit.and flags 0x08) != 0 then
    (
        sec.colors = #()
        for i = 1 to nv do append sec.colors (DM_read4u8 f)
    )
    if (bit.and flags 0x84) != 0 then
    (
        sec.uvs = #()
        for i = 1 to nv do
        (
            local u = DM_f32 f
            local v = DM_f32 f
            append sec.uvs #(u, v)
        )
    )
    if (bit.and flags 0x80) != 0 then DM_seek f ((ftell f) + 8 * nv)
    for i = 1 to nt do
    (
        local t = DM_read4u8 f
        if t[2] >= nv or t[3] >= nv or t[4] >= nv then DM_fail "sector triangle index out of range"
        append sec.triangles #(base + t[1], t[2], t[3], t[4])
    )
    sec
)

-- depth-first, left before right (mirrors the sector walk in dmparse.read_bsp)
fn DM_collectSectors f c flags out depth =
(
    if depth > 64 then DM_fail "sector tree too deep"
    if c.tag == 0x0A then
    (
        DM_collectSectors f c.children[2] flags out (depth + 1)
        DM_collectSectors f c.children[3] flags out (depth + 1)
    ) else (
        if c.tag != 0x09 then DM_fail "unexpected chunk in the sector tree"
        append out (DM_readSector f c.children[1] flags)
    )
    out
)

-- mirrors dmparse.read_bsp
fn DM_readBsp path =
(
    local size = getFileSize path
    local f = fopen path "rb"
    local w = (DM_parseRW f 0 size 0)[1]
    if w.tag != 0x0B then (fclose f; DM_fail "BSP._sd is not a RW World")
    DM_seek f (w.children[1].ofs + 12 + 4)
    local ox = DM_f32 f
    local oy = DM_f32 f
    local oz = DM_f32 f
    DM_seek f (w.children[1].ofs + 12 + 48)
    local flags = DM_u32 f
    local world = DM_World origin:[(-ox), (-oy), (-oz)] flags:flags slots:(DM_readMatList f w.children[2]) sectors:#()
    DM_collectSectors f w.children[3] flags world.sectors 0
    fclose f
    world
)

-- body of the record of a placed object (mirrors dmparse._placed): PRBL (0x48) / SDPT (0x4C)
fn DM_readPlaced f c withType =
(
    DM_seek f (c.ofs + 8 + 4)
    local id = DM_i32 f
    local ptype = undefined
    if withType then ptype = DM_u32 f
    local name = DM_cstr f 16
    local m9 = #()
    for i = 1 to 9 do append m9 (DM_f32 f)
    local pos = DM_readP3 f
    DM_Placed id:id ptype:ptype name:name m9:m9 pos:pos
)

-- mirrors dmparse.read_propdef_sd
fn DM_readPropDefs path =
(
    local size = getFileSize path
    local f = fopen path "rb"
    local out = #()
    for form in (DM_parse4cc f 0 size 0) do
    (
        if form.tag == "PDBL" then
        (
            local pdin = DM_kid form "PDIN"
            local modl = DM_kid form "MODL"
            if pdin == undefined or modl == undefined then (fclose f; DM_fail "PDBL without PDIN or MODL")
            DM_seek f (pdin.ofs + 8 + 4)
            local id = DM_i32 f
            local name = DM_cstr f 20
            append out (DM_PropDef id:id name:name model:(DM_readModl f modl))
        )
    )
    fclose f
    out
)

-- mirrors dmparse.read_prop_sd
fn DM_readProps path =
(
    local size = getFileSize path
    local f = fopen path "rb"
    local out = #()
    for form in (DM_parse4cc f 0 size 0) do
    (
        if form.tag == "PRBL" then
        (
            local rec = DM_kid form "PRBL"
            if rec == undefined or rec.size != 0x48 then (fclose f; DM_fail "PRBL form needs one 0x48-byte record")
            append out (DM_readPlaced f rec false)
        )
    )
    fclose f
    out
)

-- mirrors dmparse.read_scene_sd: #(points, panorama model or undefined)
fn DM_readScene path =
(
    local size = getFileSize path
    local f = fopen path "rb"
    local pts = #()
    local pano = undefined
    for form in (DM_parse4cc f 0 size 0) do
    (
        if form.tag == "SCPT" then
        (
            local rec = DM_kid form "SDPT"
            if rec == undefined or rec.size != 0x4C then (fclose f; DM_fail "SCPT needs one 0x4C-byte SDPT")
            append pts (DM_readPlaced f rec true)
        )
        if form.tag == "SDPA" then
        (
            local m = DM_kid form "MODL"
            if m != undefined then pano = DM_readModl f m
        )
    )
    fclose f
    #(pts, pano)
)

-- mirrors dmparse.read_tag_sd: #(#(v0, v1, v2, code or undefined), ...) with ZGE vertices
fn DM_readTags path =
(
    local size = getFileSize path
    local f = fopen path "rb"
    local tagb = (DM_parse4cc f 0 size 0)[1]
    local tagi = DM_kid tagb "TAGI"
    if tagb.tag != "TAGB" or tagi == undefined then (fclose f; DM_fail "Tag._sd needs TAGB with TAGI")
    local blob = tagi.ofs + 16
    local faces = #()
    for k in tagb.children do
    (
        if k.tag == "TAGF" then
        (
            if k.size != 0x50 then (fclose f; DM_fail "TAGF must be 0x50 bytes")
            DM_seek f (k.ofs + 12)
            local v0 = DM_readP3 f
            local v1 = DM_readP3 f
            local v2 = DM_readP3 f
            DM_seek f (k.ofs + 8 + 0x34)
            local ft = DM_i32 f
            local code = undefined
            if ft >= 0 then
            (
                DM_seek f (blob + ft)
                code = DM_u16 f
            )
            append faces #(v0, v1, v2, code)
        )
    )
    fclose f
    faces
)

-- mirrors dmparse.read_level
fn DM_readLevel folder =
(
    local zsd = getFiles (folder + "\\*.zsd")
    if zsd.count == 0 then DM_fail ("no .ZSD in " + folder)
    local files = DM_readZsd zsd[1]
    local lv = DM_Level name:(filenameFromPath (trimRight folder "\\")) files:files textures:#() matPath:undefined world:undefined propDefs:#() props:#() points:#() panorama:undefined tags:#()
    for e in files do
    (
        local path = folder + "\\" + e[3]
        if doesFileExist path then
        (
            if e[1] == 1 then (lv.matPath = path; lv.textures = DM_readMatSd path)
            if e[1] == 2 then lv.propDefs = DM_readPropDefs path
            if e[1] == 3 then lv.tags = DM_readTags path
            if e[1] == 4 then lv.world = DM_readBsp path
            if e[1] == 5 then lv.props = DM_readProps path
            if e[1] == 6 then
            (
                local sc = DM_readScene path
                lv.points = sc[1]
                lv.panorama = sc[2]
            )
        )
    )
    if lv.world == undefined then DM_fail "level has no BSP._sd"
    lv
)

-- ---------------------------------------------------------------------------- level coordinates (SPEC 2.6)

-- mirrors dmparse.zge_to_rw: o + (x, -y, -z) / 100
fn DM_zgeToRw p o = ([o.x + p.x / 100.0, o.y - p.y / 100.0, o.z - p.z / 100.0])

-- mirrors dmparse.pos_to_rw: o + (x, -y, -z)
fn DM_posToRw p o = ([o.x + p.x, o.y - p.y, o.z - p.z])

-- mirrors dmparse.matrix_to_rw: F M F, F = diag(1, -1, -1); returns the three rows (scene points only)
fn DM_matrixToRw m9 =
(
    local fs = #(1.0, (-1.0), (-1.0))
    local rows = #()
    for r = 1 to 3 do
    (
        local a = m9[3 * (r - 1) + 1] * fs[r] * fs[1]
        local b = m9[3 * (r - 1) + 2] * fs[r] * fs[2]
        local c = m9[3 * (r - 1) + 3] * fs[r] * fs[3]
        append rows [a, b, c]
    )
    rows
)

-- mirrors dmparse.prop_matrix_to_rw: a prop's stored rows are its RW rotation rows unchanged (the game
-- renders props through its mirrored quaternion converter, SPEC 2.6)
fn DM_propMatrixToRw m9 = (#([m9[1], m9[2], m9[3]], [m9[4], m9[5], m9[6]], [m9[7], m9[8], m9[9]]))

-- ---------------------------------------------------------------------------- level scene

-- does this BMP (inside an open file) contain a pixel of the colour key? mirrors dmparse.color_key_png's test
-- (dmparse.bmp_may_have_key, then the decoded pixels: a magenta palette entry counts only if used)
fn DM_bmpMayHaveKey f base =
(
    DM_seek f base
    local b0 = DM_u8 f
    local b1 = DM_u8 f
    if b0 != 66 or b1 != 77 then return false
    DM_seek f (base + 10)
    local pix = DM_u32 f
    local hs = DM_u32 f
    local w = DM_i32 f
    local h = DM_i32 f
    DM_u16 f
    local bpp = DM_u16 f
    if h < 0 then h = -h
    local found = false
    local bv = 0
    if bpp <= 8 then
    (
        DM_seek f (base + 46)
        local n = DM_u32 f
        if n == 0 then n = bit.shift 1 bpp
        DM_seek f (base + 14 + hs)
        local keyIdx = #()
        for i = 1 to n do
        (
            local c = DM_read4u8 f
            if c[1] == 255 and c[2] == 0 and c[3] == 255 then append keyIdx (i - 1)
        )
        -- a magenta palette entry only matters if some pixel uses it
        if keyIdx.count > 0 then
        (
            local stride = 4 * ((w * bpp + 31) / 32)
            local per = 8 / bpp
            local mask = (bit.shift 1 bpp) - 1
            for y = 0 to h - 1 do
            (
                if not found then
                (
                    DM_seek f (base + pix + y * stride)
                    for x = 0 to w - 1 do
                    (
                        if (x - per * (x / per)) == 0 then bv = DM_u8 f
                        local idx = bit.and (bit.shift bv (bpp * (x - per * (x / per) + 1) - 8)) mask
                        if (findItem keyIdx idx) > 0 then found = true
                    )
                )
            )
        )
    ) else (
        local k = bpp / 8
        local stride = 4 * ((w * bpp + 31) / 32)
        for y = 0 to h - 1 do
        (
            DM_seek f (base + pix + y * stride)
            for x = 1 to w do
            (
                local c = DM_read4u8 f
                if c[1] == 255 and c[2] == 0 and c[3] == 255 then found = true
                if k == 3 then DM_seek f ((ftell f) - 1)
            )
        )
    )
    found
)

-- level textures: BMPs with the colour key become PNGs whose magenta pixels are transparent (.NET);
-- returns texfiles #(#(UPPERNAME, path, hasAlpha), ...)
fn DM_writeLevelTextures matPath textures folder =
(
    local texfiles = DM_writeTextures matPath textures folder
    local f = fopen matPath "rb"
    for i = 1 to textures.count do
    (
        local t = textures[i]
        if t.kind == "bmp" and (DM_bmpMayHaveKey f t.dataOff) then
        (
            local png = folder + "\\" + (DM_safeName t.name) + ".png"
            local bmp = dotNetObject "System.Drawing.Bitmap" texfiles[i][2]
            bmp.MakeTransparent ((dotNetClass "System.Drawing.Color").FromArgb 255 0 255)
            bmp.Save png (dotNetClass "System.Drawing.Imaging.ImageFormat").Png
            bmp.Dispose()
            texfiles[i] = #(texfiles[i][1], png, true)
        )
    )
    fclose f
    texfiles
)

-- all atomics of a model, frames baked, as one DM_Geometry (mirrors dave_mirra_import.merge_model):
-- normals are rotated by the frame (dmparse.model_meshes); undefined where an atomic has none
fn DM_modelGeometry z =
(
    local lo = z.lods[1]
    local g = DM_Geometry flags:0 numVertices:0 positions:#() normals:#() uvs:#(#()) colors:undefined triangles:#() slots:#() sphere:undefined bones:undefined
    local hasUv = true
    for a in lo.atomics do
    (
        local w = DM_frameWorld lo (a.frame + 1)
        local wr = matrix3 w.row1 w.row2 w.row3 [0, 0, 0]
        local base = g.positions.count
        local sbase = g.slots.count
        for p in a.geometry.positions do append g.positions (p * w)
        if a.geometry.normals != undefined then (for n in a.geometry.normals do append g.normals (n * wr)) else (for p in a.geometry.positions do append g.normals undefined)
        if a.geometry.uvs.count == 0 then hasUv = false else (for uv in a.geometry.uvs[1] do append g.uvs[1] uv)
        for t in a.geometry.triangles do append g.triangles #(t[1] + base, t[2] + base, t[3] + base, t[4] + sbase)
        for sl in a.geometry.slots do append g.slots sl
    )
    if not hasUv then g.uvs = #()
    g.numVertices = g.positions.count
    g
)

-- display colour per tag code (same formula as dave_mirra_import._tag_color)
fn DM_tagColor code =
(
    if code == undefined then return #(150, 150, 150, 255)
    #(bit.and (code * 67 + 40) 255, bit.and (code * 151 + 90) 255, bit.and (code * 211 + 150) 255, 255)
)

fn DM_hex4 v =
(
    local digits = "0123456789abcdef"
    local s = ""
    for i = 3 to 0 by (-1) do s += digits[(bit.and (bit.shift v (-4 * i)) 15) + 1]
    s
)

fn DM_group name root rootM scale =
(
    local d = dummy name:name boxsize:[0.05 * scale, 0.05 * scale, 0.05 * scale]
    d.transform = rootM
    d.parent = root
    d
)

-- import a level folder; returns #(root, world, props, tagNodes, points, panorama, textureFolder).
-- exactNormals:true puts the game normals on World, the PropDef models and the Panorama (Edit_Normals;
-- sectors without normals, and zero-length normals, keep the smoothing-group normals).
fn DM_ImportLevel folder scale texDir exactNormals:false flipNormals:false =
(
    local lv = DM_readLevel folder
    local name = lv.name
    local tfolder = DM_textureFolder (folder + "\\" + name) texDir
    local texfiles = #()
    if lv.matPath != undefined then texfiles = DM_writeLevelTextures lv.matPath lv.textures tfolder
    local o = lv.world.origin
    local rootM = matrix3 [1,0,0] [0,0,1] [0,(-1),0] [0,0,0]
    local root = dummy name:name boxsize:[0.1 * scale, 0.1 * scale, 0.1 * scale]
    root.transform = rootM
    local info = ""
    for e in lv.files do info += (e[1] as string) + "=" + e[3] + ":" + (e[2] as string) + ";"
    setUserProp root "dmbmx_components" info
    -- world: all sectors in one mesh, the world origin at the Max origin
    local g = DM_Geometry flags:0 numVertices:0 positions:#() normals:#() uvs:#(#()) colors:#() triangles:#() slots:lv.world.slots sphere:undefined bones:undefined
    for sec in lv.world.sectors do
    (
        local base = g.positions.count
        for v in sec.vertices do append g.positions (v - o)
        if sec.normals != undefined then (for n in sec.normals do append g.normals n) else (for v in sec.vertices do append g.normals undefined)
        if sec.uvs != undefined then (for uv in sec.uvs do append g.uvs[1] uv) else (for v in sec.vertices do append g.uvs[1] #(0.0, 0.0))
        if sec.colors != undefined then (for c in sec.colors do append g.colors c) else (for v in sec.vertices do append g.colors #(255, 255, 255, 255))
        for t in sec.triangles do append g.triangles #(t[2] + base, t[3] + base, t[4] + base, t[1])
    )
    g.numVertices = g.positions.count
    local world = DM_buildMesh "World" g scale
    if exactNormals then DM_setExplicitNormals world g flip:flipNormals
    world.material = DM_buildMaterial (name + "_World") g.slots texfiles
    world.transform = rootM
    world.parent = root
    -- props: one mesh per definition, instances for the rest
    local propsGrp = DM_group "Props" root rootM scale
    local masterIds = #()
    local masters = #()
    local propNodes = #()
    for k = 1 to lv.props.count do
    (
        local p = lv.props[k]
        local d = undefined
        for x in lv.propDefs do (if d == undefined and x.id == p.id then d = x)
        if d != undefined then
        (
            local mi = findItem masterIds d.id
            local node = undefined
            if mi == 0 then
            (
                local mg = DM_modelGeometry d.model
                node = DM_buildMesh ("PropDef_" + d.name) mg scale
                if exactNormals then DM_setExplicitNormals node mg flip:flipNormals
                node.material = DM_buildMaterial (name + "_" + d.name) mg.slots texfiles
                append masterIds d.id
                append masters node
            ) else (
                node = instance masters[mi]
            )
            node.name = p.name + "_" + ((k - 1) as string)
            local rows = DM_propMatrixToRw p.m9
            local t = (DM_posToRw p.pos o) - o
            node.transform = (matrix3 rows[1] rows[2] rows[3] (t * scale)) * rootM
            node.parent = propsGrp
            setUserProp node "dmbmx_def_id" (d.id as string)
            append propNodes node
        )
    )
    -- tags: collision faces grouped by face-tag code, hidden
    local tagsGrp = DM_group "Tags" root rootM scale
    local codes = #()
    local groups = #()
    for fc in lv.tags do
    (
        local key = fc[4]
        if key == undefined then key = -1
        local gi = findItem codes key
        if gi == 0 then
        (
            append codes key
            append groups #()
            gi = codes.count
        )
        append groups[gi] fc
    )
    local tagNodes = #()
    for gi = 1 to codes.count do
    (
        local code = codes[gi]
        local cname = "none"
        if code >= 0 then cname = DM_hex4 code
        local tcode = undefined
        if code >= 0 then tcode = code
        local tg = DM_Geometry flags:0 numVertices:0 positions:#() normals:undefined uvs:#() colors:undefined triangles:#() slots:#(#(DM_tagColor tcode, undefined)) sphere:undefined bones:undefined
        for fc in groups[gi] do
        (
            local base = tg.positions.count
            for j = 1 to 3 do append tg.positions ((DM_zgeToRw fc[j] o) - o)
            append tg.triangles #(base, base + 1, base + 2, 0)
        )
        tg.numVertices = tg.positions.count
        local node = DM_buildMesh ("tag_" + cname) tg scale
        node.material = DM_buildMaterial (name + "_tag_" + cname) tg.slots #()
        node.transform = rootM
        node.parent = tagsGrp
        node.isHidden = true
        local label = "none"
        if code >= 0 then label = code as string
        setUserProp node "dmbmx_tag_code" label
        append tagNodes node
    )
    -- scene points
    local ptsGrp = DM_group "ScenePoints" root rootM scale
    local pointNodes = #()
    for sp in lv.points do
    (
        local node = dummy name:sp.name boxsize:[0.5 * scale, 0.5 * scale, 0.5 * scale]
        local rows = DM_matrixToRw sp.m9
        local t = (DM_posToRw sp.pos o) - o
        node.transform = (matrix3 rows[1] rows[2] rows[3] (t * scale)) * rootM
        node.parent = ptsGrp
        setUserProp node "dmbmx_point_type" (sp.ptype as string)
        append pointNodes node
    )
    -- panorama: the sky model at the world origin
    local pano = undefined
    if lv.panorama != undefined then
    (
        local pg = DM_modelGeometry lv.panorama
        pano = DM_buildMesh "Panorama" pg scale
        if exactNormals then DM_setExplicitNormals pano pg flip:flipNormals
        pano.material = DM_buildMaterial (name + "_Panorama") pg.slots texfiles
        pano.transform = rootM
        pano.parent = root
    )
    #(root, world, propNodes, tagNodes, pointNodes, pano, tfolder)
)

-- ============================================================================ scene units (real-world size)
-- Game units are metres. 3ds Max's System Unit Setup says 1 system unit = units.SystemScale units.SystemType
-- (for example 5 #centimeters), so the import scale that keeps 1 game metre = 1 real metre is
-- 1 / (metres per system unit). Mirrors dave_mirra_import.scene_units / real_world_scale / scale_label.

-- #(metres, abbreviation) of a units.SystemType name value
fn DM_unitInfo t =
(
    if t == #inches then return #(0.0254, "in")
    if t == #feet then return #(0.3048, "ft")
    if t == #miles then return #(1609.344, "mi")
    if t == #millimeters then return #(0.001, "mm")
    if t == #centimeters then return #(0.01, "cm")
    if t == #meters then return #(1.0, "m")
    if t == #kilometers then return #(1000.0, "km")
    DM_fail ("unknown system unit type: " + (t as string))
)

-- metres in one 3ds Max system unit (units.SystemScale x units.SystemType)
fn DM_metresPerSystemUnit =
(
    local s = units.SystemScale as float
    if s <= 0.0 then DM_fail "the System Unit Setup scale must be positive"
    local info = DM_unitInfo units.SystemType
    s * info[1]
)

-- the import scale that gives real-world size in this scene (1 game metre = 1 m)
fn DM_realWorldScale = (1.0 / DM_metresPerSystemUnit())

-- a scale for the label: 4 significant digits, whole numbers from 1000 up (mirrors dave_mirra_import.fmt_scale)
fn DM_fmtNum v =
(
    local f = v as float
    if f >= 1000.0 then return (formattedPrint f format:".0f")
    formattedPrint f format:".4g"
)

-- the dialog's units label. manualScale: undefined in real-world mode, else the manual multiplier
fn DM_scaleLabel manualScale =
(
    local info = DM_unitInfo units.SystemType
    local mpu = DM_metresPerSystemUnit()
    local s = "Scene: 1 unit = " + (formattedPrint (units.SystemScale as float) format:"g") + " " + info[2]
    if manualScale == undefined then
    (
        local r = 1.0 / mpu
        s + " -> scale " + (DM_fmtNum r) + " (game 1 m = " + (DM_fmtNum r) + " units)"
    ) else (
        s + " -> manual scale " + (DM_fmtNum manualScale) + ": 1 game metre = " + (DM_fmtNum (manualScale * mpu)) + " m in this scene"
    )
)

-- ============================================================================ UI
-- The dialog: a header with the options every import uses (scale, LOD, exact normals, textures), then a Models
-- tab (ZMD import; a motion from motion.zal onto a model in the scene) and a Levels tab (level import).
-- The tabs are a .NET TabControl used as a tab strip (a dotNetControl only as tall as its page headers) above a
-- subRollout. Switching tabs removes the shown tab rollout from the subRollout and adds the other one, so the
-- pages hold native MAXScript controls, which works the same from 3ds Max 2017 on. A rollout added to a
-- subRollout starts from its controls' defaults every time, so the tabs keep the user's choices in DM_uiState,
-- which lasts for the 3ds Max session (the last tab, the textures folder, the motion archive, ...).
-- The test suite runs this section too, with the interpreter's model of rollouts and controls.

struct DM_DialogState (tab, texDir, upright, bones, zalPath, motion, chain, start, flip)
global DM_uiState
if DM_uiState == undefined then DM_uiState = DM_DialogState tab:1 texDir:"" upright:true bones:1 zalPath:"" motion:1 chain:true start:0 flip:false

global DaveMirraRollout
global DM_ModelsTab
global DM_LevelsTab
try (destroyDialog DaveMirraRollout) catch ()

rollout DM_ModelsTab "Models"
(
    group "Geometry"
    (
        checkbox chkUpright "Stand riders upright" checked:true
        dropdownlist ddlBones "Bones" items:#("aim at children", "game frame orientation") selection:1
        button btnZmd "Import ZMD..." width:240 height:28
    )
    group "Animation"
    (
        button btnZal "Motion archive (motion.zal)..." width:240
        label lblZal "(no motion archive)" align:#left width:300
        dropdownlist ddlMotion "Motion" items:#() width:300
        checkbox chkChain "Start after the last key (chain)" checked:true
        spinner spnStart "Start frame" range:[-100000, 100000, 0] type:#integer enabled:false
        button btnMotion "Load motion onto the rider" width:240 height:28
        label lblTarget "Onto the selected model (a bone or its root), or the only one in the scene." align:#left width:300 height:30
    )

    local motionIds = #()

    -- fill the motion list from the archive's DM_ListMotions entries
    fn showMotions list =
    (
        local labels = #()
        motionIds = #()
        for e in list do
        (
            append labels (DM_motionLabel e)
            append motionIds e[1]
        )
        ddlMotion.items = labels
        if DM_uiState.motion >= 1 and DM_uiState.motion <= labels.count then ddlMotion.selection = DM_uiState.motion
        lblZal.text = DM_uiState.zalPath
    )
    -- chain: the spinner shows the frame after the target's last key (when there is one target)
    fn refreshStart =
    (
        spnStart.enabled = not chkChain.checked
        if chkChain.checked then
        (
            try (spnStart.value = DM_nextFreeFrame (DM_findTarget undefined)) catch ()
            DM_uiState.start = spnStart.value
        )
    )

    on DM_ModelsTab open do
    (
        chkUpright.checked = DM_uiState.upright
        ddlBones.selection = DM_uiState.bones
        chkChain.checked = DM_uiState.chain
        spnStart.value = DM_uiState.start
        if DM_uiState.zalPath != "" then
        (
            try (showMotions (DM_ListMotions DM_uiState.zalPath))
            catch
            (
                DM_uiState.zalPath = ""
                lblZal.text = "(no motion archive)"
            )
        )
        refreshStart()
    )
    on chkUpright changed state do DM_uiState.upright = state
    on ddlBones selected i do DM_uiState.bones = i
    on ddlMotion selected i do DM_uiState.motion = i
    on chkChain changed state do
    (
        DM_uiState.chain = state
        refreshStart()
    )
    on spnStart changed val do DM_uiState.start = val
    on btnZmd pressed do
    (
        local p = getOpenFileName caption:"Dave Mirra model" types:"Dave Mirra model (*.zmd)|*.zmd|All files (*.*)|*.*|"
        if p != undefined do
        (
            DaveMirraRollout.refreshUnits()
            try
            (
                local r = DM_ImportZmd p (DaveMirraRollout.importScale()) (DaveMirraRollout.spnLod.value as integer) chkUpright.checked DaveMirraRollout.edtTex.text exactNormals:DaveMirraRollout.chkNormals.checked boneAim:(ddlBones.selection == 1)
                DaveMirraRollout.lblInfo.text = "Imported " + r[1].name + ". Textures: " + r[4]
                refreshStart()
            )
            catch (messageBox (getCurrentException()) title:"Dave Mirra")
        )
    )
    on btnZal pressed do
    (
        local p = getOpenFileName caption:"Dave Mirra motions (motion.zal)" types:"Dave Mirra motions (*.zal)|*.zal|All files (*.*)|*.*|"
        if p != undefined do
        (
            try
            (
                local list = DM_ListMotions p
                DM_uiState.zalPath = p
                DM_uiState.motion = 1
                showMotions list
                refreshStart()
            )
            catch (messageBox (getCurrentException()) title:"Dave Mirra")
        )
    )
    on btnMotion pressed do
    (
        if motionIds.count == 0 or ddlMotion.selection < 1 then messageBox "Choose the motion archive (motion.zal) first." title:"Dave Mirra" else
        (
            try
            (
                local start = undefined
                if not chkChain.checked then start = spnStart.value
                local m = motionIds[ddlMotion.selection]
                local r = DM_ImportMotion DM_uiState.zalPath m startFrame:start
                if chkChain.checked then spnStart.value = r[3] + 1
                DM_uiState.start = spnStart.value
                DaveMirraRollout.lblInfo.text = "Motion " + (m as string) + " on " + r[1].name + ": frames " + (r[2] as string) + " to " + (r[3] as string)
            )
            catch (messageBox (getCurrentException()) title:"Dave Mirra")
        )
    )
)

rollout DM_LevelsTab "Levels"
(
    checkbox chkFlip "Flip normals to faces (with Exact normals)" checked:false
    button btnLevel "Import Level (.ZSD)..." width:240 height:28

    on DM_LevelsTab open do chkFlip.checked = DM_uiState.flip
    on chkFlip changed state do DM_uiState.flip = state
    on btnLevel pressed do
    (
        local d = getSavePath caption:"Level folder (contains the .ZSD)"
        if d != undefined do
        (
            DaveMirraRollout.refreshUnits()
            try
            (
                local r = DM_ImportLevel d (DaveMirraRollout.importScale()) DaveMirraRollout.edtTex.text exactNormals:DaveMirraRollout.chkNormals.checked flipNormals:chkFlip.checked
                DaveMirraRollout.lblInfo.text = "Level imported. Textures: " + r[7]
            )
            catch (messageBox (getCurrentException()) title:"Dave Mirra")
        )
    )
)

rollout DaveMirraRollout "Dave Mirra Importer" width:360
(
    checkbox chkReal "Real-world size (use scene units)" checked:true
    spinner spnScale "Scale" range:[0.000001, 1000000000, 100] type:#float enabled:false
    label lblUnits "" align:#left width:330 height:30
    button btnUnits "Refresh scene units" width:240
    spinner spnLod "LOD" range:[0, 9, 0] type:#integer
    checkbox chkNormals "Exact normals (Edit_Normals, slower)" checked:false
    edittext edtTex "Textures" text:"" labelOnTop:true
    button btnTex "Choose texture folder..." width:240
    dotNetControl tabs "System.Windows.Forms.TabControl" width:340 height:24
    subRollout subTabs "" width:340 height:390
    label lblInfo "Textures go to %TEMP% if empty." align:#left width:330 height:30

    local shown = undefined

    -- real-world mode: the spinner shows the scene's scale (read-only); manual mode: the spinner is the raw multiplier
    fn refreshUnits =
    (
        try
        (
            if chkReal.checked then spnScale.value = DM_realWorldScale()
            spnScale.enabled = not chkReal.checked
            lblUnits.text = substituteString (DM_scaleLabel (if chkReal.checked then undefined else spnScale.value)) " -> " "\n-> "
        )
        catch (lblUnits.text = getCurrentException())
    )
    fn importScale = (if chkReal.checked then DM_realWorldScale() else spnScale.value)
    -- show tab i (1 Models, 2 Levels) in the subRollout
    fn showTab i =
    (
        local r = DM_ModelsTab
        if i == 2 then r = DM_LevelsTab
        if shown != r then
        (
            if shown != undefined then removeSubRollout subTabs shown
            addSubRollout subTabs r
            shown = r
        )
        DM_uiState.tab = i
    )

    on DaveMirraRollout open do
    (
        edtTex.text = DM_uiState.texDir
        refreshUnits()
        for t in #("Models", "Levels") do tabs.TabPages.Add (dotNetObject "System.Windows.Forms.TabPage" t)
        tabs.SelectedIndex = DM_uiState.tab - 1
        showTab DM_uiState.tab
    )
    on tabs SelectedIndexChanged s e do showTab (tabs.SelectedIndex + 1)
    on chkReal changed state do refreshUnits()
    on spnScale changed val do refreshUnits()
    on btnUnits pressed do refreshUnits()
    on edtTex changed txt do DM_uiState.texDir = txt
    on btnTex pressed do
    (
        local d = getSavePath caption:"Folder for the extracted textures"
        if d != undefined do
        (
            edtTex.text = d
            DM_uiState.texDir = d
        )
    )
)
createDialog DaveMirraRollout
