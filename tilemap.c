#include <stdlib.h>

#include <SDL.h>

#include "tilemap.h"

#define FUDGE (0.0001)

#define LOG_PRINTF(LL, FMT, ...) \
    (LL)->log_cb((LL)->log_priv, \
                 FMT, \
                 ##__VA_ARGS__)

#define FLOAT_COMPARE(X, Y) ((X - FUDGE < Y) && (X + FUDGE > Y))

const SDL_Point ZEROZERO = {.x = 0, .y = 0};

typedef struct {
    SDL_Texture *tex;
    unsigned int tw;
    unsigned int th;
    unsigned int maxx;
    unsigned int max;

    unsigned int refs; /* tilemaps referencing this tileset */
} Tileset;

typedef struct {
    int tileset;
    unsigned int w;
    unsigned int h;
    unsigned int *map;
    unsigned int *attrs;
    SDL_Texture *tex; /* cached surface */

    unsigned int refs; /* layers referencing this tileset */
} Tilemap;

typedef struct {
    int tilemap;
    int x;
    int y;
    int scroll_x;
    int scroll_y;
    int w;
    int h;
    double scale_x;
    double scale_y;
    double angle;
} Layer;

typedef struct LayerList_t {
    SDL_Renderer *renderer;
    Uint32 format;
    void (*log_cb)(void *priv, const char *fmt, ...);
    void *log_priv;
    
    Tileset *tileset;
    unsigned int tilesetsmem;

    Tilemap *tilemap;
    unsigned int tilemapsmem;

    Layer *layer;
    unsigned int layersmem;
} LayerList;

static int debug_show_texture(LayerList *ll,
                              SDL_Texture *texture) {
    SDL_Rect src, dest;
    Uint32 format;
    int access;

    if(SDL_QueryTexture(texture,
                        &format,
                        &access,
                        &src.w,
                        &src.h) < 0) {
        LOG_PRINTF(ll, "Couldn't query texture.\n");
        return(-1);
    }
    src.x = 0; src.y = 0; dest.x = 0; dest.y = 0;
    dest.w = src.w * 2; dest.h = src.h * 2;

    if(SDL_SetRenderDrawColor(ll->renderer, 0, 0, 0, SDL_ALPHA_OPAQUE) < 0) {
        LOG_PRINTF(ll, "Couldn't set render color.\n");
        return(-1);
    }

    if(SDL_RenderClear(ll->renderer) < 0) {
        LOG_PRINTF(ll, "Couldn't clear screen.\n");
        return(-1);
    }

    if(SDL_SetRenderDrawColor(ll->renderer,
                              0, 0, 0,
                              SDL_ALPHA_TRANSPARENT) < 0) {
        LOG_PRINTF(ll, "Couldn't restore render color.\n");
        return(-1);
    }

    if(SDL_RenderCopy(ll->renderer, texture, &src, &dest) < 0) {
        LOG_PRINTF(ll, "Couldn't render texture.\n");
        return(-1);
    }
    SDL_RenderPresent(ll->renderer);

    return(0);
}

#define DEBUG_SHOW_TEXTURE(LL, TEXTURE) \
    if(debug_show_texture(LL, TEXTURE) < 0) { \
        LOG_PRINTF(LL, "Couldn't show texture.\n"); \
    }

LayerList *layerlist_new(SDL_Renderer *renderer,
                         Uint32 format,
                         void (*log_cb)(void *priv,
                                        const char *fmt,
                                        ...),
                         void *log_priv) {
    LayerList *ll;

    ll = malloc(sizeof(LayerList));
    if(ll == NULL) {
        log_cb(log_priv, "Couldn't allocate memory for LayerList.\n");
        return(NULL);
    }

    ll->renderer = renderer;
    ll->format = format;
    ll->log_cb = log_cb;
    ll->log_priv = log_priv;
    ll->tilesetsmem = 0;
    ll->tilemapsmem = 0;
    ll->layersmem = 0;

    return(ll);
}

void layerlist_free(LayerList *ll) {
    if(ll->tilesetsmem > 0) {
        free(ll->tileset);
    }

    if(ll->tilemapsmem > 0) {
        free(ll->tilemap);
    }

    if(ll->layersmem > 0) {
        free(ll->layer);
    }

    free(ll);
}

int tilemap_add_tileset(LayerList *ll,
                        void *pixels,
                        unsigned int w,
                        unsigned int h,
                        unsigned int pitch,
                        unsigned int tw,
                        unsigned int th) {
    Tileset *temp;
    SDL_Surface *surface;
    SDL_Texture *tex;
    unsigned int i, j;
    unsigned int maxx, maxy;

    /* tiles should at least be 1x1 */
    if(tw == 0 || th == 0) {
        LOG_PRINTF(ll, "Tile dimensions are 0.\n");
        return(-1);
    }

    /* check if there would be 0 tiles */
    if(tw > w || th > h) {
        LOG_PRINTF(ll, "Tile dimensions greater than set.\n");
        return(-1);
    }

    maxx = w / tw;
    maxy = h / th;

    /* create the surface */
    surface = SDL_CreateRGBSurfaceFrom(pixels,
                                        w,
                                        h,
                                        32,
                                        pitch,
                                        TILEMAP_RMASK,
                                        TILEMAP_GMASK,
                                        TILEMAP_BMASK,
                                        TILEMAP_AMASK);
    if(surface == NULL) {
        LOG_PRINTF(ll, "Failed to create surface.\n");
        return(-1);
    }

    /* create the texture */
    tex = SDL_CreateTextureFromSurface(ll->renderer, surface);
    if(tex == NULL) {
        LOG_PRINTF(ll, "Failed to create texture from surface.\n");
        return(-1);
    }
    SDL_FreeSurface(surface);
    
    /* make values overwrite existing values */
    if(SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE) < 0) {
        LOG_PRINTF(ll, "Failed to set blend mode.\n");
        return(-1);
    }
 
    /* first loaded surface, so do some initial setup */
    if(ll->tilesetsmem == 0) {
        ll->tileset = malloc(sizeof(Tileset));
        if(ll->tileset == NULL) {
            LOG_PRINTF(ll, "Failed to allocate tileset.\n");
            SDL_DestroyTexture(tex);
            return(-1);
        }
        ll->tilesetsmem = 1;
        ll->tileset[0].tex = tex;
        ll->tileset[0].tw = tw;
        ll->tileset[0].th = th;
        ll->tileset[0].maxx = maxx;
        ll->tileset[0].max = maxx * maxy;
        ll->tileset[0].refs = 0;
        return(0);
    }

    /* find first NULL surface and assign it */
    for(i = 0; i < ll->tilesetsmem; i++) {
        if(ll->tileset[i].tex == NULL) {
            ll->tileset[i].tex = tex;
            ll->tileset[i].tw = tw;
            ll->tileset[i].th = th;
            ll->tileset[i].maxx = maxx;
            ll->tileset[i].max = maxx * maxy;
            ll->tileset[i].refs = 0;
            return(i);
        }
    }

    /* expand buffer if there's no free slots */
    temp = realloc(ll->tileset,
            sizeof(Tileset) * ll->tilesetsmem * 2);
    if(temp == NULL) {
        LOG_PRINTF(ll, "Failed to allocate tileset.\n");
        SDL_DestroyTexture(tex);
        return(-1);
    }
    ll->tileset = temp;
    ll->tilesetsmem *= 2;
    ll->tileset[i].tex = tex;
    ll->tileset[i].tw = tw;
    ll->tileset[i].th = th;
    ll->tileset[i].maxx = maxx;
    ll->tileset[i].max = maxx * maxy;
    ll->tileset[i].refs = 0;
    /* initialize empty excess surfaces as NULL */
    for(j = i + 1; j < ll->tilesetsmem; j++) {
        ll->tileset[j].tex = NULL;
    }
 
    return(i);
}

