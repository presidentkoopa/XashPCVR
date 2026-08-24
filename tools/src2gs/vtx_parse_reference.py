import struct, sys, os

def read_at(buf, off, fmt):
    size = struct.calcsize(fmt)
    return struct.unpack_from(fmt, buf, off)

def parse_vvd_vertex_count(vvd_path):
    with open(vvd_path, 'rb') as f:
        data = f.read()
    # vertexFileHeader_t: id(4) version(4) checksum(4) numLODs(4) numLODVertexes[8](32)
    # numFixups(4) fixupTableStart(4) vertexDataStart(4) tangentDataStart(4)
    id_, ver, checksum, numLODs = struct.unpack_from('<4i', data, 0)
    numLODVertexes = struct.unpack_from('<8i', data, 16)
    numFixups, fixupTableStart, vertexDataStart, tangentDataStart = struct.unpack_from('<4i', data, 48)
    return {
        'id': id_, 'version': ver, 'checksum': checksum, 'numLODs': numLODs,
        'numLODVertexes': numLODVertexes, 'numFixups': numFixups,
        'fixupTableStart': fixupTableStart, 'vertexDataStart': vertexDataStart,
        'tangentDataStart': tangentDataStart, 'totalVerts_lod0': numLODVertexes[0],
        'filesize': len(data)
    }

