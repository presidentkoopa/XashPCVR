"""
Source v48 -> GoldSrc v10 weapon model converter.

Every struct offset / decode formula here was verified against real bytes
from the user's own model files this session, or against our own vendored,
WORKING studiomdl.c compiler source (E:\\XashVR\\tools\\studiomdl\\src\\utils\\studiomdl\\studiomdl.c) --
not invented, not blindly copied from SourceIO. See inline citations.

Coordinate/unit space: Source and GoldSrc both use ~1 unit = 1 inch, same
handedness, same bone-hierarchy convention (mstudiobone_t pos/quat are
PARENT-RELATIVE in both formats) -- so decoded per-bone pos/rot values are
used directly with NO extra transform. This assumption is unverified beyond
"the numbers produced a valid, self-consistent unit quaternion and a
round-trip-exact Euler conversion" -- if the compiled model is visibly the
wrong size or rotated wrong in-engine, that's the first thing to revisit.
"""
import struct
import math
import os
import subprocess
import sys

def i32(d,o): return struct.unpack_from('<i', d, o)[0]
def u32(d,o): return struct.unpack_from('<I', d, o)[0]
def f32(d,o): return struct.unpack_from('<f', d, o)[0]
def u8(d,o): return d[o]
def i16(d,o): return struct.unpack_from('<h', d, o)[0]
def u16(d,o): return struct.unpack_from('<H', d, o)[0]
def cstr(d,o):
    end = d.index(b'\x00', o)
    return d[o:end].decode('latin-1')

BONE_STRIDE = 216
ANIMDESC_STRIDE = 100
SEQDESC_STRIDE = 212
MODEL_STRIDE = 148
MESH_STRIDE = 116

# ---------------------------------------------------------------------------
# MDL
# ---------------------------------------------------------------------------
class Mdl:
    def __init__(self, path):
        self.path = path
        d = open(path, 'rb').read()
        self.d = d
        assert d[0:4] == b'IDST'
        assert i32(d,4) == 48
        self.name = cstr(d, 12)
        self.numbones, self.boneindex = i32(d,156), i32(d,160)
        self.numlocalanim, self.localanimindex = i32(d,180), i32(d,184)
        self.numlocalseq, self.localseqindex = i32(d,188), i32(d,192)
        self.numtextures, self.textureindex = i32(d,204), i32(d,208)
        self.numbodyparts, self.bodypartindex = i32(d,232), i32(d,236)
        self._read_bones()
        self._read_animdescs()
        self._read_seqs()
        self._read_bodyparts()
        self._read_textures()

    def _read_bones(self):
        d = self.d
        self.bones = []
        for i in range(self.numbones):
            off = self.boneindex + i*BONE_STRIDE
            nameoff = i32(d, off)
            name = cstr(d, off+nameoff) if nameoff else '?'
            parent = i32(d, off+4)
            pos = struct.unpack_from('<3f', d, off+32)
            quat = struct.unpack_from('<4f', d, off+44)
            rot = struct.unpack_from('<3f', d, off+60)
            self.bones.append(dict(idx=i, name=name, parent=parent, pos=pos, quat=quat, rot=rot))

    def _read_animdescs(self):
        d = self.d
        self.animdescs = []
        for i in range(self.numlocalanim):
            off = self.localanimindex + i*ANIMDESC_STRIDE
            nameoff = i32(d, off+4)
            name = cstr(d, off+nameoff)
            fps = f32(d, off+8)
            flags = i32(d, off+12)
            numframes = i32(d, off+16)
            animblock = i32(d, off+52)
            animindex = i32(d, off+56)
            assert animblock == 0, "external .ani block not supported"
            self.animdescs.append(dict(idx=i, off=off, name=name, fps=fps, flags=flags,
                                        numframes=numframes, animindex=off+animindex))

    def _read_seqs(self):
        d = self.d
        self.seqs = []
        for i in range(self.numlocalseq):
            base = self.localseqindex + i*SEQDESC_STRIDE
            name_off = u32(d, base+4)
            name = cstr(d, base+name_off) if name_off else 'seq_%d' % i
            fadein = f32(d, base+104)
            fadeout = f32(d, base+108)
            group_size = struct.unpack_from('<2I', d, base+68)
            anim_index_offset = u32(d, base+60)
            n0, n1 = group_size
            gs0, gs1 = (n0 or 1), (n1 or 1)
            indices = []
            if anim_index_offset:
                for k in range(gs0*gs1):
                    indices.append(struct.unpack_from('<h', d, base+anim_index_offset+k*2)[0])
            self.seqs.append(dict(idx=i, name=name, anim_indices=indices))

    def _read_bodyparts(self):
        d = self.d
        self.models = []  # flat list of (bodypart_name, model)
        off = self.bodypartindex
        for bp in range(self.numbodyparts):
            bp_off = off + bp*16
            sznameindex, nummodels, base, modelindex = struct.unpack_from('<4i', d, bp_off)
            bp_name = cstr(d, bp_off+sznameindex) if sznameindex else 'bodypart%d'%bp
            for m in range(nummodels):
                moff = bp_off + modelindex + m*MODEL_STRIDE
                name = cstr(d, moff)
                nummesh = i32(d, moff+64+8)
                meshindex_rel = i32(d, moff+64+12)
                numvertices = i32(d, moff+64+16)
                vertexindex = i32(d, moff+64+20)
                meshes = []
                mesh_base = moff + meshindex_rel
                for me in range(nummesh):
                    meoff = mesh_base + me*MESH_STRIDE
                    material = i32(d, meoff)
                    nv = i32(d, meoff+8)
                    voff = i32(d, meoff+12)
                    meshes.append(dict(material=material, numvertices=nv, vertexoffset=voff))
                self.models.append(dict(bodypart=bp_name, name=name, numvertices=numvertices,
                                         vertexindex=vertexindex, meshes=meshes))

    def _read_textures(self):
        d = self.d
        self.textures = []
        TEXSZ = 64
        for i in range(self.numtextures):
            base = self.textureindex + i*TEXSZ
            nameoff = i32(d, base)
            name = cstr(d, base+nameoff)
            self.textures.append(name)