int tilemap_free_tileset(LayerList *ll, unsigned int index) {
    /* can't free a vacant slot, nor one with open references */
    if(index >= ll->tilesetsmem ||
       ll->tileset[index].tex == NULL ||
       ll->tileset[index].refs > 0) {
        LOG_PRINTF(ll, "Invalid tileset index or index referenced.\n");
        return(-1);
    }

    SDL_DestroyTexture(ll->tileset[index].tex);
    ll->tileset[index].tex = NULL;

    return(0);
}

int tilemap_add_tilemap(LayerList *ll,
                        unsigned int w,
                        unsigned int h) {
    Tilemap *temp;
    unsigned int i, j;

    if(w == 0 || h == 0) {
        LOG_PRINTF(ll, "Tilemap must have area.\n");
        return(-1);
    }

    /* first created tilemap, so do some initial setup */
    if(ll->tilemapsmem == 0) {
        ll->tilemap = malloc(sizeof(Tilemap));
        if(ll->tilemap == NULL) {
            LOG_PRINTF(ll, "Failed to allocate first tilemap.\n");
            return(-1);
        }
        ll->tilemapsmem = 1;
        ll->tilemap[0].map = malloc(sizeof(unsigned int) * w * h);
        if(ll->tilemap[0].map == NULL) {
            LOG_PRINTF(ll, "Failed to allocate first tilemap map.\n");
            return(-1);
        }
        memset(ll->tilemap[0].map, 0,
               sizeof(unsigned int) * w * h);
        ll->tilemap[0].w = w;
        ll->tilemap[0].h = h;
        ll->tilemap[0].tileset = -1;
        ll->tilemap[0].tex = NULL;
        ll->tilemap[0].attrs = NULL;
        ll->tilemap[0].refs = 0;
        return(0);
    }

    /* find first NULL surface and assign it */
    for(i = 0; i < ll->tilemapsmem; i++) {
        if(ll->tilemap[i].map == NULL) {
            ll->tilemap[i].map = malloc(sizeof(unsigned int) * w * h);
            if(ll->tilemap[i].map == NULL) {
                LOG_PRINTF(ll, "Failed to allocate tilemap map.\n");
                return(-1);
            }
            memset(ll->tilemap[i].map, 0,
                   sizeof(unsigned int) * w * h);
            ll->tilemap[i].w = w;
            ll->tilemap[i].h = h;
            ll->tilemap[i].tileset = -1;
            ll->tilemap[i].tex = NULL;
            ll->tilemap[i].attrs = NULL;
            ll->tilemap[i].refs = 0;
            return(i);
        }
    }

    /* expand buffer if there's no free slots */
    temp = realloc(ll->tilemap,
            sizeof(Tilemap) * ll->tilemapsmem * 2);
    if(temp == NULL) {
        LOG_PRINTF(ll, "Failed to expand tilemap space.\n");
        return(-1);
    }
    ll->tilemap = temp;
    ll->tilemapsmem *= 2;
    ll->tilemap[i].map = malloc(sizeof(unsigned int) * w * h);
    if(ll->tilemap[i].map == NULL) {
        LOG_PRINTF(ll, "Failed to allocate expanded tilemap map.\n");
        return(-1);
    }
    memset(ll->tilemap[i].map, 0,
           sizeof(unsigned int) * w * h);
    ll->tilemap[i].w = w;
    ll->tilemap[i].h = h;
    ll->tilemap[i].tileset = -1;
    ll->tilemap[i].tex = NULL;
    ll->tilemap[i].attrs = NULL;
    ll->tilemap[i].refs = 0;
    /* initialize empty excess surfaces as NULL */
    for(j = i + 1; j < ll->tilemapsmem; j++) {
        ll->tilemap[j].map = NULL;
    }
 
    return(i);
}

