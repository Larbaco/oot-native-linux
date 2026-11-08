"""A small SPIR-V module model (words in, words out) and the Vulkan edits nxshader makes to the translator's SPIR-V. Written from the SPIR-V 1.6 specification
(Khronos, the binary form and the instruction table); the edits mirror what nxnative's Metal chain did to its MSL where Vulkan needs an equivalent
(docs/m5-record-shaders.md, "The SPIR-V edits"). Every pass has a name and a version: both go into the cache key of what it produces (nxs_cache.py)."""
import os, struct

MAGIC = 0x07230203
OpLoopMerge, TERMINATORS = 246, {249, 250, 251, 252, 253, 254, 255}   # OpBranch, OpBranchConditional, OpSwitch, OpKill, OpReturn, OpReturnValue, OpUnreachable
# opcodes used here (SPIR-V specification, 3.52)
OpName, OpMemberName, OpExtension, OpExtInstImport, OpMemoryModel, OpEntryPoint, OpExecutionMode, OpCapability = 5, 6, 10, 11, 14, 15, 16, 17
OpTypeVoid, OpTypeBool, OpTypeInt, OpTypeFloat, OpTypeVector, OpTypeMatrix, OpTypeImage, OpTypeSampler, OpTypeSampledImage = 19, 20, 21, 22, 23, 24, 25, 26, 27
OpTypeArray, OpTypeRuntimeArray, OpTypeStruct, OpTypePointer, OpTypeFunction = 28, 29, 30, 32, 33
OpConstantTrue, OpConstantFalse, OpConstant, OpConstantComposite, OpConstantNull = 41, 42, 43, 44, 46
OpFunction, OpFunctionParameter, OpFunctionEnd, OpVariable, OpLoad, OpStore, OpAccessChain, OpInBoundsAccessChain = 54, 55, 56, 59, 61, 62, 65, 66
OpFunctionCall = 57
OpDecorate, OpMemberDecorate, OpCompositeConstruct, OpCompositeExtract, OpVectorShuffle, OpCopyObject = 71, 72, 80, 81, 79, 83
OpImageRead, OpImageWrite, OpBitcast, OpFAdd, OpFMul, OpLabel, OpReturn, OpReturnValue, OpEmitVertex, OpEmitStreamVertex = 98, 99, 124, 129, 133, 248, 253, 254, 218, 220
OpExecutionModeId, OpModuleProcessed = 331, 330
# decorations, builtins, storage classes, capabilities, execution models
DecBuiltIn, DecLocation, DecComponent, DecIndex, DecBinding, DecDescriptorSet, DecBlock, DecFlat = 11, 30, 31, 32, 33, 34, 2, 14
BuiltInPosition, BuiltInPointSize, BuiltInLayer, BuiltInBaseInstance = 0, 1, 9, 4425
SC_UniformConstant, SC_Input, SC_Uniform, SC_Output, SC_Workgroup, SC_Private, SC_Function, SC_PushConstant, SC_StorageBuffer = 0, 1, 2, 3, 4, 6, 7, 9, 12
EXEC_MODELS = {0: 'vertex', 1: 'tess_control', 2: 'tess_eval', 3: 'geometry', 4: 'fragment', 5: 'compute'}
# capability names (spirv.h of the Vulkan SDK 1.4.363, the ones these modules can carry)
CAP_NAMES = {0: 'Matrix', 1: 'Shader', 2: 'Geometry', 3: 'Tessellation', 4: 'Addresses', 5: 'Linkage', 6: 'Kernel', 7: 'Vector16', 8: 'Float16Buffer', 9: 'Float16', 10: 'Float64',
             11: 'Int64', 12: 'Int64Atomics', 13: 'ImageBasic', 14: 'ImageReadWrite', 15: 'ImageMipmap', 17: 'Pipes', 18: 'Groups', 19: 'DeviceEnqueue', 20: 'LiteralSampler', 21:
             'AtomicStorage', 22: 'Int16', 23: 'TessellationPointSize', 24: 'GeometryPointSize', 25: 'ImageGatherExtended', 27: 'StorageImageMultisample', 28:
             'UniformBufferArrayDynamicIndexing', 29: 'SampledImageArrayDynamicIndexing', 30: 'StorageBufferArrayDynamicIndexing', 31: 'StorageImageArrayDynamicIndexing', 32:
             'ClipDistance', 33: 'CullDistance', 34: 'ImageCubeArray', 35: 'SampleRateShading', 36: 'ImageRect', 37: 'SampledRect', 38: 'GenericPointer', 39: 'Int8', 40:
             'InputAttachment', 41: 'SparseResidency', 42: 'MinLod', 43: 'Sampled1D', 44: 'Image1D', 45: 'SampledCubeArray', 46: 'SampledBuffer', 47: 'ImageBuffer', 48:
             'ImageMSArray', 49: 'StorageImageExtendedFormats', 50: 'ImageQuery', 51: 'DerivativeControl', 52: 'InterpolationFunction', 53: 'TransformFeedback', 54:
             'GeometryStreams', 55: 'StorageImageReadWithoutFormat', 56: 'StorageImageWriteWithoutFormat', 57: 'MultiViewport', 58: 'SubgroupDispatch', 59: 'NamedBarrier', 60:
             'PipeStorage', 61: 'GroupNonUniform', 62: 'GroupNonUniformVote', 63: 'GroupNonUniformArithmetic', 64: 'GroupNonUniformBallot', 65: 'GroupNonUniformShuffle', 66:
             'GroupNonUniformShuffleRelative', 67: 'GroupNonUniformClustered', 68: 'GroupNonUniformQuad', 69: 'ShaderLayer', 70: 'ShaderViewportIndex', 71: 'UniformDecoration',
             4422: 'FragmentShadingRateKHR', 4423: 'SubgroupBallotKHR', 4427: 'DrawParameters', 4428: 'WorkgroupMemoryExplicitLayoutKHR', 4429:
             'WorkgroupMemoryExplicitLayout8BitAccessKHR', 4430: 'WorkgroupMemoryExplicitLayout16BitAccessKHR', 4431: 'SubgroupVoteKHR', 4433: 'StorageBuffer16BitAccess', 4434:
             'StorageUniform16', 4435: 'StoragePushConstant16', 4436: 'StorageInputOutput16', 4437: 'DeviceGroup', 4439: 'MultiView', 4441: 'VariablePointersStorageBuffer', 4442:
             'VariablePointers', 4445: 'AtomicStorageOps', 4447: 'SampleMaskPostDepthCoverage', 4448: 'StorageBuffer8BitAccess', 4449: 'UniformAndStorageBuffer8BitAccess', 4450:
             'StoragePushConstant8', 4464: 'DenormPreserve', 4465: 'DenormFlushToZero', 4466: 'SignedZeroInfNanPreserve', 4467: 'RoundingModeRTE', 4468: 'RoundingModeRTZ', 4471:
             'RayQueryProvisionalKHR', 4472: 'RayQueryKHR', 4473: 'UntypedPointersKHR', 4478: 'RayTraversalPrimitiveCullingKHR', 4479: 'RayTracingKHR', 4484:
             'TextureSampleWeightedQCOM', 4485: 'TextureBoxFilterQCOM', 4486: 'TextureBlockMatchQCOM', 4495: 'TileShadingQCOM', 4496: 'CooperativeMatrixConversionQCOM', 4498:
             'TextureBlockMatch2QCOM', 5009: 'ImageGatherBiasLodAMD', 5013: 'StencilExportEXT', 5015: 'ImageReadWriteLodAMD', 5016: 'Int64ImageEXT', 5249:
             'SampleMaskOverrideCoverageNV', 5251: 'GeometryShaderPassthroughNV', 5254: 'ShaderViewportIndexLayerEXT', 5255: 'ShaderViewportMaskNV', 5297:
             'GroupNonUniformPartitionedEXT', 5301: 'ShaderNonUniform', 5306: 'UniformBufferArrayNonUniformIndexing', 5345: 'VulkanMemoryModel', 5378:
             'FragmentShaderPixelInterlockEXT'}


def string_of(words):
    b = struct.pack(f'<{len(words)}I', *words)
    return b[:b.index(0)].decode('utf-8', 'replace') if 0 in b else b.decode('utf-8', 'replace')


def string_words(s):
    b = s.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return list(struct.unpack(f'<{len(b) // 4}I', b))


def string_len(words):   # words a nul-terminated literal string takes, from its start
    for i, w in enumerate(words):
        if any((w >> (8 * k)) & 0xff == 0 for k in range(4)): return i + 1
    return len(words)


