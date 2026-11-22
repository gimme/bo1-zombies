// JavaScript side of the WebGPU executor (d3d9_webgpu.cpp), linked with --js-library. Everything here runs on the page's
// thread, where the executor replays the engine's draw commands.
//
// The page made the GPUDevice before the engine started (zombies-hub's web/public/player.js: gpuInit). GPU objects are
// handles, indexes into BO1GPU.objs. One command encoder records until bo1gpu_submit, with at most one render pass open
// in it. Pipelines compile in the background: the executor draws with one once its status word says it is ready (with a
// stand-in meanwhile: d3d9_webgpu.cpp). Pointers
// arrive as signed numbers, and memory goes past 2 GB: they are read unsigned (>>> 0).

addToLibrary({
  $BO1GPU: {
    device: null,
    queue: null,
    context: null,
    objs: [null],
    freeIds: [],
    encoder: null,
    pass: null,
    errors: 0,
    calls: 0,
    bytes: 0,
    compiles: 0, compileMs: 0, compileMax: 0, // pipelines compiled since bo1gpu_take_compiles: how many, their ms, the longest
    slowAt: 0, // &compile=<n>: when the last pipeline held back is ready (bo1gpu_pipeline)
    framesSubmitted: 0,
    framesDone: 0,
    doneMs: 0, doneFrames: 0, // frames the GPU finished since bo1gpu_take_done, and how long after their submit, in all
    doneAt: 0, // &donerate=<n>: when the page heard, or will hear, that the last frame held back is done (bo1gpu_submit)
    // d3d9_webgpu.cpp: enum GpuFormat, VertexFormat
    formats: ['', 'bgra8unorm', 'bgra8unorm-srgb', 'rgba8unorm', 'rgba8unorm-srgb', 'rgba8snorm', 'rg8snorm', 'rgb10a2unorm',
      'r16float', 'rg16float', 'rgba16float', 'r32float', 'rg32float', 'rgba32float', 'bc1-rgba-unorm', 'bc1-rgba-unorm-srgb',
      'bc2-rgba-unorm', 'bc2-rgba-unorm-srgb', 'bc3-rgba-unorm', 'bc3-rgba-unorm-srgb', 'bc4-r-unorm', 'bc5-rg-unorm',
      'depth16unorm', 'depth24plus', 'depth24plus-stencil8', 'depth32float', 'r8unorm', 'rg8unorm'],
    vertexFormats: ['float32', 'float32x2', 'float32x3', 'float32x4', 'unorm8x4', 'snorm16x2', 'snorm16x4', 'unorm16x2',
      'unorm16x4', 'float16x2', 'float16x4'],
    compares: ['never', 'less', 'equal', 'less-equal', 'greater', 'not-equal', 'greater-equal', 'always'],
    stencilOps: ['keep', 'zero', 'replace', 'invert', 'increment-clamp', 'decrement-clamp', 'increment-wrap', 'decrement-wrap'],
    blendFactors: ['zero', 'one', 'src', 'one-minus-src', 'src-alpha', 'one-minus-src-alpha', 'dst-alpha', 'one-minus-dst-alpha',
      'dst', 'one-minus-dst', 'src-alpha-saturated', 'constant', 'one-minus-constant'],
    blendOps: ['add', 'subtract', 'reverse-subtract', 'min', 'max'],
    topologies: ['point-list', 'line-list', 'line-strip', 'triangle-list', 'triangle-strip'],
    add: (o) => {
      const h = BO1GPU.freeIds.length ? BO1GPU.freeIds.pop() : BO1GPU.objs.length;
      BO1GPU.objs[h] = o;
      return h;
    },
    // a line in the console; "gpu error:" lines fail a test (zombies-hub's e2e/game/player.ts)
    error: (text) => {
      // what was under way as the engine stopped (bo1GpuStop) fails with its device: no error of the game's
      if (BO1GPU.stopped) return;
      if (BO1GPU.errors++ < 100) console.log('[bo1] gpu error: ' + text);
    },
    enc: () => BO1GPU.encoder || (BO1GPU.encoder = BO1GPU.device.createCommandEncoder()),
    // A draw of indices waits here for the next call to the open pass (bo1gpu_draw_indexed), and the next draw goes
    // with it where the two are one draw of two instances: every other call to the pass goes through p, which draws
    // it first.
    held: false, hCount: 0, hInstances: 0, hFirst: 0, hBase: 0, hInstance: 0, hList: 0,
    // since bo1gpu_take_drawn: calls that drew, and draws that went as instances of the one before, or as more indices of it
    drawn: 0, asInstances: 0, asIndices: 0,
    flush: () => {
      const G = BO1GPU;
      G.held = false;
      G.drawn++;
      G.pass.drawIndexed(G.hCount, G.hInstances, G.hFirst, G.hBase, G.hInstance);
    },
    p: () => {
      if (BO1GPU.held) BO1GPU.flush();
      return BO1GPU.pass;
    },
    end: () => {
      if (BO1GPU.pass) BO1GPU.p().end();
      BO1GPU.pass = null;
    },
  },

  // the engine ends (library_bo1.js: bo1Stop): its device goes, and all made on it, and the canvas is the next engine's
  $bo1GpuStop__deps: ['$BO1GPU'],
  $bo1GpuStop: () => {
    BO1GPU.stopped = true;
    if (BO1GPU.context) BO1GPU.context.unconfigure();
    if (BO1GPU.device) BO1GPU.device.destroy();
    BO1GPU.device = BO1GPU.queue = BO1GPU.context = BO1GPU.encoder = BO1GPU.pass = null;
    BO1GPU.held = false;
    BO1GPU.objs = [null];
    BO1GPU.freeIds = [];
  },

  // the device and the canvas: 0 if there is no device, else 1 | 2 BC textures | 4 float32-filterable | 8 float32-blendable
  bo1gpu_init__deps: ['$BO1GPU', '$bo1Life'],
  bo1gpu_init: () => {
    const device = Module['bo1GpuDevice'];
    if (!device) return 0;
    BO1GPU.device = device;
    BO1GPU.queue = device.queue;
    device.addEventListener('uncapturederror', (e) => BO1GPU.error('WebGPU: ' + e.error.message),
      { signal: bo1Life.abort.signal });
    // destroyed: as the engine stopped (bo1GpuStop)
    device.lost.then((info) => {
      if (info.reason !== 'destroyed') BO1GPU.error('WebGPU device lost (' + info.reason + '): ' + info.message);
    });
    const canvas = Module['canvas'] || document.getElementById('canvas');
    BO1GPU.context = canvas.getContext('webgpu');
    BO1GPU.canvasFormat = navigator.gpu.getPreferredCanvasFormat();
    BO1GPU.context.configure({ device, format: BO1GPU.canvasFormat, alphaMode: 'opaque', usage: GPUTextureUsage.RENDER_ATTACHMENT });
    // &gpu=8 (zombies-hub's web/src/main.ts): the calls to WebGPU and the bytes written, counted for
    // d3d9_webgpu.cpp's report
    if (Module['bo1GpuFlags'] & 8) {
      const count = (proto, names, bytes) => {
        for (const name of names) {
          const f = proto[name];
          proto[name] = function (...args) {
            BO1GPU.calls++;
            if (bytes) BO1GPU.bytes += bytes(args);
            return f.apply(this, args);
          };
        }
      };
      count(GPURenderPassEncoder.prototype, ['setPipeline', 'setBindGroup', 'setVertexBuffer', 'setIndexBuffer', 'setViewport',
        'setScissorRect', 'setStencilReference', 'setBlendConstant', 'draw', 'drawIndexed', 'beginOcclusionQuery',
        'endOcclusionQuery']);
      count(GPUQueue.prototype, ['writeBuffer'], (a) => a[4]);
      count(GPUQueue.prototype, ['writeTexture'], (a) => a[2].bytesPerRow * (a[2].rowsPerImage || a[3].height) * (a[3].depthOrArrayLayers || 1));
    }
    const f = device.features;
    return 1 | (f.has('texture-compression-bc') ? 2 : 0) | (f.has('float32-filterable') ? 4 : 0) | (f.has('float32-blendable') ? 8 : 0);
  },

  bo1gpu_canvas_format__deps: ['$BO1GPU'],
  bo1gpu_canvas_format: () => BO1GPU.formats.indexOf(BO1GPU.canvasFormat),

  bo1gpu_canvas_size__deps: ['$BO1GPU'],
  bo1gpu_canvas_size: (w, h) => {
    const c = BO1GPU.context.canvas;
    HEAP32[w >>> 2] = c.width;
    HEAP32[h >>> 2] = c.height;
  },

  // dimension: 0 2d (cube maps: 6 layers), 1 3d. viewFormat: another format views may have (sRGB), 0 none. samples:
  // a pixel's, 1 or 4 (anti-aliasing: d3d9_webgpu.cpp)
  bo1gpu_texture__deps: ['$BO1GPU'],
  bo1gpu_texture: (dimension, format, width, height, layers, mips, usage, viewFormat, samples) => {
    const desc = {
      dimension: dimension ? '3d' : '2d',
      format: BO1GPU.formats[format],
      size: { width, height, depthOrArrayLayers: layers },
      mipLevelCount: mips,
      sampleCount: samples,
      usage,
    };
    if (viewFormat) desc.viewFormats = [BO1GPU.formats[viewFormat]];
    return BO1GPU.add(BO1GPU.device.createTexture(desc));
  },

  // dimension: 0 2d, 1 cube, 2 3d; aspect: 0 all, 1 depth-only; format 0: the texture's
  bo1gpu_view__deps: ['$BO1GPU'],
  bo1gpu_view: (tex, format, dimension, aspect, baseMip, mips, baseLayer, layers) => {
    const desc = {
      dimension: ['2d', 'cube', '3d'][dimension],
      aspect: aspect ? 'depth-only' : 'all',
      baseMipLevel: baseMip,
      mipLevelCount: mips,
      baseArrayLayer: baseLayer,
      arrayLayerCount: layers,
    };
    if (format) desc.format = BO1GPU.formats[format];
    return BO1GPU.add(BO1GPU.objs[tex].createView(desc));
  },

  bo1gpu_write_texture__deps: ['$BO1GPU'],
  bo1gpu_write_texture: (tex, mip, x, y, z, data, bytesPerRow, rowsPerImage, width, height, depth) => {
    BO1GPU.queue.writeTexture({ texture: BO1GPU.objs[tex], mipLevel: mip, origin: { x, y, z } }, HEAPU8,
      { offset: data >>> 0, bytesPerRow, rowsPerImage }, { width, height, depthOrArrayLayers: depth });
  },

  bo1gpu_buffer__deps: ['$BO1GPU'],
  bo1gpu_buffer: (size, usage) => BO1GPU.add(BO1GPU.device.createBuffer({ size, usage })),

  bo1gpu_write_buffer__deps: ['$BO1GPU'],
  bo1gpu_write_buffer: (buf, offset, data, size) => {
    BO1GPU.queue.writeBuffer(BO1GPU.objs[buf], offset, HEAPU8, data >>> 0, size);
  },

  // a texture's or buffer's memory goes now (nothing recorded may use it after), and the handle
  bo1gpu_destroy__deps: ['$BO1GPU'],
  bo1gpu_destroy: (h) => {
    const o = BO1GPU.objs[h];
    if (o && o.destroy) o.destroy();
    BO1GPU.objs[h] = null;
    BO1GPU.freeIds.push(h);
  },

  bo1gpu_release__deps: ['$BO1GPU'],
  bo1gpu_release: (h) => {
    BO1GPU.objs[h] = null;
    BO1GPU.freeIds.push(h);
  },

  // address: 0 repeat, 1 mirror-repeat, 2 clamp-to-edge; filters: 0 nearest, 1 linear; compare: 0 none, else a
  // compare function + 1
  bo1gpu_sampler__deps: ['$BO1GPU'],
  bo1gpu_sampler: (u, v, w, mag, min, mip, lodMin, lodMax, compare, aniso) => {
    const address = ['repeat', 'mirror-repeat', 'clamp-to-edge'];
    const filter = ['nearest', 'linear'];
    const desc = {
      addressModeU: address[u],
      addressModeV: address[v],
      addressModeW: address[w],
      magFilter: filter[mag],
      minFilter: filter[min],
      mipmapFilter: filter[mip],
      lodMinClamp: lodMin,
      lodMaxClamp: lodMax,
      maxAnisotropy: aniso,
    };
    if (compare) desc.compare = BO1GPU.compares[compare - 1];
    return BO1GPU.add(BO1GPU.device.createSampler(desc));
  },

  // a WGSL module; its compile errors are logged with the line they are on
  bo1gpu_shader__deps: ['$BO1GPU'],
  bo1gpu_shader: (code, label) => {
    let text = UTF8ToString(code >>> 0);
    const name = UTF8ToString(label >>> 0);
    // &salt=<n> (zombies-hub's web/src/main.ts: Module.bo1ShaderSalt): each of the game's shaders adds a constant no
    // shader had before to what it puts out (n times 1e-30, to the depth and the colour: nothing of the picture
    // changes), so that neither the browser's cache nor the driver's, which is the machine's and outlives a browser's
    // profile, has what it compiles to: a pipeline then compiles as on a machine that never drew the game
    // (d3d9_wgsl.cpp writes the lines replaced)
    const salt = Module['bo1ShaderSalt'];
    if (salt > 0) {
      const s = (salt * 1e-30).toExponential(6);
      text = text.replace('p.zw);\n    return res;', 'p.zw);\n    res.bo1_pos.z = res.bo1_pos.z + ' + s + ' * res.bo1_pos.w;\n    return res;');
      if (text.includes('oC0')) text = text.replace('    var res: PsOut;\n', '    oC0 = oC0 + vec4f(' + s + ');\n    var res: PsOut;\n');
    }
    const module = BO1GPU.device.createShaderModule({ code: text, label: name });
    module.getCompilationInfo().then((info) => {
      for (const m of info.messages) {
        if (m.type !== 'error') continue;
        const line = text.split('\n')[m.lineNum - 1] || '';
        BO1GPU.error('shader ' + name + ' line ' + m.lineNum + ': ' + m.message + ' (' + line.trim() + ')');
      }
    });
    return BO1GPU.add(module);
  },

  // entries: count x [binding, visibility, kind, a, b]. kind 0: a uniform buffer with a dynamic offset; 1: a texture,
  // a: sample type (0 float, 1 unfilterable-float, 2 depth), b: view dimension (0 2d, 1 cube, 2 3d); 2: a sampler, a: 0
  // filtering, 1 non-filtering, 2 comparison; 3: a storage buffer, read only
  bo1gpu_bind_group_layout__deps: ['$BO1GPU'],
  bo1gpu_bind_group_layout: (entries, count) => {
    const list = [];
    for (let i = 0; i < count; i++) {
      const at = (entries >>> 2) + i * 5;
      const [binding, visibility, kind, a, b] = HEAPU32.subarray(at, at + 5);
      const e = { binding, visibility };
      if (kind === 0) e.buffer = { type: 'uniform', hasDynamicOffset: true };
      else if (kind === 3) e.buffer = { type: 'read-only-storage' };
      else if (kind === 1 && b === 3) e.texture = { sampleType: 'unfilterable-float', viewDimension: '2d', multisampled: true };
      else if (kind === 1) e.texture = { sampleType: ['float', 'unfilterable-float', 'depth'][a], viewDimension: ['2d', 'cube', '3d'][b] };
      else e.sampler = { type: ['filtering', 'non-filtering', 'comparison'][a] };
      list.push(e);
    }
    return BO1GPU.add(BO1GPU.device.createBindGroupLayout({ entries: list }));
  },

  bo1gpu_pipeline_layout__deps: ['$BO1GPU'],
  bo1gpu_pipeline_layout: (layouts, count) => {
    const list = [];
    for (let i = 0; i < count; i++) list.push(BO1GPU.objs[HEAPU32[(layouts >>> 2) + i]]);
    return BO1GPU.add(BO1GPU.device.createPipelineLayout({ bindGroupLayouts: list }));
  },

  // entries: count x [binding, kind, handle, offset, size]. kind 0: a buffer, 1: a texture view, 2: a sampler
  bo1gpu_bind_group__deps: ['$BO1GPU'],
  bo1gpu_bind_group: (layout, entries, count) => {
    const list = [];
    for (let i = 0; i < count; i++) {
      const at = (entries >>> 2) + i * 5;
      const [binding, kind, handle, offset, size] = HEAPU32.subarray(at, at + 5);
      const o = BO1GPU.objs[handle];
      list.push({ binding, resource: kind === 0 ? { buffer: o, offset, size } : o });
    }
    return BO1GPU.add(BO1GPU.device.createBindGroup({ layout: BO1GPU.objs[layout], entries: list }));
  },

  // A render pipeline, from JSON (d3d9_webgpu.cpp: PipelineJson). *status: 0 while it compiles, then 1, or -1 if it
  // failed; the float after it: how long it compiled, in ms. async 0: made at once (the executor's own few: the first
  // draw waits for them).
  bo1gpu_pipeline__deps: ['$BO1GPU'],
  bo1gpu_pipeline: (json, status, async) => {
    const d = JSON.parse(UTF8ToString(json >>> 0));
    const G = BO1GPU, objs = G.objs;
    const blend = (b) => ({
      color: { srcFactor: G.blendFactors[b[0]], dstFactor: G.blendFactors[b[1]], operation: G.blendOps[b[2]] },
      alpha: { srcFactor: G.blendFactors[b[3]], dstFactor: G.blendFactors[b[4]], operation: G.blendOps[b[5]] },
    });
    const desc = {
      label: d.label,
      layout: objs[d.layout],
      vertex: {
        module: objs[d.vs],
        entryPoint: 'main',
        constants: d.vc,
        buffers: d.buffers.map((b) => ({
          arrayStride: b[0],
          stepMode: b[1] ? 'instance' : 'vertex',
          attributes: b[2].map((a) => ({ format: G.vertexFormats[a[0]], offset: a[1], shaderLocation: a[2] })),
        })),
      },
      primitive: { topology: G.topologies[d.topology], frontFace: 'cw', cullMode: ['none', 'front', 'back'][d.cull] },
      fragment: {
        module: objs[d.ps],
        entryPoint: 'main',
        constants: d.pc,
        targets: d.targets.map((t) => t && Object.assign({ format: G.formats[t[0]], writeMask: t[1] }, t[2] ? { blend: blend(t[2]) } : {})),
      },
    };
    if (d.strip) desc.primitive.stripIndexFormat = d.strip === 1 ? 'uint16' : 'uint32';
    if (d.ms) desc.multisample = { count: d.ms };
    if (d.depth) {
      const z = d.depth;
      desc.depthStencil = {
        format: G.formats[z.f],
        depthWriteEnabled: !!z.w,
        depthCompare: G.compares[z.c],
        depthBias: z.bias || 0,
        depthBiasSlopeScale: z.slope || 0,
      };
      if (z.sf) {
        const face = (s) => ({ compare: G.compares[s[0]], failOp: G.stencilOps[s[1]], depthFailOp: G.stencilOps[s[2]], passOp: G.stencilOps[s[3]] });
        Object.assign(desc.depthStencil, { stencilFront: face(z.sf), stencilBack: face(z.sb), stencilReadMask: z.rm, stencilWriteMask: z.wm });
      }
    }
    const h = G.add(null);
    if (!async) {
      objs[h] = G.device.createRenderPipeline(desc);
      HEAP32[status >>> 2] = 1;
      return h;
    }
    const failed = (e) => {
      HEAP32[status >>> 2] = -1;
      G.error('pipeline ' + d.label + ': ' + e.message);
    };
    const started = performance.now();
    const ready = (p) => {
      objs[h] = p;
      const ms = performance.now() - started;
      G.compiles++;
      G.compileMs += ms;
      if (ms > G.compileMax) G.compileMax = ms;
      HEAPF32[(status >>> 2) + 1] = ms;
      HEAP32[status >>> 2] = 1;
    };
    // &compile=<n> (zombies-hub's web/src/main.ts: Module.bo1CompileRate): a pipeline is ready no sooner than 1/n s
    // after the one before, as on a device that compiles n of them a second, to see what its player waits for and what
    // is drawn meanwhile
    const rate = Module['bo1CompileRate'];
    try {
      G.device.createRenderPipelineAsync(desc).then((p) => {
        if (!(rate > 0)) return ready(p);
        const now = performance.now();
        G.slowAt = Math.max(G.slowAt, now) + 1000 / rate;
        setTimeout(() => ready(p), G.slowAt - now);
      }, failed);
    } catch (e) {
      failed(e); // a descriptor it can't take at all (pipelines from the list's recipes: d3d9_webgpu.cpp)
    }
    return h;
  },

  // desc: 4 color attachments x [view, clear, r, g, b, a] (view 0: none; clear 0: load), then the depth attachment:
  // [view, clear depth (0 load), depth, stencil (0 load, 1 clear, 2 the format has none), value, read only], then the
  // occlusion query set (0: none), then the set of timestamp queries (0: none) and the first of the pass' two slots in
  // it, where the GPU writes its clock as the pass begins and ends (d3d9_webgpu.cpp: the GPU's time)
  bo1gpu_begin_pass__deps: ['$BO1GPU'],
  bo1gpu_begin_pass: (desc) => {
    const G = BO1GPU, u = desc >>> 2;
    const colorAttachments = [];
    for (let i = 0; i < 4; i++) {
      const at = u + i * 6;
      const view = HEAPU32[at];
      if (!view) {
        colorAttachments.push(null);
        continue;
      }
      const a = { view: G.objs[view], loadOp: HEAPU32[at + 1] ? 'clear' : 'load', storeOp: 'store' };
      if (HEAPU32[at + 1]) a.clearValue = [HEAPF32[at + 2], HEAPF32[at + 3], HEAPF32[at + 4], HEAPF32[at + 5]];
      colorAttachments.push(a);
    }
    while (colorAttachments.length && !colorAttachments[colorAttachments.length - 1]) colorAttachments.pop();
    const pass = { colorAttachments };
    if (HEAPU32[u + 30]) pass.occlusionQuerySet = G.objs[HEAPU32[u + 30]];
    if (HEAPU32[u + 31]) {
      const slot = HEAPU32[u + 32];
      pass.timestampWrites = { querySet: G.objs[HEAPU32[u + 31]], beginningOfPassWriteIndex: slot, endOfPassWriteIndex: slot + 1 };
    }
    const d = u + 24;
    if (HEAPU32[d] && HEAPU32[d + 5]) {
      pass.depthStencilAttachment = { view: G.objs[HEAPU32[d]], depthReadOnly: true };
      if (HEAPU32[d + 3] !== 2) pass.depthStencilAttachment.stencilReadOnly = true;
    } else if (HEAPU32[d]) {
      const z = { view: G.objs[HEAPU32[d]], depthLoadOp: HEAPU32[d + 1] ? 'clear' : 'load', depthStoreOp: 'store', depthClearValue: HEAPF32[d + 2] };
      if (HEAPU32[d + 3] !== 2) Object.assign(z, { stencilLoadOp: HEAPU32[d + 3] ? 'clear' : 'load', stencilStoreOp: 'store', stencilClearValue: HEAPU32[d + 4] });
      pass.depthStencilAttachment = z;
    }
    G.pass = G.enc().beginRenderPass(pass);
  },

  // a multisampled view's pixels, each the mean of its samples, into a view of one sample a pixel of its size and
  // format: a pass of its own, which draws nothing (none may be open)
  bo1gpu_resolve__deps: ['$BO1GPU'],
  bo1gpu_resolve: (view, target) => {
    const G = BO1GPU;
    G.enc().beginRenderPass({
      colorAttachments: [{ view: G.objs[view], resolveTarget: G.objs[target], loadOp: 'load', storeOp: 'store' }],
    }).end();
  },

  // occlusion queries (d3d9_webgpu.cpp): a set of them, begun and ended in the open pass, resolved into a buffer and
  // copied into one to read back
  bo1gpu_query_set__deps: ['$BO1GPU'],
  bo1gpu_query_set: (count) => BO1GPU.add(BO1GPU.device.createQuerySet({ type: 'occlusion', count })),

  // a set of timestamp queries; 0 where the device has none (the page asks for them with &perf=1: zombies-hub's
  // web/public/player.js, gpuInit)
  bo1gpu_time_set__deps: ['$BO1GPU'],
  bo1gpu_time_set: (count) =>
    BO1GPU.device.features.has('timestamp-query') ? BO1GPU.add(BO1GPU.device.createQuerySet({ type: 'timestamp', count })) : 0,

  bo1gpu_begin_occlusion__deps: ['$BO1GPU'],
  bo1gpu_begin_occlusion: (index) => BO1GPU.p().beginOcclusionQuery(index),

  bo1gpu_end_occlusion__deps: ['$BO1GPU'],
  bo1gpu_end_occlusion: () => BO1GPU.p().endOcclusionQuery(),

  bo1gpu_resolve_queries__deps: ['$BO1GPU'],
  bo1gpu_resolve_queries: (set, count, resolve, read) => {
    const G = BO1GPU, enc = G.enc();
    enc.resolveQuerySet(G.objs[set], 0, count, G.objs[resolve], 0);
    enc.copyBufferToBuffer(G.objs[resolve], 0, G.objs[read], 0, count * 8);
  },

  bo1gpu_end_pass__deps: ['$BO1GPU'],
  bo1gpu_end_pass: () => BO1GPU.end(),

  bo1gpu_set_pipeline__deps: ['$BO1GPU'],
  bo1gpu_set_pipeline: (p) => BO1GPU.p().setPipeline(BO1GPU.objs[p]),

  bo1gpu_set_bind_group__deps: ['$BO1GPU'],
  bo1gpu_set_bind_group: (index, group, offsets, count) => {
    if (count) BO1GPU.p().setBindGroup(index, BO1GPU.objs[group], HEAPU32, offsets >>> 2, count);
    else BO1GPU.p().setBindGroup(index, BO1GPU.objs[group]);
  },

  bo1gpu_set_vertex_buffer__deps: ['$BO1GPU'],
  bo1gpu_set_vertex_buffer: (slot, buf, offset, size) => BO1GPU.p().setVertexBuffer(slot, BO1GPU.objs[buf], offset, size),

  bo1gpu_set_index_buffer__deps: ['$BO1GPU'],
  bo1gpu_set_index_buffer: (buf, index32, offset, size) => BO1GPU.p().setIndexBuffer(BO1GPU.objs[buf], index32 ? 'uint32' : 'uint16', offset, size),

  bo1gpu_set_viewport__deps: ['$BO1GPU'],
  bo1gpu_set_viewport: (x, y, w, h, minZ, maxZ) => BO1GPU.p().setViewport(x, y, w, h, minZ, maxZ),

  bo1gpu_set_scissor__deps: ['$BO1GPU'],
  bo1gpu_set_scissor: (x, y, w, h) => BO1GPU.p().setScissorRect(x, y, w, h),

  bo1gpu_set_stencil_ref__deps: ['$BO1GPU'],
  bo1gpu_set_stencil_ref: (ref) => BO1GPU.p().setStencilReference(ref),

  bo1gpu_set_blend_constant__deps: ['$BO1GPU'],
  bo1gpu_set_blend_constant: (r, g, b, a) => BO1GPU.p().setBlendConstant([r, g, b, a]),

  bo1gpu_draw__deps: ['$BO1GPU'],
  bo1gpu_draw: (count, instances, first, firstInstance) => {
    BO1GPU.drawn++;
    BO1GPU.p().draw(count, instances, first, firstInstance);
  },

  // A draw of indices. The engine draws a static model's surface at each place the model stands in turn: draws of the
  // same indices from the same first vertex, with no call to the pass between them, so with the same pipeline, bind
  // groups, buffers, viewport and the rest, each with its own block of vertex shader constants, the model's place in
  // it, one block after the other. The draw's first instance says which block (in blocks of the shader's size:
  // d3d9_webgpu.cpp, s_byInstance), so such a draw is one more instance of the draw before it, and they go as one
  // call, their instances drawn in the draws' order: each call is one for Chrome's GPU process to check and carry
  // out, however little it draws (Kino's heaviest view on mobile: 3,160 draws in 2,200 calls). World surfaces don't
  // go together so: each material's have a first vertex of their own for their 16-bit indices. Not with &gpu=2048
  // (zombies-hub's web/src/main.ts), to compare.
  // A draw of a list of triangles (list) whose indices follow the held draw's, from the same first vertex and for the
  // same instance, is more of that draw: its triangles after the others, as the two draws would draw them. The world's
  // surfaces go so where the engine draws them from one index buffer of the world's (r_mod_worldStaticIndices).
  bo1gpu_draw_indexed__deps: ['$BO1GPU'],
  bo1gpu_draw_indexed: (count, instances, firstIndex, baseVertex, firstInstance, list) => {
    const G = BO1GPU;
    if (G.held) {
      if (instances === 1 && count === G.hCount && firstIndex === G.hFirst && baseVertex === G.hBase &&
          firstInstance === G.hInstance + G.hInstances) {
        G.hInstances++;
        G.asInstances++;
        return;
      }
      if (instances === 1 && list && G.hList && G.hInstances === 1 && firstIndex === G.hFirst + G.hCount &&
          baseVertex === G.hBase && firstInstance === G.hInstance) {
        G.hCount += count;
        G.asIndices++;
        return;
      }
      G.flush();
    }
    if (instances !== 1 || (Module['bo1GpuFlags'] & 2048)) {
      G.drawn++;
      G.pass.drawIndexed(count, instances, firstIndex, baseVertex, firstInstance);
      return;
    }
    G.held = true;
    G.hCount = count;
    G.hInstances = 1;
    G.hFirst = firstIndex;
    G.hBase = baseVertex;
    G.hInstance = firstInstance;
    G.hList = list;
  },

  bo1gpu_copy_texture__deps: ['$BO1GPU'],
  bo1gpu_copy_texture: (src, srcMip, srcZ, sx, sy, dst, dstMip, dstZ, dx, dy, width, height) => {
    BO1GPU.enc().copyTextureToTexture({ texture: BO1GPU.objs[src], mipLevel: srcMip, origin: { x: sx, y: sy, z: srcZ } },
      { texture: BO1GPU.objs[dst], mipLevel: dstMip, origin: { x: dx, y: dy, z: dstZ } }, { width, height, depthOrArrayLayers: 1 });
  },

  bo1gpu_copy_to_buffer__deps: ['$BO1GPU'],
  bo1gpu_copy_to_buffer: (tex, mip, z, width, height, buf, bytesPerRow) => {
    BO1GPU.enc().copyTextureToBuffer({ texture: BO1GPU.objs[tex], mipLevel: mip, origin: { x: 0, y: 0, z } },
      { buffer: BO1GPU.objs[buf], bytesPerRow, rowsPerImage: height }, { width, height, depthOrArrayLayers: 1 });
  },

  // what was recorded goes to the GPU. frame: this ends a frame (bo1gpu_frames_pending counts it until the GPU is done)
  bo1gpu_submit__deps: ['$BO1GPU'],
  bo1gpu_submit: (frame) => {
    const G = BO1GPU;
    G.end();
    if (G.encoder) G.queue.submit([G.encoder.finish()]);
    G.encoder = null;
    if (frame) {
      G.framesSubmitted++;
      const sent = performance.now();
      const done = () => {
        G.framesDone++;
        G.doneMs += performance.now() - sent;
        G.doneFrames++;
      };
      // &done=<ms>: the page hears that a frame is done that much later, as from a device whose GPU keeps pace and
      // takes long to say so (a phone's); &donerate=<n>: of no more than n frames a second, as from a GPU that is
      // behind. To try how far ahead of the GPU the page goes (d3d9_webgpu.cpp: Replay).
      const late = Module['bo1DoneLate'], rate = Module['bo1DoneRate'];
      G.queue.onSubmittedWorkDone().then(() => {
        if (!(late > 0) && !(rate > 0)) return done();
        const now = performance.now();
        let at = now + (late > 0 ? late : 0);
        if (rate > 0) at = G.doneAt = Math.max(G.doneAt + 1000 / rate, at);
        setTimeout(done, at - now);
      });
    }
  },

  // the frames the GPU finished since the last time, and how long after their submit the page heard so, in all, in ms
  bo1gpu_take_done__deps: ['$BO1GPU'],
  bo1gpu_take_done: (ms, frames) => {
    HEAPF64[ms >>> 3] = BO1GPU.doneMs;
    HEAPF64[frames >>> 3] = BO1GPU.doneFrames;
    BO1GPU.doneMs = BO1GPU.doneFrames = 0;
  },

  // the calls and bytes counted since the last time (above: &gpu=8)
  bo1gpu_take_counts__deps: ['$BO1GPU'],
  bo1gpu_take_counts: (calls, bytes) => {
    HEAPF64[calls >>> 3] = BO1GPU.calls;
    HEAPF64[bytes >>> 3] = BO1GPU.bytes;
    BO1GPU.calls = BO1GPU.bytes = 0;
  },

  // since the last time: the calls that drew, and the draws that went as instances of the draw before them, or as more
  // indices of it (bo1gpu_draw_indexed)
  bo1gpu_take_drawn__deps: ['$BO1GPU'],
  bo1gpu_take_drawn: (out) => {
    const G = BO1GPU;
    HEAPU32[out >>> 2] = G.drawn;
    HEAPU32[(out >>> 2) + 1] = G.asInstances;
    HEAPU32[(out >>> 2) + 2] = G.asIndices;
    G.drawn = G.asInstances = G.asIndices = 0;
  },

  // the pipelines compiled since the last time: how many, the sum of their compile times and the longest, in ms
  bo1gpu_take_compiles__deps: ['$BO1GPU'],
  bo1gpu_take_compiles: (count, ms, longest) => {
    const G = BO1GPU;
    HEAPF64[count >>> 3] = G.compiles;
    HEAPF64[ms >>> 3] = G.compileMs;
    HEAPF64[longest >>> 3] = G.compileMax;
    G.compiles = G.compileMs = G.compileMax = 0;
  },

  bo1gpu_frames_pending__deps: ['$BO1GPU'],
  bo1gpu_frames_pending: () => BO1GPU.framesSubmitted - BO1GPU.framesDone,

  // A hidden page (another tab in front of it, its window minimized) has no animation frames, which replay the
  // engine's (d3d9_webgpu.cpp: Replay), and its own timers wake it once a second at most: a worker's, which go on as
  // they are, wake it every ms instead, and call wake while it is hidden.
  bo1gpu_wake_hidden__deps: ['$bo1Life'],
  bo1gpu_wake_hidden: (ms, wake) => {
    const code = `setInterval(() => postMessage(0), ${ms})`;
    const worker = new Worker(URL.createObjectURL(new Blob([code], { type: 'text/javascript' })), { name: 'bo1-wake' });
    bo1Life.workers.push(worker);
    worker.onmessage = () => {
      if (document.hidden) {{{ makeDynCall('v', 'wake') }}}();
    };
  },

  // a view of the canvas' texture for this frame
  bo1gpu_canvas_view__deps: ['$BO1GPU'],
  bo1gpu_canvas_view: () => BO1GPU.add(BO1GPU.context.getCurrentTexture().createView()),

  // The canvas lets what is under it through where the frame is transparent (premultiplied) while the page's loading
  // movie shows under it (web/src/bink_web.cpp), which the loading screen draws nothing over; else it is opaque, as a
  // frame's alpha means nothing to the game.
  bo1gpu_canvas_see_through__deps: ['$BO1GPU'],
  bo1gpu_canvas_see_through: (on) => {
    const alphaMode = on ? 'premultiplied' : 'opaque';
    if ((BO1GPU.alphaMode || 'opaque') === alphaMode) return;
    BO1GPU.alphaMode = alphaMode;
    BO1GPU.context.configure({ device: BO1GPU.device, format: BO1GPU.canvasFormat, alphaMode, usage: GPUTextureUsage.RENDER_ATTACHMENT });
  },

  // The list of pipelines the game draws with on the map it loads (d3d9_webgpu.cpp: recipes; Module.bo1LoadingMap's,
  // web/pipelines/<map>.bin, which the server serves), asked for when the executor starts. bo1gpu_take_recipes hands it
  // over once it is here. &pipelines=0 (zombies-hub's web/src/main.ts: Module.bo1PipelineList false) asks for none, so
  // that every pipeline is a miss, to see the stand-ins (d3d9_webgpu.cpp) at work.
  bo1gpu_fetch_recipes__deps: ['$BO1GPU'],
  bo1gpu_fetch_recipes: () => {
    const G = BO1GPU;
    const map = Module['bo1LoadingMap'];
    G.recipes = undefined;
    if (!map || Module['bo1PipelineList'] === false) {
      G.recipes = null;
      return;
    }
    fetch('/maps/' + encodeURIComponent(map) + '/pipelines')
      .then((r) => (r.ok ? r.arrayBuffer() : null))
      .then((b) => (G.recipes = b), () => (G.recipes = null));
  },

  // the list, in a copy the executor frees; *len: its bytes (0: none), or -1 while it is on the way
  bo1gpu_take_recipes__deps: ['$BO1GPU', 'malloc'],
  bo1gpu_take_recipes: (len) => {
    const b = BO1GPU.recipes;
    if (b === undefined) {
      HEAP32[len >>> 2] = -1;
      return 0;
    }
    BO1GPU.recipes = null;
    if (!b || !b.byteLength) {
      HEAP32[len >>> 2] = 0;
      return 0;
    }
    const p = _malloc(b.byteLength);
    HEAPU8.set(new Uint8Array(b), p >>> 0);
    HEAP32[len >>> 2] = b.byteLength;
    return p;
  },

  // once the GPU has filled the buffer (submitted before): its first `size` bytes to dst, then *flag = 1 (-1: failed)
  bo1gpu_map_read__deps: ['$BO1GPU'],
  bo1gpu_map_read: (buf, size, dst, flag) => {
    const b = BO1GPU.objs[buf];
    b.mapAsync(GPUMapMode.READ, 0, size).then(() => {
      HEAPU8.set(new Uint8Array(b.getMappedRange(0, size)), dst >>> 0);
      b.unmap();
      HEAP32[flag >>> 2] = 1;
    }, (e) => {
      BO1GPU.error('read back: ' + e.message);
      HEAP32[flag >>> 2] = -1;
    });
  },
});