int tilemap_free_tilemap(LayerList *ll, unsigned int index) {
    /* can't free a vacant slot, nor one with open references */
    if(index >= ll->tilemapsmem ||
       ll->tilemap[index].map == NULL ||
       ll->tilemap[index].refs > 0) {
        LOG_PRINTF(ll, "Invalid tilemap index or tilemap is referenced.\n");
        return(-1);
    }

    /* decrement reference from tileset from this tilemap */
    if(ll->tilemap[index].tileset >= 0) {
        ll->tileset[ll->tilemap[index].tileset].refs--;
    }
    free(ll->tilemap[index].map);
    ll->tilemap[index].map = NULL;
    /* free any attribute map */
    if(ll->tilemap[index].attrs != NULL) {
        free(ll->tilemap[index].attrs);
        ll->tilemap[index].attrs = NULL;
    }
    /* clear cached surface */
    if(ll->tilemap[index].tex != NULL) {
        SDL_DestroyTexture(ll->tilemap[index].tex);
        ll->tilemap[index].tex = NULL;
    }

    return(0);
}

int tilemap_set_tilemap_tileset(LayerList *ll,
                                unsigned int index,
                                unsigned int tileset) {
    /* make sure index is a valid tilemap */
    if(index >= ll->tilemapsmem ||
       ll->tilemap[index].map == NULL) {
        LOG_PRINTF(ll, "Invalid tilemap index: %u\n", index);
        return(-1);
    }

    /* make sure tileset is a valid tileset */
    if(tileset >= ll->tilesetsmem ||
       ll->tileset[tileset].tex == NULL) {
        LOG_PRINTF(ll, "Invalid tileset index: %u\n", index);
        return(-1);
    }

    ll->tilemap[index].tileset = tileset;
    ll->tileset[tileset].refs++;

    return(0);
}

int tilemap_set_tilemap_map(LayerList *ll,
                            unsigned int index,
                            unsigned int x,
                            unsigned int y,
                            unsigned int pitch,
                            unsigned int w,
                            unsigned int h,
                            unsigned int *value) {
    unsigned int i;

    /* make sure index is a valid tilemap */
    if(index >= ll->tilemapsmem ||
       ll->tilemap[index].map == NULL) {
        LOG_PRINTF(ll, "Invalid tilemap index %u.\n", index);
        return(-1);
    }

    /* make sure start coordinate and end position don't go out of
     * range */
    if(x > ll->tilemap[index].w ||
       y > ll->tilemap[index].h ||
       x + w > ll->tilemap[index].w ||
       y + h > ll->tilemap[index].h) {
        LOG_PRINTF(ll, "Position/size would expand outside of "
                       "tilemap.\n");
        return(-1);
    }

    for(i = y; i < y + h; i++) {
        memcpy(&(ll->tilemap[index].map[ll->tilemap[index].w * i + x]),
               &(value[(pitch * i) + x]),
               sizeof(unsigned int) * w); 
    }

    return(0);
}

int tilemap_set_tilemap_attrs(LayerList *ll,
                              unsigned int index,
                              unsigned int x,
                              unsigned int y,
                              unsigned int pitch,
                              unsigned int w,
                              unsigned int h,
                              unsigned int *value) {
    unsigned int i;

    /* make sure index is a valid tilemap */
    if(index >= ll->tilemapsmem ||
       ll->tilemap[index].map == NULL) {
        LOG_PRINTF(ll, "Invalid tilemap index %u.\n", index);
        return(-1);
    }

    /* make sure start coordinate and end position don't go out of
     * range */
    if(x > ll->tilemap[index].w ||
       y > ll->tilemap[index].h ||
       x + w > ll->tilemap[index].w ||
       y + h > ll->tilemap[index].h) {
        LOG_PRINTF(ll, "Position/size would expand outside of "
                       "tilemap.\n");
        return(-1);
    }
    
    /* allocate space for an attribute map if one doesn't exist */
    if(ll->tilemap[index].attrs == NULL) {
        ll->tilemap[index].attrs =
            malloc(sizeof(unsigned int) *
                   ll->tilemap[index].w *
                   ll->tilemap[index].h);
        if(ll->tilemap[index].attrs == NULL) {
            LOG_PRINTF(ll, "Failed to allocate tilemap attribute map.\n");
            return(-1);
        }
        memset(ll->tilemap[index].attrs, 0,
               sizeof(unsigned int) * w * h);
    }
 
    for(i = y; i < y + h; i++) {
        memcpy(&(ll->tilemap[index].attrs[ll->tilemap[index].w * i + x]),
               &(value[(pitch * i) + x]),
               sizeof(unsigned int) * w); 
    }

    return(0);
}