class Module:
    def __init__(self, data):
        if len(data) % 4 or len(data) < 20: raise ValueError('not a SPIR-V module (size)')
        w = list(struct.unpack(f'<{len(data) // 4}I', data))
        if w[0] != MAGIC: raise ValueError('not a SPIR-V module (magic)')
        self.header = w[:5]
        self.ins = []
        i = 5
        while i < len(w):
            n = w[i] >> 16
            if n == 0 or i + n > len(w): raise ValueError(f'bad instruction length at word {i}')
            self.ins.append(w[i:i + n]); i += n
        self.reindex()

    def bytes(self):
        flat = self.header + [x for i in self.ins for x in i]
        return struct.pack(f'<{len(flat)}I', *flat)

    @property
    def version(self): return (self.header[1] >> 16 & 0xff, self.header[1] >> 8 & 0xff)

    def new_id(self):
        i = self.header[3]; self.header[3] += 1; return i

    def reindex(self):
        self.names, self.decor, self.member_decor, self.defs = {}, {}, {}, {}
        for k, x in enumerate(self.ins):
            op = x[0] & 0xffff
            if op == OpName: self.names[x[1]] = string_of(x[2:])
            elif op == OpDecorate: self.decor.setdefault(x[1], []).append((x[2], x[3:]))
            elif op == OpMemberDecorate: self.member_decor.setdefault(x[1], {}).setdefault(x[2], []).append((x[3], x[4:]))
            else:
                rid = result_id(x)
                if rid: self.defs[rid] = k

    def op(self, i): return self.ins[i][0] & 0xffff
    def inst(self, rid): return self.ins[self.defs[rid]] if rid in self.defs else None

    def deco(self, rid, d):
        for dd, args in self.decor.get(rid, ()):
            if dd == d: return args or [1]
        return None

    def caps(self): return [x[1] for x in self.ins if x[0] & 0xffff == OpCapability]
    def has_op(self, opcode): return any(x[0] & 0xffff == opcode for x in self.ins)

    def entry_points(self):
        out = []
        for k, x in enumerate(self.ins):
            if x[0] & 0xffff == OpEntryPoint:
                n = string_len(x[3:])
                out.append({'index': k, 'model': x[1], 'func': x[2], 'name': string_of(x[3:3 + n]), 'interface': x[3 + n:]})
        return out

    def type_desc(self, tid, depth=0):
        """a type as a small dict (recursive); pointers give their storage class and pointee"""
        x = self.inst(tid)
        if x is None or depth > 16: return {'t': '?'}
        op = x[0] & 0xffff
        if op == OpTypeVoid: return {'t': 'void'}
        if op == OpTypeBool: return {'t': 'bool'}
        if op == OpTypeInt: return {'t': 'int' if x[3] else 'uint', 'w': x[2]}
        if op == OpTypeFloat: return {'t': 'float', 'w': x[2]}
        if op == OpTypeVector: return {'t': 'vec', 'n': x[3], 'of': self.type_desc(x[2], depth + 1)}
        if op == OpTypeMatrix: return {'t': 'mat', 'n': x[3], 'of': self.type_desc(x[2], depth + 1)}
        if op == OpTypeArray: return {'t': 'array', 'n': self.const_value(x[3]), 'of': self.type_desc(x[2], depth + 1)}
        if op == OpTypeRuntimeArray: return {'t': 'array', 'n': None, 'of': self.type_desc(x[2], depth + 1)}
        if op == OpTypeStruct: return {'t': 'struct', 'id': tid, 'members': [self.type_desc(m, depth + 1) for m in x[2:]]}
        if op == OpTypePointer: return {'t': 'ptr', 'sc': x[2], 'of': self.type_desc(x[3], depth + 1)}
        if op == OpTypeImage: return {'t': 'image', 'sampled_type': self.type_desc(x[2], depth + 1), 'dim': x[3], 'depth': x[4], 'arrayed': x[5], 'ms': x[6], 'sampled': x[7], 'format': x[8]}
        if op == OpTypeSampler: return {'t': 'sampler'}
        if op == OpTypeSampledImage: return {'t': 'sampled_image', 'image': self.type_desc(x[2], depth + 1)}
        return {'t': 'op%d' % op}

    def const_value(self, cid):
        x = self.inst(cid)
        if x is None: return None
        op = x[0] & 0xffff
        if op == OpConstant: return x[3] if len(x) == 4 else x[3] | x[4] << 32
        return None

    def global_vars(self):
        """module-scope OpVariables: (id, storage class, pointer type id)"""
        out, in_func = [], False
        for x in self.ins:
            op = x[0] & 0xffff
            if op == OpFunction: in_func = True
            elif op == OpFunctionEnd: in_func = False
            elif op == OpVariable and not in_func: out.append((x[2], x[3], x[1]))
        return out

    # ---- building blocks for the edits ----
    def first_function_index(self):
        return next(k for k, x in enumerate(self.ins) if x[0] & 0xffff == OpFunction)

    def find_type(self, words_tail, op):
        """an existing type instruction with exactly these operands (after the result id), else None"""
        for x in self.ins:
            if x[0] & 0xffff == op and x[2:] == words_tail: return x[1]
        return None

    def ensure_type(self, op, tail):
        t = self.find_type(tail, op)
        if t: return t
        t = self.new_id(); k = self.first_function_index()
        self.ins.insert(self.types_end(), [(2 + len(tail)) << 16 | op, t] + tail); self.reindex(); return t

    def types_end(self):
        """where new types and constants go: after the last type, constant or global variable before the first function"""
        k = self.first_function_index()
        return k

    def ensure_const(self, type_id, value_words):
        for x in self.ins:
            if x[0] & 0xffff == OpConstant and x[1] == type_id and x[3:] == value_words: return x[2]
        c = self.new_id()
        self.ins.insert(self.types_end(), [(3 + len(value_words)) << 16 | OpConstant, type_id, c] + value_words); self.reindex(); return c

    def ensure_composite(self, type_id, parts):
        for x in self.ins:
            if x[0] & 0xffff == OpConstantComposite and x[1] == type_id and x[3:] == parts: return x[2]
        c = self.new_id()
        self.ins.insert(self.types_end(), [(3 + len(parts)) << 16 | OpConstantComposite, type_id, c] + parts); self.reindex(); return c

    def add_decoration(self, words):
        """an annotation (OpDecorate ...) at the end of the annotation section"""
        ann = [k for k, x in enumerate(self.ins) if x[0] & 0xffff in (71, 72, 73, 74, 332, 5632, 5633)]
        k = ann[-1] + 1 if ann else next(k for k, x in enumerate(self.ins) if x[0] & 0xffff in range(19, 47))
        self.ins.insert(k, words); self.reindex()

    def add_capability(self, cap):
        if cap in self.caps(): return False
        k = max(i for i, x in enumerate(self.ins) if x[0] & 0xffff == OpCapability) + 1
        self.ins.insert(k, [2 << 16 | OpCapability, cap]); self.reindex(); return True

    def function_body_start(self, func_id):
        """index of the first instruction of the function's first block after its OpLabel and leading OpVariables"""
        k = self.defs[func_id]
        while self.ins[k][0] & 0xffff != OpLabel: k += 1
        k += 1
        while self.ins[k][0] & 0xffff == OpVariable: k += 1
        return k

    def function_range(self, func_id):
        k = self.defs[func_id]; e = k
        while self.ins[e][0] & 0xffff != OpFunctionEnd: e += 1
        return k, e


def result_id(x):
    """the result id an instruction defines, or 0 (types and the instructions with a result type)"""
    op = x[0] & 0xffff
    if op in (OpTypeVoid, OpTypeBool, OpTypeInt, OpTypeFloat, OpTypeVector, OpTypeMatrix, OpTypeImage, OpTypeSampler, OpTypeSampledImage, OpTypeArray, OpTypeRuntimeArray,
              OpTypeStruct, 31, OpTypePointer, OpTypeFunction, 34, 35, 36, 37, 38, 39, OpLabel, OpExtInstImport, 322, 327, 5341):
        return x[1] if len(x) > 1 else 0
    if op in RESULT_TYPED and len(x) > 2: return x[2]
    return 0


# instructions with <result type> <result id> (the ones these modules use; the rest of the table only matters for ids we never look up)
RESULT_TYPED = set(range(41, 53)) | {1, 54, 55, 57, 59, 60, 61, 64, 65, 66, 67, 68, 69, 70, 73, 77, 78, 79, 80, 81, 82, 83, 84} | set(range(86, 106)) | set(range(109, 205)) | \
    set(range(207, 216)) | set(range(227, 243)) | {245, 246, 247, 251, 252} | set(range(262, 316)) | {12} | set(range(333, 367)) | {4421, 4424, 4429, 4430, 4432, 4433, 4434, 5249, 5250, 5251, 5252, 5253, 5296, 5297, 5333, 5334, 5335, 5336, 5337, 5338, 5339, 5340, 5380, 5381, 5383, 6086, 4450, 4453, 4454, 4455, 4456, 5000, 5001}


def float_bits(f): return struct.unpack('<I', struct.pack('<f', f))[0]


# ===================================================================================== the edits

class Pass:
    name, version, options = '', 1, ''
    def applies(self, m, stage): return False
    def run(self, m, stage): raise NotImplementedError
    def ident(self): return f'{self.name} v{self.version} {self.options}'.strip()


class VertexOutputInit(Pass):
    """research/01 lesson 3 (nxnative's vs_init in MSL): every user output of a vertex, tessellation-evaluation or geometry stage starts as (0,0,0,1), the value an
    unwritten attribute component reads on the hardware; the translator's outputs start undefined in Vulkan (RADV gives NaN, NVIDIA and AMD Windows 0). The stores
    go at the start of the entry function, before the translated body, which overwrites what it writes. A geometry shader is initialised once, at its start: after
    an EmitVertex the outputs are undefined again, and the translator's code writes every output it emits. Integer outputs get the bits of the same values
    (1.0f = 0x3f800000), as the hardware's untyped attribute slots hold them. A component-decorated output takes its part of the vector."""
    name, version = 'vout_init', 1

    def applies(self, m, stage): return stage in ('vs', 'gs', 'ds')

    def run(self, m, stage):
        ep = m.entry_points()[0]
        stores = []
        for vid, sc, ptr in m.global_vars():
            if sc != SC_Output or m.deco(vid, DecBuiltIn) is not None or m.deco(vid, DecLocation) is None: continue
            x = m.inst(vid)
            if len(x) > 4: continue   # has an initializer
            pointee = m.inst(ptr)[3]
            comp = (m.deco(vid, DecComponent) or [0])[0]
            c = self.value(m, pointee, comp)
            if c is not None: stores.append((vid, c))
        if not stores: return False
        k = m.function_body_start(ep['func'])
        m.ins[k:k] = [[3 << 16 | OpStore, v, c] for v, c in stores]
        m.reindex()
        return True

    def value(self, m, tid, comp):
        x = m.inst(tid); op = x[0] & 0xffff
        dflt = [0.0, 0.0, 0.0, 1.0]
        def scalar(st, i):
            s = m.inst(st); sop = s[0] & 0xffff
            v = dflt[i] if i < 4 else 0.0
            if sop == OpTypeFloat and s[2] == 32: return m.ensure_const(st, [float_bits(v)])
            if sop == OpTypeInt and s[2] == 32: return m.ensure_const(st, [float_bits(v)])
            return None
        if op in (OpTypeFloat, OpTypeInt): return scalar(tid, comp)
        if op == OpTypeVector:
            parts = [scalar(x[2], comp + i) for i in range(x[3])]
            return None if None in parts else m.ensure_composite(tid, parts)
        if op == OpTypeArray:
            e = self.value(m, x[2], comp)
            n = m.const_value(x[3])
            return None if e is None or not n or n > 64 else m.ensure_composite(tid, [e] * n)
        return None


class TrimCapabilities(Pass):
    """The translator declares capabilities a module never uses: Float64 and the other widths in modules without such a type, GeometryStreams without a stream,
    pixel interlock without an interlock. Vulkan requires the device feature for every declared capability before it creates the module (VkShaderModuleCreateInfo
    pCode-08740), and some GPUs lack them (no shaderFloat64 on Intel Arc, no geometryStreams without VK_EXT_transform_feedback, no fragment shader interlock on AMD).
    Removed when the module has nothing that needs it; a module that does use one keeps it, and its reflection says so"""
    name, version = 'caps_trim', 1
    WIDTH = {10: (OpTypeFloat, 64), 11: (OpTypeInt, 64), 9: (OpTypeFloat, 16), 22: (OpTypeInt, 16), 39: (OpTypeInt, 8)}
    KEEP_IF = {11: {12}}   # Int64Atomics implies Int64
    USES = {54: ({220, 221}, {29}, set()),                              # GeometryStreams: OpEmitStreamVertex / OpEndStreamPrimitive, the Stream decoration
            5378: ({5364, 5365}, set(), {5366, 5367, 5368, 5369, 5370, 5371})}   # FragmentShaderPixelInterlockEXT: Begin / EndInvocationInterlock, the interlock modes

    def applies(self, m, stage): return any(c in self.WIDTH or c in self.USES for c in m.caps())

    def run(self, m, stage):
        caps = set(m.caps()); drop = set()
        for c, (op, w) in self.WIDTH.items():
            if c in caps and not (self.KEEP_IF.get(c, set()) & caps) and not any(x[0] & 0xffff == op and x[2] == w for x in m.ins): drop.add(c)
        for c, (ops, decos, modes) in self.USES.items():
            if c in caps and not any(x[0] & 0xffff in ops or (x[0] & 0xffff == OpDecorate and x[2] in decos) or (x[0] & 0xffff == OpExecutionMode and x[2] in modes) for x in m.ins): drop.add(c)
        if not drop: return False
        m.ins = [x for x in m.ins if not (x[0] & 0xffff == OpCapability and x[1] in drop)]; m.reindex()
        return True


