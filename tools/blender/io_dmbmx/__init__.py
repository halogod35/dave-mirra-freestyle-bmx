"""Dave Mirra Freestyle BMX (.zmd) import/export for Blender 5.3 (DM-022).

File > Import > Dave Mirra ZMD (.zmd), File > Export > Dave Mirra ZMD (.zmd), and a "Dave Mirra" tab
in the 3D View sidebar (N) with Export / Replace Texture buttons. User guide: docs/tools/blender-addon.md.

The add-on is a thin layer over the ``dmbmx`` library (vendored under ``vendor/`` by
tools/blender/build_blender_addon.py); it never uses Blender's glTF exporter for ZMD output.
"""
import os
import sys

_VENDOR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vendor")
if os.path.isdir(_VENDOR) and _VENDOR not in sys.path:
    sys.path.insert(0, _VENDOR)

bl_info = {  # used when installed as a legacy add-on; extensions use blender_manifest.toml
    "name": "Dave Mirra ZMD (.zmd)",
    "author": "dmbmx project",
    "version": (0, 1, 0),
    "blender": (4, 2, 0),
    "location": "File > Import/Export > Dave Mirra ZMD; 3D View > Sidebar > Dave Mirra",
    "description": "Import/export Dave Mirra Freestyle BMX models (.zmd) losslessly",
    "category": "Import-Export",
}

import bpy  # noqa: E402
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, StringProperty  # noqa: E402
from bpy_extras.io_utils import ExportHelper, ImportHelper  # noqa: E402

from . import zmd_export, zmd_import  # noqa: E402

BINMESH_ITEMS = [("auto", "Same as original", "Regenerated triangle strips/lists keep the original form (safest)"),
                 ("strips", "Strips", "Always write triangle strips"),
                 ("lists", "Lists", "Always write plain triangle lists")]
PAL_ITEMS = [("promote", "Promote to 24-bit", "Edited 4/8-bit textures become 24-bit BMPs (exact colours)"),
             ("quantize", "Keep palette", "Map edited colours to the nearest colour of the original palette")]


def _active_root(context):
    ob = context.active_object
    root = zmd_export.find_root(ob) if ob else None
    if root is None:
        roots = [o for o in context.scene.objects if "dmbmx_gltf" in o]
        if len(roots) == 1:
            root = roots[0]
    return root


class DMBMX_OT_import_zmd(bpy.types.Operator, ImportHelper):
    """Import a Dave Mirra Freestyle BMX model (.zmd)"""
    bl_idname = "import_scene.dmbmx_zmd"
    bl_label = "Import Dave Mirra ZMD"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".zmd"
    filter_glob: StringProperty(default="*.zmd;*.ZMD", options={"HIDDEN"})
    lod: IntProperty(name="LOD", default=-1, min=-1,
                     description="Level of detail to import (-1 = all; only DaveMirra_shadow has two)")
    upright: BoolProperty(name="Stand riders upright", default=True,
                          description="Riders are stored lying down; rotate them to stand on the floor")
    scale: FloatProperty(name="Scale", default=1.0, min=0.001, max=1000.0,
                         description="Display scale of the model (does not change the exported file)")
    texture_dir: StringProperty(name="Texture folder", subtype="DIR_PATH", default="",
                                description="Write textures here as PNG files and link them, so you can paint "
                                            "them in any program (empty = pack them into the .blend)")

    def execute(self, context):
        try:
            root = zmd_import.load(context, self.filepath, lod=self.lod, upright=self.upright, scale=self.scale,
                                   texture_dir=self.texture_dir)
        except Exception as e:  # report every failure to the user instead of a traceback
            self.report({"ERROR"}, f"Import failed: {e}")
            return {"CANCELLED"}
        for ob in context.selected_objects:
            ob.select_set(False)
        root.select_set(True)
        context.view_layer.objects.active = root
        self.report({"INFO"}, f"Imported {root.name}")
        return {"FINISHED"}