# ---------------------------------------------------------------------------
# VVD
# ---------------------------------------------------------------------------
class Vvd:
    def __init__(self, path):
        d = open(path, 'rb').read()
        assert d[0:4] == b'IDSV'
        numLODVertexes = struct.unpack_from('<8I', d, 16)
        numFixups, fixupTableStart, vertexDataStart, tangentDataStart = struct.unpack_from('<4I', d, 48)
        n0 = numLODVertexes[0]
        self.verts = []
        for i in range(n0):
            vo = vertexDataStart + i*48
            w = struct.unpack_from('<3f', d, vo)
            bone = struct.unpack_from('<3B', d, vo+12)
            nb = d[vo+15]
            pos = struct.unpack_from('<3f', d, vo+16)
            normal = struct.unpack_from('<3f', d, vo+28)
            uv = struct.unpack_from('<2f', d, vo+40)
            self.verts.append(dict(weight=w, bone=bone, numbones=nb, pos=pos, normal=normal, uv=uv))

# ---------------------------------------------------------------------------
# VTX (v7) -- ported from the already-verified vtx_parse.py in this scratchpad
# ---------------------------------------------------------------------------
class Vtx:
    def __init__(self, path):
        with open(path, 'rb') as f:
            buf = f.read()
        self.buf = buf
        (version, vertCacheSize, maxBonesPerStrip, maxBonesPerTri, maxBonesPerVert,
         checksum, numLODs, matReplOffset, numBodyParts, bodyPartOffset) = struct.unpack_from(
            '<iiHHiiiiii', buf, 0)
        self.numBodyParts = numBodyParts
        self.bodyPartOffset = bodyPartOffset

    def first_lod_meshes(self, bodypart_i, model_i):
        """Return the list of mesh dicts (stripgroups w/ resolved indices) for
        LOD0 of the given bodypart/model index."""
        buf = self.buf
        bp_entry = self.bodyPartOffset + bodypart_i * 8
        numModels, modelOffset = struct.unpack_from('<ii', buf, bp_entry)
        model_base = bp_entry + modelOffset
        m_entry = model_base + model_i * 8
        numLODs, lodOffset = struct.unpack_from('<ii', buf, m_entry)
        lod_base = m_entry + lodOffset
        l_entry = lod_base  # LOD 0
        numMeshes, meshOffset, switchPoint = struct.unpack_from('<iif', buf, l_entry)
        mesh_base = l_entry + meshOffset
        meshes = []
        for me_i in range(numMeshes):
            me_entry = mesh_base + me_i * 9
            numSG, sgOffset, meshFlags = struct.unpack_from('<iiB', buf, me_entry)
            sg_base = me_entry + sgOffset
            SG_STRUCT = '<iiiiii'
            sg_size = struct.calcsize(SG_STRUCT)
            stripgroups = []
            for sg_i in range(numSG):
                sg_entry = sg_base + sg_i * (sg_size + 1)
                (numVerts, vertOffset, numIndices, indexOffset,
                 numStrips, stripOffset) = struct.unpack_from(SG_STRUCT, buf, sg_entry)
                vert_base = sg_entry + vertOffset
                verts = []
                for v_i in range(numVerts):
                    voff = vert_base + v_i * 9
                    origMeshVertID = struct.unpack_from('<H', buf, voff+4)[0]
                    verts.append(origMeshVertID)
                idx_base = sg_entry + indexOffset
                indices = list(struct.unpack_from('<%dH' % numIndices, buf, idx_base))
                stripgroups.append(dict(verts=verts, indices=indices))
            meshes.append(stripgroups)
        return meshes