class VtxFile:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.buf = f.read()
        self.path = path
        self.parse_header()

    def parse_header(self):
        buf = self.buf
        # FileHeader_t (36 bytes)
        (version, vertCacheSize, maxBonesPerStrip, maxBonesPerTri, maxBonesPerVert,
         checksum, numLODs, matReplOffset, numBodyParts, bodyPartOffset) = struct.unpack_from(
            '<iiHHiiiiii', buf, 0)
        self.header = dict(version=version, vertCacheSize=vertCacheSize,
                            maxBonesPerStrip=maxBonesPerStrip, maxBonesPerTri=maxBonesPerTri,
                            maxBonesPerVert=maxBonesPerVert, checksum=checksum,
                            numLODs=numLODs, matReplOffset=matReplOffset,
                            numBodyParts=numBodyParts, bodyPartOffset=bodyPartOffset)
        self.header_size = struct.calcsize('<iiHHiiiiii')

    def parse_bodyparts(self):
        buf = self.buf
        h = self.header
        bodyparts = []
        for bp_i in range(h['numBodyParts']):
            bp_entry = h['bodyPartOffset'] + bp_i * 8  # numModels(4) modelOffset(4)
            numModels, modelOffset = struct.unpack_from('<ii', buf, bp_entry)
            models = []
            model_base = bp_entry + modelOffset
            for m_i in range(numModels):
                m_entry = model_base + m_i * 8  # numLODs(4) lodOffset(4)
                numLODs, lodOffset = struct.unpack_from('<ii', buf, m_entry)
                lods = []
                lod_base = m_entry + lodOffset
                for l_i in range(numLODs):
                    l_entry = lod_base + l_i * 12  # numMeshes(4) meshOffset(4) switchPoint(4 float)
                    numMeshes, meshOffset, switchPoint = struct.unpack_from('<iif', buf, l_entry)
                    meshes = []
                    mesh_base = l_entry + meshOffset
                    for me_i in range(numMeshes):
                        me_entry = mesh_base + me_i * 9  # numStripGroups(4) sgOffset(4) meshflags(1)
                        numSG, sgOffset, meshFlags = struct.unpack_from('<iiB', buf, me_entry)
                        stripgroups = []
                        sg_base = me_entry + sgOffset
                        SG_STRUCT = '<iiiiii'  # numVerts vertOffset numIndices indexOffset numStrips stripOffset
                        sg_size_before_flag = struct.calcsize(SG_STRUCT)
                        for sg_i in range(numSG):
                            sg_entry = sg_base + sg_i * (sg_size_before_flag + 1)
                            (numVerts, vertOffset, numIndices, indexOffset,
                             numStrips, stripOffset) = struct.unpack_from(SG_STRUCT, buf, sg_entry)
                            sgFlags = struct.unpack_from('<B', buf, sg_entry + sg_size_before_flag)[0]

                            # vertices: Vertex_t 9 bytes each
                            vert_base = sg_entry + vertOffset
                            verts = []
                            for v_i in range(numVerts):
                                voff = vert_base + v_i * 9
                                bwi = struct.unpack_from('<3B', buf, voff)
                                numBones = struct.unpack_from('<B', buf, voff+3)[0]
                                origMeshVertID = struct.unpack_from('<H', buf, voff+4)[0]
                                boneID = struct.unpack_from('<3b', buf, voff+6)
                                verts.append(dict(boneWeightIndex=bwi, numBones=numBones,
                                                   origMeshVertID=origMeshVertID, boneID=boneID))

                            # indices: uint16 each
                            idx_base = sg_entry + indexOffset
                            indices = list(struct.unpack_from('<%dH' % numIndices, buf, idx_base))

                            # strips
                            strips = []
                            strip_base = sg_entry + stripOffset
                            STRIP_STRUCT = '<iiiiHBii'  # indexCount indexMeshOffset vertCount vertMeshOffset boneCount(H) flags(B) boneStateChangeCount boneStateChangeOffset
                            strip_size = struct.calcsize(STRIP_STRUCT)
                            for st_i in range(numStrips):
                                st_entry = strip_base + st_i * strip_size
                                (indexCount, indexMeshOffset, vertCount, vertMeshOffset,
                                 boneCount, flags_, bscCount, bscOffset) = struct.unpack_from(STRIP_STRUCT, buf, st_entry)
                                strips.append(dict(indexCount=indexCount, indexMeshOffset=indexMeshOffset,
                                                    vertCount=vertCount, vertMeshOffset=vertMeshOffset,
                                                    boneCount=boneCount, flags=flags_,
                                                    boneStateChangeCount=bscCount, boneStateChangeOffset=bscOffset,
                                                    entry_off=st_entry, struct_size=strip_size))

                            stripgroups.append(dict(numVerts=numVerts, vertOffset=vertOffset,
                                                     numIndices=numIndices, indexOffset=indexOffset,
                                                     numStrips=numStrips, stripOffset=stripOffset,
                                                     flags=sgFlags, verts=verts, indices=indices, strips=strips,
                                                     entry_off=sg_entry, struct_size_before_flag=sg_size_before_flag))
                        meshes.append(dict(numStripGroups=numSG, sgOffset=sgOffset, flags=meshFlags,
                                            stripgroups=stripgroups, entry_off=me_entry))
                    lods.append(dict(numMeshes=numMeshes, meshOffset=meshOffset, switchPoint=switchPoint,
                                      meshes=meshes, entry_off=l_entry))
                models.append(dict(numLODs=numLODs, lodOffset=lodOffset, lods=lods, entry_off=m_entry))
            bodyparts.append(dict(numModels=numModels, modelOffset=modelOffset, models=models, entry_off=bp_entry))
        self.bodyparts = bodyparts