class StorageFormatCaps(Pass):
    """Reading or writing a storage image declared without a format needs StorageImageReadWithoutFormat / WriteWithoutFormat in Vulkan (Metal needed nothing):
    the translator declares Unknown for an image the answer table gives no format. The capabilities are added where the module does it"""
    name, version = 'storage_fmt_caps', 1

    def applies(self, m, stage):
        return any(x[0] & 0xffff == OpTypeImage and x[7] == 2 and x[8] == 0 for x in m.ins)

    def run(self, m, stage):
        ch = False
        if m.has_op(OpImageRead): ch |= m.add_capability(55)    # StorageImageReadWithoutFormat
        if m.has_op(OpImageWrite): ch |= m.add_capability(56)   # StorageImageWriteWithoutFormat
        return ch


class DepthGL(Pass):
    """Variant .gl: NVN's default depth convention (near = -w, the GL clip space) for a device without VK_EXT_depth_clip_control: z' = (z + w) / 2 before every
    return of the last pre-rasterisation stage (nxnative's main0 entry point in MSL, vs_depth). Pokopia sets near = 0 (nvnDeviceSetDepthMode), Vulkan's own convention,
    so its renderer needs no variant; the pass is here for the convention's other value"""
    name, version = 'depth_gl', 1

    def applies(self, m, stage): return stage in ('vs', 'gs', 'ds')

    def run(self, m, stage):
        pos = self.position(m)
        if not pos: return False
        var, chain = pos
        f32 = m.ensure_type(OpTypeFloat, [32])
        u32 = m.ensure_type(OpTypeInt, [32, 0])
        pf = m.ensure_type(OpTypePointer, [SC_Output, f32])
        half = m.ensure_const(f32, [float_bits(0.5)])
        idx = [m.ensure_const(u32, [i]) for i in chain]
        i2, i3 = m.ensure_const(u32, [2]), m.ensure_const(u32, [3])
        ep = m.entry_points()[0]
        lo, hi = m.function_range(ep['func'])
        stop = (OpEmitVertex, OpEmitStreamVertex) if stage == 'gs' else (OpReturn,)
        k, n = lo, 0
        while k < hi:
            if m.ins[k][0] & 0xffff in stop:
                pz, pw, z, w, s, h = (m.new_id() for _ in range(6))
                seq = [[(4 + len(idx) + 1) << 16 | OpAccessChain, pf, pz, var] + idx + [i2], [(4 + len(idx) + 1) << 16 | OpAccessChain, pf, pw, var] + idx + [i3],
                       [4 << 16 | OpLoad, f32, z, pz], [4 << 16 | OpLoad, f32, w, pw], [5 << 16 | OpFAdd, f32, s, z, w], [5 << 16 | OpFMul, f32, h, s, half], [3 << 16 | OpStore, pz, h]]
                m.ins[k:k] = seq; k += len(seq); hi += len(seq); n += 1
            k += 1
        m.reindex()
        return n > 0

    @staticmethod
    def position(m):
        """(variable, member indices of the vec4) of the Position output: a gl_PerVertex block member or a variable of its own"""
        for vid, sc, ptr in m.global_vars():
            if sc != SC_Output: continue
            if (m.deco(vid, DecBuiltIn) or [None])[0] == BuiltInPosition: return vid, []
            st = m.inst(m.inst(ptr)[3])
            if st[0] & 0xffff == OpTypeStruct:
                for mi, ds in m.member_decor.get(st[1], {}).items():
                    if any(d == DecBuiltIn and a and a[0] == BuiltInPosition for d, a in ds): return vid, [mi]
        return None


class PadOutputs(Pass):
    """Variant .pad<mask>: outputs at the given locations that the paired fragment shader reads but this vertex shader does not write, (0,0,0,1) (nxnative's vs_pad).
    Vulkan links such a pair (the input reads an undefined value); the renderer asks for the variant when a pipeline's pair needs it (the reflection names both sides' locations)"""
    name, version = 'pad_outputs', 1

    def __init__(self, locations): self.locations = sorted(set(locations)); self.options = 'locations=' + ','.join(map(str, self.locations))

    def applies(self, m, stage): return stage in ('vs', 'gs', 'ds') and bool(self.locations)

    def run(self, m, stage):
        have = {(m.deco(v, DecLocation) or [None])[0] for v, sc, _ in m.global_vars() if sc == SC_Output}
        miss = [l for l in self.locations if l not in have]
        if not miss: return False
        f32 = m.ensure_type(OpTypeFloat, [32]); v4 = m.ensure_type(OpTypeVector, [f32, 4]); pv = m.ensure_type(OpTypePointer, [SC_Output, v4])
        c = VertexOutputInit().value(m, v4, 0)
        ep = m.entry_points()[0]
        new = []
        for l in miss:
            v = m.new_id(); new.append(v)
            k = m.first_function_index(); m.ins.insert(k, [4 << 16 | OpVariable, pv, v, SC_Output])
            m.add_decoration([4 << 16 | OpDecorate, v, DecLocation, l])
        m.reindex()
        epi = m.entry_points()[0]['index']; x = m.ins[epi]; m.ins[epi] = [(len(x) + len(new)) << 16 | OpEntryPoint] + x[1:] + new
        k = m.function_body_start(ep['func'])
        m.ins[k:k] = [[3 << 16 | OpStore, v, c] for v in new]
        m.reindex()
        return True


class DualSource(Pass):
    """Variant .dual: NVN's SRC1 blend factors read the pixel shader's output 1 as target 0's second source (Maxwell feeds MRT 1 to it); Vulkan reads it from
    Location 0 Index 1 (nxnative's fs_dual for Metal's [[color(0), index(1)]]). For a shader that writes exactly outputs 0 and 1"""
    name, version = 'dual_source', 1

    def applies(self, m, stage): return stage == 'ps'

    def run(self, m, stage):
        outs = {(m.deco(v, DecLocation) or [None])[0]: v for v, sc, _ in m.global_vars() if sc == SC_Output and m.deco(v, DecBuiltIn) is None}
        if sorted(k for k in outs if k is not None) != [0, 1]: return False
        v1 = outs[1]
        for x in m.ins:
            if x[0] & 0xffff == OpDecorate and x[1] == v1 and x[2] == DecLocation: x[3] = 0
        k = next(i for i, x in enumerate(m.ins) if x[0] & 0xffff == OpDecorate and x[1] == v1 and x[2] == DecLocation)
        m.ins.insert(k + 1, [4 << 16 | OpDecorate, v1, DecIndex, 1]); m.reindex()
        return True


class Fetch1DTexelBuffer(Pass):
    """The translator reads a 1D integer fetch (texelFetch of a buffer texture) as a 2D fetch at (x & 16383, x >> 14) of a combined image sampler, for Metal,
    whose host binds a texture view of the buffer with rows of 16384 texels there (nvn_mtl.m tbuf_as_2d). Vulkan cannot view a buffer as an image: such a
    binding becomes a uniform texel buffer and each fetch reads element x + (y << 14) of it, the same element. The bindings are the translator's fetch1d slots
    (its res.json, given to the module as m.fetch1d_vars before the passes run); a variable used any other way than load → OpImage → OpImageFetch is left as it is"""
    name, version = 'fetch1d_tbuf', 1
    OpImageFetch, OpImage, OpShiftLeftLogical, OpBitwiseOr = 95, 100, 196, 197

    def applies(self, m, stage): return bool(getattr(m, 'fetch1d_vars', None))

    def uses(self, m, rid):   # instructions that take rid as an operand (debug, name, decoration and constant literals are not operands)
        return [k for k, x in enumerate(m.ins) if x[0] & 0xffff not in (3, 5, 6, 7, 8, 43, 71, 72) and rid in x[1:] and result_id(x) != rid]

    def run(self, m, stage):
        changed = False
        for v in sorted(m.fetch1d_vars):
            var = m.inst(v)
            if not var or var[0] & 0xffff != OpVariable: continue
            si = m.inst(m.inst(var[1])[3])
            if si[0] & 0xffff != OpTypeSampledImage: continue
            img = m.inst(si[2])
            if img[3] != 1 or img[5] or img[6]: continue   # 2D, not arrayed, not multisampled
            loads = [k for k, x in enumerate(m.ins) if x[0] & 0xffff == OpLoad and x[3] == v]
            ok, plan = True, []
            for k in loads:
                for u in self.uses(m, m.ins[k][2]):
                    x = m.ins[u]
                    if x[0] & 0xffff != self.OpImage or x[3] != m.ins[k][2]: ok = False; break
                    for f in self.uses(m, x[2]):
                        y = m.ins[f]
                        if y[0] & 0xffff != self.OpImageFetch or y[3] != x[2] or (len(y) > 5 and y[5] != 2) or len(y) > 7: ok = False; break
                        plan.append(f)
                    plan.append(u)
                if not ok: break
            if not ok or not loads: continue
            fetches = [m.ins[f] for f in plan if m.ins[f][0] & 0xffff == self.OpImageFetch]   # taken now: the insertions below move indices
            tb = m.ensure_type(OpTypeImage, [img[2], 5, 0, 0, 0, 1, img[8]])   # Dim Buffer, sampled
            ptb = m.ensure_type(OpTypePointer, [SC_UniformConstant, tb])
            # the coordinate's component type (int or uint) for the index arithmetic
            ctype = None
            for y in fetches:
                c = m.inst(y[4]); vt = m.inst(c[1]) if c else None
                if vt and vt[0] & 0xffff == OpTypeVector and vt[3] == 2: ctype = vt[2]; break
            if fetches and ctype is None: continue
            c14 = m.ensure_const(ctype, [14]) if ctype else None
            for t in (tb, ptb):   # new types go after the module's globals (ensure_type): the variable needs them before it
                if m.defs[t] > m.defs[v]: x = m.ins.pop(m.defs[t]); m.ins.insert(m.defs[v], x); m.reindex()
            var = m.inst(v); var[1] = ptb
            for k in [k for k, x in enumerate(m.ins) if x[0] & 0xffff == OpLoad and x[3] == v]: m.ins[k][1] = tb
            for k, x in enumerate(m.ins):
                if x[0] & 0xffff == self.OpImage and m.inst(x[3]) is not None and m.inst(x[3])[0] & 0xffff == OpLoad and m.inst(x[3])[3] == v:
                    m.ins[k] = [4 << 16 | OpCopyObject, tb, x[2], x[3]]
            out = []
            fetch_ids = {y[2] for y in fetches}
            for x in m.ins:
                if x[0] & 0xffff == self.OpImageFetch and x[2] in fetch_ids:
                    e0, e1, sh, idx = m.new_id(), m.new_id(), m.new_id(), m.new_id()
                    out += [[5 << 16 | OpCompositeExtract, ctype, e0, x[4], 0], [5 << 16 | OpCompositeExtract, ctype, e1, x[4], 1],
                            [5 << 16 | self.OpShiftLeftLogical, ctype, sh, e1, c14], [5 << 16 | self.OpBitwiseOr, ctype, idx, e0, sh],
                            [5 << 16 | self.OpImageFetch, x[1], x[2], x[3], idx]]
                else: out.append(x)
            m.ins = out; m.reindex(); changed = True
        return changed