class DMBMX_OT_export_zmd(bpy.types.Operator, ExportHelper):
    """Export the selected Dave Mirra model back to a .zmd file"""
    bl_idname = "export_scene.dmbmx_zmd"
    bl_label = "Export Dave Mirra ZMD"
    filename_ext = ".zmd"
    filter_glob: StringProperty(default="*.zmd;*.ZMD", options={"HIDDEN"})
    binmesh: EnumProperty(name="Triangle layout", items=BINMESH_ITEMS, default="auto")
    palettised: EnumProperty(name="Edited 16/256-colour textures", items=PAL_ITEMS, default="promote")
    normals: EnumProperty(name="Normals", default="keep", items=[
        ("keep", "Keep original", "Use the game's original normals (edits carry them along); best for small edits"),
        ("recalc", "Recalculate", "Use Blender's current shading normals (after big shape changes)")])
    strict: BoolProperty(name="Fail on warnings", default=False)
    copy_to_testgame: BoolProperty(name="Copy into _testgame", default=False,
                                   description="Also replace the same-named file in _testgame\\assets "
                                               "(the test copy of the game; never the real install)")

    def invoke(self, context, event):
        root = _active_root(context)
        if root is not None and not self.filepath:
            self.filepath = os.path.join(os.path.expanduser("~"), root.get("dmbmx_source_name", root.name + ".zmd"))
        return super().invoke(context, event)

    def execute(self, context):
        root = _active_root(context)
        if root is None:
            self.report({"ERROR"}, "Select an object of an imported Dave Mirra model first")
            return {"CANCELLED"}
        try:
            rep, notes, dest = zmd_export.save(root, self.filepath, binmesh=self.binmesh, palettised=self.palettised,
                                               strict=self.strict, copy_to_testgame=self.copy_to_testgame,
                                               normals=self.normals)
        except Exception as e:
            self.report({"ERROR"}, f"Export failed: {e}")
            return {"CANCELLED"}
        for n in notes:
            self.report({"INFO"}, n)
        for c in rep.changes:
            self.report({"INFO"}, c)
        for w in rep.warnings:
            self.report({"WARNING"}, w)
        msg = f"Exported {root.name} to {self.filepath}" + (f" and copied to {dest}" if dest else "")
        if not rep.changes and not notes:
            msg += " (no changes: identical to the original)"
        self.report({"INFO"}, msg)
        return {"FINISHED"}


def _image_items(self, context):
    root = _active_root(context)
    if root is None:
        return [("", "(no model)", "")]
    uid = root.get("dmbmx_uid")
    imgs = sorted((i for i in bpy.data.images if i.get("dmbmx_owner") == uid), key=lambda i: i["dmbmx_image"])
    return [(i.name, i.name, f"{i.size[0]}x{i.size[1]}") for i in imgs] or [("", "(no textures)", "")]


class DMBMX_OT_replace_texture(bpy.types.Operator, ImportHelper):
    """Replace one of the model's textures with a picture file (checked against the game's rules)"""
    bl_idname = "dmbmx.replace_texture"
    bl_label = "Replace Texture"
    bl_options = {"REGISTER", "UNDO"}
    filter_glob: StringProperty(default="*.png;*.bmp;*.PNG;*.BMP", options={"HIDDEN"})
    image: EnumProperty(name="Texture", items=_image_items)
    resize: BoolProperty(name="Resize to the original size", default=True)

    def execute(self, context):
        from dmbmx.convert import imaging
        from dmbmx.formats import texture
        root = _active_root(context)
        img = bpy.data.images.get(self.image)
        if root is None or img is None:
            self.report({"ERROR"}, "Select a Dave Mirra model and one of its textures")
            return {"CANCELLED"}
        import json
        js = json.loads(root["dmbmx_gltf"])
        rx = zmd_export.gltf_ex(js["nodes"][root["dmbmx_node"]])
        it = next((x for x in rx["items"] if x.get("type") == "MATI" and x.get("image") == img["dmbmx_image"]), None)
        kind = "png" if it and it.get("img_tag") == "TPNG" else "bmp"
        try:
            data = open(self.filepath, "rb").read()
            out, notes = texture.prepare_replacement(data, kind, size=tuple(img.size), resize=self.resize)
            _, warns = texture.check(out, kind)
        except Exception as e:
            self.report({"ERROR"}, f"Cannot use this picture: {e}")
            return {"CANCELLED"}
        png = out if kind == "png" else imaging.bmp_to_png(out)
        set_image_png(img, png)
        for n in notes + warns:
            self.report({"WARNING"}, n)
        self.report({"INFO"}, f"Replaced {img.name}" + (" (magenta 255,0,255 = transparent in game)" if kind == "bmp" else ""))
        return {"FINISHED"}


def set_image_png(img, png):
    """Replace a Blender image's content with PNG bytes (keeps it packed or file-linked)."""
    import tempfile
    path = bpy.path.abspath(img.filepath) if img.packed_file is None else ""
    if path and os.path.isfile(path):
        with open(path, "wb") as f:
            f.write(png)
        img.reload()
        return
    fd, tmp = tempfile.mkstemp(suffix=".png")
    with os.fdopen(fd, "wb") as f:
        f.write(png)
    try:
        old = img.filepath
        if img.packed_file is not None:
            img.unpack(method="REMOVE")
        img.filepath = tmp
        img.reload()
        img.pack()
        img.filepath = old
    finally:
        os.unlink(tmp)