int tilemap_update_tilemap(LayerList *ll,
                           unsigned int index,
                           unsigned int x,
                           unsigned int y,
                           unsigned int w,
                           unsigned int h) {
    unsigned int i, j;
    SDL_Rect dest, src, finaldest;
    Tilemap *tilemap;
    Tileset *tileset;
    unsigned int attr;
    double angle;
    SDL_RendererFlip flip;

    /* make sure index is a valid tilemap */
    if(index >= ll->tilemapsmem ||
       ll->tilemap[index].map == NULL) {
        LOG_PRINTF(ll, "Invalid tilemap index: %s\n");
        return(-1);
    }
    tilemap = &(ll->tilemap[index]);

    /* make sure there's a tileset referenced */
    if(tilemap->tileset < 0) {
        LOG_PRINTF(ll, "Tilemap has no tileset.\n");
        return(-1);
    }
    tileset = &(ll->tileset[tilemap->tileset]);

    /* make sure the range specified is within the map */
    if(x > tilemap->w || x + w > tilemap->w ||
       y > tilemap->h || y + h > tilemap->h) {
        LOG_PRINTF(ll, "Dimensions extens outside of tilemap.\n");
        return(-1);
    }

    /* create the surface if it doesn't exist */
    if(tilemap->tex == NULL) {
        tilemap->tex = SDL_CreateTexture(ll->renderer,
                                         ll->format,
                                         SDL_TEXTUREACCESS_STATIC |
                                         SDL_TEXTUREACCESS_TARGET,
                                         tilemap->w * tileset->tw,
                                         tilemap->h * tileset->th);
        if(tilemap->tex == NULL) {
            LOG_PRINTF(ll, "Failed to create texture.\n");
            return(-1);
        }
        if(SDL_RenderClear(ll->renderer) < 0) {
            LOG_PRINTF(ll, "Failed to clear texture.\n");
            return(-1);
        }
        if(SDL_SetTextureBlendMode(tilemap->tex, SDL_BLENDMODE_BLEND) < 0) {
            LOG_PRINTF(ll, "Failed to set texture blend mode.\n");
            return(-1);
        }
    }
    /* set it to be rendered to */
    if(SDL_SetRenderTarget(ll->renderer, tilemap->tex) < 0) {
        LOG_PRINTF(ll, "Failed to set render target: %s.\n",
                       SDL_GetError());
        return(-1);
    }

    if(SDL_SetRenderDrawColor(ll->renderer,
                              0, 0, 0,
                              SDL_ALPHA_TRANSPARENT) < 0) {
        LOG_PRINTF(ll, "Failed to set render draw color.\n");
        return(-1);
    }
    dest.x = x; dest.y = y;
    dest.w = w * tileset->tw; dest.h = h * tileset->th;
    if(SDL_RenderFillRect(ll->renderer, &dest) < 0) {
        LOG_PRINTF(ll, "Failed to clear region.\n");
        return(-1);
    }
    
    /* blit each tile to the tilemap */
    src.w = tileset->tw; src.h = tileset->th; src.y = 0;
    dest.w = src.w; dest.h = src.h;
    dest.x = dest.w * x; dest.y = dest.h * y;
    for(j = y; j < y + h; j++) {
        dest.x = dest.w * x;
        for(i = x; i < x + w; i++) {
            src.x = tilemap->map[tilemap->w * j + i];
            /* check to see if index is within tileset */
            if(src.x > tileset->max) {
                LOG_PRINTF(ll, "Tilemap index beyond tileset.\n");
                return(-1);
            }
            /* calculate the source texture coords and render */
            src.y = src.x / tileset->maxx;
            src.x %= tileset->maxx;
            src.x *= tileset->tw; src.y *= tileset->th;
            if(tilemap->attrs &&
               tilemap->attrs[tilemap->w * j + i] != 0) {
                attr = tilemap->attrs[tilemap->w * j + i];
                memcpy(&finaldest, &dest, sizeof(SDL_Rect));
                flip = SDL_FLIP_NONE;
                if(attr & TILEMAP_HFLIP_MASK) {
                    flip |= SDL_FLIP_HORIZONTAL;
                }
                if(attr & TILEMAP_VFLIP_MASK) {
                    flip |= SDL_FLIP_VERTICAL;
                }
                if((attr & TILEMAP_ROTATE_MASK) == TILEMAP_ROTATE_NONE) {
                    angle = 0.0;
                } else if((attr & TILEMAP_ROTATE_MASK) == TILEMAP_ROTATE_90) {
                    if(tileset->tw != tileset->th) {
                        LOG_PRINTF(ll, "Invalid rotation for rectangular "
                                       "tilemap.\n");
                        return(-1);
                    }
                    angle = 90.0;
                    finaldest.x += tileset->tw;
                } else if((attr & TILEMAP_ROTATE_MASK) == TILEMAP_ROTATE_180) {
                    angle = 180.0;
                    finaldest.x += tileset->tw;
                    finaldest.y += tileset->th;
                } else { /* TILEMAP_ROTATE_270 */
                     if(tileset->tw != tileset->th) {
                        LOG_PRINTF(ll, "Invalid rotation for rectangular "
                                       "tilemap.\n");
                        return(-1);
                    }
                    angle = 270.0;
                    finaldest.y += tileset->th;
                }
                if(SDL_RenderCopyEx(ll->renderer,
                                    tileset->tex,
                                    &src,
                                    &finaldest,
                                    angle,
                                    &ZEROZERO,
                                    flip) < 0) {
                    LOG_PRINTF(ll, "Failed to render tile.\n");
                    return(-1);
                }
            } else {
                if(SDL_RenderCopy(ll->renderer,
                                  tileset->tex,
                                  &src,
                                  &dest) < 0) {
                    LOG_PRINTF(ll, "Failed to render tile.\n");
                    return(-1);
                }
            }
            dest.x += dest.w;
        }
        dest.y += dest.h;
    }

    /* restore default render target */
    if(SDL_SetRenderTarget(ll->renderer, NULL) < 0) {
        LOG_PRINTF(ll, "Failed to restore default render target.\n");
        return(-1);
    }

    return(0);
}

int tilemap_add_layer(LayerList *ll,
                      unsigned int tilemap) {
    Layer *temp;
    unsigned int i, j;
    Tilemap *tm;
    Tileset *ts;

    if(tilemap >= ll->tilemapsmem ||
       ll->tilemap[tilemap].map == NULL) {
        LOG_PRINTF(ll, "Invalid tilemap index.\n");
        return(-1);
    }
    tm = &(ll->tilemap[tilemap]);
    ts = &(ll->tileset[tm->tileset]);

    /* first created layer, so do some initial setup */
    if(ll->layersmem == 0) {
        ll->layer = malloc(sizeof(Layer));
        if(ll->layer == NULL) {
            LOG_PRINTF(ll, "Failed to allocate first layer.\n");
            return(-1);
        }
        ll->layersmem = 1;
        ll->layer[0].x = 0;
        ll->layer[0].y = 0;
        ll->layer[0].w = tm->w * ts->tw;
        ll->layer[0].h = tm->h * ts->th;
        ll->layer[0].scroll_x = 0;
        ll->layer[0].scroll_y = 0;
        ll->layer[0].scale_x = 1.0;
        ll->layer[0].scale_y = 1.0;
        ll->layer[0].angle = 0.0;
        ll->layer[0].tilemap = tilemap;
        return(0);
    }

    /* find first NULL surface and assign it */
    for(i = 0; i < ll->layersmem; i++) {
        if(ll->layer[i].tilemap == -1) {
            ll->layer[i].x = 0;
            ll->layer[i].y = 0;
            ll->layer[i].w = tm->w * ts->tw;
            ll->layer[i].h = tm->h * ts->th;
            ll->layer[i].scroll_x = 0;
            ll->layer[i].scroll_y = 0;
            ll->layer[i].scale_x = 1.0;
            ll->layer[i].scale_y = 1.0;
            ll->layer[i].angle = 0.0;
            ll->layer[i].tilemap = tilemap;
            return(i);
        }
    }

    /* expand buffer if there's no free slots */
    temp = realloc(ll->layer,
            sizeof(Layer) * ll->layersmem * 2);
    if(temp == NULL) {
        LOG_PRINTF(ll, "Failed to expand layer memory.\n");
        return(-1);
    }
    ll->layer = temp;
    ll->layersmem *= 2;
    ll->layer[i].x = 0;
    ll->layer[i].y = 0;
    ll->layer[i].w = tm->w * ts->tw;
    ll->layer[i].h = tm->h * ts->th;
    ll->layer[i].scroll_x = 0;
    ll->layer[i].scroll_y = 0;
    ll->layer[i].scale_x = 1.0;
    ll->layer[i].scale_y = 1.0;
    ll->layer[i].angle = 0.0;
    ll->layer[i].tilemap = tilemap;
    /* initialize empty excess surfaces as NULL */
    for(j = i + 1; j < ll->layersmem; j++) {
        ll->layer[j].tilemap = -1;
    }
 
    return(i);
}

