/*
 * freetype.c
 * Copyright (C) 2016 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "data-types.h"
#include <math.h>
#include <structmember.h>
#include <ft2build.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <hb.h>
#pragma GCC diagnostic pop
#include <hb-ft.h>

#if HB_VERSION_MAJOR > 1 || (HB_VERSION_MAJOR == 1 && (HB_VERSION_MINOR > 0 || (HB_VERSION_MINOR == 0 && HB_VERSION_MICRO >= 5)))
#define HARBUZZ_HAS_LOAD_FLAGS
#endif

#include FT_FREETYPE_H
typedef struct {
    PyObject_HEAD

    FT_Face face;
    unsigned int units_per_EM;
    int ascender, descender, height, max_advance_width, max_advance_height, underline_position, underline_thickness;
    int hinting, hintstyle;
    bool is_scalable;
    PyObject *path;
    hb_buffer_t *harfbuzz_buffer;
    hb_font_t *harfbuzz_font;
} Face;

static PyObject* FreeType_Exception = NULL;

void 
set_freetype_error(const char* prefix, int err_code) {
    int i = 0;
#undef FTERRORS_H_
#undef __FTERRORS_H__
#define FT_ERRORDEF( e, v, s )  { e, s },
#define FT_ERROR_START_LIST     {
#define FT_ERROR_END_LIST       { 0, NULL } };

    static const struct {
        int          err_code;
        const char*  err_msg;
    } ft_errors[] =

#ifdef FT_ERRORS_H
#include FT_ERRORS_H
#else 
    FT_ERROR_START_LIST FT_ERROR_END_LIST
#endif

    while(ft_errors[i].err_msg != NULL) {
        if (ft_errors[i].err_code == err_code) {
            PyErr_Format(FreeType_Exception, "%s %s", prefix, ft_errors[i].err_msg);
            return;
        }
        i++;
    }
    PyErr_Format(FreeType_Exception, "%s (error code: %d)", prefix, err_code);
}

static FT_Library  library;


static PyObject*
new(PyTypeObject *type, PyObject *args, PyObject UNUSED *kwds) {
    Face *self;
    char *path;
    int error, hinting, hintstyle;
    long index;
    /* unsigned int columns=80, lines=24, scrollback=0; */
    if (!PyArg_ParseTuple(args, "slii", &path, &index, &hinting, &hintstyle)) return NULL;

    self = (Face *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->path = PyTuple_GET_ITEM(args, 0);
        Py_INCREF(self->path);
        error = FT_New_Face(library, path, index, &(self->face));
        if(error) { set_freetype_error("Failed to load face, with error:", error); Py_CLEAR(self); return NULL; }
#define CPY(n) self->n = self->face->n;
        CPY(units_per_EM); CPY(ascender); CPY(descender); CPY(height); CPY(max_advance_width); CPY(max_advance_height); CPY(underline_position); CPY(underline_thickness);
#undef CPY
        self->is_scalable = FT_IS_SCALABLE(self->face);
        self->harfbuzz_buffer = hb_buffer_create();
        self->hinting = hinting; self->hintstyle = hintstyle;
        if (self->harfbuzz_buffer == NULL || !hb_buffer_allocation_successful(self->harfbuzz_buffer) || !hb_buffer_pre_allocate(self->harfbuzz_buffer, 20)) { Py_CLEAR(self); return PyErr_NoMemory(); }
        self->harfbuzz_font = hb_ft_font_create(self->face, NULL);
        if (self->harfbuzz_font == NULL) { Py_CLEAR(self); return PyErr_NoMemory(); }
    }
    return (PyObject*)self;
}
 
