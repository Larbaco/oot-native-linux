#!/usr/bin/env python3
"""nxshader's unit tests (no game data): the SPIR-V edits on small modules assembled here, each result accepted by spirv-val; the integer-input analysis; and
the cache's invalidation with a stand-in translator: a rerun hits, a new translator misses every shader, a new version of one edit misses only the shaders it
applies to, new facts for one shader miss only that shader, and an entry made by an older chain is stale for the runtime's lookup.
  test_nxshader.py            exit 0 = pass (77 when the Vulkan SDK's spirv-as / spirv-val are missing)"""
import json, os, shutil, struct, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import nxs_spv as S, nxs_cache as C, nxs_front as F, nxshader as N, nxs_refl as R

FAIL = []
def T(cond, what):
    if not cond: FAIL.append(what); print('FAIL', what)

VS = r'''
               OpCapability Shader
               OpCapability Float64
               OpCapability GeometryStreams
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Vertex %main "main" %pos_in %bits_in %o0 %o1a %o1b %oi %pv
               OpDecorate %pos_in Location 0
               OpDecorate %bits_in Location 1
               OpDecorate %o0 Location 0
               OpDecorate %o1a Location 1
               OpDecorate %o1a Component 0
               OpDecorate %o1b Location 1
               OpDecorate %o1b Component 2
               OpDecorate %oi Location 2
               OpDecorate %oi Flat
               OpMemberDecorate %PerVertex 0 BuiltIn Position
               OpMemberDecorate %PerVertex 1 BuiltIn PointSize
               OpDecorate %PerVertex Block
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
       %uint = OpTypeInt 32 0
        %int = OpTypeInt 32 1
       %v4f = OpTypeVector %float 4
       %v2f = OpTypeVector %float 2
       %v4u = OpTypeVector %uint 4
 %PerVertex = OpTypeStruct %v4f %float
  %p_out_pv = OpTypePointer Output %PerVertex
  %p_in_v4f = OpTypePointer Input %v4f
 %p_out_v4f = OpTypePointer Output %v4f
 %p_out_v2f = OpTypePointer Output %v2f
 %p_out_v4u = OpTypePointer Output %v4u
     %p_in_f = OpTypePointer Input %float
     %int_0 = OpConstant %int 0
    %uint_1 = OpConstant %uint 1
     %pos_in = OpVariable %p_in_v4f Input
    %bits_in = OpVariable %p_in_v4f Input
         %o0 = OpVariable %p_out_v4f Output
        %o1a = OpVariable %p_out_v2f Output
        %o1b = OpVariable %p_out_v2f Output
         %oi = OpVariable %p_out_v4u Output
         %pv = OpVariable %p_out_pv Output
       %main = OpFunction %void None %fn
      %entry = OpLabel
          %p = OpLoad %v4f %pos_in
         %pp = OpAccessChain %p_out_v4f %pv %int_0
               OpStore %pp %p
         %bc = OpAccessChain %p_in_f %bits_in %uint_1
          %b = OpLoad %float %bc
         %bu = OpBitcast %uint %b
         %bv = OpCompositeConstruct %v4u %bu %bu %bu %bu
               OpStore %oi %bv
         %xy = OpVectorShuffle %v2f %p %p 0 1
               OpStore %o1a %xy
               OpReturn
               OpFunctionEnd
'''

FS = r'''
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %c0 %c1
               OpExecutionMode %main OriginUpperLeft
               OpDecorate %c0 Location 0
               OpDecorate %c1 Location 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
       %v4f = OpTypeVector %float 4
 %p_out_v4f = OpTypePointer Output %v4f
     %f_one = OpConstant %float 1
      %white = OpConstantComposite %v4f %f_one %f_one %f_one %f_one
         %c0 = OpVariable %p_out_v4f Output
         %c1 = OpVariable %p_out_v4f Output
       %main = OpFunction %void None %fn
      %entry = OpLabel
               OpStore %c0 %white
               OpStore %c1 %white
               OpReturn
               OpFunctionEnd
'''

CS = r'''
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %img
               OpExecutionMode %main LocalSize 8 8 1
               OpName %img "cp_i_tcb_12_"
               OpDecorate %img DescriptorSet 0
               OpDecorate %img Binding 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
        %int = OpTypeInt 32 1
       %v4f = OpTypeVector %float 4
       %v2i = OpTypeVector %int 2
       %im = OpTypeImage %float 2D 0 0 0 2 Unknown
      %p_im = OpTypePointer UniformConstant %im
      %int_0 = OpConstant %int 0
      %coord = OpConstantComposite %v2i %int_0 %int_0
       %img = OpVariable %p_im UniformConstant
       %main = OpFunction %void None %fn
      %entry = OpLabel
          %i = OpLoad %im %img
          %v = OpImageRead %v4f %i %coord
               OpImageWrite %i %coord %v
               OpReturn
               OpFunctionEnd
'''


# texelFetch with an Offset operand: a constant one is ConstOffset in Vulkan, a register one is an addition on the coordinates
OFF = r'''
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %tex %dyn
               OpExecutionMode %main LocalSize 8 8 1
               OpDecorate %tex DescriptorSet 0
               OpDecorate %tex Binding 0
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
        %int = OpTypeInt 32 1
       %v4f = OpTypeVector %float 4
       %v2i = OpTypeVector %int 2
       %im = OpTypeImage %float 2D 0 0 0 1 Unknown
      %p_im = OpTypePointer UniformConstant %im
      %p_v2 = OpTypePointer Private %v2i
      %int_0 = OpConstant %int 0
      %int_1 = OpConstant %int 1
      %coord = OpConstantComposite %v2i %int_0 %int_1
       %tex = OpVariable %p_im UniformConstant
       %dyn = OpVariable %p_v2 Private
       %main = OpFunction %void None %fn
      %entry = OpLabel
          %i = OpLoad %im %tex
          %o = OpLoad %v2i %dyn
          %b = OpImageFetch %v4f %i %coord Lod|Offset %int_0 %coord
          %c = OpImageFetch %v4f %i %coord Lod|Offset %int_0 %o
               OpReturn
               OpFunctionEnd
'''


def test_image_offset(w):
    spv = assemble(OFF, w, 'off')
    T(val(spv, w, 'off0') != 'ok', 'a texel fetch with Offset is invalid without the edit')
    m = S.Module(spv)
    p = S.ImageOffset(); T(p.applies(m, 'cs') and p.run(m, 'cs'), 'image_offset applies to a fetch with Offset')
    out = m.bytes(); T(val(out, w, 'off1') == 'ok', 'the edited module is valid: ' + val(out, w, 'off1'))
    fetches = [x for x in m.ins if x[0] & 0xffff == 95]
    T(len(fetches) == 2 and sorted(x[5] for x in fetches) == [2, 2 | 8], f'one ConstOffset fetch, one plain Lod fetch: {[hex(x[5]) for x in fetches]}')
    T(sum(x[0] & 0xffff == 128 for x in m.ins) == 1, 'the register offset is an OpIAdd on the coordinates')


# an exchange whose result goes to a variable nothing reads is a float add (ampere-lower's marker); one whose result is read stays
XCH = r'''
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %sb
               OpExecutionMode %main LocalSize 8 8 1
               OpDecorate %arr ArrayStride 4
               OpMemberDecorate %blk 0 Offset 0
               OpDecorate %blk Block
               OpDecorate %sb DescriptorSet 0
               OpDecorate %sb Binding 0
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
        %int = OpTypeInt 32 1
        %arr = OpTypeRuntimeArray %uint
        %blk = OpTypeStruct %arr
      %p_blk = OpTypePointer StorageBuffer %blk
      %p_u = OpTypePointer StorageBuffer %uint
      %p_fu = OpTypePointer Function %uint
       %sb = OpVariable %p_blk StorageBuffer
      %int_0 = OpConstant %int 0
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_f = OpConstant %uint 1065353216
       %main = OpFunction %void None %fn
      %entry = OpLabel
       %dead = OpVariable %p_fu Function
       %live = OpVariable %p_fu Function
        %ptr = OpAccessChain %p_u %sb %int_0 %int_0
         %r1 = OpAtomicExchange %uint %ptr %uint_1 %uint_0 %uint_f
               OpStore %dead %r1
         %r2 = OpAtomicExchange %uint %ptr %uint_1 %uint_0 %uint_f
               OpStore %live %r2
         %l = OpLoad %uint %live
               OpReturn
               OpFunctionEnd
'''


