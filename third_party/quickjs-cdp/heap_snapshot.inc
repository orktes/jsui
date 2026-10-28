/* jsui: HeapProfiler.takeHeapSnapshot. The heap (JS_WalkHeap) as V8's
   .heapsnapshot JSON, which DevTools' Memory panel reads: nodes and edges as
   flat number arrays, every name once in `strings`. Included by
   quickjs-cdp.c. */

typedef struct {
  const void *ptr;
  int kind;
  JSAtom name; /* dup'd, or JS_ATOM_NULL */
  JSValue name_str; /* dup'd string, or undefined */
  const char *label;
  size_t size;
  uint32_t edges;
} CdpHeapNode;

typedef struct {
  const void *from, *to;
  int kind;
  JSAtom name; /* dup'd, or JS_ATOM_NULL */
  uint32_t index;
  const char *label;
  int32_t from_i, to_i;
} CdpHeapEdge;

typedef struct {
  JSContext *ctx;
  CdpHeapNode *nodes;
  size_t nnodes, nodes_cap;
  CdpHeapEdge *edges;
  size_t nedges, edges_cap;
} CdpHeapWalk;

static void heap_on_node(void *opaque, const void *ptr, int kind, JSAtom name, JSValueConst name_str, const char *label,
                         size_t size) {
  CdpHeapWalk *w = opaque;
  if (w->nnodes == w->nodes_cap) {
    w->nodes_cap = w->nodes_cap ? w->nodes_cap * 2 : 4096;
    w->nodes = realloc(w->nodes, sizeof(CdpHeapNode) * w->nodes_cap);
  }
  CdpHeapNode *n = &w->nodes[w->nnodes++];
  n->ptr = ptr;
  n->kind = kind;
  n->name = name != JS_ATOM_NULL ? JS_DupAtom(w->ctx, name) : JS_ATOM_NULL;
  n->name_str = JS_DupValue(w->ctx, name_str);
  n->label = label;
  n->size = size;
  n->edges = 0;
}

static void heap_on_edge(void *opaque, const void *from, const void *to, int kind, JSAtom name, uint32_t index,
                         const char *label) {
  CdpHeapWalk *w = opaque;
  if (w->nedges == w->edges_cap) {
    w->edges_cap = w->edges_cap ? w->edges_cap * 2 : 16384;
    w->edges = realloc(w->edges, sizeof(CdpHeapEdge) * w->edges_cap);
  }
  CdpHeapEdge *e = &w->edges[w->nedges++];
  e->from = from;
  e->to = to;
  e->kind = kind;
  e->name = name != JS_ATOM_NULL ? JS_DupAtom(w->ctx, name) : JS_ATOM_NULL;
  e->index = index;
  e->label = label;
}

/* --- a pointer -> node index map and a string table ---------------------- */

typedef struct {
  const void **keys;
  int32_t *vals;
  size_t cap;
} CdpPtrMap;

static size_t ptr_hash(const void *p) {
  uintptr_t x = (uintptr_t)p;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return (size_t)x;
}

static void ptrmap_init(CdpPtrMap *m, size_t n) {
  m->cap = 16;
  while (m->cap < n * 2) m->cap <<= 1;
  m->keys = calloc(m->cap, sizeof(void *));
  m->vals = malloc(m->cap * sizeof(int32_t));
}

static void ptrmap_put(CdpPtrMap *m, const void *k, int32_t v) {
  size_t i = ptr_hash(k) & (m->cap - 1);
  while (m->keys[i] && m->keys[i] != k) i = (i + 1) & (m->cap - 1);
  m->keys[i] = k;
  m->vals[i] = v;
}

static int32_t ptrmap_get(const CdpPtrMap *m, const void *k) {
  size_t i = ptr_hash(k) & (m->cap - 1);
  while (m->keys[i]) {
    if (m->keys[i] == k) return m->vals[i];
    i = (i + 1) & (m->cap - 1);
  }
  return -1;
}

typedef struct {
  char **strs;
  size_t n, cap;
  /* string -> index (open addressing over strs) */
  int32_t *slots;
  size_t slot_cap;
} CdpStrTable;

static size_t str_hash(const char *s) {
  size_t h = 1469598103934665603ULL;
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
  return h;
}

static void strtab_grow(CdpStrTable *t) {
  size_t cap = t->slot_cap ? t->slot_cap * 2 : 4096;
  int32_t *slots = malloc(cap * sizeof(int32_t));
  for (size_t i = 0; i < cap; i++) slots[i] = -1;
  for (size_t j = 0; j < t->n; j++) {
    size_t i = str_hash(t->strs[j]) & (cap - 1);
    while (slots[i] >= 0) i = (i + 1) & (cap - 1);
    slots[i] = (int32_t)j;
  }
  free(t->slots);
  t->slots = slots;
  t->slot_cap = cap;
}