static void
dealloc(Face* self) {
    if (self->harfbuzz_buffer) hb_buffer_destroy(self->harfbuzz_buffer);
    if (self->harfbuzz_font) hb_font_destroy(self->harfbuzz_font);
    if (self->face) FT_Done_Face(self->face);
    Py_CLEAR(self->path);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *
repr(Face *self) {
    return PyUnicode_FromFormat(
        "Face(path=%S, is_scalable=%S, units_per_EM=%u, ascender=%i, descender=%i, height=%i, max_advance_width=%i max_advance_height=%i, underline_position=%i, underline_thickness=%i)",
        self->path, self->is_scalable ? Py_True : Py_False, 
        self->ascender, self->descender, self->height, self->max_advance_width, self->max_advance_height, self->underline_position, self->underline_thickness
    );
}


static PyObject*
set_char_size(Face *self, PyObject *args) {
#define set_char_size_doc "set_char_size(width, height, xdpi, ydpi) -> set the character size. width, height is in 1/64th of a pt. dpi is in pixels per inch"
    long char_width, char_height;
    unsigned int xdpi, ydpi;
    int error;
    if (!PyArg_ParseTuple(args, "llII", &char_width, &char_height, &xdpi, &ydpi)) return NULL;
    error = FT_Set_Char_Size(self->face, char_width, char_height, xdpi, ydpi);
    if (error) { set_freetype_error("Failed to set char size, with error:", error); Py_CLEAR(self); return NULL; }
    if (self->harfbuzz_font) hb_font_destroy(self->harfbuzz_font);
    self->harfbuzz_font = hb_ft_font_create(self->face, NULL);
    if (self->harfbuzz_font == NULL) { Py_CLEAR(self); return PyErr_NoMemory(); }
    Py_RETURN_NONE;
}

static inline int
get_load_flags(int hinting, int hintstyle, int base) {
    int flags = base;
    if (hinting) {
        if (hintstyle >= 3) flags |= FT_LOAD_TARGET_NORMAL;
        else if (0 < hintstyle  && hintstyle < 3) flags |= FT_LOAD_TARGET_LIGHT;
    } else flags |= FT_LOAD_NO_HINTING;
    return flags;
}

static inline bool 
_load_char(Face *self, char_type codepoint) {
    int flags = get_load_flags(self->hinting, self->hintstyle, FT_LOAD_RENDER);
    int glyph_index = FT_Get_Char_Index(self->face, codepoint);
    int error = FT_Load_Glyph(self->face, glyph_index, flags);
    if (error) { set_freetype_error("Failed to load glyph, with error:", error); Py_CLEAR(self); return false; }
    return true;
}

static PyObject*
load_char(Face *self, PyObject *ch) {
#define load_char_doc "load_char(char)"
    if (!_load_char(self, (char_type)PyLong_AsUnsignedLong(ch))) return NULL;
    Py_RETURN_NONE;
}

static PyObject*
get_char_index(Face *self, PyObject *args) {
#define get_char_index_doc ""
    int code;
    unsigned int ans;
    if (!PyArg_ParseTuple(args, "C", &code)) return NULL;
    ans = FT_Get_Char_Index(self->face, code);

    return Py_BuildValue("I", ans);
}
 
static PyStructSequence_Field gm_fields[] = {
    {"width", NULL},
    {"height", NULL},
    {"horiBearingX", NULL},
    {"horiBearingY", NULL},
    {"horiAdvance", NULL},
    {"vertBearingX", NULL},
    {"vertBearingY", NULL},
    {"vertAdvance", NULL},
    {NULL}
};

static PyStructSequence_Desc gm_desc = {"GlpyhMetrics", NULL, gm_fields, 8};
static PyTypeObject GlpyhMetricsType = {{{0}}};

static inline PyObject*
gm_to_py(FT_Glyph_Metrics *gm) {
    PyObject *ans = PyStructSequence_New(&GlpyhMetricsType);
    if (ans != NULL) {
#define SI(num, attr) PyStructSequence_SET_ITEM(ans, num, PyLong_FromLong(gm->attr)); if (PyStructSequence_GET_ITEM(ans, num) == NULL) { Py_CLEAR(ans); return PyErr_NoMemory(); }
        SI(0, width); SI(1, height);
        SI(2, horiBearingX); SI(3, horiBearingY); SI(4, horiAdvance);
        SI(5, vertBearingX); SI(6, vertBearingY); SI(7, vertAdvance);
#undef SI
    } else return PyErr_NoMemory();
    return ans;
}

static PyObject*
glyph_metrics(Face *self) {
#define glyph_metrics_doc ""
    return gm_to_py(&self->face->glyph->metrics);
}

static PyStructSequence_Field bm_fields[] = {
    {"rows", NULL},
    {"width", NULL},
    {"pitch", NULL},
    {"buffer", NULL},
    {"num_grays", NULL},
    {"pixel_mode", NULL},
    {"palette_mode", NULL},
    {NULL}
};
static PyStructSequence_Desc bm_desc = {"Bitmap", NULL, bm_fields, 7};
static PyTypeObject BitmapType = {{{0}}};

static PyObject*
bitmap(Face *self) {
#define bitmap_doc ""
    PyObject *ans = PyStructSequence_New(&BitmapType);
    if (ans != NULL) {
#define SI(num, attr, func, conv) PyStructSequence_SET_ITEM(ans, num, func((conv)self->face->glyph->bitmap.attr)); if (PyStructSequence_GET_ITEM(ans, num) == NULL) { Py_CLEAR(ans); return PyErr_NoMemory(); }
        SI(0, rows, PyLong_FromUnsignedLong, unsigned long); SI(1, width, PyLong_FromUnsignedLong, unsigned long);
        SI(2, pitch, PyLong_FromLong, long); 
        PyObject *t = PyByteArray_FromStringAndSize((const char*)self->face->glyph->bitmap.buffer, self->face->glyph->bitmap.rows * self->face->glyph->bitmap.pitch);
        if (t == NULL) { Py_CLEAR(ans); return PyErr_NoMemory(); }
        PyStructSequence_SET_ITEM(ans, 3, t);
        SI(4, num_grays, PyLong_FromUnsignedLong, unsigned long);
        SI(5, pixel_mode, PyLong_FromUnsignedLong, unsigned long); SI(6, palette_mode, PyLong_FromUnsignedLong, unsigned long);
#undef SI
    } else return PyErr_NoMemory();
    return ans;
}
 
static PyStructSequence_Field shape_fields[] = {
    {"glyph_id", NULL},
    {"cluster", NULL},
    {"mask", NULL},
    {"x_offset", NULL},
    {"y_offset", NULL},
    {"x_advance", NULL},
    {"y_advance", NULL},
    {NULL}
};
static PyStructSequence_Desc shape_fields_desc = {"Shape", NULL, shape_fields, 7};
static PyTypeObject ShapeFieldsType = {{{0}}};

static inline PyObject*
shape_to_py(unsigned int i, hb_glyph_info_t *info, hb_glyph_position_t *pos) {
    PyObject *ans = PyStructSequence_New(&ShapeFieldsType);
    if (ans == NULL) return NULL;
#define SI(num, src, attr, conv, func, div) PyStructSequence_SET_ITEM(ans, num, func(((conv)src[i].attr) / div)); if (PyStructSequence_GET_ITEM(ans, num) == NULL) { Py_CLEAR(ans); return PyErr_NoMemory(); }
#define INFO(num, attr) SI(num, info, attr, unsigned long, PyLong_FromUnsignedLong, 1)
#define POS(num, attr) SI(num + 3, pos, attr, double, PyFloat_FromDouble, 64.0)
    INFO(0, codepoint); INFO(1, cluster); INFO(2, mask);
    POS(0, x_offset); POS(1, y_offset); POS(2, x_advance); POS(3, y_advance);
#undef INFO
#undef POS
#undef SI
    return ans;
}

typedef struct {
    unsigned int length;
    hb_glyph_info_t *info;
    hb_glyph_position_t *positions;
} ShapeData;


static inline void
_shape(Face *self, const char *string, int len, ShapeData *ans) {
    hb_buffer_clear_contents(self->harfbuzz_buffer);
#ifdef HARBUZZ_HAS_LOAD_FLAGS
    hb_ft_font_set_load_flags(self->harfbuzz_font, get_load_flags(self->hinting, self->hintstyle, FT_LOAD_DEFAULT));
#endif
    hb_buffer_add_utf8(self->harfbuzz_buffer, string, len, 0, len);
    hb_buffer_guess_segment_properties(self->harfbuzz_buffer);
    hb_shape(self->harfbuzz_font, self->harfbuzz_buffer, NULL, 0);

    unsigned int info_length, positions_length;
    ans->info = hb_buffer_get_glyph_infos(self->harfbuzz_buffer, &info_length);
    ans->positions = hb_buffer_get_glyph_positions(self->harfbuzz_buffer, &positions_length);
    ans->length = MIN(info_length, positions_length);
}


static PyObject*
shape(Face *self, PyObject *args) {
#define shape_doc "shape(text)"
    const char *string;
    int len;
    if (!PyArg_ParseTuple(args, "s#", &string, &len)) return NULL;

    ShapeData sd;
    _shape(self, string, len, &sd);
    PyObject *ans = PyTuple_New(sd.length);
    if (ans == NULL) return NULL;
    for (unsigned int i = 0; i < sd.length; i++) {
        PyObject *s = shape_to_py(i, sd.info, sd.positions);
        if (s == NULL) { Py_CLEAR(ans); return NULL; }
        PyTuple_SET_ITEM(ans, i, s);
    }
    return ans;
}

typedef struct {
    char *buf;
    unsigned int width, height;
    FT_Glyph_Metrics metrics;
} GlyphBuffer;


static inline bool
ensure_space(GlyphBuffer *g, unsigned int width, unsigned int height) {
    if (g->width >= width && g->height >= height) return true;
    char *newbuf = calloc(width * height, sizeof(char));
    if (newbuf == NULL) { free(g->buf); g->buf = NULL; g->width = 0; g->height = 0; return false; }
    for (unsigned int r = 0; r < g->height; r++) memcpy(newbuf + r * width, g->buf + r * g->width, g->width);
    free(g->buf); g->buf = newbuf; g->width = width; g->height = height;
    return true;
}

typedef struct {
    size_t x, y;
} BitmapPoint;


static inline void
apply_bitmap(GlyphBuffer *dest, FT_Bitmap *bitmap, BitmapPoint src_start, BitmapPoint dest_start) {
    char *src = (char*)bitmap->buffer;
    size_t src_height = bitmap->rows, src_width = bitmap->pitch;
#define ZERO_SUB(a, b) ((a) <= (b) ? 0 : (a) - (b))
    size_t width = MIN(ZERO_SUB(dest->width, dest_start.x), ZERO_SUB(src_width, src_start.x));
#undef ZERO_SUB

    for (size_t sy = src_start.y, dy = dest_start.y; sy < src_height && dy < dest->height; sy++, dy++) {
        memcpy(dest->buf + dest_start.x + dy * dest->width, src + sy * src_width, width);
    }
}


static PyObject*
draw_complex_glyph(Face *self, PyObject *args) {
#define draw_complex_glyph_doc "draw_complex_glyph(text)"
    const char *string;
    int len;
    float x = 0, y = 0;
    unsigned int width = 0, height = 0;
    if (!PyArg_ParseTuple(args, "s#", &string, &len)) return NULL;
    ShapeData sd;
    _shape(self, string, len, &sd);
    GlyphBuffer g = {0}; 
    BitmapPoint src, dest;
    for (unsigned i = 0; i < sd.length; i++) {
        if (sd.info[i].codepoint == 0) continue;
        _load_char(self, sd.info[i].codepoint);
        if (i == 0) { g.metrics = self->face->glyph->metrics; }
        x += (float)sd.positions[i].x_offset / 64.0;
        y -= (float)sd.positions[i].y_offset / 64.0;
        width = MAX(width, (unsigned int)ceilf(x + self->face->glyph->bitmap.pitch));
        height = MAX(height, (unsigned int)ceilf(y + self->face->glyph->bitmap.rows));
        if (!ensure_space(&g, width, height)) return PyErr_NoMemory();
        src.x = (size_t)(x < 0 ? ceilf(-x) : 0); 
        src.y = (size_t)(y < 0 ? ceilf(-y) : 0); 
        dest.x = (size_t)(x < 0 ? 0 : roundf(x));
        dest.y = (size_t)(y < 0 ? 0 : roundf(y));
        if (self->face->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) { 
            free(g.buf); 
            PyErr_Format(PyExc_ValueError, "FreeType rendered a complex glyph with an unsupported pixel mode: %d", self->face->glyph->bitmap.pixel_mode);
            return NULL;
        }
        apply_bitmap(&g, &self->face->glyph->bitmap, src, dest);
        x += (float)sd.positions[i].x_advance / 64.0;
        y = 0;
    }
    if (!g.buf) { 
        PyErr_Format(PyExc_ValueError, "No glyphs found for string: %s", string);
        return NULL;
    }
    PyObject *t = PyByteArray_FromStringAndSize((const char*)g.buf, g.width * g.height);
    free(g.buf);
    return Py_BuildValue("NNII", t, gm_to_py(&g.metrics), g.width, g.height);
}


static PyObject*
trim_to_width(Face UNUSED *self, PyObject *args) {
#define trim_to_width_doc "Trim edges from the supplied bitmap to make it fit in the specified cell-width"
    PyObject *bitmap, *t;
    unsigned long cell_width, rows, width, rtrim = 0, extra, ltrim;
    unsigned char *src, *dest;
    bool column_has_text = false;
    if (!PyArg_ParseTuple(args, "O!k", &BitmapType, &bitmap, &cell_width)) return NULL;
    rows = PyLong_AsUnsignedLong(PyStructSequence_GET_ITEM(bitmap, 0));
    width = PyLong_AsUnsignedLong(PyStructSequence_GET_ITEM(bitmap, 1));
    extra = width - cell_width;
    if (extra >= cell_width) { PyErr_SetString(FreeType_Exception, "Too large for trimming"); return NULL; }
    PyObject *ans = PyStructSequence_New(&BitmapType);
    if (ans == NULL) return PyErr_NoMemory();
    src = (unsigned char*)PyByteArray_AS_STRING(PyStructSequence_GET_ITEM(bitmap, 3));
    PyObject *abuf = PyByteArray_FromStringAndSize(NULL, cell_width * rows);
    if (abuf == NULL) { Py_CLEAR(ans); return PyErr_NoMemory(); }
    dest = (unsigned char*)PyByteArray_AS_STRING(abuf);
    PyStructSequence_SET_ITEM(ans, 1, PyLong_FromUnsignedLong(cell_width));
    PyStructSequence_SET_ITEM(ans, 2, PyLong_FromUnsignedLong(cell_width));
    PyStructSequence_SET_ITEM(ans, 3, abuf);
#define COPY(which) t = PyStructSequence_GET_ITEM(bitmap, which); Py_INCREF(t); PyStructSequence_SET_ITEM(ans, which, t);
    COPY(0); COPY(4); COPY(5); COPY(6);
#undef COPY

    for (long x = width - 1; !column_has_text && x > -1 && rtrim < extra; x--) {
        for (unsigned long y = 0; y < rows * width; y += width) {
            if (src[x + y] > 200) { column_has_text = true; break; }
        }
        if (!column_has_text) rtrim++;
    }
    rtrim = MIN(extra, rtrim);
    ltrim = extra - rtrim;
    for (unsigned long y = 0; y < rows; y++) {
        memcpy(dest + y*cell_width, src + ltrim + y*width, cell_width);
    }

    return ans;
}

// Boilerplate {{{

static PyMemberDef members[] = {
#define MEM(name, type) {#name, type, offsetof(Face, name), READONLY, #name}
    MEM(units_per_EM, T_UINT),
    MEM(ascender, T_INT),
    MEM(descender, T_INT),
    MEM(height, T_INT),
    MEM(max_advance_width, T_INT),
    MEM(max_advance_height, T_INT),
    MEM(underline_position, T_INT),
    MEM(underline_thickness, T_INT),
    MEM(is_scalable, T_BOOL),
    MEM(path, T_OBJECT_EX),
    {NULL}  /* Sentinel */
};

static PyMethodDef methods[] = {
    METHOD(set_char_size, METH_VARARGS)
    METHOD(load_char, METH_O)
    METHOD(shape, METH_VARARGS)
    METHOD(draw_complex_glyph, METH_VARARGS)
    METHOD(get_char_index, METH_VARARGS)
    METHOD(glyph_metrics, METH_NOARGS)
    METHOD(bitmap, METH_NOARGS)
    METHOD(trim_to_width, METH_VARARGS)
    {NULL}  /* Sentinel */
};


PyTypeObject Face_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "fast_data_types.Face",
    .tp_basicsize = sizeof(Face),
    .tp_dealloc = (destructor)dealloc, 
    .tp_flags = Py_TPFLAGS_DEFAULT,        
    .tp_doc = "FreeType Font face",
    .tp_methods = methods,
    .tp_members = members,
    .tp_new = new,                
    .tp_repr = (reprfunc)repr,
};