int tilemap_free_layer(LayerList *ll, unsigned int index) {
    /* can't free a vacant slot */
    if(index >= ll->layersmem ||
       ll->layer[index].tilemap == -1) {
        LOG_PRINTF(ll, "Invalid layer index.\n");
        return(-1);
    }

    ll->layer[index].tilemap = -1;

    return(0);
}

int tilemap_set_layer_x(LayerList *ll, unsigned int index, int x) {
    if(index >= ll->layersmem ||
       ll->layer[index].tilemap == -1) {
        LOG_PRINTF(ll, "Invalid layer index.\n");
        return(-1);
    }

    ll->layer[index].x = x;

    return(0);
}

int tilemap_set_layer_y(LayerList *ll, unsigned int index, int y) {
    if(index >= ll->layersmem ||
       ll->layer[index].tilemap == -1) {
        LOG_PRINTF(ll, "Invalid layer index.\n");
        return(-1);
    }

    ll->layer[index].y = y;

    return(0);
}

int tilemap_set_layer_w(LayerList *ll, unsigned int index, int w) {
    Layer *layer;
    Tilemap *tilemap;
    Tileset *tileset;
    unsigned int tmw;

    if(index >= ll->layersmem ||
       ll->layer[index].tilemap == -1) {
        LOG_PRINTF(ll, "Invalid layer index.\n");
        return(-1);
    }

    layer = &(ll->layer[index]);
    tilemap = &(ll->tilemap[layer->tilemap]);
    tileset = &(ll->tileset[tilemap->tileset]);
    tmw = tilemap->w * tileset->tw;

    if(w < 0 ||
       w > tmw) {
        LOG_PRINTF(ll, "Layer width out of range.\n");
        return(-1);
    }
 
    layer->w = w;

    return(0);
}

int tilemap_set_layer_h(LayerList *ll, unsigned int index, int h) {
    Layer *layer;
    Tilemap *tilemap;
    Tileset *tileset;
    unsigned int tmh;

    if(index >= ll->layersmem ||
       ll->layer[index].tilemap == -1) {
        LOG_PRINTF(ll, "Invalid layer index.\n");
        return(-1);
    }

    layer = &(ll->layer[index]);
    tilemap = &(ll->tilemap[layer->tilemap]);
    tileset = &(ll->tileset[tilemap->tileset]);
    tmh = tilemap->h * tileset->th;

    if(h < 0 ||
       h > tmh) {
        LOG_PRINTF(ll, "Layer height out of range.\n");
        return(-1);
    }
 
    layer->h = h;

    return(0);
}

int tilemap_set_layer_scroll_x(LayerList *ll, unsigned int index, int scroll_x) {
    Layer *layer;
    Tilemap *tilemap;
    Tileset *tileset;
    unsigned int tmw;

    if(index >= ll->layersmem ||
       ll->layer[index].tilemap == -1) {
        LOG_PRINTF(ll, "Invalid layer index.\n");
        return(-1);
    }

    layer = &(ll->layer[index]);
    tilemap = &(ll->tilemap[layer->tilemap]);
    tileset = &(ll->tileset[tilemap->tileset]);
    tmw = tilemap->w * tileset->tw;

    if(scroll_x < 0 ||
       scroll_x > tmw - 1) {
        LOG_PRINTF(ll, "Layer X scroll out of range.\n");
        return(-1);
    }
 
    layer->scroll_x = scroll_x;

    return(0);
}

int tilemap_set_layer_scroll_y(LayerList *ll, unsigned int index, int scroll_y) {
    Layer *layer;
    Tilemap *tilemap;
    Tileset *tileset;
    unsigned int tmh;

    if(index >= ll->layersmem ||
       ll->layer[index].tilemap == -1) {
        LOG_PRINTF(ll, "Invalid layer index.\n");
        return(-1);
    }

    layer = &(ll->layer[index]);
    tilemap = &(ll->tilemap[layer->tilemap]);
    tileset = &(ll->tileset[tilemap->tileset]);
    tmh = tilemap->h * tileset->th;

    if(scroll_y < 0 ||
       scroll_y > tmh - 1) {
        LOG_PRINTF(ll, "Layer Y scroll out of range.\n");
        return(-1);
    }
 
    layer->scroll_y = scroll_y;

    return(0);
}

int tilemap_set_layer_scale_x(LayerList *ll, unsigned int index, double scale_x) {
    /* SDL doesn't seem to allow negative rect coords and just clamps to 0 so
     * to avoid unexpected behavior, just throw an error to the user. */
    if(scale_x < 0.0) {
        LOG_PRINTF(ll, "Negative X scale.\n");
        return(-1);
    }

    if(index >= ll->layersmem ||
       ll->layer[index].tilemap == -1) {
        LOG_PRINTF(ll, "Invalid layer index.\n");
        return(-1);
    }

    ll->layer[index].scale_x = scale_x;

    return(0);
}