def dump_summary(vtx_path, vvd_path, label):
    print("=" * 70)
    print(label)
    print("=" * 70)
    filesize = os.path.getsize(vtx_path)
    v = VtxFile(vtx_path)
    print(f"file: {vtx_path}  size={filesize}")
    print(f"FileHeader_t (struct size={v.header_size} bytes):")
    for k, val in v.header.items():
        print(f"    {k} = {val}")
    vvd = parse_vvd_vertex_count(vvd_path)
    print(f"\nCompanion VVD: {vvd_path}")
    print(f"  vertexFileHeader_t: numLODs={vvd['numLODs']} numLODVertexes={vvd['numLODVertexes']} "
          f"vertexDataStart={vvd['vertexDataStart']} totalVerts(lod0)={vvd['totalVerts_lod0']} filesize={vvd['filesize']}")

    v.parse_bodyparts()
    for bp_i, bp in enumerate(v.bodyparts):
        print(f"\nBodyPartHeader_t[{bp_i}] @ off {bp['entry_off']}: numModels={bp['numModels']} modelOffset={bp['modelOffset']} (rel to bp entry)")
        for m_i, m in enumerate(bp['models']):
            print(f"  ModelHeader_t[{m_i}] @ off {m['entry_off']}: numLODs={m['numLODs']} lodOffset={m['lodOffset']} (rel to model entry)")
            for l_i, lod in enumerate(m['lods']):
                print(f"    ModelLODHeader_t[{l_i}] @ off {lod['entry_off']}: numMeshes={lod['numMeshes']} meshOffset={lod['meshOffset']} switchPoint={lod['switchPoint']}")
                for me_i, me in enumerate(lod['meshes']):
                    print(f"      MeshHeader_t[{me_i}] @ off {me['entry_off']}: numStripGroups={me['numStripGroups']} sgOffset={me['sgOffset']} meshFlags={me['flags']:#04x}")
                    for sg_i, sg in enumerate(me['stripgroups']):
                        print(f"        StripGroupHeader_t[{sg_i}] @ off {sg['entry_off']}: "
                              f"numVerts={sg['numVerts']} vertOffset={sg['vertOffset']} "
                              f"numIndices={sg['numIndices']} indexOffset={sg['indexOffset']} "
                              f"numStrips={sg['numStrips']} stripOffset={sg['stripOffset']} "
                              f"flags={sg['flags']:#04x} (IS_FLEXED={bool(sg['flags']&1)}, IS_HWSKINNED={bool(sg['flags']&2)}, IS_DELTA_FLEXED={bool(sg['flags']&4)})")
                        for st_i, st in enumerate(sg['strips']):
                            print(f"          StripHeader_t[{st_i}] @ off {st['entry_off']} (struct size={st['struct_size']}): "
                                  f"indexCount={st['indexCount']} indexMeshOffset={st['indexMeshOffset']} "
                                  f"vertCount={st['vertCount']} vertMeshOffset={st['vertMeshOffset']} "
                                  f"boneCount={st['boneCount']} flags={st['flags']:#04x} "
                                  f"(IS_TRILIST={bool(st['flags']&1)}, IS_QUADLIST_REG={bool(st['flags']&2)}, IS_QUADLIST_EXTRA={bool(st['flags']&4)}) "
                                  f"boneStateChangeCount={st['boneStateChangeCount']} boneStateChangeOffset={st['boneStateChangeOffset']}")

                        # bone id stats
                        boneids = set()
                        for vv in sg['verts']:
                            boneids.update(vv['boneID'])
                        numbones_vals = set(vv['numBones'] for vv in sg['verts'])
                        print(f"        --> Vertex_t stats: numVerts={len(sg['verts'])} distinct boneID values={sorted(boneids)} distinct numBones values={sorted(numbones_vals)}")

                        # first 12 indices -> resolve to vvd index
                        first_idx = sg['indices'][:12]
                        print(f"        --> first 12 raw indices (into strip-group vertex array): {first_idx}")
                        resolved = []
                        for ii in first_idx:
                            vt = sg['verts'][ii]
                            resolved.append(vt['origMeshVertID'])
                        print(f"        --> resolved origMeshVertID (mesh-local vvd index, since mesh.vertexoffset=0 for LOD0 single mesh): {resolved}")
                        print(f"        --> sanity: max origMeshVertID in this stripgroup = {max(vv['origMeshVertID'] for vv in sg['verts'])}, vvd totalVerts(lod0)={vvd['totalVerts_lod0']}")
                        # print a few full vertex records
                        print(f"        --> sample Vertex_t records (first 3): ")
                        for vv in sg['verts'][:3]:
                            print(f"            {vv}")
    return v, vvd


if __name__ == '__main__':
    pistol_vtx = r"C:\Users\Command\Desktop\ass\models\weapons\vr_pistol.dx90.vtx"
    pistol_vvd = r"C:\Users\Command\Desktop\ass\models\weapons\vr_pistol.vvd"
    player_vtx = r"C:\Users\Command\Desktop\hndz\content\models\player.dx90.vtx"
    player_vvd = r"C:\Users\Command\Desktop\hndz\content\models\player.vvd"

    dump_summary(pistol_vtx, pistol_vvd, "VR_PISTOL.DX90.VTX")
    dump_summary(player_vtx, player_vvd, "PLAYER.DX90.VTX (hands)")