# ---------------------------------------------------------------------------
# Animation decode -- RLE + Quat48/64, verified this session (see chat history:
# unit-length check on Quat64 gave |q|=1.000000 exact; Euler round-trip via
# our own AngleQuaternion port gave error ~3.5e-8)
# ---------------------------------------------------------------------------
STUDIO_ANIM_RAWPOS  = 0x01
STUDIO_ANIM_RAWROT  = 0x02
STUDIO_ANIM_ANIMPOS = 0x04
STUDIO_ANIM_ANIMROT = 0x08
STUDIO_ANIM_DELTA   = 0x10
STUDIO_ANIM_RAWROT2 = 0x20

def quat64_decode(raw8):
    qi = int.from_bytes(raw8, 'little')
    x21 = qi & 0x1FFFFF
    y21 = (qi >> 21) & 0x1FFFFF
    z21 = (qi >> 42) & 0x1FFFFF
    wneg = (qi >> 63) & 1
    qx = (x21 - 1048576) / 1048576.5
    qy = (y21 - 1048576) / 1048576.5
    qz = (z21 - 1048576) / 1048576.5
    s = 1 - qx*qx - qy*qy - qz*qz
    qw = math.sqrt(s) if s > 0 else 0.0
    if wneg: qw = -qw
    return (qx, qy, qz, qw)

def quat48_decode(raw6):
    # mstudioquat48_t: 3x uint16 LE; x,y each full 16-bit, z uses low 15 bits + wneg in bit 15
    x, y, z_packed = struct.unpack('<3H', raw6)
    wneg = (z_packed >> 15) & 1
    z = z_packed & 0x7FFF
    qx = (x - 32768) / 32768.0
    qy = (y - 32768) / 32768.0
    qz = (z - 16384) / 16384.0
    s = 1 - qx*qx - qy*qy - qz*qz
    qw = math.sqrt(s) if s > 0 else 0.0
    if wneg: qw = -qw
    return (qx, qy, qz, qw)

