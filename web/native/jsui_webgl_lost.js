// Emscripten's WebGL glue (src/lib/libwebgl.js), changed for a context lost
// in the middle of a frame. WebKit marks a context lost when a call into its
// GPU process fails, so isContextLost() can turn true between two GL calls of
// one Skia frame. WebGL then returns null where the glue expects an object:
// glCreateProgram threw on `program.name = id`, glCreateShader stored null
// (and glShaderSource threw on it), glGetShaderPrecisionFormat threw on the
// result. Here a failed create returns 0, as GL does (Skia checks for it), and
// a failed query writes zeros. The frame then fails without a throw and the
// host sees the lost context after it (web/jsui-web.js, CanvasView._checkGL).
//
// Both names of each function are replaced: Emscripten 4+ defines glX as an
// alias of emscripten_glX, 3.1 the other way round, and the two Skia builds
// call different ones (the Graphite build's WebGL interface emscripten_glX,
// the WebGL-only build's glX). addToLibrary is Emscripten >= 3.1.50; the
// WebGL-only build is on 3.1.44, which has mergeInto only. Plain functions
// for the same reason.
(function() {
  var fns = {
    glCreateProgram: function() {
      var program = GLctx.createProgram();
      if (!program) return 0;
      var id = GL.getNewId(GL.programs);
      program.name = id;
      // Lazy cache results of glGetProgramiv(GL_ACTIVE_*_MAX_LENGTH), as the original.
      program.maxUniformLength = program.maxAttributeLength = program.maxUniformBlockNameLength = 0;
      program.uniformIdCounter = 1;
      GL.programs[id] = program;
      return id;
    },

    glCreateShader: function(shaderType) {
      var shader = GLctx.createShader(shaderType);
      if (!shader) return 0;
      var id = GL.getNewId(GL.shaders);
      GL.shaders[id] = shader;
      return id;
    },

    glGetShaderPrecisionFormat: function(shaderType, precisionType, range, precision) {
      var result = GLctx.getShaderPrecisionFormat(shaderType, precisionType);
      {{{ makeSetValue('range', '0', 'result ? result.rangeMin : 0', 'i32') }}};
      {{{ makeSetValue('range', '4', 'result ? result.rangeMax : 0', 'i32') }}};
      {{{ makeSetValue('precision', '0', 'result ? result.precision : 0', 'i32') }}};
    },
  };
  var lib = {};
  for (var name in fns) {
    lib[name] = lib['emscripten_' + name] = fns[name];
    lib[name + '__deps'] = lib['emscripten_' + name + '__deps'] = ['$GL'];
  }
  (typeof addToLibrary === 'function' ? addToLibrary : function(l) { mergeInto(LibraryManager.library, l); })(lib);
})();