def test_atomic_float_add(w):
    spv = assemble(XCH, w, 'xch')
    T(val(spv, w, 'xch0') == 'ok', 'the exchange module is valid as written')
    m = S.Module(spv)
    p = S.AtomicFloatAdd(); T(p.applies(m, 'cs') and p.run(m, 'cs'), 'atomic_float_add applies to a discarded exchange')
    out = m.bytes(); T(val(out, w, 'xch1') == 'ok', 'the edited module is valid: ' + val(out, w, 'xch1'))
    ops = [x[0] & 0xffff for x in m.ins]
    T(ops.count(229) == 1 and ops.count(230) == 1 and ops.count(129) == 1, 'one exchange kept (its result is read), one became a compare-exchange loop with a float add')

# a translated program's untracked global memory: an empty helper the translator made for a load and one for a store, and a multi-target load that falls through to zero
GM = r"""
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
               OpName %cp_s0 "cp_s0"
               OpDecorate %arr ArrayStride 4
               OpMemberDecorate %blk 0 Offset 0
               OpDecorate %blk BufferBlock
               OpDecorate %cp_s0 DescriptorSet 0
               OpDecorate %cp_s0 Binding 3
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
        %int = OpTypeInt 32 1
       %uint = OpTypeInt 32 0
       %bool = OpTypeBool
        %pfi = OpTypePointer Function %int
        %arr = OpTypeRuntimeArray %uint
        %blk = OpTypeStruct %arr
      %p_blk = OpTypePointer Uniform %blk
        %p_u = OpTypePointer Uniform %uint
      %cp_s0 = OpVariable %p_blk Uniform
      %int_0 = OpConstant %int 0
      %int_7 = OpConstant %int 7
     %fnload = OpTypeFunction %int %pfi %pfi
    %fnstore = OpTypeFunction %void %pfi %pfi %pfi
       %main = OpFunction %void None %fn
      %entry = OpLabel
          %a = OpVariable %pfi Function
          %b = OpVariable %pfi Function
          %c = OpVariable %pfi Function
               OpStore %a %int_7
               OpStore %b %int_0
               OpStore %c %int_7
          %r = OpFunctionCall %int %load %a %b
         %r2 = OpFunctionCall %int %multi %a %b
         %r3 = OpFunctionCall %void %store %a %b %c
               OpReturn
               OpFunctionEnd
       %load = OpFunction %int None %fnload
         %l0 = OpFunctionParameter %pfi
         %l1 = OpFunctionParameter %pfi
         %ll = OpLabel
               OpReturnValue %int_0
               OpFunctionEnd
      %store = OpFunction %void None %fnstore
         %s0 = OpFunctionParameter %pfi
         %s1 = OpFunctionParameter %pfi
         %s2 = OpFunctionParameter %pfi
         %sl = OpLabel
               OpReturn
               OpFunctionEnd
      %multi = OpFunction %int None %fnload
         %m0 = OpFunctionParameter %pfi
         %m1 = OpFunctionParameter %pfi
         %ml = OpLabel
         %mv = OpLoad %int %m0
         %mu = OpBitcast %uint %mv
         %mc = OpULessThan %bool %mu %mu
               OpSelectionMerge %mend None
               OpBranchConditional %mc %mthen %mend
      %mthen = OpLabel
         %mp = OpAccessChain %p_u %cp_s0 %int_0 %int_0
         %mx = OpLoad %uint %mp
         %my = OpBitcast %int %mx
               OpReturnValue %my
       %mend = OpLabel
               OpReturnValue %int_0
               OpFunctionEnd
"""
LOADNOTE = 'translator: Failed to find storage buffer for global memory operation "Load".'
STORENOTE = 'translator: Failed to find storage buffer for global memory operation "Store".'


def test_gmem(w):
    spv = assemble(GM, w, 'gm', 'spv1.3')   # as the translator writes it
    T(val(spv, w, 'gm0') == 'ok', 'the global memory module is valid as written: ' + val(spv, w, 'gm0'))
    m = S.Module(spv); m.gmem_notes = [LOADNOTE, LOADNOTE, STORENOTE]
    p = S.GlobalMemoryBda()
    T(p.applies(m, 'cs') and p.run(m, 'cs'), 'gmem_bda applies to the empty helpers')
    out = m.bytes(); r = val(out, w, 'gm1'); T(r == 'ok', 'the edited module is valid under Vulkan 1.3: ' + r)
    T(p.skipped == 0, 'nothing was skipped')
    ops = [x[0] & 0xffff for x in m.ins]
    T(ops.count(120) == 3, 'three physical pointers: load, store, the multi-target fall-through (%d)' % ops.count(120))
    T(S.GlobalMemoryBda.CAP_PSB in m.caps() and S.GlobalMemoryBda.CAP_INT64 in m.caps(), 'the capabilities are declared')
    T(next(x for x in m.ins if x[0] & 0xffff == S.OpMemoryModel)[1] == 5348, 'the addressing model is PhysicalStorageBuffer64')
    T(any(x[0] & 0xffff == S.OpStore and len(x) == 5 and x[3] == 2 and x[4] == 4 for x in m.ins), 'the store is aligned to 4')
    mm = S.Module(out)
    T(not S.GlobalMemoryBda().applies(mm, 'cs'), 'the edit does not apply twice')
    meta = R.reflect('k', 'cs', mm, {'sbufs': [{'binding': 3, 'slot': 0, 'cb_slot': 0, 'cb_offset': 12, 'flags': 0}]})
    gb = [b for b in meta['bindings'] if b['name'] == 'gmem_table']
    T(len(gb) == 1 and gb[0]['descriptor'] == 'storage_buffer' and gb[0]['binding'] == 4 and gb[0]['nvn']['kind'] == 'gmem' and gb[0]['nvn']['rw'] is True, f'reflected as a gmem binding that writes: {gb}')
    T(next(b for b in meta['bindings'] if b['name'] == 'cp_s0')['nvn']['kind'] == 'sbuf', 'the tracked storage buffer stays what it was')
    T(gb[0]['nvn']['open'] and gb[0]['nvn']['roots'] == [], f'constant addresses are not a pointer the runtime can read: {gb}')
    # an address from a fixed word of a tracked storage buffer plus a carry: a root the runtime can dereference, and a second call through the first one's result is not
    ptr = GM.replace('               OpStore %a %int_7\n               OpStore %b %int_0\n', '''         %q0 = OpAccessChain %p_u %cp_s0 %int_0 %int_4
         %q1 = OpAccessChain %p_u %cp_s0 %int_0 %int_5
         %v0 = OpLoad %uint %q0
         %v1 = OpLoad %uint %q1
         %c0 = OpBitcast %int %v0
         %c1 = OpBitcast %int %v1
         %s9 = OpIAdd %int %c0 %int_7
               OpStore %a %s9
               OpStore %b %c1
''').replace('      %int_7 = OpConstant %int 7\n', '      %int_7 = OpConstant %int 7\n      %int_4 = OpConstant %int 4\n      %int_5 = OpConstant %int 5\n')
    m = S.Module(assemble(ptr, w, 'gmp', 'spv1.3')); m.gmem_notes = [LOADNOTE, LOADNOTE, STORENOTE]
    p = S.GlobalMemoryBda(); T(p.run(m, 'cs'), 'gmem_bda applies to the table-pointer program')
    r = val(m.bytes(), w, 'gmp1'); T(r == 'ok', 'valid: ' + r)
    roots, open_ = S.GlobalMemoryBda().roots(S.Module(m.bytes()))
    T(roots == [[3, 4]] and not open_, f'the pointer at word 4 of binding 3 is the only root: {roots} {open_}')
    # the notes decide: a store-shaped helper the notes call something else stays dropped, and a module without the notes' operation keeps its helper
    m = S.Module(spv); m.gmem_notes = [LOADNOTE, 'translator: Failed to find storage buffer for global memory operation "AtomicSwap".']
    p = S.GlobalMemoryBda(); p.run(m, 'cs'); ops = [x[0] & 0xffff for x in m.ins]
    T(ops.count(120) == 2 and p.skipped == 1, 'an unsupported operation keeps the old behaviour (%d pointers, %d skipped)' % (ops.count(120), p.skipped))
    r = val(m.bytes(), w, 'gm2'); T(r == 'ok', 'still valid: ' + r)
    m = S.Module(spv); m.gmem_notes = [LOADNOTE, 'translator: Failed to find storage buffer for global memory operation "AtomicMaxU32".']
    p = S.GlobalMemoryBda(); p.run(m, 'cs'); ops = [x[0] & 0xffff for x in m.ins]
    T(ops.count(239) == 1 and p.skipped == 0, 'an atomic without a result becomes OpAtomicUMax on the pointer')
    r = val(m.bytes(), w, 'gm3'); T(r == 'ok', 'the atomic form is valid: ' + r)
    # the kind of a small integer helper cannot be told: not in the corpus, and a second load shape would be skipped
    two = GM.replace('         %r3 = OpFunctionCall %void %store %a %b %c\n', '         %r3 = OpFunctionCall %int %load2 %a %b\n').replace('      %store = OpFunction %void None %fnstore\n         %s0 = OpFunctionParameter %pfi\n         %s1 = OpFunctionParameter %pfi\n         %s2 = OpFunctionParameter %pfi\n         %sl = OpLabel\n               OpReturn\n', '      %load2 = OpFunction %int None %fnload\n         %s0 = OpFunctionParameter %pfi\n         %s1 = OpFunctionParameter %pfi\n         %sl = OpLabel\n               OpReturnValue %int_0\n')
    m = S.Module(assemble(two, w, 'gm4', 'spv1.3')); m.gmem_notes = [LOADNOTE, LOADNOTE]
    p = S.GlobalMemoryBda(); p.run(m, 'cs'); ops = [x[0] & 0xffff for x in m.ins]
    T(ops.count(120) == 1 and p.skipped == 2, 'two load helpers are ambiguous: both stay dropped, the fall-through still works (%d, %d)' % (ops.count(120), p.skipped))
    # untouched: a module with no helper, as tracked programs are
    m = S.Module(assemble(CS, w, 'gm5')); m.gmem_notes = []
    T(not S.GlobalMemoryBda().applies(m, 'cs'), 'a program without such helpers is not touched')

