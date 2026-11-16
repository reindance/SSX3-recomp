/* Builds without a renderer backend (src/vk/vkr_backend.c): the toolkit's
 * software rasteriser draws, so the live renderer settings (src/live.c, F4
 * and F6) and the player mark's renderer line (src/mark.c) have nothing to
 * act on. The Switch build uses this until its deko3d backend exists. */
#include <stdint.h>

int ssx3_LiveHdTextures(int on) { (void)on; return 0; }
int ssx3_LiveHdTexturesOn(void) { return 0; }
void ssx3_LiveShadows(int on) { (void)on; }
int ssx3_LiveShadowsOn(void) { return 1; }

/* for the player mark's state line: 0 without a renderer */
int ssx3_RenderInfo(uint32_t *w, uint32_t *h, uint32_t *msaa, uint64_t *tex_bytes)
{
    (void)w; (void)h; (void)msaa; (void)tex_bytes;
    return 0;
}