static int32_t strtab_add(CdpStrTable *t, const char *s) {
  if (!s) s = "";
  if ((t->n + 1) * 2 > t->slot_cap) strtab_grow(t);
  size_t i = str_hash(s) & (t->slot_cap - 1);
  while (t->slots[i] >= 0) {
    if (strcmp(t->strs[t->slots[i]], s) == 0) return t->slots[i];
    i = (i + 1) & (t->slot_cap - 1);
  }
  if (t->n == t->cap) {
    t->cap = t->cap ? t->cap * 2 : 1024;
    t->strs = realloc(t->strs, sizeof(char *) * t->cap);
  }
  t->strs[t->n] = strdup(s);
  t->slots[i] = (int32_t)t->n;
  return (int32_t)t->n++;
}

/* --- output --------------------------------------------------------------- */

typedef struct {
  char *buf;
  size_t len, cap;
} CdpOut;

static void out_reserve(CdpOut *o, size_t n) {
  if (o->len + n + 1 <= o->cap) return;
  while (o->len + n + 1 > o->cap) o->cap = o->cap ? o->cap * 2 : 1 << 20;
  o->buf = realloc(o->buf, o->cap);
}

static void out_str(CdpOut *o, const char *s) {
  size_t n = strlen(s);
  out_reserve(o, n);
  memcpy(o->buf + o->len, s, n);
  o->len += n;
  o->buf[o->len] = 0;
}

static void out_num(CdpOut *o, double d) {
  char b[40];
  if (d == (double)(int64_t)d) snprintf(b, sizeof b, "%lld", (long long)d);
  else snprintf(b, sizeof b, "%.17g", d);
  out_str(o, b);
}

static void out_json_string(CdpOut *o, const char *s) {
  out_reserve(o, strlen(s) * 6 + 2);
  o->buf[o->len++] = '"';
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    unsigned char c = *p;
    if (c == '"' || c == '\\') {
      o->buf[o->len++] = '\\';
      o->buf[o->len++] = (char)c;
    } else if (c < 0x20) {
      o->len += (size_t)snprintf(o->buf + o->len, 7, "\\u%04x", c);
    } else {
      o->buf[o->len++] = (char)c;
    }
  }
  o->buf[o->len++] = '"';
  o->buf[o->len] = 0;
}

static int32_t heap_atom_string(QJSCDPAgent *agent, CdpStrTable *t, JSAtom a, const char *prefix) {
  if (a == JS_ATOM_NULL) return strtab_add(t, prefix ? prefix : "");
  const char *s = JS_AtomToCString(agent->ctx, a);
  int32_t r;
  if (prefix) {
    size_t n = strlen(prefix) + (s ? strlen(s) : 0) + 1;
    char *b = malloc(n);
    snprintf(b, n, "%s%s", prefix, s ? s : "");
    r = strtab_add(t, b);
    free(b);
  } else {
    r = strtab_add(t, s ? s : "");
  }
  if (s) JS_FreeCString(agent->ctx, s);
  return r;
}