INIT_TYPE(Face)

static void
free_freetype() {
    FT_Done_FreeType(library);
}

bool 
init_freetype_library(PyObject *m) {
    FreeType_Exception = PyErr_NewException("fast_data_types.FreeTypeError", NULL, NULL);
    if (FreeType_Exception == NULL) return false;
    if (PyModule_AddObject(m, "FreeTypeError", FreeType_Exception) != 0) return false;
    int error = FT_Init_FreeType(&library);
    if (error) {
        set_freetype_error("Failed to initialize FreeType library, with error:", error);
        return false;
    }
    if (Py_AtExit(free_freetype) != 0) {
        PyErr_SetString(FreeType_Exception, "Failed to register the freetype library at exit handler");
        return false;
    }
    if (PyStructSequence_InitType2(&GlpyhMetricsType, &gm_desc) != 0) return false;
    if (PyStructSequence_InitType2(&BitmapType, &bm_desc) != 0) return false;
    if (PyStructSequence_InitType2(&ShapeFieldsType, &shape_fields_desc) != 0) return false;
    PyModule_AddObject(m, "GlyphMetrics", (PyObject*)&GlpyhMetricsType);
    PyModule_AddObject(m, "Bitmap", (PyObject*)&BitmapType);
    PyModule_AddObject(m, "ShapeFields", (PyObject*)&ShapeFieldsType);
    PyModule_AddIntMacro(m, FT_LOAD_RENDER);
    PyModule_AddIntMacro(m, FT_LOAD_TARGET_NORMAL);
    PyModule_AddIntMacro(m, FT_LOAD_TARGET_LIGHT);
    PyModule_AddIntMacro(m, FT_LOAD_NO_HINTING);
    PyModule_AddIntMacro(m, FT_PIXEL_MODE_GRAY);
    return true;
}

// }}}