class ImageSizeExtract(Pass):
    """A TXQ of the depth (or any component past the texture's dimensions) is a CompositeExtract of OpImageQuerySize[Lod]'s result at an index the vector does not have
    (a 2D texture answers a vec2): invalid SPIR-V. The hardware answers 1 for the missing dimensions, and so does the extract after this pass"""
    name, version = 'size_extract', 1
    OpImageQuerySizeLod, OpImageQuerySize = 103, 104

    def applies(self, m, stage): return m.has_op(self.OpImageQuerySizeLod) or m.has_op(self.OpImageQuerySize)

    def run(self, m, stage):
        todo = []   # (result id, result type): found first, the constants inserted below move every index
        for x in m.ins:
            if x[0] & 0xffff != OpCompositeExtract or len(x) != 5: continue
            src = m.inst(x[3])
            if not src or src[0] & 0xffff not in (self.OpImageQuerySizeLod, self.OpImageQuerySize): continue
            vt = m.inst(src[1])
            if vt and vt[0] & 0xffff == OpTypeVector and x[4] >= vt[3]: todo.append((x[2], x[1]))
        for rid, tid in todo:
            one = m.ensure_const(tid, [1])
            k = m.defs[rid]
            m.ins[k] = [4 << 16 | OpCopyObject, tid, rid, one]
        return bool(todo)


class ImageOffset(Pass):
    """Vulkan accepts the Offset image operand only on the gather operations (VUID-RuntimeSpirv-Offset-10213); every other image instruction needs ConstOffset. The
    translator writes a texel offset it could not fold, or that comes from a register, as Offset. A constant one becomes ConstOffset (same operand, other bit). A
    non-constant one on an OpImageFetch is exactly an addition on the integer coordinates (texelFetch(t, p + o, lod)), so the fetch reads at p + o and loses the operand.
    Offsets on the sampling operations (float coordinates, an unknown level for the implicit forms) stay as they are: there the module is still invalid"""
    name, version = 'image_offset', 2   # invalidate shader chains for the ISA shadow audit
    OpIAdd, OpImageFetch = 128, 95
    SAMPLES = range(87, 96)   # OpImageSample{Implicit,Explicit}Lod, Dref*, Proj*, OpImageFetch
    DREF = (89, 90, 93, 94)
    CONSTANTS = (OpConstant, OpConstantComposite, OpConstantNull)

    def operand_position(self, x, mask_at, bit):
        mask, pos = x[mask_at], mask_at + 1
        for b in (1, 2, 4, 8, 16):
            if b == bit: return pos
            if mask & b: pos += 2 if b == 4 else 1
        return pos

    def hits(self, x):
        op = x[0] & 0xffff
        if op not in self.SAMPLES: return False
        at = 6 if op in self.DREF else 5
        return len(x) > at and bool(x[at] & 16)

    def applies(self, m, stage): return any(self.hits(x) for x in m.ins)

    def run(self, m, stage):
        out, changed = [], False
        for x in m.ins:
            if not self.hits(x): out.append(x); continue
            op = x[0] & 0xffff
            at = 6 if op in self.DREF else 5
            pos = self.operand_position(x, at, 16)
            offset = m.inst(x[pos])
            if offset is not None and offset[0] & 0xffff in self.CONSTANTS:
                x = list(x); x[at] = x[at] & ~16 | 8; out.append(x); changed = True; continue   # ConstOffset sits where Offset was in the operand order
            if op != self.OpImageFetch: out.append(x); continue
            coordinate = m.inst(x[4]); ctype = m.inst(coordinate[1]) if coordinate else None
            otype = m.inst(offset[1]) if offset else None
            def components(t):
                if t is None: return None
                if t[0] & 0xffff == OpTypeVector: return t[3]
                return 1 if t[0] & 0xffff == OpTypeInt else None
            if ctype is None or components(ctype) is None or components(ctype) != components(otype): out.append(x); continue
            moved = m.new_id()
            out.append([5 << 16 | self.OpIAdd, coordinate[1], moved, x[4], x[pos]])
            y = list(x); y[4] = moved; y[at] &= ~16; del y[pos]
            if y[at] == 0: del y[at]
            y[0] = len(y) << 16 | (y[0] & 0xffff)
            out.append(y); changed = True
        if changed: m.ins = out; m.reindex()
        return changed


class AtomicFloatAdd(Pass):
    """The translator has no float atomics: a RED.F32.ADD / ATOM.F32.ADD is dropped ("Invalid reduction type: F32FtzRn"). ampere-lower therefore lowers the float add to an
    exchange (ATOM.EXCH with no destination: the corpus has no other exchange), which the translator keeps as OpAtomicExchange on the storage buffer; the result goes to
    a variable that is never read. This pass turns such an exchange into the add it stands for: a compare-and-swap loop on the word (load, add as floats, compare-exchange,
    again until the word was unchanged), the portable form of Vulkan's atomic float add (no extension, no feature). An exchange whose result is read is a real exchange and
    stays. A block that is a loop header (OpLoopMerge before its terminator) cannot be split here and keeps the exchange; the pass reports those in .skipped"""
    name, version = 'atomic_float_add', 1
    OpAtomicLoad, OpAtomicExchange, OpAtomicCompareExchange = 227, 229, 230
    OpBitcast, OpFAdd, OpIEqual, OpPhi = 124, 129, 170, 245
    OpLoopMerge, OpSelectionMerge, OpBranch, OpBranchConditional, OpSwitch = 246, 247, 249, 250, 251
    OpStore_, OpLoad_ = 62, 61
    TERMINATORS = (249, 250, 251, 253, 254, 255, 252, 255)

    def __init__(self): self.skipped = 0

    def applies(self, m, stage): return m.has_op(self.OpAtomicExchange)

    def discarded(self, m, x):
        """the exchange's result is only stored to a variable nothing loads"""
        rid = x[2]; sinks = []
        for y in m.ins:
            op = y[0] & 0xffff
            if op in (3, 5, 6, 7, 8, 43, 71, 72) or y is x: continue
            operands = y[3:] if op in RESULT_TYPED else y[1:]
            if rid in operands:
                if op == self.OpStore_ and y[2] == rid: sinks.append(y[1])
                else: return False
        return all(not any((y[0] & 0xffff) == self.OpLoad_ and y[3] == v for y in m.ins) for v in sinks)

    def run(self, m, stage):
        todo = [i for i, x in enumerate(m.ins) if x[0] & 0xffff == self.OpAtomicExchange and self.discarded(m, x)]
        if not todo: return False
        marks = {id(m.ins[i]) for i in todo}
        uint_t = m.ins[todo[0]][1]
        float_t = m.ensure_type(OpTypeFloat, [32]); bool_t = m.ensure_type(OpTypeBool, [])
        zero = m.ensure_const(uint_t, [0])
        out, label, relabel, done = [], None, {}, 0
        ins = m.ins
        n = len(ins); i = 0
        while i < n:
            x = ins[i]; op = x[0] & 0xffff
            if op == 248: label = x[1]   # OpLabel
            if id(x) not in marks:
                out.append(x); i += 1; continue
            # a loop header (OpLoopMerge before the block's terminator) cannot be split
            j = i + 1
            while ins[j][0] & 0xffff not in self.TERMINATORS and ins[j][0] & 0xffff != self.OpLoopMerge: j += 1
            if ins[j][0] & 0xffff == self.OpLoopMerge: out.append(x); self.skipped += 1; i += 1; continue
            _, _, rid, ptr, scope, sem, val = x[:7]
            hdr, body, cont, exit_ = m.new_id(), m.new_id(), m.new_id(), m.new_id()
            cur, cf, vf, sm, nw, old, ok = (m.new_id() for _ in range(7))
            out += [[2 << 16 | self.OpBranch, hdr],
                    [2 << 16 | 248, hdr], [4 << 16 | self.OpLoopMerge, exit_, cont, 0], [2 << 16 | self.OpBranch, body],
                    [2 << 16 | 248, body],
                    [6 << 16 | self.OpAtomicLoad, uint_t, cur, ptr, scope, zero],
                    [4 << 16 | self.OpBitcast, float_t, cf, cur], [4 << 16 | self.OpBitcast, float_t, vf, val],
                    [5 << 16 | self.OpFAdd, float_t, sm, cf, vf], [4 << 16 | self.OpBitcast, uint_t, nw, sm],
                    [9 << 16 | self.OpAtomicCompareExchange, uint_t, old, ptr, scope, zero, zero, nw, cur],
                    [5 << 16 | self.OpIEqual, bool_t, ok, old, cur],
                    [4 << 16 | self.OpBranchConditional, ok, exit_, cont],
                    [2 << 16 | 248, cont], [2 << 16 | self.OpBranch, hdr],
                    [2 << 16 | 248, exit_]]
            # the exchange's result id stays defined (for the dead store): the loop's last observed word
            out.append([4 << 16 | 83, uint_t, rid, old])   # OpCopyObject
            relabel[label] = exit_; label = exit_; done += 1; i += 1
        if not done: return False
        # a phi that named the split block as its predecessor now comes from the block that ends it
        for y in out:
            if y[0] & 0xffff == self.OpPhi:
                for k in range(4, len(y), 2):
                    while y[k] in relabel: y[k] = relabel[y[k]]
        m.ins = out; m.reindex()
        return True