# the smoke test: a program with both helpers on a real device (lavapipe), through tests/vk_gmem_probe.c: eight queries from binding 0, results at word 128, one store at words 64-66
GM_RUN = r"""
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
               OpName %cp_s0 "cp_s0"
               OpDecorate %arr ArrayStride 4
               OpMemberDecorate %blk 0 Offset 0
               OpDecorate %blk BufferBlock
               OpDecorate %cp_s0 DescriptorSet 0
               OpDecorate %cp_s0 Binding 0
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
        %int = OpTypeInt 32 1
       %uint = OpTypeInt 32 0
       %bool = OpTypeBool
        %pfi = OpTypePointer Function %int
        %pfu = OpTypePointer Function %uint
        %arr = OpTypeRuntimeArray %uint
        %blk = OpTypeStruct %arr
      %p_blk = OpTypePointer Uniform %blk
        %p_u = OpTypePointer Uniform %uint
      %cp_s0 = OpVariable %p_blk Uniform
      %int_0 = OpConstant %int 0
       %uint_0 = OpConstant %uint 0
       %uint_1 = OpConstant %uint 1
       %uint_2 = OpConstant %uint 2
       %uint_8 = OpConstant %uint 8
     %uint_64 = OpConstant %uint 64
     %uint_65 = OpConstant %uint 65
     %uint_66 = OpConstant %uint 66
    %uint_128 = OpConstant %uint 128
     %fnload = OpTypeFunction %int %pfi %pfi
    %fnstore = OpTypeFunction %void %pfi %pfi %pfi
       %main = OpFunction %void None %fn
      %entry = OpLabel
          %a = OpVariable %pfi Function
          %b = OpVariable %pfi Function
          %c = OpVariable %pfi Function
          %i = OpVariable %pfu Function
               OpStore %i %uint_0
               OpBranch %hdr
        %hdr = OpLabel
               OpLoopMerge %done %cont None
               OpBranch %check
      %check = OpLabel
         %iv = OpLoad %uint %i
         %go = OpULessThan %bool %iv %uint_8
               OpBranchConditional %go %body %done
       %body = OpLabel
         %i2 = OpIMul %uint %iv %uint_2
        %ilo = OpIAdd %uint %i2 %uint_1
        %ihi = OpIAdd %uint %i2 %uint_2
        %plo = OpAccessChain %p_u %cp_s0 %int_0 %ilo
        %phi = OpAccessChain %p_u %cp_s0 %int_0 %ihi
         %vl = OpLoad %uint %plo
         %vh = OpLoad %uint %phi
         %sl = OpBitcast %int %vl
         %sh = OpBitcast %int %vh
               OpStore %a %sl
               OpStore %b %sh
          %r = OpFunctionCall %int %load %a %b
         %ru = OpBitcast %uint %r
         %io = OpIAdd %uint %iv %uint_128
         %po = OpAccessChain %p_u %cp_s0 %int_0 %io
               OpStore %po %ru
               OpBranch %cont
       %cont = OpLabel
        %inc = OpIAdd %uint %iv %uint_1
               OpStore %i %inc
               OpBranch %hdr
       %done = OpLabel
        %p64 = OpAccessChain %p_u %cp_s0 %int_0 %uint_64
        %p65 = OpAccessChain %p_u %cp_s0 %int_0 %uint_65
        %p66 = OpAccessChain %p_u %cp_s0 %int_0 %uint_66
        %v64 = OpLoad %uint %p64
        %v65 = OpLoad %uint %p65
        %v66 = OpLoad %uint %p66
        %s64 = OpBitcast %int %v64
        %s65 = OpBitcast %int %v65
        %s66 = OpBitcast %int %v66
               OpStore %a %s64
               OpStore %b %s65
               OpStore %c %s66
         %r3 = OpFunctionCall %void %store %a %b %c
               OpReturn
               OpFunctionEnd
       %load = OpFunction %int None %fnload
         %l0 = OpFunctionParameter %pfi
         %l1 = OpFunctionParameter %pfi
         %ll = OpLabel
               OpReturnValue %int_0
               OpFunctionEnd
      %store = OpFunction %void None %fnstore
         %s0 = OpFunctionParameter %pfi
         %s1 = OpFunctionParameter %pfi
         %s2 = OpFunctionParameter %pfi
         %ls = OpLabel
               OpReturn
               OpFunctionEnd
"""


