/* Builds against a packaged JSUI (include/ + lib/, the release layout) the
 * way a host does, with nothing but the C API: creates a runtime, renders a
 * View and a line of text on the CPU and checks the pixels. CI runs it
 * against every platform's package (scripts/dist/check-shared.sh).
 *
 *   consumer            exit status 0 when the package works
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsui/jsui.h"

#define W 96
#define H 32

int main(void) {
  JsuiConfig cfg;
  jsui_config_init(&cfg);
  cfg.width = W;
  cfg.height = H;
  JsuiRuntime* rt = jsui_create(&cfg);
  if (!rt) {
    fprintf(stderr, "jsui_create failed\n");
    return 1;
  }
  const char* src =
      "const n = __jsui_native;"
      "const v = n.createNode('View');"
      "n.setProps(v, {style: {flex: 1, backgroundColor: '#ff0000', padding: 4}});"
      "const t = n.createNode('Text'); const s = n.createNode('RawText'); n.setText(s, 'JSUI');"
      "n.setProps(t, {style: {color: '#ffffff', fontSize: 20}});"
      "n.appendChild(t, s); n.appendChild(v, t); n.appendChild(n.rootId, v);";
  if (jsui_load_bundle_source(rt, src, strlen(src), "consumer.js") != 0) {
    fprintf(stderr, "jsui_load_bundle_source failed: %s\n", jsui_last_error(rt) ? jsui_last_error(rt) : "?");
    return 1;
  }
  jsui_tick(rt, 0);
  unsigned char* px = (unsigned char*)calloc(W * H, 4);
  jsui_render_rgba(rt, px, W, H, W * 4);
  /* The corner is the red background; somewhere in the text area a pixel
   * is (near) white, so text shaping and the embedded font work. */
  int ok = px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255;
  int white = 0;
  for (int i = 0; i < W * H; i++) {
    const unsigned char* p = px + i * 4;
    if (p[0] > 230 && p[1] > 230 && p[2] > 230) white++;
  }
  printf("corner rgba %d,%d,%d,%d, text pixels %d\n", px[0], px[1], px[2], px[3], white);
  free(px);
  jsui_destroy(rt);
  if (!ok || white < 10) {
    fprintf(stderr, "consumer: unexpected pixels\n");
    return 1;
  }
  printf("consumer: ok\n");
  return 0;
}