class _Decls:
    """Module-scope declarations a pass needs (types, constants, one variable): looked up in the module first, the missing ones collected and inserted before the first
    function in one go (one reindex, not one per declaration: the NDSR modules have a million instructions)"""
    def __init__(self, m):
        self.m, self.new, self.have = m, [], {}
        for x in m.ins[:m.first_function_index()]:
            op = x[0] & 0xffff
            if op in (OpTypeInt, OpTypeBool, OpTypePointer): self.have.setdefault((op, tuple(x[2:])), x[1])
            elif op == OpConstant and len(x) == 4: self.have.setdefault((op, (x[1], x[3])), x[2])
            elif op == OpConstantNull: self.have.setdefault((op, (x[1],)), x[2])

    def _get(self, op, key, build):
        k = (op, key)
        if k in self.have: return self.have[k]
        rid = self.m.new_id(); self.new.append(build(rid)); self.have[k] = rid; return rid

    def uint(self): return self._get(OpTypeInt, (32, 0), lambda r: [4 << 16 | OpTypeInt, r, 32, 0])
    def bool_(self): return self._get(OpTypeBool, (), lambda r: [2 << 16 | OpTypeBool, r])
    def ptr(self, sc, t): return self._get(OpTypePointer, (sc, t), lambda r: [4 << 16 | OpTypePointer, r, sc, t])
    def const(self, v):
        t = self.uint()
        return self._get(OpConstant, (t, v), lambda r: [4 << 16 | OpConstant, t, r, v])
    def null(self, t): return self._get(OpConstantNull, (t,), lambda r: [3 << 16 | OpConstantNull, t, r])

    def flush(self):
        if not self.new: return
        k = self.m.first_function_index()
        self.m.ins[k:k] = self.new; self.new = []; self.m.reindex()


def _functions(ins):
    """[(start, end)] index ranges of the OpFunction .. OpFunctionEnd runs"""
    out, k = [], None
    for i, x in enumerate(ins):
        op = x[0] & 0xffff
        if op == OpFunction: k = i
        elif op == OpFunctionEnd and k is not None: out.append((k, i)); k = None
    return out


def loop_guard_settings(env=None):
    """(enabled, cap) from the environment: NXSHADER_LOOP_GUARD=0 (or off, no, false) leaves the guard out of the pass list (and so out of the cache's chain id);
    NXSHADER_LOOP_CAP=<n> sets the budget. Read when nxs_spv is imported (the pass list is built then)"""
    env = os.environ if env is None else env
    on = str(env.get('NXSHADER_LOOP_GUARD', '1')).strip().lower() not in ('0', 'off', 'no', 'false', '')
    try: cap = int(str(env.get('NXSHADER_LOOP_CAP', LOOP_CAP_DEFAULT)), 0)
    except ValueError: cap = LOOP_CAP_DEFAULT
    return on, max(1, min(cap, 0x7fffffff))


LOOP_CAP_DEFAULT = 16384


class CasAtomicLoad(Pass):
    """A compare-exchange retry loop on shared or storage memory (the translator's sub-word stores to LDS: load the word, insert the bits, OpAtomicCompareExchange, again
    while the word changed) reads the word with a plain OpLoad. A plain load of memory another invocation changes is a data race that a compiler may hoist out of the
    loop (the next iteration then compares against the stale value and never succeeds); the load is made an OpAtomicLoad (relaxed), which it may not. Only 32-bit integer
    loads inside a loop that contains an OpAtomicCompareExchange are changed"""
    name, version = 'cas_atomic_load', 1
    OpAtomicLoad, OpAtomicCompareExchange = 227, 230

    def applies(self, m, stage): return m.has_op(self.OpAtomicCompareExchange) and m.has_op(OpLoopMerge)

    def run(self, m, stage):
        k0 = m.first_function_index()
        tsc = {x[1]: x[2] for x in m.ins[:k0] if x[0] & 0xffff == OpTypePointer}
        uints = {x[1] for x in m.ins[:k0] if x[0] & 0xffff == OpTypeInt and x[2] == 32}   # 32-bit integers of either signedness
        psc = {x[2]: x[3] for x in m.ins if x[0] & 0xffff == OpVariable}
        d = _Decls(m); todo = []   # (instruction index, scope)
        for a, e in _functions(m.ins):
            for x in m.ins[a:e]:
                if x[0] & 0xffff in (OpAccessChain, OpInBoundsAccessChain) and x[1] in tsc: psc[x[2]] = tsc[x[1]]
            labels = {}; headers = []   # layout order: a loop is the blocks from its header to its merge block
            for i in range(a, e):
                x = m.ins[i]
                if x[0] & 0xffff == OpLabel: labels[x[1]] = i
            for i in range(a, e):
                x = m.ins[i]
                if x[0] & 0xffff == OpLoopMerge and x[1] in labels: headers.append((i, labels[x[1]]))
            for h, mg in headers:
                if mg <= h or not any(m.ins[i][0] & 0xffff == self.OpAtomicCompareExchange for i in range(h, mg)): continue
                for i in range(h, mg):
                    x = m.ins[i]
                    if x[0] & 0xffff == OpLoad and len(x) == 4 and x[1] in uints and psc.get(x[3]) in (SC_Workgroup, SC_StorageBuffer): todo.append((i, 2 if psc[x[3]] == SC_Workgroup else 1))
        if not todo: return False
        sem = d.const(0); scopes = {2: d.const(2), 1: d.const(1)}
        for i, sc in todo:
            x = m.ins[i]; m.ins[i] = [6 << 16 | self.OpAtomicLoad, x[1], x[2], x[3], scopes[sc], sem]
        d.flush()
        return True


class LocalIndexClamp(Pass):
    """A dynamic index past the end of a Function- or Private-storage array is undefined in SPIR-V, and a driver that keeps the array in scratch memory (RADV) reads or writes
    wherever the offset lands: Orchid's 98e0cd716 (Kirby Air Riders) saw a wild index in a compute shader fault the GPU 30 GiB away from the scratch window. Every OpAccessChain /
    OpInBoundsAccessChain on a Function or Private pointer whose index into an array (or vector, or matrix: a dynamic component index is just as undefined) is not a constant
    gets OpExtInst UMin(index, len - 1) in front of it, so the access stays inside the object whatever the index holds (a negative signed index is a huge unsigned one: it
    clamps to the last element too). Orchid takes one spare element instead, so that an out-of-range store cannot land on a live one; here the array keeps its type (and its
    layout), and a wild store overwrites the last element: that value came from an out-of-range index, which is undefined on the hardware as well.
    Constant indices, runtime arrays and Workgroup/StorageBuffer arrays are left alone. Applies to every stage; GLSL.std.450 is imported when the module has none"""
    name, version = 'local_index_clamp', 1
    OpExtInst, UMin = 12, 38
    ARRAYS = (OpTypeArray, OpTypeVector, OpTypeMatrix)

    def __init__(self): self._memo = None

    def plan(self, m):
        """[(instruction index, [(operand position, last valid index, index type id)])] of the access chains that need a clamp"""
        k0 = m.first_function_index()
        ptr = {}   # pointer type id -> pointee type id, Function and Private pointers only
        for x in m.ins[:k0]:
            if x[0] & 0xffff == OpTypePointer and x[2] in (SC_Function, SC_Private): ptr[x[1]] = x[3]
        out = []
        if not ptr: return out
        for i in range(k0, len(m.ins)):
            x = m.ins[i]
            if x[0] & 0xffff not in (OpAccessChain, OpInBoundsAccessChain) or len(x) < 5: continue
            base = m.inst(x[3]); cur = ptr.get(base[1]) if base is not None and len(base) > 1 else None
            if cur is None: continue
            hits = []
            for pos in range(4, len(x)):
                t = m.inst(cur)
                if t is None: break
                op = t[0] & 0xffff
                if op == OpTypeStruct:
                    v = m.const_value(x[pos])
                    if v is None or 2 + v >= len(t): break
                    cur = t[2 + v]; continue
                if op == OpTypeRuntimeArray: cur = t[2]; continue
                if op not in self.ARRAYS: break
                n = m.const_value(t[3]) if op == OpTypeArray else t[3]
                idx = m.inst(x[pos])
                if n and idx is not None and idx[0] & 0xffff != OpConstant and len(idx) > 1 and not self.clamped(m, idx, n - 1):
                    it = m.inst(idx[1])
                    if it is not None and it[0] & 0xffff == OpTypeInt and it[2] in (32, 64): hits.append((pos, n - 1, idx[1]))
                cur = t[2]
            if hits: out.append((i, hits))
        return out

    def clamped(self, m, idx, hi):
        """the index is already OpExtInst GLSL.std.450 UMin(_, hi): a second run adds nothing"""
        if idx[0] & 0xffff != self.OpExtInst or len(idx) != 7 or idx[4] != self.UMin or m.const_value(idx[6]) != hi: return False
        imp = m.inst(idx[3]); return imp is not None and imp[0] & 0xffff == OpExtInstImport and string_of(imp[2:]) == 'GLSL.std.450'

    def _plan(self, m):
        if self._memo is None or self._memo[0] is not m or self._memo[1] != len(m.ins): self._memo = (m, len(m.ins), self.plan(m))
        return self._memo[2]

    def applies(self, m, stage): return m.has_op(OpAccessChain) and bool(self._plan(m))

    def run(self, m, stage):
        if not self._plan(m): return False
        imp = next((x[1] for x in m.ins if x[0] & 0xffff == OpExtInstImport and string_of(x[2:]) == 'GLSL.std.450'), 0)
        if not imp:
            imp = m.new_id(); name = string_words('GLSL.std.450')
            m.ins.insert(next(k for k, x in enumerate(m.ins) if x[0] & 0xffff == OpMemoryModel), [(2 + len(name)) << 16 | OpExtInstImport, imp] + name)
            m.reindex(); self._memo = None   # every index moved
        todo = self._plan(m); self._memo = None
        k0 = m.first_function_index()
        consts = {(x[1], tuple(x[3:])): x[2] for x in m.ins[:k0] if x[0] & 0xffff == OpConstant}
        new = []
        def const(t, v):
            words = [v & 0xffffffff] if m.inst(t)[2] == 32 else [v & 0xffffffff, v >> 32 & 0xffffffff]
            if (t, tuple(words)) not in consts:
                rid = m.new_id(); new.append([(3 + len(words)) << 16 | OpConstant, t, rid] + words); consts[(t, tuple(words))] = rid
            return consts[(t, tuple(words))]
        out, last = [], 0
        for i, hits in todo:
            out += m.ins[last:i]; last = i + 1
            x = list(m.ins[i])
            for pos, hi, t in hits:
                r = m.new_id()
                out.append([7 << 16 | self.OpExtInst, t, r, imp, self.UMin, x[pos], const(t, hi)]); x[pos] = r
            out.append(x)
        out += m.ins[last:]
        m.ins = out
        m.ins[k0:k0] = new   # k0 is before every edit: the constants go in front of the first function
        m.reindex()
        return True