def test_gmem_lavapipe(w):
    icd = os.environ.get('NXSHADER_LVP_ICD') or next((p for p in ('/usr/share/vulkan/icd.d/lvp_icd.json', '/etc/vulkan/icd.d/lvp_icd.json') if os.path.exists(p)), None)
    cc = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
    if not icd or not cc: print('gmem smoke test skipped: no lavapipe ICD or C compiler'); return
    exe = os.path.join(w, 'vk_gmem_probe')
    r = subprocess.run([cc, '-O1', '-o', exe, os.path.join(HERE, 'vk_gmem_probe.c'), '-lvulkan'], capture_output=True, text=True)
    if r.returncode: print('gmem smoke test skipped: the probe does not build here:', r.stderr.strip()[:200]); return
    m = S.Module(assemble(GM_RUN, w, 'gmrun', 'spv1.3')); m.gmem_notes = [LOADNOTE, STORENOTE]
    p = S.GlobalMemoryBda(); T(p.applies(m, 'cs') and p.run(m, 'cs'), 'gmem_bda applies to the smoke test program')
    out = m.bytes(); v = val(out, w, 'gmrun1'); T(v == 'ok', 'the smoke test program is valid: ' + v)
    sp = os.path.join(w, 'gmrun.spv'); open(sp, 'wb').write(out)
    binding = R.reflect('k', 'cs', S.Module(out), {'sbufs': []})
    tb = next(b['binding'] for b in binding['bindings'] if b['name'] == 'gmem_table')
    env = dict(os.environ, VK_DRIVER_FILES=icd, VK_ICD_FILENAMES=icd, LP_NUM_THREADS='2')
    r = subprocess.run([exe, sp, str(tb)], capture_output=True, text=True, env=env, timeout=60)
    T(r.returncode == 0, f'the probe ran: exit {r.returncode}: {r.stdout[-200:]} {r.stderr[-200:]}')
    if r.returncode: return
    runs = [[int(x) for x in l.split()[1:]] for l in r.stdout.splitlines() if l.startswith('RESULT')]
    stored = [int(l.split()[1]) for l in r.stdout.splitlines() if l.startswith('STORED')]
    T(len(runs) == 2 and len(stored) == 2, 'two runs reported: ' + r.stdout[:200])
    if len(runs) != 2: return
    A = 0xA0000000; B = 0xB0000000
    q = runs[0]
    T(q[0] == A + 0x10 // 4, f'a load through the table reads the pool: {q[0]:#x}')
    T(q[1] == A + 0x1010 // 4, f'on the next page, an unaligned address reads its aligned word: {q[1]:#x}')
    T(q[2] == B + 8 // 4, f'another pool, under another level-1 entry: {q[2]:#x}')
    T(q[3] == 0, f'an unmapped page in a mapped block reads 0: {q[3]:#x}')
    T(q[4] == 0, f'an address with no block reads 0: {q[4]:#x}')
    T(q[5] == 0, f'an address of 40 bits or more reads 0: {q[5]:#x}')
    if 'SPARSE 1' in r.stdout: T(q[6] == 0xC0000001, f'a bound granule of a sparse mirror reads through its device address: {q[6]:#x}')
    T(stored[0] == 0x12345678, f'a store through the table lands in the pool: {stored[0]:#x}')
    T(not any(runs[1]), f'an empty table: everything reads 0: {runs[1]}')
    T(stored[1] == 0x12345678, f'an empty table: the store is dropped: {stored[1]:#x}')
    # the runtime's own table, sync and mirrors, when a build of its test is given
    rt = os.environ.get('NXSHADER_GMEM_TEST')
    if rt:
        r = subprocess.run([rt, sp, str(tb)], capture_output=True, text=True, env=dict(env, NXRT_LOG_DIR=w, NXRT_VK_CACHE_DIR=w), timeout=120)
        T(r.returncode == 0, f'nxrt-tests-vk-gmem: exit {r.returncode}: {r.stdout[-600:]}')
        print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else 'nxrt-tests-vk-gmem printed nothing')


def tool(n): return N.vk_tool(n)


def assemble(text, w, name, env='vulkan1.3'):
    s, o = os.path.join(w, name + '.spvasm'), os.path.join(w, name + '.spv')
    open(s, 'w').write(text)
    r = subprocess.run([tool('spirv-as'), '--target-env', env, s, '-o', o], capture_output=True, text=True)
    if r.returncode: raise RuntimeError('spirv-as: ' + r.stderr)
    return open(o, 'rb').read()


def val(spv, w, name):
    p = os.path.join(w, name + '.val.spv'); open(p, 'wb').write(spv)
    return N.spirv_val(p)


def test_edits(w):
    vs = assemble(VS, w, 'vs'); fs = assemble(FS, w, 'fs'); cs = assemble(CS, w, 'cs')
    T(val(vs, w, 'vs0') == 'ok', 'the test vertex shader is valid as written')
    m = S.Module(vs)
    applied = [p.ident() for p in S.DEFAULT_PASSES if p.applies(m, 'vs') and p.run(m, 'vs')]
    T(applied == ['caps_trim v1', 'vout_init v1'], f'edits on the vertex shader: {applied}')
    T(10 not in m.caps() and 54 not in m.caps(), 'Float64 and GeometryStreams without a use are trimmed')
    out = m.bytes(); T(val(out, w, 'vs1') == 'ok', 'the edited vertex shader is valid: ' + val(out, w, 'vs1'))
    ep = m.entry_points()[0]; k = m.function_body_start(ep['func'])
    stores = []
    while m.ins[k + len(stores)][0] & 0xffff == S.OpStore: stores.append(m.ins[k + len(stores)])
    T(len(stores) == 4, f'one (0,0,0,1) store per user output at the entry: {len(stores)}')
    # the values: vec4 (0,0,0,1); the component-2 vec2 gets (0,1); the integer output the bits of 1.0f
    vals = {}
    for st in stores:
        c = m.inst(st[2]); vals[st[1]] = [m.inst(p)[3] for p in c[3:]] if c[0] & 0xffff == S.OpConstantComposite else None
    byloc = {(m.deco(v, S.DecLocation)[0], (m.deco(v, S.DecComponent) or [0])[0]): vv for v, vv in vals.items()}
    T(byloc.get((0, 0)) == [0, 0, 0, S.float_bits(1.0)], f'vec4 output starts (0,0,0,1): {byloc.get((0, 0))}')
    T(byloc.get((1, 2)) == [0, S.float_bits(1.0)], f'a component-2 vec2 takes z, w: {byloc.get((1, 2))}')
    T(byloc.get((2, 0)) == [0, 0, 0, 0x3f800000], f'an integer output gets the bits of 1.0f: {byloc.get((2, 0))}')
    # variants
    g = S.Module(out); T(S.DepthGL().run(g, 'vs') and val(g.bytes(), w, 'gl') == 'ok', 'depth_gl variant is valid')
    T(any(x[0] & 0xffff == S.OpFMul for x in g.ins), 'depth_gl rewrites z')
    pmod = S.Module(out); T(S.PadOutputs([5, 7]).run(pmod, 'vs') and val(pmod.bytes(), w, 'pad') == 'ok', 'pad_outputs variant is valid')
    T({(pmod.deco(v, S.DecLocation) or [None])[0] for v, sc, _ in pmod.global_vars() if sc == S.SC_Output} >= {5, 7}, 'pad_outputs adds locations 5 and 7')
    f = S.Module(fs); T(S.DualSource().run(f, 'ps') and val(f.bytes(), w, 'dual') == 'ok', 'dual_source variant is valid')
    T(any(x[0] & 0xffff == S.OpDecorate and x[2] == S.DecIndex and x[3] == 1 for x in f.ins), 'output 1 becomes location 0 index 1')
    c = S.Module(cs)
    T(val(cs, w, 'cs0') != 'ok', 'reading an image without a format is invalid without the capability')
    T(S.StorageFormatCaps().run(c, 'cs') and val(c.bytes(), w, 'cs1') == 'ok', 'storage_fmt_caps makes it valid')
    T(55 in c.caps() and 56 in c.caps(), 'StorageImageRead/WriteWithoutFormat added')
    # the integer-input analysis (on the SSA form, as nxshader runs it)
    a = N.ssa_copy(vs, w, 'an')
    T(a is not None and S.int_only_inputs(a) == {'1': 'uint'}, f'location 1 is used only as integer bits: {S.int_only_inputs(a) if a else None}')
    return vs, fs, cs


class FakeTranslator:
    """the translator's interface; writes the test modules by stage"""
    def __init__(self, mods, ident): self.mods, self.id, self.runs = mods, ident, 0
    def run(self, jobs, w, low_priority=True):
        self.runs += len(jobs)
        for j in jobs:
            open(os.path.join(w, j['key'] + '.spv'), 'wb').write(self.mods[j['stage']])
            json.dump({'key': j['key'], 'stage': j['stage'], 'notes': [], 'cbufs': [], 'sbufs': [], 'textures': [], 'images': []}, open(os.path.join(w, j['key'] + '.res.json'), 'w'))
        return {}


def record(code):
    ctl = bytearray(0x880); ctl[0:4] = F.CTL_MAGIC; struct.pack_into('<I', ctl, 0x20, 0x880); struct.pack_into('<2I', ctl, 0x700, len(code), len(code))
    rec = bytearray(b'BTCDBTCD' + bytes(0x30)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(code))
    return bytes(rec + code + ctl)


class FakeCorpus:
    def __init__(self):
        self.recs = {'aaaa000000000001': ('vs_test.bin', record(bytes(0x80) + b'\x01' * 32)), 'aaaa000000000002': ('ps_test.bin', record(bytes(0x80) + b'\x02' * 32)),
                     'aaaa000000000003': ('ps_test.bin', record(bytes(0x80) + b'\x03' * 32))}
        self.facts, self.facts_raw, self.source = {}, {}, {}
    def keys(self): return sorted(self.recs)
    def record(self, k): return self.recs[k]


def test_cache(w, vs, fs):
    root = os.path.join(w, 'cache'); keys = FakeCorpus().keys()
    def build(xl, corpus=None, label=''):
        cache = C.Cache(root); wd = tempfile.mkdtemp(dir=w)
        b = N.Builder(corpus or FakeCorpus(), cache, xl, wd, validate=True)
        cache.set_chain(b.chain)
        res = b.make(keys)
        T(all(r['status'] == 'ok' and r['spirv_val'] == 'ok' for r in res.values()), f'{label}: every shader builds and validates: {[(r["key"], r["status"], r.get("spirv_val")) for r in res.values()]}')
        return cache.stats, b
    mods = {'vs': vs, 'ps': fs}
    st, b1 = build(FakeTranslator(mods, 'xlate A'), label='first build')
    T(st['xlate miss'] == 3 and st['final miss'] == 3, f'first build misses everything: {st}')
    st, _ = build(FakeTranslator(mods, 'xlate A'), label='rerun')
    T(st['xlate hit'] == 3 and st['final hit'] == 3, f'a rerun hits everything: {st}')
    st, _ = build(FakeTranslator(mods, 'xlate B'), label='new translator')
    T(st['xlate miss'] == 3, f'a new translator misses every shader: {st}')
    old = S.VertexOutputInit.version
    try:
        S.VertexOutputInit.version = old + 1
        st, b2 = build(FakeTranslator(mods, 'xlate B'), label='new vout_init version')
        T(st['xlate hit'] == 3 and st['final miss'] == 1 and st['final hit'] == 2, f'a new version of the vertex-output edit misses only the vertex shader: {st}')
        T(b2.chain != b1.chain, 'the chain id follows the edit versions')
        stale = C.Cache(root)
        T(stale.lookup('aaaa000000000001', chain=b1.chain) is None and stale.lookup('aaaa000000000001') is not None, 'an entry made by another chain is stale; the current one is found')
    finally: S.VertexOutputInit.version = old
    fc = FakeCorpus(); fc.facts['aaaa000000000002'] = {'fout': {'0': 'uint'}}
    st, _ = build(FakeTranslator(mods, 'xlate B'), fc, label='new facts')
    T(st['xlate miss'] == 1 and st['xlate hit'] == 2, f'new facts for one shader miss only that shader: {st}')
    # gc drops the older generations and keeps what the published shaders need (a vertex shader's first translation too): the rerun still hits everything
    n, freed = C.Cache(root).gc()
    T(n > 0 and freed > 0, f'gc removes the entries no published shader refers to: {n}')
    st, _ = build(FakeTranslator(mods, 'xlate B'), fc, label='after gc')
    T(st['xlate hit'] == 3 and st['final hit'] == 3, f'after gc a rerun hits everything: {st}')


# ---- the loop guard (nxs_spv.LoopGuard, CasAtomicLoad) ----
# A compute shader whose storage buffer (binding 0) word 0 decides whether a loop continues: with word 0 = 0 it runs the body once, with word 0 != 0 (fill "ones") it
# never ends. The body counts into word 1 with an atomic (a loop without a side effect is deleted by LLVM: lavapipe would run it as no loop at all)
LG_HEAD = r"""
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %v
               OpExecutionMode %main LocalSize 1 1 1
               OpDecorate %sb Block
               OpMemberDecorate %sb 0 Offset 0
               OpDecorate %arr ArrayStride 4
               OpDecorate %v Binding 0
               OpDecorate %v DescriptorSet 0
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
     %uint_5 = OpConstant %uint 5
        %arr = OpTypeRuntimeArray %uint
         %sb = OpTypeStruct %arr
       %pssb = OpTypePointer StorageBuffer %sb
         %pu = OpTypePointer StorageBuffer %uint
          %v = OpVariable %pssb StorageBuffer
         %fnu = OpTypeFunction %uint
"""
LG_ENDLESS = LG_HEAD + r"""
       %main = OpFunction %void None %fn
      %entry = OpLabel
               OpBranch %hdr
        %hdr = OpLabel
               OpLoopMerge %mrg %cont None
               OpBranch %body
       %body = OpLabel
         %p1 = OpAccessChain %pu %v %uint_0 %uint_1
          %a = OpAtomicIAdd %uint %p1 %uint_1 %uint_0 %uint_1
               OpBranch %cont
       %cont = OpLabel
         %p0 = OpAccessChain %pu %v %uint_0 %uint_0
          %x = OpAtomicLoad %uint %p0 %uint_1 %uint_0
          %c = OpINotEqual %bool %x %uint_0
               OpBranchConditional %c %hdr %mrg
        %mrg = OpLabel
               OpReturn
               OpFunctionEnd
"""
# a while loop whose header branches (a phi counter, the merge reads it through a phi of its own: the header is its predecessor), run 5 times, result in word 2
LG_WHILE = LG_HEAD + r"""
       %main = OpFunction %void None %fn
      %entry = OpLabel
               OpBranch %hdr
        %hdr = OpLabel
          %i = OpPhi %uint %uint_0 %entry %inext %cont
         %lt = OpULessThan %bool %i %uint_5
               OpLoopMerge %mrg %cont None
               OpBranchConditional %lt %body %mrg
       %body = OpLabel
         %p1 = OpAccessChain %pu %v %uint_0 %uint_1
          %a = OpAtomicIAdd %uint %p1 %uint_1 %uint_0 %uint_1
               OpBranch %cont
       %cont = OpLabel
      %inext = OpIAdd %uint %i %uint_1
               OpBranch %hdr
        %mrg = OpLabel
          %r = OpPhi %uint %i %hdr
         %p2 = OpAccessChain %pu %v %uint_0 %uint_2
               OpStore %p2 %r
               OpReturn
               OpFunctionEnd
"""
# a loop of one block: the header is its own continue target and back-edge block
LG_ONEBLOCK = LG_HEAD + r"""
       %main = OpFunction %void None %fn
      %entry = OpLabel
               OpBranch %hdr
        %hdr = OpLabel
         %p1 = OpAccessChain %pu %v %uint_0 %uint_1
          %a = OpAtomicIAdd %uint %p1 %uint_1 %uint_0 %uint_1
         %p0 = OpAccessChain %pu %v %uint_0 %uint_0
          %x = OpAtomicLoad %uint %p0 %uint_1 %uint_0
          %c = OpINotEqual %bool %x %uint_0
               OpLoopMerge %mrg %hdr None
               OpBranchConditional %c %hdr %mrg
        %mrg = OpLabel
               OpReturn
               OpFunctionEnd
"""
# nested endless loops (the budget is shared: the sum of both, not a budget each) and a callee that returns a value out of an endless loop
LG_NESTED = LG_HEAD + r"""
       %main = OpFunction %void None %fn
      %entry = OpLabel
               OpBranch %h1
         %h1 = OpLabel
               OpLoopMerge %m1 %c1 None
               OpBranch %b1
         %b1 = OpLabel
               OpBranch %h2
         %h2 = OpLabel
               OpLoopMerge %m2 %c2 None
               OpBranch %b2
         %b2 = OpLabel
         %p1 = OpAccessChain %pu %v %uint_0 %uint_1
          %a = OpAtomicIAdd %uint %p1 %uint_1 %uint_0 %uint_1
               OpBranch %c2
         %c2 = OpLabel
         %p0 = OpAccessChain %pu %v %uint_0 %uint_0
          %x = OpAtomicLoad %uint %p0 %uint_1 %uint_0
          %c = OpINotEqual %bool %x %uint_0
               OpBranchConditional %c %h2 %m2
         %m2 = OpLabel
               OpBranch %c1
         %c1 = OpLabel
         %y = OpAtomicLoad %uint %p0 %uint_1 %uint_0
         %d = OpINotEqual %bool %y %uint_0
               OpBranchConditional %d %h1 %m1
         %m1 = OpLabel
               OpReturn
               OpFunctionEnd
"""
LG_CALLEE = LG_HEAD + r"""
     %callee = OpFunction %uint None %fnu
        %ce = OpLabel
               OpBranch %hdr
        %hdr = OpLabel
               OpLoopMerge %mrg %cont None
               OpBranch %body
       %body = OpLabel
         %p1 = OpAccessChain %pu %v %uint_0 %uint_1
          %a = OpAtomicIAdd %uint %p1 %uint_1 %uint_0 %uint_1
               OpBranch %cont
       %cont = OpLabel
         %p0 = OpAccessChain %pu %v %uint_0 %uint_0
          %x = OpAtomicLoad %uint %p0 %uint_1 %uint_0
          %c = OpINotEqual %bool %x %uint_0
               OpBranchConditional %c %hdr %mrg
        %mrg = OpLabel
               OpReturnValue %uint_5
               OpFunctionEnd
       %main = OpFunction %void None %fn
      %entry = OpLabel
         %rv = OpFunctionCall %uint %callee
         %p2 = OpAccessChain %pu %v %uint_0 %uint_2
               OpStore %p2 %rv
               OpReturn
               OpFunctionEnd
"""
# a fragment shader with a loop (no behaviour check: valid, and the loop ends in a return)
LG_FRAG = r"""
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %out %in
               OpExecutionMode %main OriginUpperLeft
               OpDecorate %out Location 0
               OpDecorate %in Location 0
               OpDecorate %in Flat
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
      %float = OpTypeFloat 32
       %uint = OpTypeInt 32 0
     %uint_0 = OpConstant %uint 0
       %v4f = OpTypeVector %float 4
     %pout = OpTypePointer Output %v4f
      %pin = OpTypePointer Input %uint
        %out = OpVariable %pout Output
         %in = OpVariable %pin Input
       %main = OpFunction %void None %fn
      %entry = OpLabel
               OpBranch %hdr
        %hdr = OpLabel
               OpLoopMerge %mrg %cont None
               OpBranch %body
       %body = OpLabel
          %x = OpLoad %uint %in
          %c = OpINotEqual %bool %x %uint_0
               OpBranch %cont
       %cont = OpLabel
               OpBranchConditional %c %hdr %mrg
        %mrg = OpLabel
               OpReturn
               OpFunctionEnd
"""
# the translator's sub-word store to shared memory: load the word (plain), insert, compare-exchange, again while it changed
LG_CAS = r"""
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %sh
               OpExecutionMode %main LocalSize 8 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_9 = OpConstant %uint 9
       %arr = OpTypeArray %uint %uint_9
       %psh = OpTypePointer Workgroup %arr
        %pu = OpTypePointer Workgroup %uint
         %sh = OpVariable %psh Workgroup
       %main = OpFunction %void None %fn
      %entry = OpLabel
               OpBranch %hdr
        %hdr = OpLabel
               OpLoopMerge %mrg %cont None
               OpBranch %body
       %body = OpLabel
         %p0 = OpAccessChain %pu %sh %uint_1
        %old = OpLoad %uint %p0
        %new = OpBitFieldInsert %uint %old %uint_9 %uint_0 %uint_1
        %prev = OpAtomicCompareExchange %uint %p0 %uint_1 %uint_0 %uint_0 %new %old
         %ne = OpINotEqual %bool %prev %old
               OpBranch %cont
       %cont = OpLabel
               OpBranchConditional %ne %hdr %mrg
        %mrg = OpLabel
               OpReturn
               OpFunctionEnd
"""


def probe_tool(w):
    """vk_dispatch_probe (built here from tests/vk_dispatch_probe.c, or NXSHADER_PROBE) and the lavapipe ICD; (None, why) when this machine has neither"""
    icd = os.environ.get('NXSHADER_LVP_ICD') or next((p for p in ('/usr/share/vulkan/icd.d/lvp_icd.json', '/etc/vulkan/icd.d/lvp_icd.json') if os.path.exists(p)), None)
    if not icd: return None, 'no lavapipe ICD'
    exe = os.environ.get('NXSHADER_PROBE')
    if exe and os.path.exists(exe): return exe, icd
    cc = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
    if not cc: return None, 'no C compiler'
    out = os.path.join(w, 'vk_dispatch_probe')
    r = subprocess.run([cc, '-O1', '-o', out, os.path.join(HERE, 'vk_dispatch_probe.c'), '-lvulkan'], capture_output=True, text=True)
    return (out, icd) if r.returncode == 0 else (None, 'the probe does not build here: ' + r.stderr.strip()[:200])


def run_probe(exe, icd, spv, fill, nwords=4, timeout=6, groups=(1, 1, 1)):
    env = dict(os.environ, VK_DRIVER_FILES=icd, VK_ICD_FILENAMES=icd, LP_NUM_THREADS='2', PROBE_DUMP=str(nwords))
    try: r = subprocess.run([exe, spv, *map(str, groups), str(timeout), fill, '0:ssbo:4096'], capture_output=True, text=True, env=env, timeout=timeout + 30)
    except subprocess.TimeoutExpired: return 'hung', []
    out = r.stdout
    words = [int(x) for x in out.split('WORDS')[1].split()] if 'WORDS' in out else []
    return ('ok' if r.returncode == 0 else 'timeout' if r.returncode == 3 else 'error %d: %s' % (r.returncode, out.strip()[:100])), words


def guard(spv, w, name, cap):
    m = S.Module(spv); g = S.LoopGuard(cap)
    T(g.applies(m, 'cs') and g.run(m, 'cs') and g.guarded >= 1, f'{name}: loop_guard applies')
    out = m.bytes(); v = val(out, w, name + 'g')
    T(v == 'ok', f'{name}: the guarded module is valid: {v}')
    return m, out


def test_loop_guard(w):
    exe, icd = probe_tool(w)
    if not exe: print('loop_guard behaviour checks skipped:', icd)
    mods = {n: assemble(t, w, n) for n, t in (('endless', LG_ENDLESS), ('while', LG_WHILE), ('oneblock', LG_ONEBLOCK), ('nested', LG_NESTED), ('callee', LG_CALLEE), ('frag', LG_FRAG))}
    for n, spv in mods.items(): T(val(spv, w, n + '0') == 'ok', f'the {n} module is valid as written')
    # the pass list: on by default, left out by NXSHADER_LOOP_GUARD=0, the cap from NXSHADER_LOOP_CAP, the chain id follows both
    T(S.loop_guard_settings({}) == (True, S.LOOP_CAP_DEFAULT), 'the guard is on by default')
    T(S.loop_guard_settings({'NXSHADER_LOOP_GUARD': '0'})[0] is False and S.loop_guard_settings({'NXSHADER_LOOP_GUARD': 'off'})[0] is False, 'NXSHADER_LOOP_GUARD=0/off switches it off')
    T(S.loop_guard_settings({'NXSHADER_LOOP_CAP': '99'}) == (True, 99), 'NXSHADER_LOOP_CAP sets the budget')
    T(any(isinstance(p, S.LoopGuard) for p in S.DEFAULT_PASSES) == S.loop_guard_settings()[0], 'DEFAULT_PASSES holds the guard exactly when the environment asks for it')
    T(S.LoopGuard(5).ident() != S.LoopGuard(6).ident(), 'the pass identity (and so the cache key) includes the cap')
    for n in ('endless', 'while', 'oneblock', 'nested', 'callee'):
        m, out = guard(mods[n], w, n, 7)
        ops = [x[0] & 0xffff for x in m.ins]
        T(ops.count(246) >= 1, f'{n}: loops kept')
    # a module without a loop is left alone
    cs = assemble(CS, w, 'cs_noloop'); mm = S.Module(cs); T(not S.LoopGuard(7).applies(mm, 'cs'), 'no loop: not applicable')
    # the version-1.6 module lists the budget variable in its interface; a 1.3 module does not
    m = S.Module(mods['endless']); S.LoopGuard(7).run(m, 'cs'); ep = [x for x in m.ins if x[0] & 0xffff == S.OpEntryPoint][0]
    T(m.version >= (1, 4) and len(ep) == len(S.Module(mods['endless']).ins[[i for i, x in enumerate(S.Module(mods['endless']).ins) if x[0] & 0xffff == S.OpEntryPoint][0]]) + 1, 'the budget variable joins the entry point interface (SPIR-V 1.4 and later)')
    s13 = os.path.join(w, 'endless13.spvasm'); open(s13, 'w').write(LG_ENDLESS.replace(' %v\n', '\n', 1).replace('"main" %v', '"main"', 1))
    r = subprocess.run([tool('spirv-as'), '--target-env', 'spv1.3', s13, '-o', os.path.join(w, 'endless13.spv')], capture_output=True, text=True)
    if r.returncode == 0:
        m13 = S.Module(open(os.path.join(w, 'endless13.spv'), 'rb').read()); n_ep = len([x for x in m13.ins if x[0] & 0xffff == S.OpEntryPoint][0])
        T(m13.version == (1, 3) and S.LoopGuard(7).run(m13, 'cs') and len([x for x in m13.ins if x[0] & 0xffff == S.OpEntryPoint][0]) == n_ep, 'a SPIR-V 1.3 module keeps its interface')
    # the passes inside the default pipeline: the module comes out valid and the loop guard is the last pass
    T(not any(isinstance(p, S.LoopGuard) for p in S.DEFAULT_PASSES[:-1]), 'the guard is the last pass')
    # behaviour on lavapipe: the unguarded endless loop does not end, the guarded one ends with exactly the budget's iterations
    if exe:
        lg = lambda n, cap: guard(mods[n], w, n + str(cap), cap)[1]
        def run(n, spv, fill, nw=4, t=5):
            p = os.path.join(w, f'{n}.run.spv'); open(p, 'wb').write(spv); return run_probe(exe, icd, p, fill, nw, t)
        T(run('endless0', mods['endless'], 'zero')[1][:2] == [0, 1], 'endless, word 0 = 0: the body runs once')
        st, _ = run('endless1', mods['endless'], 'flag', t=2); T(st == 'timeout', f'the unguarded endless loop does not end on lavapipe ({st})')
        st, wd = run('endless2', lg('endless', 7), 'flag'); T(st == 'ok' and wd[1] == 7, f'the guarded endless loop ends after 7 iterations: {st} {wd}')
        st, wd = run('endless3', lg('endless', 16384), 'zero'); T(st == 'ok' and wd[1] == 1, f'a loop that ends by itself is unchanged: {st} {wd}')
        st, wd = run('while1', lg('while', 16384), 'zero', 4); T(st == 'ok' and wd[1:3] == [5, 5], f'a while loop with a phi at its merge still runs 5 times: {st} {wd}')
        st, wd = run('while2', lg('while', 3), 'zero', 4); T(st == 'ok' and wd[1] == 3, f'the same loop with a budget of 3 stops at 3: {st} {wd}')
        st, wd = run('one1', lg('oneblock', 9), 'flag'); T(st == 'ok' and wd[1] == 10, f'a one-block loop stops at the budget (its first pass runs before the check: 9 + 1): {st} {wd}')
        st, wd = run('nest1', lg('nested', 40), 'flag'); T(st == 'ok' and 0 < wd[1] <= 40, f'nested endless loops share one budget: {st} {wd}')
        st, wd = run('callee1', lg('callee', 6), 'flag'); T(st == 'ok' and wd[1] == 6 and wd[2] == 0, f'a callee leaves its endless loop with the null value (0; its own return would give 5): {st} {wd}')


def test_cas_atomic_load(w):
    cas = assemble(LG_CAS, w, 'cas')
    T(val(cas, w, 'cas0') == 'ok', 'the compare-exchange loop module is valid as written')
    m = S.Module(cas); p = S.CasAtomicLoad()
    T(p.applies(m, 'cs') and p.run(m, 'cs'), 'cas_atomic_load applies to a plain load inside a compare-exchange loop')
    ops = [x[0] & 0xffff for x in m.ins]
    T(ops.count(227) == 1 and ops.count(61) == 0, f'the plain load became an atomic load: {ops.count(227)} atomic loads, {ops.count(61)} loads')
    out = m.bytes(); v = val(out, w, 'cas1'); T(v == 'ok', f'the edited module is valid: {v}')
    # a load outside any compare-exchange loop stays
    m2 = S.Module(assemble(CS, w, 'cs_nocas')); T(not p.applies(m2, 'cs'), 'no compare-exchange: not applicable')
    # both passes together, in pipeline order
    m3 = S.Module(cas); T(p.run(m3, 'cs') and S.LoopGuard(8).run(m3, 'cs'), 'both passes apply in order'); T(val(m3.bytes(), w, 'cas2') == 'ok', 'and the result is valid')


# local_index_clamp: a Private and a Function array indexed by values the shader reads from memory (word 0 = 1 under fill 'flag')
LIC = r"""
               OpCapability Shader
%IMPORT%               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %v %priv
               OpExecutionMode %main LocalSize 1 1 1
               OpDecorate %sb Block
               OpMemberDecorate %sb 0 Offset 0
               OpDecorate %arr ArrayStride 4
               OpDecorate %v Binding 0
               OpDecorate %v DescriptorSet 0
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
        %int = OpTypeInt 32 1
       %v4u = OpTypeVector %uint 4
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
     %uint_3 = OpConstant %uint 3
     %uint_4 = OpConstant %uint 4
     %uint_7 = OpConstant %uint 7
     %uint_9 = OpConstant %uint 9
   %uint_10 = OpConstant %uint 10
   %uint_20 = OpConstant %uint 20
   %uint_30 = OpConstant %uint 30
   %uint_40 = OpConstant %uint 40
   %uint_77 = OpConstant %uint 77
  %uint_100 = OpConstant %uint 100
        %arr = OpTypeRuntimeArray %uint
         %sb = OpTypeStruct %arr
       %pssb = OpTypePointer StorageBuffer %sb
         %pu = OpTypePointer StorageBuffer %uint
          %v = OpVariable %pssb StorageBuffer
       %arr4 = OpTypeArray %uint %uint_4
       %arr2 = OpTypeArray %v4u %uint_2
      %ppriv = OpTypePointer Private %arr4
       %ppu = OpTypePointer Private %uint
       %pfarr = OpTypePointer Function %arr2
       %pfu = OpTypePointer Function %uint
       %priv = OpVariable %ppriv Private
       %main = OpFunction %void None %fn
      %entry = OpLabel
         %fv = OpVariable %pfarr Function
         %p0 = OpAccessChain %pu %v %uint_0 %uint_0
         %w0 = OpLoad %uint %p0
         %c0 = OpAccessChain %ppu %priv %uint_0
               OpStore %c0 %uint_10
         %c1 = OpAccessChain %ppu %priv %uint_1
               OpStore %c1 %uint_20
         %c2 = OpAccessChain %ppu %priv %uint_2
               OpStore %c2 %uint_30
         %c3 = OpAccessChain %ppu %priv %uint_3
               OpStore %c3 %uint_40
       %i100 = OpIMul %uint %w0 %uint_100
         %q1 = OpAccessChain %ppu %priv %i100
         %r1 = OpLoad %uint %q1
         %o1 = OpAccessChain %pu %v %uint_0 %uint_1
               OpStore %o1 %r1
         %i2 = OpIAdd %uint %w0 %uint_1
         %q2 = OpAccessChain %ppu %priv %i2
         %r2 = OpLoad %uint %q2
         %o2 = OpAccessChain %pu %v %uint_0 %uint_2
               OpStore %o2 %r2
         %wi = OpBitcast %int %w0
        %neg = OpSNegate %int %wi
         %q3 = OpAccessChain %ppu %priv %neg
         %r3 = OpLoad %uint %q3
         %o3 = OpAccessChain %pu %v %uint_0 %uint_3
               OpStore %o3 %r3
         %i7 = OpIMul %uint %w0 %uint_7
         %i9 = OpIMul %uint %w0 %uint_9
         %f1 = OpAccessChain %pfu %fv %i7 %i9
               OpStore %f1 %uint_77
         %f2 = OpAccessChain %pfu %fv %uint_1 %uint_3
         %r4 = OpLoad %uint %f2
         %o4 = OpAccessChain %pu %v %uint_0 %uint_4
               OpStore %o4 %r4
         %sx = OpAccessChain %pu %v %uint_0 %w0
         %sv = OpLoad %uint %sx
               OpReturn
               OpFunctionEnd
"""
GLSL_IMPORT = '          %glsl = OpExtInstImport "GLSL.std.450"\n'


def test_local_index_clamp(w):
    for tag, text in (('noimp', LIC.replace('%IMPORT%', '')), ('imp', LIC.replace('%IMPORT%', GLSL_IMPORT))):
        spv = assemble(text, w, 'lic_' + tag)
        T(val(spv, w, 'lic_' + tag + '0') == 'ok', f'lic {tag}: valid as written')
        m = S.Module(spv); p = S.LocalIndexClamp()
        plan = p.plan(m)
        T(len(plan) == 4 and sum(len(h) for _, h in plan) == 5, f'lic {tag}: 4 chains / 5 dynamic indices planned (3 Private, one Function with two), not the SSBO or the constant ones: {len(plan)}')
        T(p.applies(m, 'cs') and p.run(m, 'cs'), f'lic {tag}: applies')
        ops = [x[0] & 0xffff for x in m.ins]
        T(ops.count(12) == 5, f'lic {tag}: 5 UMin: {ops.count(12)}')
        T(ops.count(11) == 1, f'lic {tag}: exactly one GLSL.std.450 import: {ops.count(11)}')
        T(sorted(m.const_value(x[6]) for x in m.ins if x[0] & 0xffff == 12) == [1, 3, 3, 3, 3], f'lic {tag}: bounds are len-1 (3 for the 4-array, 1 for the 2-array and 3 for the uvec4)')
        out = m.bytes(); v = val(out, w, 'lic_' + tag + '1'); T(v == 'ok', f'lic {tag}: the edited module is valid: {v}')
        m2 = S.Module(out); T(not p.applies(m2, 'cs'), f'lic {tag}: a second run finds nothing to clamp')
        # in the pipeline, before the guard
        T(any(isinstance(q, S.LocalIndexClamp) for q in S.DEFAULT_PASSES), 'local_index_clamp is in DEFAULT_PASSES')
        if S.loop_guard_settings()[0]:
            ids = [type(q).__name__ for q in S.DEFAULT_PASSES]
            T(ids.index('LocalIndexClamp') < ids.index('LoopGuard') == len(ids) - 1, 'the clamp runs before the loop guard, which stays last')
        if tag == 'imp':
            exe, icd = probe_tool(w)
            if exe:
                sp = os.path.join(w, 'lic_run.spv'); open(sp, 'wb').write(out)
                st, wd = run_probe(exe, icd, sp, 'flag', 5, 20)
                T(st == 'ok' and wd == [1, 40, 30, 40, 77], f'lic: on lavapipe a wild index reads the last element (40), an in-range one its own (30), a negative one the last, a wild Function store lands on the last element: {st} {wd}')
    # a module with only constant indices and a StorageBuffer array is not touched
    m = S.Module(assemble(LG_ENDLESS, w, 'lic_none')); T(not S.LocalIndexClamp().applies(m, 'cs'), 'lic: nothing dynamic on Private/Function: not applicable')


NC_FS = """OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %out %in
OpExecutionMode %main OriginUpperLeft
OpDecorate %out Location 0
OpDecorate %in Location 0
OpDecorate %m NoContraction
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%pf_out = OpTypePointer Output %float
%pf_in = OpTypePointer Input %float
%out = OpVariable %pf_out Output
%in = OpVariable %pf_in Input
%main = OpFunction %void None %fn
%l = OpLabel
%a = OpLoad %float %in
%m = OpFMul %float %a %a
%s = OpFAdd %float %m %a
%d = OpFSub %float %s %a
%q = OpFDiv %float %d %a
%n = OpFNegate %float %q
OpStore %out %n
OpReturn
OpFunctionEnd
"""


def test_no_contraction(w):
    spv = assemble(NC_FS, w, 'nc0'); T(val(spv, w, 'nc00') == 'ok', 'nc: valid as written')
    m = S.Module(spv); p = S.NoContraction()
    T(p.applies(m, 'ps') and p.run(m, 'ps'), 'nc: applies to a module with FP arithmetic')
    nc = sorted(x[1] for x in m.ins if x[0] & 0xffff == S.OpDecorate and x[2] == 42)
    T(len(nc) == 4 and len(set(nc)) == 4, f'nc: FMul (already decorated: not twice), FAdd, FSub, FDiv each have NoContraction once: {nc}')
    T(not any(x[0] & 0xffff == S.OpDecorate and x[2] == 42 and (m.ins[m.defs[x[1]]][0] & 0xffff) == 127 for x in m.ins), 'nc: the FNegate is left alone')
    out = m.bytes(); v = val(out, w, 'nc1'); T(v == 'ok', f'nc: the edited module is valid: {v}')
    m2 = S.Module(out); T(not p.run(m2, 'ps'), 'nc: a second run adds nothing')
    T((S.NoContraction in [type(q) for q in S.DEFAULT_PASSES]) == (os.environ.get('NXS_NOCONTRACT') == '1'), 'nc: no_contraction is in DEFAULT_PASSES exactly when NXS_NOCONTRACT=1')
    if S.loop_guard_settings()[0]: T(type(S.DEFAULT_PASSES[-1]).__name__ == 'LoopGuard', 'nc: the loop guard is still the last pass')


def main():
    if not tool('spirv-as') or not tool('spirv-val') or not tool('spirv-opt'): print('the Vulkan SDK tools are missing: skipped'); return 77
    w = tempfile.mkdtemp(prefix='nxshader_test_')
    try:
        vs, fs, cs = test_edits(w)
        test_image_offset(w)
        test_atomic_float_add(w)
        test_cas_atomic_load(w)
        test_local_index_clamp(w)
        test_no_contraction(w)
        test_loop_guard(w)
        test_gmem(w)
        test_gmem_lavapipe(w)
        test_cache(w, vs, fs)
    finally: shutil.rmtree(w, ignore_errors=True)
    print('nxshader.unit: ' + ('FAILED' if FAIL else 'ok'))
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