/* The whole snapshot as one string (the caller frees it). */
static char *heap_snapshot_json(QJSCDPAgent *agent) {
  CdpHeapWalk w = {agent->ctx, NULL, 0, 0, NULL, 0, 0};
  JSHeapWalker walker = {heap_on_node, heap_on_edge, &w};
  JS_WalkHeap(agent->ctx, &walker);

  /* Node 0 is the synthetic root; GC objects follow. */
  CdpPtrMap map;
  ptrmap_init(&map, w.nnodes + 1);
  for (size_t i = 0; i < w.nnodes; i++) ptrmap_put(&map, w.nodes[i].ptr, (int32_t)(i + 1));
  size_t nn = w.nnodes + 1;
  uint32_t *edge_count = calloc(nn, sizeof(uint32_t));
  size_t kept = 0;
  for (size_t i = 0; i < w.nedges; i++) {
    CdpHeapEdge *e = &w.edges[i];
    e->from_i = e->from ? ptrmap_get(&map, e->from) : 0;
    e->to_i = ptrmap_get(&map, e->to);
    if (e->from_i < 0 || e->to_i < 0) {
      if (e->name != JS_ATOM_NULL) JS_FreeAtom(agent->ctx, e->name);
      continue;
    }
    edge_count[e->from_i]++;
    w.edges[kept++] = *e;
  }
  /* Edges grouped by their node, in node order (stable within a node). */
  size_t *start = calloc(nn + 1, sizeof(size_t));
  for (size_t i = 0; i < nn; i++) start[i + 1] = start[i] + edge_count[i];
  CdpHeapEdge *sorted = malloc(sizeof(CdpHeapEdge) * (kept ? kept : 1));
  size_t *fill = calloc(nn, sizeof(size_t));
  for (size_t i = 0; i < kept; i++) {
    int32_t f = w.edges[i].from_i;
    sorted[start[f] + fill[f]++] = w.edges[i];
  }

  CdpStrTable strs = {0};
  /* node_types: hidden array string object code closure regexp number
     native synthetic "concatenated string" "sliced string" symbol bigint
     "object shape" */
  static const int node_type[] = {3, 5, 1, 6, 4, 14, 0};
  CdpOut o = {0};
  out_str(&o,
          "{\"snapshot\":{\"meta\":{\"node_fields\":[\"type\",\"name\",\"id\",\"self_size\",\"edge_count\","
          "\"trace_node_id\",\"detachedness\"],\"node_types\":[[\"hidden\",\"array\",\"string\",\"object\","
          "\"code\",\"closure\",\"regexp\",\"number\",\"native\",\"synthetic\",\"concatenated string\","
          "\"sliced string\",\"symbol\",\"bigint\",\"object shape\"],\"string\",\"number\",\"number\","
          "\"number\",\"number\",\"number\"],\"edge_fields\":[\"type\",\"name_or_index\",\"to_node\"],"
          "\"edge_types\":[[\"context\",\"element\",\"property\",\"internal\",\"hidden\",\"shortcut\","
          "\"weak\"],\"string_or_number\",\"node\"],\"trace_function_info_fields\":[\"function_id\",\"name\","
          "\"script_name\",\"script_id\",\"line\",\"column\"],\"trace_node_fields\":[\"id\",\"function_info_index\","
          "\"count\",\"size\",\"children\"],\"sample_fields\":[\"timestamp_us\",\"last_assigned_id\"],"
          "\"location_fields\":[\"object_index\",\"script_id\",\"line\",\"column\"]},\"node_count\":");
  out_num(&o, (double)nn);
  out_str(&o, ",\"edge_count\":");
  out_num(&o, (double)kept);
  out_str(&o, ",\"trace_function_count\":0},\"nodes\":[");
  /* the root */
  out_num(&o, 9);
  out_str(&o, ",");
  out_num(&o, strtab_add(&strs, "(GC roots)"));
  out_str(&o, ",1,0,");
  out_num(&o, edge_count[0]);
  out_str(&o, ",0,0");
  for (size_t i = 0; i < w.nnodes; i++) {
    CdpHeapNode *n = &w.nodes[i];
    int32_t name;
    if (JS_IsString(n->name_str)) {
      const char *s = JS_ToCString(agent->ctx, n->name_str);
      name = strtab_add(&strs, s ? s : "");
      if (s) JS_FreeCString(agent->ctx, s);
    } else {
      name = n->name != JS_ATOM_NULL ? heap_atom_string(agent, &strs, n->name, NULL)
                                     : strtab_add(&strs, n->label ? n->label : "(anonymous)");
    }
    if (n->name == JS_ATOM_NULL && !JS_IsString(n->name_str) && n->kind == JS_HEAP_NODE_CLOSURE) name = strtab_add(&strs, "(anonymous function)");
    if (n->name == JS_ATOM_NULL && n->kind == JS_HEAP_NODE_CODE) name = strtab_add(&strs, "(code)");
    out_str(&o, ",");
    out_num(&o, node_type[n->kind]);
    out_str(&o, ",");
    out_num(&o, name);
    out_str(&o, ",");
    /* ids: odd, stable for an object while it lives (its address) */
    out_num(&o, (double)((((uint64_t)(uintptr_t)n->ptr >> 4) << 1) + 3));
    out_str(&o, ",");
    out_num(&o, (double)n->size);
    out_str(&o, ",");
    out_num(&o, edge_count[i + 1]);
    out_str(&o, ",0,0");
  }
  out_str(&o, "],\"edges\":[");
  /* edge_types: context element property internal hidden shortcut weak */
  for (size_t i = 0; i < kept; i++) {
    CdpHeapEdge *e = &sorted[i];
    int type = e->kind == JS_HEAP_EDGE_PROPERTY ? 2 : e->kind == JS_HEAP_EDGE_ELEMENT ? 1 : 3;
    double name_or_index;
    if (e->kind == JS_HEAP_EDGE_ELEMENT) name_or_index = e->index;
    else if (e->name != JS_ATOM_NULL) name_or_index = heap_atom_string(agent, &strs, e->name, e->label);
    else name_or_index = strtab_add(&strs, e->label ? e->label : "");
    if (i) out_str(&o, ",");
    out_num(&o, type);
    out_str(&o, ",");
    out_num(&o, name_or_index);
    out_str(&o, ",");
    out_num(&o, (double)e->to_i * 7);
  }
  out_str(&o, "],\"trace_function_infos\":[],\"trace_tree\":[],\"samples\":[],\"locations\":[],\"strings\":[");
  for (size_t i = 0; i < strs.n; i++) {
    if (i) out_str(&o, ",");
    out_json_string(&o, strs.strs[i]);
    free(strs.strs[i]);
  }
  out_str(&o, "]}");

  for (size_t i = 0; i < w.nnodes; i++) {
    if (w.nodes[i].name != JS_ATOM_NULL) JS_FreeAtom(agent->ctx, w.nodes[i].name);
    JS_FreeValue(agent->ctx, w.nodes[i].name_str);
  }
  for (size_t i = 0; i < kept; i++)  /* the dropped ones were freed as they went */
    if (w.edges[i].name != JS_ATOM_NULL) JS_FreeAtom(agent->ctx, w.edges[i].name);
  free(w.nodes);
  free(w.edges);
  free(map.keys);
  free(map.vals);
  free(edge_count);
  free(start);
  free(sorted);
  free(fill);
  free(strs.strs);
  free(strs.slots);
  return o.buf;
}