int tilemap_set_layer_scale_y(LayerList *ll, unsigned int index, double scale_y) {
    if(scale_y < 0.0) {
        LOG_PRINTF(ll, "Negative Y scale.\n");
        return(-1);
    }

    if(index >= ll->layersmem ||
       ll->layer[index].tilemap == -1) {
        LOG_PRINTF(ll, "Invalid layer index.\n");
        return(-1);
    }

    ll->layer[index].scale_y = scale_y;

    return(0);
}

int tilemap_set_layer_rotation(LayerList *ll, unsigned int index, double angle) {
    if(index >= ll->layersmem ||
       ll->layer[index].tilemap == -1) {
        LOG_PRINTF(ll, "Invalid layer index.\n");
        return(-1);
    }

    ll->layer[index].angle = angle;

    return(0);
}

int tilemap_draw_layer(LayerList *ll, unsigned int index) {
    Tileset *tileset;
    Tilemap *tilemap;
    Layer *layer;
    SDL_Rect dest, src;
    unsigned int tmw, tmh;
    unsigned int right, bottom;
    int overRight, overBottom;
    int remainRight, remainBottom;
    
    /* Make sure it's a valid layer with graphics */
    if(index >= ll->layersmem ||
       ll->tilemap[ll->layer[index].tilemap].tex == NULL) {
        LOG_PRINTF(ll, "Invalid layer index or layer without graphics.\n");
        return(-1);
    }

    layer = &(ll->layer[index]);
    tilemap = &(ll->tilemap[layer->tilemap]);
    tileset = &(ll->tileset[tilemap->tileset]);
    tmw = tilemap->w * tileset->tw;
    tmh = tilemap->h * tileset->th;
    right = layer->scroll_x + layer->w;
    bottom = layer->scroll_y + layer->h;
    overRight = right - tmw;
    overBottom = bottom - tmh;
    remainRight = layer->w - overRight;
    remainBottom = layer->h - overBottom;

    src.x = layer->scroll_x;
    src.y = layer->scroll_y;
    src.w = overRight > 0 ? remainRight : layer->w;
    src.h = overBottom > 0 ? remainBottom : layer->h;
    dest.x = layer->x;
    dest.y = layer->y;
    dest.w = src.w * layer->scale_x;
    dest.h = src.h * layer->scale_y;
    if(FLOAT_COMPARE(layer->angle, 0.0)) {
        if(SDL_RenderCopy(ll->renderer,
                          tilemap->tex,
                          &src,
                          &dest) < 0) {
            LOG_PRINTF(ll, "Failed to render layer.\n");
            return(-1);
        }
        if(overRight > 0) {
            src.x = 0;
            src.y = layer->scroll_y;
            src.w = overRight;
            src.h = overBottom > 0 ? remainBottom : layer->h;
            dest.x = layer->x + (remainRight * layer->scale_x);
            dest.y = layer->y;
            dest.w = src.w * layer->scale_x;
            dest.h = src.h * layer->scale_y;
            if(SDL_RenderCopy(ll->renderer,
                              tilemap->tex,
                              &src,
                              &dest) < 0) {
                LOG_PRINTF(ll, "Failed to render layer.\n");
                return(-1);
            }
        }
        if(overBottom > 0) {
            src.x = layer->scroll_x;
            src.y = 0;
            src.w = overRight > 0 ? remainRight : layer->w;
            src.h = overBottom;
            dest.x = layer->x;
            dest.y = layer->y + (remainBottom * layer->scale_y);
            dest.w = src.w * layer->scale_x;
            dest.h = src.h * layer->scale_y;
            if(SDL_RenderCopy(ll->renderer,
                              tilemap->tex,
                              &src,
                              &dest) < 0) {
                LOG_PRINTF(ll, "Failed to render layer.\n");
                return(-1);
            }
        }
        if(overRight > 0 && overBottom > 0) {
            src.x = 0;
            src.y = 0;
            src.w = overRight;
            src.h = overBottom;
            dest.x = layer->x + (remainRight * layer->scale_x);
            dest.y = layer->y + (remainBottom * layer->scale_y);
            dest.w = src.w * layer->scale_x;
            dest.h = src.h * layer->scale_y;
            if(SDL_RenderCopy(ll->renderer,
                              tilemap->tex,
                              &src,
                              &dest) < 0) {
                LOG_PRINTF(ll, "Failed to render layer.\n");
                return(-1);
            }
        }
    } else {
        if(SDL_RenderCopyEx(ll->renderer,
                            tilemap->tex,
                            &src,
                            &dest,
                            layer->angle,
                            &ZEROZERO,
                            SDL_FLIP_NONE) < 0) {
            LOG_PRINTF(ll, "Failed to render layer.\n");
            return(-1);
        }
    }

    return(0);
}

#ifdef CRUSTY_TEST
#include <stdio.h>

#define WINDOW_TITLE "Tilemap Test"
#define WINDOW_WIDTH (640)
#define WINDOW_HEIGHT (480)

void vprintf_cb(void *priv, const char *fmt, ...) {
    va_list ap;
    FILE *out = priv;

    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
}