class LoopGuard(Pass):
    """Every loop gets an iteration budget, so a loop that never ends on the GPU (a translation that loops forever on a case it did not list, a compare-exchange retry
    that never wins) ends the invocation instead of hanging the ring: the kernel's ring timeout resets the whole GPU (amdgpu "ring gfx_0.0.0 timeout", 2026-11-05, the
    first 3D frame). The budget is one Private variable, initialised to the cap per invocation and shared by every loop of the module (nested loops cost the product, not
    the sum, if each had its own). At the top of every iteration of every loop (the header block, which is where each iteration begins, the first one too) the loop
    reads it: zero ends the invocation (OpReturn; OpReturnValue of a null for a function that returns a value, whose caller's own loops then end the same way); else
    it is decremented and the loop body runs as before. That needs no phi at the loop merge and no change in what dominates what, so every translated CFG that
    was valid stays valid, and it does not depend on recognising the shape of the loop. Applies to every stage; a module without a loop is left alone.
    Cap: NXSHADER_LOOP_CAP (default 16384 iterations in all loops of one invocation); NXSHADER_LOOP_GUARD=0 leaves the pass out of the list."""
    name, version = 'loop_guard', 1
    OpSelectionMerge_, OpBranch_, OpBranchConditional_, OpPhi, OpINotEqual, OpISub = 247, 249, 250, 245, 171, 130

    def __init__(self, cap=LOOP_CAP_DEFAULT):
        self.cap = cap; self.options = f'cap={cap}'; self.guarded = 0

    def applies(self, m, stage): return m.has_op(OpLoopMerge)

    def run(self, m, stage):
        ins = m.ins; k0 = m.first_function_index()
        d = _Decls(m)
        uint_t, bool_t = d.uint(), d.bool_()
        ptr_t = d.ptr(SC_Private, uint_t); cap_c, zero_c, one_c = d.const(self.cap), d.const(0), d.const(1)
        var = m.new_id(); d.new.append([5 << 16 | OpVariable, ptr_t, var, SC_Private, cap_c])
        void_t = {x[1] for x in ins[:k0] if x[0] & 0xffff == OpTypeVoid}
        rets = {}   # function id -> its null return value (None: void)
        for a, e in _functions(ins):
            if not any(x[0] & 0xffff == OpLoopMerge for x in ins[a:e]): continue
            rt = ins[a][1]; rets[ins[a][2]] = None if rt in void_t else d.null(rt)
        body = ins[k0:]
        out, remap, label, ret = [], {}, 0, None
        i, n = 0, len(body)
        while i < n:
            x = body[i]; op = x[0] & 0xffff
            if op == OpFunction: ret = rets.get(x[2])
            elif op == OpLabel: label = x[1]
            out.append(x); i += 1
            if op != OpLoopMerge: continue
            while body[i][0] & 0xffff not in TERMINATORS: out.append(body[i]); i += 1
            t = body[i]; i += 1
            g, r, nb = m.new_id(), m.new_id(), m.new_id()
            b0, alive, b1 = m.new_id(), m.new_id(), m.new_id()
            # a header that is its own continue target (a loop of one block) cannot have the guard blocks in its continue construct: they go before a continue block of their own,
            # which takes the header's branch
            cont = m.new_id() if x[2] == label else 0
            if cont: x[2] = cont
            tail = [[2 << 16 | self.OpBranch_, cont], [2 << 16 | OpLabel, cont], t] if cont else [t]
            out += [[2 << 16 | self.OpBranch_, g],
                    [2 << 16 | OpLabel, g], [4 << 16 | OpLoad, uint_t, b0, var], [5 << 16 | self.OpINotEqual, bool_t, alive, b0, zero_c],
                    [3 << 16 | self.OpSelectionMerge_, nb, 0], [4 << 16 | self.OpBranchConditional_, alive, nb, r],
                    [2 << 16 | OpLabel, r], [1 << 16 | 253] if ret is None else [2 << 16 | 254, ret],
                    [2 << 16 | OpLabel, nb], [5 << 16 | self.OpISub, uint_t, b1, b0, one_c], [3 << 16 | OpStore, var, b1], *tail]
            top = t[0] & 0xffff
            succ = [t[1]] if top == self.OpBranch_ else [t[2], t[3]] if top == self.OpBranchConditional_ else [t[2]] + t[4::2] if top == 251 else []
            for s in succ: remap[(s, label)] = cont or nb   # a phi of the successor named the header as predecessor: now the block that holds the branch
            self.guarded += 1
        if not self.guarded: return False
        m.ins = ins[:k0] + out
        if m.version >= (1, 4):   # SPIR-V 1.4 and later list every global variable in the entry points' interfaces
            for x in m.ins:
                if x[0] & 0xffff == OpEntryPoint: x.append(var); x[0] = len(x) << 16 | OpEntryPoint
        m.ins[k0:k0] = d.new
        if remap:
            lab = 0
            for x in m.ins[k0:]:
                op = x[0] & 0xffff
                if op == OpLabel: lab = x[1]
                elif op == self.OpPhi:
                    for j in range(4, len(x), 2):
                        nbk = remap.get((lab, x[j]))
                        if nbk: x[j] = nbk
        m.reindex()
        return True