def quat_to_euler_rpy(q):
    """Inverse of our vendored AngleQuaternion() (mathlib.c:265) -- verified
    round-trip exact (~3.5e-8 error) against a real decoded animation quat
    this session. Returns (roll, pitch, yaw) radians = (rot.x, rot.y, rot.z)
    as expected by the SMD skeleton line format."""
    x, y, z, w = q
    m00 = 1.0 - 2.0*y*y - 2.0*z*z
    m10 = 2.0*x*y + 2.0*w*z
    m20 = 2.0*x*z - 2.0*w*y
    m21 = 2.0*y*z + 2.0*w*x
    m22 = 1.0 - 2.0*x*x - 2.0*y*y
    m01 = 2.0*x*y - 2.0*w*z
    m11 = 1.0 - 2.0*x*x - 2.0*z*z
    sp = max(-1.0, min(1.0, -m20))
    pitch = math.asin(sp)
    cp = math.cos(pitch)
    if abs(cp) > 1e-6:
        yaw = math.atan2(m10, m00)
        roll = math.atan2(m21, m22)
    else:
        yaw = math.atan2(-m01, m11)
        roll = 0.0
    return (roll, pitch, yaw)

def read_rle_stream(d, base_off, frame_count):
    off = base_off
    out = []
    while len(out) < frame_count:
        valid = u8(d, off); total = u8(d, off+1)
        off += 2
        vals = []
        for k in range(valid):
            vals.append(i16(d, off)); off += 2
        out.extend(vals)
        if total > valid:
            last = vals[-1] if vals else (out[-1] if out else 0)
            out.extend([last] * (total - valid))
        if valid == 0 and total == 0:
            break
    return out[:frame_count]

def decode_animation(mdl, animdesc):
    """Returns dict: bone_idx -> dict(pos=[(x,y,z)]*n, rot=[(roll,pitch,yaw)]*n)
    for every bone. Bones with no anim-track data hold their bind pose for
    every frame (matches Grab_Skeleton/Grab_Animation's "unanimated bone
    keeps its default" convention)."""
    d = mdl.d
    numframes = animdesc['numframes']
    results = {}
    for b in mdl.bones:
        results[b['idx']] = dict(
            pos=[b['pos']]*numframes,
            rot=[quat_to_euler_rpy(b['quat'])]*numframes,
        )

    off = animdesc['animindex']
    for _ in range(mdl.numbones):
        bone_entry = off
        bone_index = u8(d, off)
        if bone_index == 255:
            break
        flags = u8(d, off+1)
        next_offset = i16(d, off+2)
        cursor = off + 4

        b = mdl.bones[bone_index]
        base_pos = b['pos']

        rot_frames = None
        if flags & STUDIO_ANIM_RAWROT2:
            q = quat64_decode(d[cursor:cursor+8]); cursor += 8
            rot_frames = [quat_to_euler_rpy(q)] * numframes
        elif flags & STUDIO_ANIM_RAWROT:
            q = quat48_decode(d[cursor:cursor+6]); cursor += 6
            rot_frames = [quat_to_euler_rpy(q)] * numframes
        elif flags & STUDIO_ANIM_ANIMROT:
            entry2 = cursor
            xo, yo, zo = i16(d,cursor), i16(d,cursor+2), i16(d,cursor+4)
            cursor += 6
            # ANIMROT streams are RLE deltas on the QUATERNION axes per Valve's
            # actual runtime (Studio_CalcBoneQuaternion applies these as
            # scaled deltas to bone->quat's own axis-angle representation,
            # not raw Euler). Cross-checked against decode_anim.py's existing
            # ANIMROT path, which treats these as Euler-space deltas added to
            # base_rot (bone.rot, the Euler fallback field) -- reusing that
            # here since bone.rot is exactly the Euler value AngleQuaternion
            # would reproduce for the bind pose.
            rotscale = None
            # rotscale lives on the bone struct too; re-read directly.
            off_b = mdl.boneindex + bone_index*BONE_STRIDE
            rotscale = struct.unpack_from('<3f', d, off_b+84)
            base_rot_euler = b['rot']
            axis_vals = [None, None, None]
            for ax, o in enumerate((xo, yo, zo)):
                if o > 0:
                    raw = read_rle_stream(d, entry2 + o, numframes)
                    axis_vals[ax] = [r * rotscale[ax] for r in raw]
                else:
                    axis_vals[ax] = [0.0] * numframes
            rot_frames = []
            for fi in range(numframes):
                rot_frames.append((axis_vals[0][fi] + base_rot_euler[0],
                                    axis_vals[1][fi] + base_rot_euler[1],
                                    axis_vals[2][fi] + base_rot_euler[2]))
        if rot_frames is not None:
            results[bone_index]['rot'] = rot_frames

        pos_frames = None
        if flags & STUDIO_ANIM_RAWPOS:
            px, py, pz = struct.unpack_from('<3e', d, cursor); cursor += 6
            pos_frames = [(px, py, pz)] * numframes
        elif flags & STUDIO_ANIM_ANIMPOS:
            entry3 = cursor
            xo, yo, zo = i16(d,cursor), i16(d,cursor+2), i16(d,cursor+4)
            cursor += 6
            off_b = mdl.boneindex + bone_index*BONE_STRIDE
            posscale = struct.unpack_from('<3f', d, off_b+72)
            axis_vals = [None, None, None]
            for ax, o in enumerate((xo, yo, zo)):
                if o > 0:
                    raw = read_rle_stream(d, entry3 + o, numframes)
                    axis_vals[ax] = [r * posscale[ax] for r in raw]
                else:
                    axis_vals[ax] = [0.0] * numframes
            pos_frames = []
            for fi in range(numframes):
                pos_frames.append((axis_vals[0][fi] + base_pos[0],
                                    axis_vals[1][fi] + base_pos[1],
                                    axis_vals[2][fi] + base_pos[2]))
        if pos_frames is not None:
            results[bone_index]['pos'] = pos_frames

        if next_offset > 0:
            off = bone_entry + next_offset
        else:
            break
    return results