int main(int argc, char **argv) {
    LayerList *ll;
    int drivers;
    int bestdrv, softdrv, selectdrv;
    Uint32 bestfmt, softfmt, selectfmt;
    int i, j;
    SDL_RendererInfo driver;
    SDL_Window *win;
    SDL_Renderer *renderer;
    Uint32 R, G, B, Z, W, T, P;
    int tileset_id;
    int tilemap_id;
    int tilemap_layer_id;
    int sprite_id;
    int spritemap_id;
    int spritemap_layer_id;

    R = TILEMAP_COLOR(255, 0, 0, 255);
    G = TILEMAP_COLOR(0, 255, 0, 255);
    B = TILEMAP_COLOR(0, 0, 255, 255);
    Z = TILEMAP_COLOR(0, 0, 0, 0);
    W = TILEMAP_COLOR(255, 255, 255, 255);
    T = TILEMAP_COLOR(0, 0, 0, 0);
    P = TILEMAP_COLOR(255, 0, 255, 255);

    Uint32 tileset[18 * 18] = {
        Z,Z,Z,Z,R,Z,Z,Z,Z,R,R,R,R,R,Z,Z,P,P,
        Z,Z,Z,G,G,Z,Z,Z,G,G,Z,Z,Z,G,G,Z,P,P,
        Z,Z,B,B,B,Z,Z,Z,Z,Z,Z,Z,Z,B,B,Z,P,P,
        Z,Z,Z,W,W,Z,Z,Z,Z,Z,Z,Z,W,W,Z,Z,P,P,
        Z,Z,Z,B,B,Z,Z,Z,Z,Z,B,B,B,Z,Z,Z,P,P,
        Z,Z,Z,G,G,Z,Z,Z,Z,G,G,Z,Z,Z,Z,Z,P,P,
        Z,Z,R,R,R,R,Z,Z,R,R,R,R,R,R,R,Z,P,P,
        Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,P,P,
        Z,R,R,R,R,R,Z,Z,Z,Z,Z,R,R,R,Z,Z,P,P,
        G,G,Z,Z,Z,G,G,Z,Z,Z,G,G,G,G,Z,Z,P,P,
        Z,Z,Z,Z,Z,B,B,Z,Z,B,B,Z,B,B,Z,Z,P,P,
        Z,Z,Z,W,W,W,Z,Z,W,W,Z,Z,W,W,Z,Z,P,P,
        Z,Z,Z,Z,Z,B,B,Z,B,B,B,B,B,B,B,Z,P,P,
        G,G,Z,Z,Z,G,G,Z,Z,Z,Z,Z,G,G,Z,Z,P,P,
        Z,R,R,R,R,R,Z,Z,Z,Z,Z,Z,R,R,Z,Z,P,P,
        Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,P,P,
        P,P,P,P,P,P,P,P,P,P,P,P,P,P,P,P,P,P,
        P,P,P,P,P,P,P,P,P,P,P,P,P,P,P,P,P,P
    };

    unsigned int tilemap[16 * 16] = {
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        2,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,
        2,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,
        2,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,
        2,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,
        2,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,
        2,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,
        2,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,
        2,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,
        2,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,
        2,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,
        2,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,
        2,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,
        3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
        3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3
    };

    Uint32 sprite[8 * 16] = {
        T,T,G,G,G,G,T,T,
        T,G,G,G,G,G,G,T,
        T,G,B,G,G,B,G,T,
        T,P,P,P,P,P,P,T,
        T,P,P,P,W,P,P,T,
        T,P,P,P,P,P,P,T,
        T,P,R,R,R,R,P,T,
        T,T,P,P,P,P,T,T,
        T,T,T,G,G,T,T,T,
        T,G,G,G,G,G,G,T,
        T,T,T,G,G,T,T,T,
        T,T,B,B,B,B,T,T,
        T,B,B,T,T,B,B,T,
        T,B,B,T,T,B,B,T,
        T,B,B,T,T,B,B,T,
        T,W,W,T,T,W,W,T
    };

    unsigned int spritemap[1 * 1] = {
        0
    };

    if(SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }

    drivers = SDL_GetNumRenderDrivers();
    fprintf(stderr, "Video Drivers: %d\n", drivers);

    bestdrv = -1;
    bestfmt = SDL_PIXELFORMAT_UNKNOWN;
    softdrv = -1;
    softfmt = SDL_PIXELFORMAT_UNKNOWN;
    for(i = 0; i < drivers; i++) {
        if(SDL_GetRenderDriverInfo(i, &driver) < 0) {
            fprintf(stderr, "Couldn't get driver info for index %d.\n",
                    i);
            continue;
        }

        fprintf(stderr, "Driver %d: %s", i, driver.name);
        if((driver.flags & SDL_RENDERER_SOFTWARE) &&
           softdrv == -1) {
            for(j = 0; j < driver.num_texture_formats; j++) {
                if(SDL_BITSPERPIXEL(driver.texture_formats[j]) >= 24) {
                    fprintf(stderr, " (fallback)");
                    softfmt = driver.texture_formats[j];
                    softdrv = i;
                    break;
                }
            }
        } else if((driver.flags & SDL_RENDERER_ACCELERATED) &&
                  (driver.flags & SDL_RENDERER_TARGETTEXTURE) &&
                  bestdrv == -1) {
            for(j = 0; j < driver.num_texture_formats; j++) {
                if(SDL_BITSPERPIXEL(driver.texture_formats[j]) >= 24) {
                    fprintf(stderr, " (selected)");
                    bestfmt = driver.texture_formats[j];
                    bestdrv = i;
                    break;
                }
            }
        }
        fprintf(stderr, "\n");
        fprintf(stderr, "Flags: (%08X) ", driver.flags);
        if(driver.flags & SDL_RENDERER_SOFTWARE)
            fprintf(stderr, "SOFTWARE ");
        if(driver.flags & SDL_RENDERER_ACCELERATED)
            fprintf(stderr, "ACCELERATED ");
        if(driver.flags & SDL_RENDERER_PRESENTVSYNC)
            fprintf(stderr, "PRESENTVSYNC ");
        if(driver.flags & SDL_RENDERER_TARGETTEXTURE)
            fprintf(stderr, "TARGETTEXTURE ");
        fprintf(stderr, "\n");
        fprintf(stderr, "Formats: ");
        for(j = 0; j < driver.num_texture_formats; j++) {
            fprintf(stderr, "(%08X) %s ",
                    driver.texture_formats[i],
                    SDL_GetPixelFormatName(driver.texture_formats[i]));
        }
        fprintf(stderr, "\n");
        fprintf(stderr, "Max Texture Size: %d x %d\n",
                driver.max_texture_width,
                driver.max_texture_height);
    }

    win = SDL_CreateWindow(WINDOW_TITLE,
                           SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED,
                           WINDOW_WIDTH,
                           WINDOW_HEIGHT,
                           0);
    if(win == NULL) {
        fprintf(stderr, "Failed to create SDL window.\n");
        goto error0;
    }
 
    if(bestdrv < 0) {
        if(softdrv < 0) {
            fprintf(stderr, "No accelerated or software driver found? "
                            "Trying index 0...\n");
            if(SDL_GetRenderDriverInfo(0, &driver) < 0) {
                fprintf(stderr, "Couldn't get driver info for index "
                                "0.\n");
                goto error1;
            }
            selectfmt = SDL_PIXELFORMAT_UNKNOWN;
            for(j = 0; j < driver.num_texture_formats; j++) {
                if(SDL_BITSPERPIXEL(driver.texture_formats[j]) >= 24) {
                    selectfmt = driver.texture_formats[j];
                    break;
                }
            }
            if(selectfmt == SDL_PIXELFORMAT_UNKNOWN) {
                fprintf(stderr, "Coulnd't find true color pixel "
                                "format.\n");
                goto error1;
            }

            selectdrv = 0;
        } else {
            fprintf(stderr, "No accelerated driver found, falling "
                            "back to software (%d).\n", softdrv);
            selectfmt = softfmt;
            selectdrv = softdrv;
        }
    } else {
        fprintf(stderr, "Selecting first accelerated driver (%d).\n",
                        bestdrv);
        selectfmt = bestfmt;
        selectdrv = bestdrv;
    }

    renderer = SDL_CreateRenderer(win, selectdrv, 0);
    if(renderer == NULL) {
        fprintf(stderr, "Failed to create SDL renderer.\n");
        goto error1;
    }

    if(SDL_RenderSetScale(renderer, 2.0, 2.0) < 0) {
        fprintf(stderr, "Failed to set render scale.\n");
        goto error1;
    }

    ll = layerlist_new(renderer, selectfmt, vprintf_cb, stderr);
    if(ll == NULL) {
        fprintf(stderr, "Failed to create layerlist.\n");
        goto error1;
    }

    tileset_id = tilemap_add_tileset(ll,
                                     tileset,
                                     16, 16,
                                     18 * sizeof(Uint32),
                                     8, 8);
    if(tileset_id < 0) {
        fprintf(stderr, "Failed to add tileset.\n");
        goto error2;
    }
    tilemap_id = tilemap_add_tilemap(ll, 16, 16);
    if(tilemap_id < 0) {
        fprintf(stderr, "Failed to add tilemap.\n");
        goto error2;
    }
    if(tilemap_set_tilemap_tileset(ll, tilemap_id, tileset_id) < 0) {
        fprintf(stderr, "Failed to apply tileset.\n");
        goto error2;
    }
    if(tilemap_set_tilemap_map(ll,
                               tilemap_id,
                               0,
                               0,
                               tilemap,
                               sizeof(tilemap) /
                               sizeof(tilemap[0])) < 0) {
        fprintf(stderr, "Failed to apply tilemap.\n");
        goto error2;
    }
    if(tilemap_update_tilemap(ll, tilemap_id, 0, 0, 16, 16) < 0) {
        fprintf(stderr, "Failed to update tilemap.\n");
        goto error2;
    }
    tilemap_layer_id = tilemap_add_layer(ll, tilemap_id);
    if(tilemap_layer_id < 0) {
        fprintf(stderr, "Failed to add tilemap layer.\n");
        goto error2;
    }

    sprite_id = tilemap_add_tileset(ll,
                                    sprite,
                                    8, 16,
                                    8 * sizeof(Uint32),
                                    8, 16);
    if(sprite_id < 0) {
        fprintf(stderr, "Failed to add sprite.\n");
        goto error2;
    }
    spritemap_id = tilemap_add_tilemap(ll, 1, 1);
    if(spritemap_id < 0) {
        fprintf(stderr, "Failed to add spritemap.\n");
        goto error2;
    }
    if(tilemap_set_tilemap_tileset(ll, spritemap_id, sprite_id) < 0) {
        fprintf(stderr, "Failed to apply spriteset.\n");
        goto error2;
    }
    if(tilemap_set_tilemap_map(ll,
                               spritemap_id,
                               0,
                               0,
                               spritemap,
                               sizeof(spritemap) /
                               sizeof(spritemap[0])) < 0) {
        fprintf(stderr, "Failed to apply spritemap.\n");
        goto error2;
    }
    if(tilemap_update_tilemap(ll, spritemap_id, 0, 0, 1, 1) < 0) {
        fprintf(stderr, "Failed to update spritemap.\n");
        goto error2;
    }
    spritemap_layer_id = tilemap_add_layer(ll, spritemap_id);
    if(tilemap_layer_id < 0) {
        fprintf(stderr, "Failed to add spritemap layer.\n");
        goto error2;
    }

    if(SDL_SetRenderDrawColor(renderer,
                              0, 0, 255,
                              SDL_ALPHA_OPAQUE) < 0) {
        fprintf(stderr, "Failed to set render draw color.\n");
        goto error2;
    }
    if(SDL_RenderClear(renderer) < 0) {
        fprintf(stderr, "Failed to clear screen.\n");
        goto error2;
    }
    if(SDL_SetRenderDrawColor(renderer,
                              0, 0, 0,
                              SDL_ALPHA_TRANSPARENT) < 0) {
        fprintf(stderr, "Failed to set render draw color.\n");
        goto error2;
    }
 
    if(tilemap_draw_layer(ll, tilemap_layer_id) < 0) {
        fprintf(stderr, "Failed to draw layer.\n");
        goto error2;
    }
    if(tilemap_draw_layer(ll, spritemap_layer_id) < 0) {
        fprintf(stderr, "Failed to draw layer.\n");
        goto error2;
    }

    SDL_RenderPresent(renderer);
    SDL_Delay(1000);

error2:
    layerlist_free(ll);

error1:
    SDL_DestroyWindow(win);
error0:
    SDL_Quit();

    return(EXIT_SUCCESS);
}
#endif