class GlobalMemoryBda(Pass):
    """Real global memory for what the translator could not trace to a storage buffer. GlobalToStorage.TryGenerateMultiTargetStorageOp still makes a helper that takes the 64-bit
    address (low word, high word, then the value): with no candidate buffer its body is only `return 0` / `return`, so loads read zero and stores vanish. With candidates it checks
    each buffer's range and falls through to the same `return 0` when none holds the address. This pass gives those helpers, and that fall-through, a real access: the shader looks the
    guest address up in a two-level table the runtime keeps (vk_mem.c, gmem_*: all u64) and goes through a PhysicalStorageBuffer pointer into the pool's mirror:
        table[va >> 21]                     word index of the page's level-2 block, 0 = nothing mapped there
        table[block + ((va >> 12) & 511)]   device address minus guest address for that page, 0 = not mapped
    Any miss, or an address of 40 bits or more, keeps the old result (load 0, store dropped), so a bad pointer never becomes a device access and an empty table is the old behaviour.
    The helpers carry no name in the module and their signature does not tell a 32-bit load from a small-integer one or a store from an atomic, so the kinds come from the translator's
    notes (m.gmem_notes, its res.json): a helper shape gets its access only when exactly one helper has it and the notes name one operation for it; anything else stays dropped and is
    counted in .skipped. Small-integer accesses are not produced here: tools/gmem_kinds.py checks that the corpus has none (a module with such a helper would need a rebuilt translator)"""
    name, version = 'gmem_bda', 2   # 2: the reflection carries the address roots
    TABLE_NAME = 'gmem_table'
    L2_BITS, PAGE_BITS, VA_HIGH_LIMIT = 9, 12, 256   # 512 entries a block, 4 KiB pages, addresses under 2^40
    OpUConvert, OpConvertUToPtr, OpIAdd, OpINotEqual, OpULessThan = 113, 120, 128, 171, 176
    OpShiftRightLogical, OpShiftLeftLogical, OpBitwiseOr, OpBitwiseAnd = 194, 196, 197, 199
    OpSelectionMerge, OpBranch, OpBranchConditional = 247, 249, 250
    OpBitFieldSExtract, OpBitFieldUExtract = 201, 202
    SC_PhysicalStorageBuffer, AM_PhysicalStorageBuffer64, CAP_PSB, CAP_INT64, ALIGNED = 5349, 5348, 5347, 11, 2
    ATOMICS = {'AtomicAdd': 234, 'AtomicMinS32': 236, 'AtomicMinU32': 237, 'AtomicMaxS32': 238, 'AtomicMaxU32': 239, 'AtomicAnd': 240, 'AtomicOr': 241, 'AtomicXor': 242}
    OpAtomicCompareExchange = 230
    NOTE = 'global memory operation "'

    def __init__(self): self.skipped = 0

    def noted(self, m):
        """{operation: count} of the translator's notes, or None when the module came without them"""
        notes = getattr(m, 'gmem_notes', None)
        if notes is None: return None
        out = {}
        for n in notes:
            i = n.find(self.NOTE)
            if i >= 0:
                op = n[i + len(self.NOTE):].split('"')[0]; out[op] = out.get(op, 0) + 1
        return out

    def helpers(self, m):
        """every helper this pass can act on: (kind, function index, end index, parameter ids) with kind 'empty' or 'fallthrough'"""
        ep = {e['func'] for e in m.entry_points()}
        out = []; k = 0; n = len(m.ins)
        while k < n:
            x = m.ins[k]
            if x[0] & 0xffff != OpFunction or x[2] in ep: k += 1; continue
            e = k
            while m.ins[e][0] & 0xffff != OpFunctionEnd: e += 1
            params = [y[2] for y in m.ins[k + 1:e] if y[0] & 0xffff == OpFunctionParameter]
            body = m.ins[k + 1 + len(params):e]
            ok = len(params) >= 2 and all(self.is_int_ptr(m, m.inst(p)[1]) for p in params)
            if ok:
                ops = [y[0] & 0xffff for y in body]
                ret_zero = bool(body) and ops[-1] == OpReturnValue and m.const_value(body[-1][1]) == 0 and self.is_const_int(m, body[-1][1])
                if ops == [OpLabel, OpReturnValue] and ret_zero: out.append(('empty', k, e, params))
                elif ops == [OpLabel, OpReturn]: out.append(('empty', k, e, params))
                elif len(params) == 2 and ret_zero and ops[-2] == OpLabel and self.checks_ranges(m, body, params) and self.OpBitFieldUExtract not in ops and self.OpBitFieldSExtract not in ops:
                    out.append(('fallthrough', k, e, params))
            k = e + 1
        return out

    def is_int_ptr(self, m, tid):
        t = m.inst(tid)
        if not t or t[0] & 0xffff != OpTypePointer or t[2] != SC_Function: return False
        i = m.inst(t[3])
        return bool(i) and i[0] & 0xffff == OpTypeInt and i[2] == 32

    def is_const_int(self, m, cid):
        c = m.inst(cid); t = m.inst(c[1]) if c else None
        return bool(t) and t[0] & 0xffff == OpTypeInt and t[2] == 32

    def checks_ranges(self, m, body, params):
        """the multi-target shape: reads the address's low word and compares it unsigned against something, and reads a storage buffer in a branch"""
        first = next((y for y in body if y[0] & 0xffff not in (OpLabel, OpVariable)), None)
        if not first or first[0] & 0xffff != OpLoad or first[3] != params[0]: return False
        if not any(y[0] & 0xffff == self.OpULessThan for y in body): return False
        return any(y[0] & 0xffff == OpAccessChain and m.names.get(y[3], '').rsplit('_', 1)[-1][:1] == 's' for y in body)

    def plan(self, m):
        """[(helper, operation)] the notes allow; the rest counted as skipped"""
        hs = self.helpers(m); noted = self.noted(m); plan = []; skipped = 0
        empty = [h for h in hs if h[0] == 'empty']
        shapes = {}
        for h in empty:
            void = m.inst(m.ins[h[1]][1])[0] & 0xffff == OpTypeVoid
            shapes.setdefault((len(h[3]), void), []).append(h)
        known = dict(noted) if noted is not None else None
        calls = {}   # function id -> call sites in the module: the translator makes one call per access, so a helper's calls equal the notes' count for its operation
        for x in m.ins:
            if x[0] & 0xffff == OpFunctionCall: calls[x[3]] = calls.get(x[3], 0) + 1
        claimed = set()   # operations already given to a helper by a call count; void helpers come last, with what the others left
        for (nargs, void), hl in sorted(shapes.items(), key=lambda kv: (kv[0][1], kv[0][0])):
            if nargs == 2 and not void: candidates = ['Load']
            elif nargs == 3 and void: candidates = ['Store'] + list(self.ATOMICS)
            elif nargs == 3: candidates = list(self.ATOMICS)
            elif nargs == 4: candidates = ['AtomicCompareAndSwap']
            else: skipped += len(hl); continue
            names = [c for c in candidates if known is None and c in ('Load', 'Store') or known and c in known]
            if len(hl) == 1 and len(names) == 1: plan.append((hl[0], names[0])); claimed.add(names[0]); continue
            # several helpers of a shape (the culling producer has an AtomicAdd and an AtomicMaxU32 helper) or several operations a shape could be: match them by how often each is called
            names = [c for c in names if c not in claimed]
            if known is None or len(hl) > len(names): skipped += len(hl); continue
            by_count = {}
            for c in names: by_count.setdefault(known[c], []).append(c)
            picked, ok = [], True
            for h in hl:
                cand = by_count.get(calls.get(m.ins[h[1]][2], -1), [])
                if len(cand) != 1 or any(len(hh) and calls.get(m.ins[hh[1]][2], -1) == calls.get(m.ins[h[1]][2], -1) for hh in hl if hh is not h): ok = False; break
                picked.append((h, cand[0]))
            if not ok or len({n for _, n in picked}) != len(picked): skipped += len(hl); continue
            for h, n in picked: plan.append((h, n)); claimed.add(n)
        for h in hs:
            if h[0] == 'fallthrough': plan.append((h, 'Load'))
        self.skipped += skipped
        return plan

    def applies(self, m, stage):
        if any(x[0] & 0xffff == OpName and string_of(x[2:]) == self.TABLE_NAME for x in m.ins): return False
        return any(h[0] == 'fallthrough' or self.noted(m) is None or self.noted(m) for h in self.helpers(m))

    def run(self, m, stage):
        plan = self.plan(m)
        if not plan: return False
        funcs = [(m.ins[h[1]][2], h[0], h[3], op) for h, op in plan]   # function ids: prepare moves the indices
        self.prepare(m)
        for fid, kind, params, op in sorted(funcs, key=lambda f: -m.defs[f[0]]):   # last first: the earlier ones' indices stay good
            self.rewrite(m, fid, kind, params, op)
        m.reindex()
        return True

    def prepare(self, m):
        """capabilities, extension, addressing model, the table's variable and the types and constants the bodies use, all before any index below is taken"""
        m.add_capability(self.CAP_INT64); m.add_capability(self.CAP_PSB)
        if not any(x[0] & 0xffff == OpExtension and string_of(x[1:]) == 'SPV_KHR_physical_storage_buffer' for x in m.ins):
            w = string_words('SPV_KHR_physical_storage_buffer')
            k = max(i for i, x in enumerate(m.ins) if x[0] & 0xffff == OpCapability) + 1
            m.ins.insert(k, [(1 + len(w)) << 16 | OpExtension] + w)
        for x in m.ins:
            if x[0] & 0xffff == OpMemoryModel: x[1] = self.AM_PhysicalStorageBuffer64
        m.reindex()
        t = type('T', (), {})
        t.void = m.ensure_type(OpTypeVoid, []); t.bool = m.ensure_type(OpTypeBool, [])
        t.int = m.ensure_type(OpTypeInt, [32, 1]); t.uint = m.ensure_type(OpTypeInt, [32, 0]); t.ulong = m.ensure_type(OpTypeInt, [64, 0])
        arr = m.new_id(); st = m.new_id()   # fresh ones: the decorations below belong to them alone
        k = m.first_function_index()
        m.ins[k:k] = [[3 << 16 | OpTypeRuntimeArray, arr, t.ulong], [3 << 16 | OpTypeStruct, st, arr]]
        m.reindex()
        t.ptr_sb = m.ensure_type(OpTypePointer, [SC_StorageBuffer, t.ulong]); pst = m.ensure_type(OpTypePointer, [SC_StorageBuffer, st])
        t.ptr_psb = m.ensure_type(OpTypePointer, [self.SC_PhysicalStorageBuffer, t.uint])
        t.table = m.new_id(); k = m.first_function_index(); m.ins.insert(k, [4 << 16 | OpVariable, pst, t.table, SC_StorageBuffer])
        m.reindex()
        binding = max([d[0] for v, _, _ in m.global_vars() for d in [m.deco(v, DecBinding)] if d] + [-1]) + 1
        for w in ([4 << 16 | OpDecorate, arr, 6, 8], [5 << 16 | OpMemberDecorate, st, 0, 35, 0], [4 << 16 | OpMemberDecorate, st, 0, 24], [3 << 16 | OpDecorate, st, DecBlock],
                  [4 << 16 | OpDecorate, t.table, DecDescriptorSet, 0], [4 << 16 | OpDecorate, t.table, DecBinding, binding]):
            m.add_decoration(w)
        nm = string_words(self.TABLE_NAME)
        names = [i for i, x in enumerate(m.ins) if x[0] & 0xffff == OpName]
        at = names[-1] + 1 if names else next(i for i, x in enumerate(m.ins) if x[0] & 0xffff in (71, 72))
        m.ins.insert(at, [(2 + len(nm)) << 16 | OpName, t.table] + nm)
        m.reindex()
        if m.version >= (1, 4):   # from 1.4 every global the entry point uses is in its interface
            for ep in m.entry_points():
                x = m.ins[ep['index']]; m.ins[ep['index']] = [(len(x) + 1) << 16 | OpEntryPoint] + x[1:] + [t.table]
            m.reindex()
        def u64(v): return m.ensure_const(t.ulong, [v & 0xffffffff, v >> 32])
        t.i0 = m.ensure_const(t.int, [0]); t.u0 = m.ensure_const(t.uint, [0]); t.u1 = m.ensure_const(t.uint, [1])
        t.limit = m.ensure_const(t.uint, [self.VA_HIGH_LIMIT]); t.l0 = u64(0); t.l32 = u64(32)
        t.l21 = u64(self.PAGE_BITS + self.L2_BITS); t.l12 = u64(self.PAGE_BITS); t.lmask = u64((1 << self.L2_BITS) - 1); t.lalign = u64(0xfffffffffffffffc)
        self.t = t

    def rewrite(self, m, fid, kind, params, op):
        k = m.defs[fid]; e = k
        while m.ins[e][0] & 0xffff != OpFunctionEnd: e += 1
        if kind == 'empty':
            at = k + 1 + len(params) + 1   # after the parameters and the label
            tail = e
        else:
            at = e - 1; tail = e   # the fall-through's return, inside its own block
        void = m.inst(m.ins[k][1])[0] & 0xffff == OpTypeVoid
        zero = m.ins[e - 1][1] if not void else None
        body = self.body(m, params, op, void, zero)
        m.ins[at:tail] = body

    # ---- where the addresses come from: what the runtime can read from guest memory before a draw, to know which ranges to bring up to date ----
    RANK = ('other', 'const', 'cb_const', 'cb_dynamic', 'sb_const', 'sb_dynamic', 'gmem')
    ARITH = (132, 194, 195, 196, 197, 198, 199, 201, 202)   # multiply, shifts, or, xor, and, bit fields

    def table_helpers(self, m):
        """the helpers of a module this pass edited: the functions that read the table"""
        tb = next((v for v, n in m.names.items() if n == self.TABLE_NAME), None); out = set(); fn = None
        for x in m.ins:
            if x[0] & 0xffff == OpFunction: fn = x[2]
            elif tb is not None and x[0] & 0xffff == OpAccessChain and x[3] == tb: out.add(fn)
        return out

    def address_bases(self, m, helpers):
        """One entry per helper call: {'root': strongest source of the address, 'where': {(variable, constant indices)}, 'index', 'paths', 'mul1'}. Walks the address's HIGH word, since a computed
        index changes only the low word and a carry is a select of constants, back through the translator's Function variables to the buffer read it comes from. An address whose high word
        comes from sb_const is a pointer in a tracked storage buffer at a fixed offset (Lichii f9acc60a / d854ff0d: the five-entry pointer tables). 'gmem' is a pointer read through another pointer."""
        stores = {}
        for k, x in enumerate(m.ins):
            if x[0] & 0xffff == OpStore: stores.setdefault(x[1], []).append((k, x[2]))
        constant = lambda c: m.const_value(c) is not None
        def walk(rid, depth, st):
            if depth > 64 or rid in st['seen']: return
            st['seen'].add(rid)
            x = m.inst(rid)
            if x is None: return
            op = x[0] & 0xffff
            if op in (OpConstant, OpConstantNull): st['roots'].add('const'); return
            if op == OpLoad:
                p = m.inst(x[3])
                if p and p[0] & 0xffff in (OpAccessChain, OpInBoundsAccessChain):
                    nm = m.names.get(p[3], ''); tail = nm.rsplit('_', 1)[-1]
                    if '_' in nm and tail[:1] in ('c', 's') and tail[1:].isdigit():
                        idx = tuple(m.const_value(i) for i in p[4:])
                        st['roots'].add(('cb' if tail[0] == 'c' else 'sb') + ('_dynamic' if None in idx else '_const'))
                        st['where'].add((p[3], idx)); return
                    st['roots'].add('other'); return
                prior = [v for k, v in stores.get(x[3], []) if k < m.defs[rid]]
                if not prior: st['roots'].add('other'); return
                if len(prior) > 1: st['paths'] = True
                for v in reversed(prior[-8:]): walk(v, depth + 1, st)
            elif op in (OpBitcast, OpCopyObject): walk(x[3], depth + 1, st)
            elif op == 169:   # OpSelect: a carry when both arms are constants, else a choice of pointers
                if constant(x[4]) and constant(x[5]): return
                st['paths'] = True; walk(x[4], depth + 1, st); walk(x[5], depth + 1, st)
            elif op in (self.OpIAdd, 130):
                for o in x[3:5]:
                    if not constant(o): walk(o, depth + 1, st)
            elif op in self.ARITH: st['arith'] = True
            elif op == 57: st['roots'].add('gmem' if x[3] in helpers else 'other')
            else: st['roots'].add('other')
        def computed(rid, depth, seen, st):
            if depth > 64 or rid in seen: return False
            seen.add(rid); x = m.inst(rid)
            if x is None: return False
            op = x[0] & 0xffff
            if op == OpLoad: return any([computed(v, depth + 1, seen, st) for k, v in reversed(stores.get(x[3], [])[-4:]) if k < m.defs[rid]])
            if op in (OpBitcast, OpCopyObject): return computed(x[3], depth + 1, seen, st)
            if op == 132 and 1 in (m.const_value(x[3]), m.const_value(x[4])): st['mul1'] = True
            if op in self.ARITH:
                for o in x[3:5]: computed(o, depth + 1, seen, st)
                return True
            if op in (self.OpIAdd, 130): return any([computed(o, depth + 1, seen, st) for o in x[3:5] if not constant(o)])
            return False
        out = []
        for k, x in enumerate(m.ins):
            if x[0] & 0xffff != 57 or x[3] not in helpers or len(x) < 6: continue
            st = {'roots': set(), 'seen': set(), 'where': set(), 'paths': False, 'arith': False, 'mul1': False}
            hi = [v for i, v in stores.get(x[5], []) if i < k]; lo = [v for i, v in stores.get(x[4], []) if i < k]
            if hi: walk(hi[-1], 0, st)
            roots = st['roots'] - {'const'} or st['roots'] or {'other'}
            out.append({'root': max(roots, key=self.RANK.index), 'where': st['where'], 'paths': st['paths'], 'mul1': st['mul1'], 'index': bool(lo) and computed(lo[-1], 0, set(), st)})
        return out

    MAX_ROOTS = 16

    def roots(self, m):
        """(the storage buffer words holding the pointers a program's helpers go through as [[binding, low word], ...], whether some address comes from anywhere else): the runtime dereferences the first
        before a draw to know what to bring up to date, and has to bring everything up to date for the second"""
        out = set(); open_ = False
        for b in self.address_bases(m, self.table_helpers(m)):
            if b['root'] != 'sb_const': open_ = True; continue
            for var, idx in b['where']:
                d = m.deco(var, DecBinding)
                if not d or len(idx) != 2 or idx[1] is None or not idx[1] & 1: open_ = True; continue
                out.add((d[0], idx[1] - 1))
        if len(out) > self.MAX_ROOTS: return [], True
        return [list(r) for r in sorted(out)], open_

    def body(self, m, params, op, void, zero):
        t = self.t; new = m.new_id; out = []
        def miss(): return [[1 << 16 | OpReturn]] if void else [[2 << 16 | OpReturnValue, zero]]
        def guard(cond):
            nxt, ms = new(), new()
            out.extend([[3 << 16 | self.OpSelectionMerge, nxt, 0], [4 << 16 | self.OpBranchConditional, cond, nxt, ms], [2 << 16 | OpLabel, ms]] + miss() + [[2 << 16 | OpLabel, nxt]])
        def op3(code, ty, a, b):
            r = new(); out.append([5 << 16 | code, ty, r, a, b]); return r
        def op1(code, ty, a):
            r = new(); out.append([4 << 16 | code, ty, r, a]); return r
        lo, hi = new(), new()
        out += [[4 << 16 | OpLoad, t.int, lo, params[0]], [4 << 16 | OpLoad, t.int, hi, params[1]]]
        ulo, uhi = op1(OpBitcast, t.uint, lo), op1(OpBitcast, t.uint, hi)
        guard(op3(self.OpULessThan, t.bool, uhi, t.limit))
        va = op3(self.OpBitwiseOr, t.ulong, op3(self.OpShiftLeftLogical, t.ulong, op1(self.OpUConvert, t.ulong, uhi), t.l32), op1(self.OpUConvert, t.ulong, ulo))
        def table(index):
            p = new(); v = new()
            out.extend([[6 << 16 | OpAccessChain, t.ptr_sb, p, t.table, t.i0, op1(self.OpUConvert, t.uint, index)], [4 << 16 | OpLoad, t.ulong, v, p]])
            return v
        block = table(op3(self.OpShiftRightLogical, t.ulong, va, t.l21))
        guard(op3(self.OpINotEqual, t.bool, block, t.l0))
        page = op3(self.OpBitwiseAnd, t.ulong, op3(self.OpShiftRightLogical, t.ulong, va, t.l12), t.lmask)
        delta = table(op3(self.OpIAdd, t.ulong, block, page))
        guard(op3(self.OpINotEqual, t.bool, delta, t.l0))
        ptr = op1(self.OpConvertUToPtr, t.ptr_psb, op3(self.OpIAdd, t.ulong, op3(self.OpBitwiseAnd, t.ulong, va, t.lalign), delta))
        def arg(i):
            a = new(); out.append([4 << 16 | OpLoad, t.int, a, params[i]]); return op1(OpBitcast, t.uint, a)
        if op == 'Load':
            v = new(); out.append([6 << 16 | OpLoad, t.uint, v, ptr, self.ALIGNED, 4])
            out.append([2 << 16 | OpReturnValue, op1(OpBitcast, t.int, v)])
        elif op == 'Store':
            out.append([5 << 16 | OpStore, ptr, arg(2), self.ALIGNED, 4]); out.append([1 << 16 | OpReturn])
        elif op == 'AtomicCompareAndSwap':
            c, v = arg(2), arg(3); r = new()
            out.append([9 << 16 | self.OpAtomicCompareExchange, t.uint, r, ptr, t.u1, t.u0, t.u0, v, c])
            out.append([2 << 16 | OpReturnValue, op1(OpBitcast, t.int, r)] if not void else [1 << 16 | OpReturn])
        else:
            v = arg(2); r = new()
            out.append([7 << 16 | self.ATOMICS[op], t.uint, r, ptr, t.u1, t.u0, v])
            out.append([2 << 16 | OpReturnValue, op1(OpBitcast, t.int, r)] if not void else [1 << 16 | OpReturn])
        return out