class DMBMX_OT_import_combo(bpy.types.Operator, ImportHelper):
    """Import a rider on a bike, animated with motion.zal clips (view only: uses Blender's glTF importer,
    so the result is not exportable back to .zmd)"""
    bl_idname = "import_scene.dmbmx_combo"
    bl_label = "Import Dave Mirra Rider on Bike"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".zmd"
    filter_glob: StringProperty(default="*.zmd;*.ZMD", options={"HIDDEN"})
    bike_path: StringProperty(name="Bike .zmd", subtype="FILE_PATH", default="",
                              description="Bike model (empty = DMbike01.zmd next to the rider file)")
    motion_zal: StringProperty(name="motion.zal", subtype="FILE_PATH", default="",
                               description="Animation bank (empty = motion.zal next to the rider file)")
    anims: StringProperty(name="Clips", default="8",
                          description="Motion ids: 8, 1,5,10-12 or all (8 = pedalling)")
    spin_wheels: BoolProperty(name="Spin wheels", default=True,
                              description="Turn the wheels with the cranks (gear ratio 44/16)")

    def execute(self, context):
        import tempfile
        from dmbmx.cli.export_combo import _ids
        from dmbmx.convert import combo
        from dmbmx.formats import motion, zmd
        folder = os.path.dirname(self.filepath)
        bike = self.bike_path or os.path.join(folder, "DMbike01.zmd")
        mz = self.motion_zal or os.path.join(folder, "motion.zal")
        tmp = None
        try:
            rdata = open(self.filepath, "rb").read()
            bdata = open(bike, "rb").read()
            bank = motion.read_bank(open(mz, "rb").read())
            clips = [(f"motion_{i:03d}", bank.motion(i)) for i in _ids(self.anims, bank.ids)]
            glb = combo.export_combo(zmd.read(rdata), zmd.read(bdata), clips, rider_source=rdata,
                                     bike_source=bdata, rider_name=os.path.splitext(os.path.basename(self.filepath))[0],
                                     bike_name=os.path.splitext(os.path.basename(bike))[0],
                                     spin_wheels=self.spin_wheels)
            fd, tmp = tempfile.mkstemp(suffix=".glb")
            os.close(fd)
            open(tmp, "wb").write(glb)
            bpy.ops.import_scene.gltf(filepath=tmp)
        except BaseException as e:  # SystemExit from _ids, file errors, format errors
            self.report({"ERROR"}, f"Import failed: {e}")
            return {"CANCELLED"}
        finally:
            if tmp and os.path.exists(tmp):
                os.unlink(tmp)
        self.report({"INFO"}, f"Imported rider on bike with {len(clips)} clip(s)")
        return {"FINISHED"}


class DMBMX_PT_panel(bpy.types.Panel):
    bl_label = "Dave Mirra ZMD"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Dave Mirra"

    def draw(self, context):
        col = self.layout.column()
        root = _active_root(context)
        col.operator(DMBMX_OT_import_zmd.bl_idname, icon="IMPORT")
        col.operator(DMBMX_OT_import_combo.bl_idname, icon="ARMATURE_DATA")
        if root is None:
            col.label(text="No Dave Mirra model selected")
            return
        col.label(text=f"Model: {root.name}")
        col.label(text=f"From: {root.get('dmbmx_source_name', '?')}")
        col.operator(DMBMX_OT_export_zmd.bl_idname, icon="EXPORT")
        col.operator(DMBMX_OT_replace_texture.bl_idname, icon="IMAGE_DATA")


def _menu_import(self, context):
    self.layout.operator(DMBMX_OT_import_zmd.bl_idname, text="Dave Mirra ZMD (.zmd)")
    self.layout.operator(DMBMX_OT_import_combo.bl_idname, text="Dave Mirra rider on bike (animated)")


def _menu_export(self, context):
    self.layout.operator(DMBMX_OT_export_zmd.bl_idname, text="Dave Mirra ZMD (.zmd)")


CLASSES = (DMBMX_OT_import_zmd, DMBMX_OT_import_combo, DMBMX_OT_export_zmd, DMBMX_OT_replace_texture, DMBMX_PT_panel)


def register():
    for c in CLASSES:
        bpy.utils.register_class(c)
    bpy.types.TOPBAR_MT_file_import.append(_menu_import)
    bpy.types.TOPBAR_MT_file_export.append(_menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(_menu_export)
    bpy.types.TOPBAR_MT_file_import.remove(_menu_import)
    for c in reversed(CLASSES):
        bpy.utils.unregister_class(c)