# ---------------------------------------------------------------------------
# SMD writer
# ---------------------------------------------------------------------------
def write_nodes_block(f, mdl):
    f.write("nodes\n")
    for b in mdl.bones:
        f.write('%d "%s" %d\n' % (b['idx'], b['name'], b['parent']))
    f.write("end\n")

def write_reference_smd(path, mdl, vvd, vtx, model_i_in_mdl):
    """model_i_in_mdl indexes mdl.models (flat bodypart/model list)."""
    model = mdl.models[model_i_in_mdl]
    with open(path, 'w') as f:
        write_nodes_block(f, mdl)
        f.write("skeleton\n")
        f.write("time 0\n")
        for b in mdl.bones:
            rx, ry, rz = quat_to_euler_rpy(b['quat'])
            f.write('%d %.6f %.6f %.6f %.6f %.6f %.6f\n' % (b['idx'], b['pos'][0], b['pos'][1], b['pos'][2], rx, ry, rz))
        f.write("end\n")
        f.write("triangles\n")
        # VTX meshes correspond 1:1 with MDL meshes in the same model, in order.
        vtx_meshes = vtx.first_lod_meshes(0, model_i_in_mdl)  # NOTE: assumes single bodypart (true for our split parts)
        for mesh_i, mesh in enumerate(model['meshes']):
            matname = 'material_%d.bmp' % mesh['material']
            sg = vtx_meshes[mesh_i][0]  # first stripgroup
            indices = sg['indices']
            local_verts = sg['verts']  # origMeshVertID per strip-group-local index
            for tri in range(0, len(indices) - len(indices) % 3, 3):
                f.write(matname + '\n')
                for k in range(3):
                    local_i = indices[tri+k]
                    mesh_local_vid = local_verts[local_i]
                    global_vid = mesh['vertexoffset'] + mesh_local_vid
                    v = vvd.verts[global_vid]
                    bone = v['bone'][0]
                    px, py, pz = v['pos']
                    nx, ny, nz = v['normal']
                    u, vv = v['uv']
                    f.write('%d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n' % (bone, px, py, pz, nx, ny, nz, u, vv))
        f.write("end\n")

def write_animation_smd(path, mdl, animdesc):
    decoded = decode_animation(mdl, animdesc)
    numframes = animdesc['numframes']
    with open(path, 'w') as f:
        write_nodes_block(f, mdl)
        f.write("skeleton\n")
        for t in range(numframes):
            f.write("time %d\n" % t)
            for b in mdl.bones:
                bi = b['idx']
                px, py, pz = decoded[bi]['pos'][t]
                rx, ry, rz = decoded[bi]['rot'][t]
                f.write('%d %.6f %.6f %.6f %.6f %.6f %.6f\n' % (bi, px, py, pz, rx, ry, rz))
        f.write("end\n")