class NoContraction(Pass):
    """The translator's FP32 arithmetic (FAdd, FSub, FMul, FDiv, FMod) carries no NoContraction decoration: dmsh_xlate runs Ryujinx in its reduced-precision mode (the Metal
    flavour; `reduced` in the job), which leaves the decoration out. A Vulkan driver may then fuse a FMul and the FAdd that uses it into one FMA, which rounds once. The Switch
    2 programs say where they want a fused multiply-add (FFMA, which stays an exact OpExtInst Fma) and where they want two roundings (FMUL then FADD), and shaders built on
    the difference break when it is ignored: a value that is exactly 0 or positive with two roundings comes out slightly negative fused, and the sqrt, log2 or pow of it is
    NaN. OoT's deferred light volumes (fd2de1f030b8ffd0 and 2,800 like it) put NaN into 0.07% of the HDR target that way (striped, along the zero crossings), which the CPU's
    exposure read-back then averaged to NaN in 61% of the frames. Every FAdd, FSub, FMul, FDiv and FMod result gets NoContraction (the same set Ryujinx decorates when
    `reduced` is off, minus its Invariant gl_Position rule)"""
    name, version = 'no_contraction', 1
    OPS = {129, 131, 133, 136, 140, 141}   # OpFAdd, OpFSub, OpFMul, OpFDiv, OpFRem, OpFMod
    DEC_NOCONTRACTION = 42

    def applies(self, m, stage): return any(x[0] & 0xffff in self.OPS for x in m.ins)

    def run(self, m, stage):
        have = {rid for rid, ds in m.decor.items() if any(d == self.DEC_NOCONTRACTION for d, _ in ds)}
        new = [[3 << 16 | OpDecorate, x[2], self.DEC_NOCONTRACTION] for x in m.ins if x[0] & 0xffff in self.OPS and x[2] not in have]
        if not new: return False
        ann = [k for k, x in enumerate(m.ins) if x[0] & 0xffff in (71, 72, 73, 74, 332, 5632, 5633)]
        k = ann[-1] + 1 if ann else next(k for k, x in enumerate(m.ins) if x[0] & 0xffff in range(19, 47))
        m.ins[k:k] = new; m.reindex()
        return True


DEFAULT_PASSES = [TrimCapabilities(), StorageFormatCaps(), VertexOutputInit(), Fetch1DTexelBuffer(), ImageSizeExtract(), ImageOffset(), AtomicFloatAdd(), CasAtomicLoad(), LocalIndexClamp()]
if os.environ.get('NXS_GMEM', '1') != '0': DEFAULT_PASSES.append(GlobalMemoryBda())   # NXS_GMEM=0: untracked global memory stays dropped, and the chain id says so
if os.environ.get('NXS_NOCONTRACT', '0') == '1': DEFAULT_PASSES.append(NoContraction())   # NXS_NOCONTRACT=1 (off by default: it did not change the OoT NaN, section 22): FP arithmetic that a driver may not fuse, and the chain id says so
_LG_ON, _LG_CAP = loop_guard_settings()
if _LG_ON: DEFAULT_PASSES.append(LoopGuard(_LG_CAP))   # last: it also guards the loops the passes before it made (atomic_float_add's, the gmem pass's)
VARIANTS = {'gl': lambda opts: [DepthGL()], 'dual': lambda opts: [DualSource()], 'pad': lambda opts: [PadOutputs(opts)]}


# ===================================================================================== analysis

def int_only_inputs(m):
    """Vertex inputs used only as integers (every use of a loaded value is a bit cast to an integer type, possibly through extracts, shuffles and copies; a store
    of the value somewhere is neutral): {location: 'uint'}. nxnative's int_inputs did this on the MSL; here on the SPIR-V after spirv-opt's SSA rewrite (a copy
    made for the analysis only). The translator declares every attribute float without an answer, and Vulkan then converts an integer vertex format to float
    values instead of passing its bits (bone indices: skinned characters stretched into streaks)"""
    uses, in_func = {}, False   # uses inside functions only: annotations and types hold literals that look like ids
    for x in m.ins:
        op = x[0] & 0xffff
        if op == OpFunction: in_func = True
        if not in_func: continue
        operands = x[3:] if op in RESULT_TYPED else x[1:]
        for o in operands:
            if o in m.defs: uses.setdefault(o, []).append(x)
    def classify(rid, seen):
        n_int, bad = 0, False
        for u in uses.get(rid, []):
            op = u[0] & 0xffff
            if op == OpBitcast:
                t = m.inst(u[1]); tt = m.inst(t[2]) if t and t[0] & 0xffff == OpTypeVector else t
                if tt and tt[0] & 0xffff == OpTypeInt: n_int += 1
                else: bad = True
            elif op in (OpCompositeExtract, OpVectorShuffle, OpCopyObject, OpCompositeConstruct) and u[2] not in seen:
                seen.add(u[2]); a, b = classify(u[2], seen); n_int += a; bad |= b
            elif op == OpStore and u[2] == rid: pass
            else: bad = True
            if bad: break
        return n_int, bad
    out = {}
    for vid, sc, ptr in m.global_vars():
        loc = m.deco(vid, DecLocation)
        if sc != SC_Input or loc is None: continue
        ptrs, loads = {vid}, []
        for x in m.ins:
            op = x[0] & 0xffff
            if op in (OpAccessChain, OpInBoundsAccessChain) and x[3] in ptrs: ptrs.add(x[2])
        for x in m.ins:
            if x[0] & 0xffff == OpLoad and x[3] in ptrs: loads.append(x[2])
        n_int, bad = 0, False
        for r in loads:
            a, b = classify(r, {r}); n_int += a; bad |= b
        if n_int and not bad: out[str(loc[0])] = 'uint'
    return out