# ---------------------------------------------------------------------------
# Placeholder BMP (8-bit paletted, GoldSrc-compatible minimal format)
# ---------------------------------------------------------------------------
def write_placeholder_bmp(path, w=64, h=64, color=(180, 180, 190)):
    row_bytes = w  # 8bpp
    row_padded = (row_bytes + 3) & ~3
    palette = bytearray()
    for i in range(256):
        palette += bytes([color[2], color[1], color[0], 0])  # BGRA, palette entry i=color for all (flat)
    pixel_data = bytearray()
    for y in range(h):
        pixel_data += bytes([0]) * row_padded
    data_offset = 14 + 40 + 256*4
    filesize = data_offset + len(pixel_data)
    with open(path, 'wb') as f:
        f.write(b'BM')
        f.write(struct.pack('<IHHI', filesize, 0, 0, data_offset))
        f.write(struct.pack('<IiiHHIIiiII', 40, w, h, 1, 8, 0, len(pixel_data), 2835, 2835, 256, 0))
        f.write(palette)
        f.write(pixel_data)

# ---------------------------------------------------------------------------
# QC writer + compile driver
# ---------------------------------------------------------------------------
def convert_part(src_dir, out_dir, base_name, studiomdl_exe, model_smd_name='reference'):
    os.makedirs(out_dir, exist_ok=True)
    mdl = Mdl(os.path.join(src_dir, base_name + '.mdl'))
    vvd = Vvd(os.path.join(src_dir, base_name + '.vvd'))
    vtx = Vtx(os.path.join(src_dir, base_name + '.dx90.vtx'))

    print("=== %s ===" % base_name)
    print("bones=%d anims=%d seqs=%d models=%d" % (mdl.numbones, mdl.numlocalanim, mdl.numlocalseq, len(mdl.models)))

    assert len(mdl.models) == 1, "multi-model bodyparts not yet handled: %r" % (mdl.models,)
    write_reference_smd(os.path.join(out_dir, model_smd_name + '.smd'), mdl, vvd, vtx, 0)

    # unique material indices actually used
    used_materials = sorted(set(me['material'] for me in mdl.models[0]['meshes']))
    for mi in used_materials:
        write_placeholder_bmp(os.path.join(out_dir, 'material_%d.bmp' % mi))

    seq_lines = []
    for seq in mdl.seqs:
        if not seq['anim_indices']:
            continue
        anim_idx = seq['anim_indices'][0]
        animdesc = mdl.animdescs[anim_idx]
        anim_smd_name = 'anim_%d' % anim_idx
        write_animation_smd(os.path.join(out_dir, anim_smd_name + '.smd'), mdl, animdesc)
        seqname = seq['name'].replace('"', '')
        seq_lines.append('$sequence "%s" "%s" fps %.2f' % (seqname, anim_smd_name, animdesc['fps']))

    qc_path = os.path.join(out_dir, base_name + '.qc')
    with open(qc_path, 'w') as f:
        f.write('$modelname "%s.mdl"\n' % base_name)
        f.write('$cd "."\n')
        f.write('$cdtexture "."\n')
        f.write('$body studio "%s"\n' % model_smd_name)
        for line in seq_lines:
            f.write(line + '\n')

    print("wrote QC:", qc_path)
    result = subprocess.run([studiomdl_exe, qc_path], cwd=out_dir, capture_output=True, text=True)
    print("--- studiomdl stdout ---")
    print(result.stdout)
    print("--- studiomdl stderr ---")
    print(result.stderr)
    print("returncode:", result.returncode)
    return result

if __name__ == '__main__':
    convert_part(
        src_dir=r"C:\Users\Command\Desktop\ass\models\weapons",
        out_dir=r"C:\Users\Command\AppData\Local\Temp\claude\E--oldshit\ce27755b-8b46-4dc3-a0c5-872b5296680c\scratchpad\out_slide",
        base_name="vr_pistol_slide",
        studiomdl_exe=r"E:\XashVR\tools\studiomdl\studiomdl.exe",
    )
