/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2011 Henrik Andersson.
    copyright (c) 2012 tobias ellinghaus.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/opencl.h"
#include "common/dtpthread.h"
#include "common/debug.h"
#include "common/interpolation.h"
#include "bauhaus/bauhaus.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/develop.h"
#include "develop/blend.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "dtgtk/button.h"
#include "dtgtk/icon.h"
#include "dtgtk/tristatebutton.h"
#include "dtgtk/slider.h"
#include "dtgtk/gradientslider.h"
#include "libs/modulegroups.h"

#include <strings.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <gmodule.h>
#include <xmmintrin.h>
#include <time.h>
#include <sys/select.h>

typedef struct dt_iop_gui_simple_callback_t {
  dt_iop_module_t *self;
  int index;
} dt_iop_gui_simple_callback_t;

static dt_develop_blend_params_t _default_blendop_params= {DEVELOP_BLEND_DISABLED, 100.0, 0, 0,
                                                           {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
                                                            0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
                                                            0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
                                                            0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f } };


void dt_iop_load_default_params(dt_iop_module_t *module)
{
  const void *op_params = NULL;
  const void *bl_params = NULL;
  memcpy(module->default_params, module->factory_params, module->params_size);
  module->default_enabled = module->factory_enabled;

  memset(module->default_blendop_params, 0, sizeof(dt_develop_blend_params_t));
  memcpy(module->default_blendop_params, &_default_blendop_params, sizeof(dt_develop_blend_params_t));
  memcpy(module->blend_params, &_default_blendop_params, sizeof(dt_develop_blend_params_t));

  // global default only allowed if the module is not enabled (pure gui convenience)
  // if these user settings would change the history stack secretly, the xmp files
  // would not carry all information and thus not be future proof.
  if(!module->default_enabled)
  {
    sqlite3_stmt *stmt;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select op_params, enabled, blendop_params, blendop_version from presets where operation = ?1 and op_version = ?2 and def=1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      op_params  = sqlite3_column_blob(stmt, 0);
      int op_length  = sqlite3_column_bytes(stmt, 0);
      // int enabled = sqlite3_column_int(stmt, 1);
      bl_params = sqlite3_column_blob(stmt, 2);
      int bl_length = sqlite3_column_bytes(stmt, 2);
      int bl_version = sqlite3_column_int(stmt, 3);
      if(op_params && (op_length == module->params_size))
      {
        memcpy(module->default_params, op_params, op_length);
        // module->default_enabled = enabled;
        if(bl_params &&  (bl_version == dt_develop_blend_version()) && (bl_length == sizeof(dt_develop_blend_params_t)))
        {
          memcpy(module->default_blendop_params, bl_params, sizeof(dt_develop_blend_params_t));
        }
        else if (bl_params)
        {
           bl_params = dt_develop_blend_legacy_params(module, bl_params, bl_version, module->default_blendop_params, dt_develop_blend_version(), bl_length) == 0 ? bl_params : (void *)1;
        }
        else
        {
          bl_params = (void *)1;
        }
      }
      else
        op_params = (void *)1;
    }
    sqlite3_finalize(stmt);

    if(op_params == (void *)1 || bl_params == (void *)1)
    {
      printf("[iop_load_defaults]: module param sizes have changed! removing default :(\n");
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from presets where operation = ?1 and op_version = ?2 and def=1", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  }
}

static void
dt_iop_modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
}

static void
dt_iop_modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
}

static gint
sort_plugins(gconstpointer a, gconstpointer b)
{
  const dt_iop_module_t *am = (const dt_iop_module_t *)a;
  const dt_iop_module_t *bm = (const dt_iop_module_t *)b;
  return am->priority - bm->priority;
}

/* default groups for modules which does not implement the groups() function */
static int
default_groups()
{
  return IOP_GROUP_ALL;
}

/* default flags for modules which does not implement the flags() function */
static int
default_flags()
{
  return 0;
}

/* default operation tags for modules which does not implement the flags() function */
static int
default_operation_tags()
{
  return 0;
}

/* default operation tags filter for modules which does not implement the flags() function */
static int
default_operation_tags_filter()
{
  return 0;
}

/* default bytes per pixel: 4*sizeof(float). */
static int
default_output_bpp(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece)
{
  return 4*sizeof(float);
}

static void
default_commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, self->params_size);
}

static void
default_init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(self->params_size);
  default_commit_params(self, self->default_params, pipe, piece);
}

static void
default_cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

static void
default_gui_cleanup(dt_iop_module_t *self)
{
  g_free(self->gui_data);
  self->gui_data = NULL;
}

static void
default_cleanup(dt_iop_module_t *module)
{
  g_free(module->gui_data);
  module->gui_data = NULL; // just to be sure
  g_free(module->params);
  module->params = NULL;
  g_free(module->data); // just to be sure
  module->data = NULL;
}


static void
default_simple_slider_callback(GtkWidget *w, dt_iop_gui_simple_callback_t *data)
{
 if(darktable.gui->reset) return;
  int *p = (int*)data->self->params;
  float *f = (float*)(&p[data->index]);
  *f = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, data->self, TRUE);
}

static void
default_simple_combobox_callback(GtkWidget *w, dt_iop_gui_simple_callback_t *data)
{
 if(darktable.gui->reset) return;
  int *p = (int*)data->self->params;
  p[data->index] = dt_bauhaus_combobox_get(w);
  dt_dev_add_history_item(darktable.develop, data->self, TRUE);
}

static void
default_simple_togglebutton_callback(GtkWidget *w, dt_iop_gui_simple_callback_t *data)
{
 if(darktable.gui->reset) return;
  int *p = (int*)data->self->params;
  p[data->index] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
  dt_dev_add_history_item(darktable.develop, data->self, TRUE);
}

static void
gui_init_simple_wrapper(dt_iop_module_t *self)
{
  if(!self->gui_init_simple)
  {
    self->widget = gtk_label_new(_("error creating gui, see stderr"));
    gtk_label_set_justify(GTK_LABEL(self->widget), GTK_JUSTIFY_LEFT);
    fprintf(stderr, "[iop_simple_gui] something went wrong while initializing the gui of `%s' using the simple api: cannot find gui_init_simple()\n", self->name());
    return;
  }
  dt_gui_simple_t *gui = self->gui_init_simple(self->so);
  if(!gui)
  {
    self->widget = gtk_label_new(_("error creating gui, see stderr"));
    gtk_label_set_justify(GTK_LABEL(self->widget), GTK_JUSTIFY_LEFT);
    fprintf(stderr, "[iop_simple_gui] something went wrong while initializing the gui of `%s' using the simple api: gui_init_simple() returned NULL\n", self->name());
    return;
  }

  size_t size = (self->params_size/sizeof(float))*sizeof(void*);
  // allocate the storage structures
  self->gui_data = g_malloc(size);
  GtkWidget **g = (GtkWidget**)self->gui_data;

  // build the gui
  self->widget = gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE);
  dt_gui_simple_element_t *it = gui->elements;
  int i=0;
  while(it->common.type != DT_SIMPLE_GUI_NONE)
  {
    switch(it->common.type){
      case DT_SIMPLE_GUI_SLIDER:
        g[i] = dt_bauhaus_slider_new_with_range(self, it->slider.min, it->slider.max, it->slider.step, it->slider.defval, it->slider.digits);
        if(it->slider.format)
          dt_bauhaus_slider_set_format(g[i], it->slider.format);
        dt_bauhaus_widget_set_label(g[i], _(it->slider.label));
        if(it->slider.value_changed)
          g_signal_connect(G_OBJECT(g[i]), "value-changed", G_CALLBACK(it->slider.value_changed), it->slider.parameter?it->slider.parameter:self);
        else
        {
          dt_iop_gui_simple_callback_t *param = malloc(sizeof(dt_iop_gui_simple_callback_t));
          param->self = self; param->index = i;
          g_signal_connect(G_OBJECT(g[i]), "value-changed", G_CALLBACK(default_simple_slider_callback), param);
        }
        break;
      case DT_SIMPLE_GUI_COMBOBOX:
        g[i] = dt_bauhaus_combobox_new(self);
        for(char** combo_iter = it->combobox.entries; *combo_iter != NULL; combo_iter++)
          dt_bauhaus_combobox_add(g[i], *combo_iter);
        dt_bauhaus_widget_set_label(g[i], _(it->combobox.label));
        dt_bauhaus_combobox_set(g[i], it->combobox.defval);
        if(it->combobox.value_changed)
          g_signal_connect(G_OBJECT(g[i]), "value-changed", G_CALLBACK(it->combobox.value_changed), it->combobox.parameter?it->combobox.parameter:self);
        else
        {
          dt_iop_gui_simple_callback_t *param = malloc(sizeof(dt_iop_gui_simple_callback_t));
          param->self = self; param->index = i;
          g_signal_connect(G_OBJECT(g[i]), "value-changed", G_CALLBACK(default_simple_combobox_callback), param);
        }
        break;
      case DT_SIMPLE_GUI_BUTTON:
        if(it->button.label != NULL)
          g[i] = dtgtk_button_new_with_label(_(it->button.label), it->button.paint, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
        else
          g[i] = dtgtk_button_new(it->button.paint, it->button.paintflags);
        if(it->button.clicked != NULL)
          g_signal_connect(G_OBJECT(g[i]), "clicked", G_CALLBACK(it->button.clicked), it->button.parameter?it->button.parameter:self);
        break;
      case DT_SIMPLE_GUI_TOGGLE_BUTTON:
        if(it->button.label != NULL)
          g[i] = dtgtk_togglebutton_new_with_label(_(it->button.label), it->button.paint, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
        else
          g[i] = dtgtk_togglebutton_new(it->button.paint, it->button.paintflags);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g[i]), it->button.defval);
        if(it->button.clicked != NULL)
          g_signal_connect(G_OBJECT(g[i]), "clicked", G_CALLBACK(it->button.clicked), it->button.parameter?it->button.parameter:self);
        else
        {
          dt_iop_gui_simple_callback_t *param = malloc(sizeof(dt_iop_gui_simple_callback_t));
          param->self = self; param->index = i;
          g_signal_connect(G_OBJECT(g[i]), "clicked", G_CALLBACK(default_simple_togglebutton_callback), param);
        }
        break;
      case DT_SIMPLE_GUI_NONE: // should never happen
        g[i] = gtk_label_new(_("error creating gui, DT_SIMPLE_GUI_NONE could not be found"));
        gtk_label_set_justify(GTK_LABEL(g[i]), GTK_JUSTIFY_LEFT);
        break;
    }
    if(it->common.tooltip)
      g_object_set(G_OBJECT(g[i]), "tooltip-text", _(it->common.tooltip), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g[i]), TRUE, TRUE, 0);

    // every control needs to have an ID. if it's missing we have to generate it :(
    if(it->common.id == NULL)
    {
      it->common.id = g_strdup_printf("%s_control_%d", self->op, i);
      fprintf(stderr, "[iop_simple_gui] control %d in `%s' doesn't have an id, using `%s' for now\n", i, self->name(), it->common.id);
    }

    i++;
    it++;
  }
}

// general gui_update for use with the simple gui api.
static void simple_gui_update(dt_iop_module_t *self)
{
  if(!self->gui_init_simple)
  {
    fprintf(stderr, "[iop_simple_gui_update] something went wrong while updating the gui of `%s' using the simple api: cannot find gui_init_simple()\n", self->name());
    return;
  }
  dt_gui_simple_t *gui = self->gui_init_simple(self->so);
  if(!gui)
  {
    fprintf(stderr, "[iop_simple_gui] something went wrong while updating the gui of `%s' using the simple api: gui_init_simple() returned NULL\n", self->name());
    return;
  }

  GtkWidget **g = (GtkWidget**)self->gui_data;
  int *p = (int*)self->params;

  dt_gui_simple_element_t *it = gui->elements;
  int i=0;
  while(it->common.type != DT_SIMPLE_GUI_NONE)
  {
    switch(it->common.type){
      case DT_SIMPLE_GUI_SLIDER:
      {
        float *f = (float*)(&p[i]);
        dt_bauhaus_slider_set(g[i], *f);
        break;
      }
      case DT_SIMPLE_GUI_COMBOBOX:
        dt_bauhaus_combobox_set(g[i], p[i]);
        break;
      case DT_SIMPLE_GUI_TOGGLE_BUTTON:
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g[i]), p[i]);
        break;
      case DT_SIMPLE_GUI_BUTTON:
      case DT_SIMPLE_GUI_NONE: // should never happen
        // nothing to do.
        break;
    }
    i++;
    it++;
  }
}

static void
simple_init(dt_iop_module_t *self)
{
  self->params = NULL;
  self->default_params = NULL;
  self->params_size = 0;

  if(!self->gui_init_simple) return;
  dt_gui_simple_t *gui = self->gui_init_simple(self->so);
  if(!gui) return;

  int elements = 0;
  dt_gui_simple_element_t *it = gui->elements;
  while(it->common.type != DT_SIMPLE_GUI_NONE)
  {
    elements++;
    it++;
  }
  size_t size = elements*sizeof(int);

  self->data = NULL;
  self->params = malloc(size);
  self->default_params = malloc(size);
  self->default_enabled = 0;
  self->params_size = size;
  self->gui_data = NULL;

  // init defaults:
  int *p = (int*)self->default_params;
  it = gui->elements;
  int i=0;
  while(it->common.type != DT_SIMPLE_GUI_NONE)
  {
    switch(it->common.type){
      case DT_SIMPLE_GUI_SLIDER:
      {
        float *f = (float*)(&p[i]);
        *f = it->slider.defval;
        break;
      }
      case DT_SIMPLE_GUI_COMBOBOX:
        p[i] = it->combobox.defval;
        break;
      case DT_SIMPLE_GUI_BUTTON:
      case DT_SIMPLE_GUI_TOGGLE_BUTTON:
        p[i] = it->button.defval;
        break;
      case DT_SIMPLE_GUI_NONE: // should never happen
        fprintf(stderr, "[iop_init_simple] DT_SIMPLE_GUI_NONE found. that should be impossible\n");
        break;
    }
    i++;
    it++;
  }
  memcpy(self->params, self->default_params, size);

  if(self->original_init)
    self->original_init(self);
}


static void
simple_init_key_accels(dt_iop_module_so_t *self)
{
  if(!self->gui_init_simple) return;
  dt_gui_simple_t *gui = self->gui_init_simple(self);
  if(!gui) return;

  dt_gui_simple_element_t *it = gui->elements;
  int i=0;
  while(it->common.type != DT_SIMPLE_GUI_NONE)
  {
    char *l = it->common.label?it->common.label:it->common.id;
    switch(it->common.type){
      case DT_SIMPLE_GUI_SLIDER:
        dt_accel_register_slider_iop(self, FALSE, NC_("accel", l));
        break;
      case DT_SIMPLE_GUI_COMBOBOX:
        // TODO: can we connect a combobox to an key accel?
        break;
      case DT_SIMPLE_GUI_BUTTON:
      case DT_SIMPLE_GUI_TOGGLE_BUTTON:
        dt_accel_register_iop(self, FALSE, NC_("accel", l), 0, 0);
        break;
      case DT_SIMPLE_GUI_NONE: // should never happen
        break;
    }
    i++;
    it++;
  }
  if(self->original_init_key_accels)
    self->original_init_key_accels(self);
}

static void
simple_connect_key_accels(dt_iop_module_t *self)
{
  GtkWidget **g = (GtkWidget**)self->gui_data;

  if(!self->gui_init_simple) return;
  dt_gui_simple_t *gui = self->gui_init_simple(self->so);
  if(!gui) return;

  dt_gui_simple_element_t *it = gui->elements;
  int i=0;
  while(it->common.type != DT_SIMPLE_GUI_NONE)
  {
    char *l = it->common.label?it->common.label:it->common.id;
    switch(it->common.type){
      case DT_SIMPLE_GUI_SLIDER:
        dt_accel_connect_slider_iop(self, l, g[i]);
        break;
      case DT_SIMPLE_GUI_COMBOBOX:
        // TODO: can we connect a combobox to an key accel?
        break;
      case DT_SIMPLE_GUI_BUTTON:
      case DT_SIMPLE_GUI_TOGGLE_BUTTON:
        dt_accel_connect_button_iop(self, l, g[i]);
        break;
      case DT_SIMPLE_GUI_NONE: // should never happen
        break;
    }
    i++;
    it++;
  }
  if(self->original_connect_key_accels)
    self->original_connect_key_accels(self);
}

int dt_iop_load_module_so(dt_iop_module_so_t *module, const char *libname, const char *op)
{
  gboolean use_simple_api = FALSE;
  g_strlcpy(module->op, op, 20);
  module->data = NULL;
  module->module = g_module_open(libname, G_MODULE_BIND_LAZY);
  if(!module->module) goto error;
  int (*version)();
  if(!g_module_symbol(module->module, "dt_module_dt_version", (gpointer)&(version))) goto error;
  if(version() != dt_version())
  {
    fprintf(stderr, "[iop_load_module] `%s' is compiled for another version of dt (module %d (%s) != dt %d (%s)) !\n", libname, abs(version()), version() < 0 ? "debug" : "opt", abs(dt_version()), dt_version() < 0 ? "debug" : "opt");
    goto error;
  }
  if(!g_module_symbol(module->module, "dt_module_mod_version",  (gpointer)&(module->version)))                goto error;
  if(!g_module_symbol(module->module, "name",                   (gpointer)&(module->name)))                   goto error;
  if(!g_module_symbol(module->module, "groups",                 (gpointer)&(module->groups)))                 module->groups = default_groups;
  if(!g_module_symbol(module->module, "flags",                  (gpointer)&(module->flags)))                  module->flags = default_flags;
  if(!g_module_symbol(module->module, "operation_tags",         (gpointer)&(module->operation_tags)))         module->operation_tags = default_operation_tags;
  if(!g_module_symbol(module->module, "operation_tags_filter",  (gpointer)&(module->operation_tags_filter)))  module->operation_tags_filter = default_operation_tags_filter;
  if(!g_module_symbol(module->module, "output_bpp",             (gpointer)&(module->output_bpp)))             module->output_bpp = default_output_bpp;
  if(!g_module_symbol(module->module, "tiling_callback",        (gpointer)&(module->tiling_callback)))        module->tiling_callback = default_tiling_callback;
  if(!g_module_symbol(module->module, "gui_reset",              (gpointer)&(module->gui_reset)))              module->gui_reset = NULL;
  if(!g_module_symbol(module->module, "gui_init",               (gpointer)&(module->gui_init)))
  {
    if(!g_module_symbol(module->module, "gui_init_simple",        (gpointer)&(module->gui_init_simple)))
    {
      module->gui_init = NULL;
      module->gui_init_simple = NULL;
    }
    else
    {
      module->gui_init = &gui_init_simple_wrapper;
      use_simple_api = TRUE;
    }
  }
  if(!g_module_symbol(module->module, "gui_update",             (gpointer)&(module->gui_update))){
    if(use_simple_api)
      module->gui_update = simple_gui_update;
    else
      module->gui_update = NULL;
  }
  if(!g_module_symbol(module->module, "gui_cleanup",            (gpointer)&(module->gui_cleanup)))            module->gui_cleanup = default_gui_cleanup;

  if(!g_module_symbol(module->module, "gui_post_expose",        (gpointer)&(module->gui_post_expose)))        module->gui_post_expose = NULL;
  if(!g_module_symbol(module->module, "gui_focus",              (gpointer)&(module->gui_focus)))              module->gui_focus = NULL;
  if(use_simple_api)
  {
    module->init_key_accels = &simple_init_key_accels;
    module->connect_key_accels = &simple_connect_key_accels;
    if(!g_module_symbol(module->module, "init_key_accels",      (gpointer)&(module->original_init_key_accels))) module->original_init_key_accels = NULL;
    if(!g_module_symbol(module->module, "connect_key_accels",   (gpointer)&(module->original_connect_key_accels))) module->original_connect_key_accels= NULL;
  }
  else
  {
    if(!g_module_symbol(module->module, "init_key_accels",      (gpointer)&(module->init_key_accels)))        module->init_key_accels = NULL;
    if(!g_module_symbol(module->module, "connect_key_accels",   (gpointer)&(module->connect_key_accels)))     module->connect_key_accels = NULL;
  }
  if(!g_module_symbol(module->module, "disconnect_key_accels",  (gpointer)&(module->disconnect_key_accels)))  module->disconnect_key_accels = NULL;
  if(!g_module_symbol(module->module, "mouse_leave",            (gpointer)&(module->mouse_leave)))            module->mouse_leave = NULL;
  if(!g_module_symbol(module->module, "mouse_moved",            (gpointer)&(module->mouse_moved)))            module->mouse_moved = NULL;
  if(!g_module_symbol(module->module, "button_released",        (gpointer)&(module->button_released)))        module->button_released = NULL;
  if(!g_module_symbol(module->module, "button_pressed",         (gpointer)&(module->button_pressed)))         module->button_pressed = NULL;
  if(!g_module_symbol(module->module, "configure",              (gpointer)&(module->configure)))              module->configure = NULL;
  if(!g_module_symbol(module->module, "scrolled",               (gpointer)&(module->scrolled)))               module->scrolled = NULL;

  if(use_simple_api)
  {
    module->init = &simple_init;
    if(!g_module_symbol(module->module, "init",                 (gpointer)&(module->original_init)))          module->original_init = NULL;
  }
  else
  {
    if(!g_module_symbol(module->module, "init",                 (gpointer)&(module->init)))                   goto error;
  }
  if(!g_module_symbol(module->module, "cleanup",                (gpointer)&(module->cleanup)))                module->cleanup = &default_cleanup;
  if(!g_module_symbol(module->module, "init_global",            (gpointer)&(module->init_global)))            module->init_global = NULL;
  if(!g_module_symbol(module->module, "cleanup_global",         (gpointer)&(module->cleanup_global)))         module->cleanup_global = NULL;
  if(!g_module_symbol(module->module, "init_presets",           (gpointer)&(module->init_presets)))           module->init_presets = NULL;
  if(!g_module_symbol(module->module, "commit_params",          (gpointer)&(module->commit_params)))          module->commit_params = default_commit_params;
  if(!g_module_symbol(module->module, "reload_defaults",        (gpointer)&(module->reload_defaults)))        module->reload_defaults = NULL;
  if(!g_module_symbol(module->module, "init_pipe",              (gpointer)&(module->init_pipe)))              module->init_pipe = default_init_pipe;
  if(!g_module_symbol(module->module, "cleanup_pipe",           (gpointer)&(module->cleanup_pipe)))           module->cleanup_pipe = default_cleanup_pipe;
  if(!g_module_symbol(module->module, "process",                (gpointer)&(module->process)))                goto error;
  if(!g_module_symbol(module->module, "process_tiling",         (gpointer)&(module->process_tiling)))         module->process_tiling = default_process_tiling;
  if(!darktable.opencl->inited ||
      !g_module_symbol(module->module, "process_cl",            (gpointer)&(module->process_cl)))             module->process_cl = NULL;
  if(!g_module_symbol(module->module, "process_tiling_cl",      (gpointer)&(module->process_tiling_cl)))      module->process_tiling_cl = darktable.opencl->inited ? default_process_tiling_cl : NULL;
  if(!g_module_symbol(module->module, "modify_roi_in",          (gpointer)&(module->modify_roi_in)))          module->modify_roi_in = dt_iop_modify_roi_in;
  if(!g_module_symbol(module->module, "modify_roi_out",         (gpointer)&(module->modify_roi_out)))         module->modify_roi_out = dt_iop_modify_roi_out;
  if(!g_module_symbol(module->module, "legacy_params",          (gpointer)&(module->legacy_params)))          module->legacy_params = NULL;
  if(module->init_global) module->init_global(module);
  return 0;
error:
  fprintf(stderr, "[iop_load_module] failed to open operation `%s': %s\n", op, g_module_error());
  if(module->module) g_module_close(module->module);
  return 1;
}

static int
dt_iop_load_module_by_so(dt_iop_module_t *module, dt_iop_module_so_t *so, dt_develop_t *dev)
{
  module->dt = &darktable;
  module->dev = dev;
  module->widget = NULL;
  module->header = NULL;
  module->showhide = NULL;
  module->off = NULL;
  module->priority = 0;
  module->hide_enable_button = 0;
  module->request_color_pick = 0;
  for(int k=0; k<3; k++)
  {
    module->picked_color[k] =
      module->picked_color_min[k] =
        module->picked_color_max[k] = 0.0f;
  }
  module->color_picker_box[0] = module->color_picker_box[1] = .25f;
  module->color_picker_box[2] = module->color_picker_box[3] = .75f;
  module->color_picker_point[0] = module->color_picker_point[1] = 0.5f;
  module->request_mask_display = 0;
  module->enabled = module->default_enabled = 1; // all modules enabled by default.
  g_strlcpy(module->op, so->op, 20);

  // only reference cached results of dlopen:
  module->module = so->module;

  module->version     = so->version;
  module->name        = so->name;
  module->groups      = so->groups;
  module->flags       = so->flags;
  module->operation_tags  = so->operation_tags;
  module->operation_tags_filter  = so->operation_tags_filter;
  module->output_bpp  = so->output_bpp;
  module->tiling_callback = so->tiling_callback;
  module->gui_update  = so->gui_update;
  module->gui_reset  = so->gui_reset;
  module->gui_init    = so->gui_init;
  module->gui_init_simple = so->gui_init_simple;
  module->gui_cleanup = so->gui_cleanup;

  module->gui_post_expose = so->gui_post_expose;
  module->gui_focus       = so->gui_focus;
  module->mouse_leave     = so->mouse_leave;
  module->mouse_moved     = so->mouse_moved;
  module->button_released = so->button_released;
  module->button_pressed  = so->button_pressed;
  module->configure       = so->configure;
  module->scrolled        = so->scrolled;

  module->init            = so->init;
  module->original_init   = so->original_init;
  module->cleanup         = so->cleanup;
  module->commit_params   = so->commit_params;
  module->reload_defaults = so->reload_defaults;
  module->init_pipe       = so->init_pipe;
  module->cleanup_pipe    = so->cleanup_pipe;
  module->process         = so->process;
  module->process_tiling  = so->process_tiling;
  module->process_cl      = so->process_cl;
  module->process_tiling_cl = so->process_tiling_cl;
  module->modify_roi_in   = so->modify_roi_in;
  module->modify_roi_out  = so->modify_roi_out;
  module->legacy_params   = so->legacy_params;

  module->connect_key_accels = so->connect_key_accels;
  module->disconnect_key_accels = so->disconnect_key_accels;

  module->accel_closures = NULL;
  module->accel_closures_local = NULL;
  module->local_closures_connected = FALSE;
  module->reset_button = NULL;
  module->presets_button = NULL;
  module->fusion_slider = NULL;

  // now init the instance:
  module->init(module);

  /* initialize blendop params and default values */
  module->blend_params=g_malloc(sizeof(dt_develop_blend_params_t));
  module->default_blendop_params=g_malloc(sizeof(dt_develop_blend_params_t));
  memset(module->blend_params, 0, sizeof(dt_develop_blend_params_t));
  // dt_develop_blend_params_t default_blendop_params= {DEVELOP_BLEND_DISABLED,100.0,0};
  memset(module->default_blendop_params, 0, sizeof(dt_develop_blend_params_t));
  memcpy(module->default_blendop_params, &_default_blendop_params, sizeof(dt_develop_blend_params_t));
  memcpy(module->blend_params, &_default_blendop_params, sizeof(dt_develop_blend_params_t));

  if(module->priority == 0)
  {
    fprintf(stderr, "[iop_load_module] `%s' needs to set priority!\n", so->op);
    return 1;      // this needs to be set
  }
  module->enabled = module->default_enabled; // apply (possibly new) default.
  return 0;
}

void dt_iop_init_pipe(struct dt_iop_module_t *module, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece)
{
  module->init_pipe(module, pipe, piece);
  piece->blendop_data = malloc(sizeof(dt_develop_blend_params_t));
  memset(piece->blendop_data, 0, sizeof(dt_develop_blend_params_t));
  memcpy(module->blend_params, &_default_blendop_params, sizeof(dt_develop_blend_params_t));
  /// FIXME: Commmit params is already done in module
  dt_iop_commit_params(module, module->default_params, module->default_blendop_params, pipe, piece);
}

static void
dt_iop_gui_off_callback(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(!darktable.gui->reset)
  {
    if(gtk_toggle_button_get_active(togglebutton)) module->enabled = 1;
    else module->enabled = 0;
    dt_dev_add_history_item(module->dev, module, FALSE);
  }
  char tooltip[512];
  snprintf(tooltip, 512, module->enabled ? _("%s is switched on") : _("%s is switched off"), module->name());
  g_object_set(G_OBJECT(togglebutton), "tooltip-text", tooltip, (char *)NULL);
}

gboolean dt_iop_is_hidden(dt_iop_module_t *module)
{
  gboolean is_hidden = TRUE;
  if ( !(module->flags() & IOP_FLAGS_HIDDEN) )
  {
    if(!module->gui_init && !module->gui_init_simple)
      g_debug("Module '%s' is not hidden and lacks implementation of gui_init() and gui_init_simple()...", module->op);
    else if (!module->gui_cleanup)
      g_debug("Module '%s' is not hidden and lacks implementation of gui_cleanup()...", module->op);
    else
      is_hidden = FALSE;
  }
  return is_hidden;
}

static void _iop_gui_update_header(dt_iop_module_t *module)
{
    /* get the enable button spacer and button */
    GtkWidget *eb = g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(module->header)),0);
    GtkWidget *ebs = g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(module->header)),1);

    if (module->hide_enable_button)
    {
      gtk_widget_hide(eb);
      gtk_widget_show(ebs);
    }
    else
    {
      gtk_widget_show(eb);
      gtk_widget_hide(ebs);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), module->enabled);
    }
}

void dt_iop_reload_defaults(dt_iop_module_t *module)
{
  if(module->reload_defaults)
  {
    module->reload_defaults(module);
    // factory defaults can only be overwritten if reload_defaults actually exists:
    memcpy(module->factory_params, module->params, module->params_size);
    module->factory_enabled = module->default_enabled;
  }
  dt_iop_load_default_params(module);

  if(module->header)
    _iop_gui_update_header(module);
}

static void
init_presets(dt_iop_module_so_t *module_so)
{
  if(module_so->init_presets) module_so->init_presets(module_so);

  // this seems like a reasonable place to check for and update legacy
  // presets.

  int32_t module_version = module_so->version();

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select name, op_version, op_params from presets where operation = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module_so->op, strlen(module_so->op), SQLITE_TRANSIENT);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (char*)sqlite3_column_text(stmt, 0);
    int32_t old_params_version = sqlite3_column_int(stmt, 1);
    void *old_params = (void *)sqlite3_column_blob(stmt, 2);
    int32_t old_params_size = sqlite3_column_bytes(stmt, 2);

    if( old_params_version == 0 )
    {
        // this preset doesn't have a version.  go digging through the database
        // to find a history entry that matches the preset params, and get
        // the module version from that.

        sqlite3_stmt *stmt2;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select module from history where operation = ?1 and op_params = ?2", -1, &stmt2, NULL);
        DT_DEBUG_SQLITE3_BIND_TEXT( stmt2, 1, module_so->op, strlen(module_so->op), SQLITE_TRANSIENT );
        DT_DEBUG_SQLITE3_BIND_BLOB( stmt2, 2, old_params, old_params_size, SQLITE_TRANSIENT );

        if( sqlite3_step(stmt2) == SQLITE_ROW)
        {
          old_params_version = sqlite3_column_int(stmt2, 0);
        }
        else
        {
          fprintf(stderr, "[imageop_init_presets] WARNING: Could not find versioning information for '%s' preset '%s'\nUntil some is found, the preset will be unavailable.\n(To make it return, please load an image that uses the preset.)\n", module_so->op, name );
          sqlite3_finalize(stmt2);
          continue;
        }

        sqlite3_finalize(stmt2);

        // we found an old params version.  Update the database with it.

        fprintf(stderr, "[imageop_init_presets] Found version %d for '%s' preset '%s'\n", old_params_version, module_so->op, name );

        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update presets set op_version=?1 where operation=?2 and name=?3", -1, &stmt2, NULL);
        DT_DEBUG_SQLITE3_BIND_INT( stmt2, 1, old_params_version );
        DT_DEBUG_SQLITE3_BIND_TEXT( stmt2, 2, module_so->op, strlen(module_so->op), SQLITE_TRANSIENT );
        DT_DEBUG_SQLITE3_BIND_TEXT( stmt2, 3, name, strlen(name), SQLITE_TRANSIENT );

        sqlite3_step(stmt2);
        sqlite3_finalize(stmt2);
    }

    if( module_version > old_params_version && module_so->legacy_params != NULL )
    {
      fprintf(stderr, "[imageop_init_presets] updating '%s' preset '%s' from version %d to version %d\n", module_so->op, name, old_params_version, module_version );

      // we need a dt_iop_module_t for legacy_params()
      dt_iop_module_t *module;
      module = (dt_iop_module_t *)malloc(sizeof(dt_iop_module_t));
      memset(module, 0, sizeof(dt_iop_module_t));
      if( dt_iop_load_module_by_so(module, module_so, NULL) )
      {
        free(module);
        continue;
      }

      module->init(module);
      int32_t new_params_size = module->params_size;
      void *new_params = malloc(new_params_size);

      // convert the old params to new
      module->legacy_params(module, old_params, old_params_version, new_params, module_version );

      // and write the new params back to the database
      sqlite3_stmt *stmt2;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update presets "
              "set op_version=?1, op_params=?2 "
              "where operation=?3 and name=?4", -1, &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, module->version());
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt2, 2, new_params, new_params_size, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 3, module->op, strlen(module_so->op), SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 4, name, strlen(name), SQLITE_TRANSIENT);

      sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);

      free(module);
    }
    else if( module_version > old_params_version )
    {
      fprintf(stderr, "[imageop_init_presets] Can't upgrade '%s' preset '%s' from version %d to %d, no legacy_params() implemented \n", module_so->op, name, old_params_version, module_version );
    }
  }
  sqlite3_finalize(stmt);
}

static void init_key_accels(dt_iop_module_so_t *module)
{
    // Calling the accelerator initialization callback, if present
  if(module->init_key_accels)
    (module->init_key_accels)(module);
  /** load shortcuts for presets **/
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select name from presets where operation=?1 order by writeprotect desc, rowid", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char path[1024];
    snprintf(path,1024,"%s/%s",_("preset"),(const char *)sqlite3_column_text(stmt, 0));
    dt_accel_register_iop(module, FALSE, NC_("accel", path), 0, 0);

  }
  sqlite3_finalize(stmt);
}

void dt_iop_load_modules_so()
{
  GList *res = NULL;
  dt_iop_module_so_t *module;
  darktable.iop = NULL;
  char plugindir[1024], op[20];
  const gchar *d_name;
  dt_loc_get_plugindir(plugindir, 1024);
  g_strlcat(plugindir, "/plugins", 1024);
  GDir *dir = g_dir_open(plugindir, 0, NULL);
  if(!dir) return;
  while((d_name = g_dir_read_name(dir)))
  {
    // get lib*.so
    if(strncmp(d_name, "lib", 3)) continue;
    if(strncmp(d_name + strlen(d_name) - 3, ".so", 3)) continue;
    strncpy(op, d_name+3, strlen(d_name)-6);
    op[strlen(d_name)-6] = '\0';
    module = (dt_iop_module_so_t *)malloc(sizeof(dt_iop_module_so_t));
    memset(module,0,sizeof(dt_iop_module_so_t));
    gchar *libname = g_module_build_path(plugindir, (const gchar *)op);
    if(dt_iop_load_module_so(module, libname, op))
    {
      free(module);
      continue;
    }
    g_free(libname);
    res = g_list_append(res, module);
    init_presets(module);
    // Calling the accelerator initialization callback, if present
    init_key_accels(module);

    if (module->flags()&IOP_FLAGS_SUPPORTS_BLENDING)
    {
      dt_accel_register_slider_iop(module, FALSE, NC_("accel", "fusion"));
    }
    if(!(module->flags() & IOP_FLAGS_DEPRECATED))
    {
      // Adding the optional show accelerator to the table (blank)
      dt_accel_register_iop(module, FALSE, NC_("accel", "show module"), 0, 0);
      dt_accel_register_iop(module, FALSE, NC_("accel", "enable module"), 0, 0);

      dt_accel_register_iop(module, FALSE,
                            NC_("accel", "reset module parameters"), 0, 0);
      dt_accel_register_iop(module, FALSE,
                            NC_("accel", "show preset menu"), 0, 0);
    }
  }
  g_dir_close(dir);
  darktable.iop = res;
}

GList *dt_iop_load_modules(dt_develop_t *dev)
{
  GList *res = NULL;
  dt_iop_module_t *module;
  dt_iop_module_so_t *module_so;
  dev->iop_instance = 0;
  GList *iop = darktable.iop;
  while(iop)
  {
    module_so = (dt_iop_module_so_t *)iop->data;
    module    = (dt_iop_module_t *)malloc(sizeof(dt_iop_module_t));
    memset(module,0,sizeof(dt_iop_module_t));
    if(dt_iop_load_module_by_so(module, module_so, dev))
    {
      free(module);
      continue;
    }
    res = g_list_insert_sorted(res, module, sort_plugins);
    module->data = module_so->data;
    module->factory_params = malloc(module->params_size);
    // copy factory params first time. reload_defaults will only overwrite
    // if module->reload_defaults exists (else the here copied values
    // stay constant for all images).
    memcpy(module->factory_params, module->params, module->params_size);
    module->factory_enabled = module->default_enabled;
    module->so = module_so;
    dt_iop_reload_defaults(module);
    iop = g_list_next(iop);
  }

  GList *it = res;
  while(it)
  {
    module = (dt_iop_module_t *)it->data;
    module->instance = dev->iop_instance++;
    it = g_list_next(it);
  }
  return res;
}

void dt_iop_cleanup_module(dt_iop_module_t *module)
{
  free(module->factory_params); module->factory_params = NULL ;
  module->cleanup(module);

  free(module->default_params); module->default_params = NULL ;
  if (module->blend_params != NULL)
  {
    free(module->blend_params) ;
    module->blend_params = NULL ;
  }
  if (module->default_blendop_params != NULL)
  {
    free(module->default_blendop_params) ;
    module->default_blendop_params = NULL ;
  }
}

void dt_iop_unload_modules_so()
{
  while(darktable.iop)
  {
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)darktable.iop->data;
    if(module->cleanup_global) module->cleanup_global(module);
    if(module->module) g_module_close(module->module);
    free(darktable.iop->data);
    darktable.iop = g_list_delete_link(darktable.iop, darktable.iop);
  }
}

void dt_iop_commit_params(dt_iop_module_t *module, dt_iop_params_t *params, dt_develop_blend_params_t * blendop_params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  uint64_t hash = 5381;
  piece->hash = 0;
  if(piece->enabled)
  {
    /* construct module params data for hash calc */
    int length = module->params_size;
    if (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) length += sizeof(dt_develop_blend_params_t);

    char *str = malloc(length);
    memcpy(str, module->params, module->params_size);

    /* if module supports blend op add blend params into account */
    if (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      memcpy(str+module->params_size, blendop_params, sizeof(dt_develop_blend_params_t));
      memcpy(piece->blendop_data, blendop_params, sizeof(dt_develop_blend_params_t));
      // this should be redundant! (but is not)
      memcpy(module->blend_params, blendop_params, sizeof(dt_develop_blend_params_t));
    }

    // assume process_cl is ready, commit_params can overwrite this.
    if(module->process_cl) piece->process_cl_ready = 1;
    module->commit_params(module, params, pipe, piece);
    for(int i=0; i<length; i++) hash = ((hash << 5) + hash) ^ str[i];
    piece->hash = hash;

    free(str);
  }
  // printf("commit params hash += module %s: %lu, enabled = %d\n", piece->module->op, piece->hash, piece->enabled);
}

void dt_iop_gui_update(dt_iop_module_t *module)
{
  int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  if (!dt_iop_is_hidden(module))
  {
    dt_iop_gui_update_expanded(module);
    module->gui_update(module);
    if (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t*)module->blend_data;

      dt_bauhaus_combobox_set(bd->blend_modes_combo, dt_iop_gui_blending_mode_seq(bd, module->blend_params->mode));
      dt_bauhaus_slider_set(bd->opacity_slider, module->blend_params->opacity);
      if(bd->blendif_support)
      {
        dt_bauhaus_combobox_set(bd->blendif_enable, (module->blend_params->blendif & (1<<DEVELOP_BLENDIF_active)) != 0);
        dt_iop_gui_update_blendif(module);
      }
    }
    if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), module->enabled);
  }
  darktable.gui->reset = reset;
}

void dt_iop_gui_reset(dt_iop_module_t *module)
{
  int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  if (module->gui_reset && !dt_iop_is_hidden(module))
    module->gui_reset(module);
  darktable.gui->reset = reset;
}

static int _iop_module_demosaic=0,_iop_module_colorout=0,_iop_module_colorin=0;
dt_iop_colorspace_type_t dt_iop_module_colorspace(const dt_iop_module_t *module)
{
  /* check if we do know what priority the color* plugins have */
  if(_iop_module_colorout==0 && _iop_module_colorin==0)
  {
    /* lets find out which priority colorin and colorout have */
    GList *iop = module->dev->iop;
    while(iop)
    {
      dt_iop_module_t *m = (dt_iop_module_t *)iop->data;
      if(m != module)
      {
        if(!strcmp(m->op,"colorin"))
          _iop_module_colorin = m->priority;
        else if(!strcmp(m->op,"colorout"))
          _iop_module_colorout = m->priority;
        else if(!strcmp(m->op,"demosaic"))
          _iop_module_demosaic = m->priority;
      }

      /* do we have both priorities, lets break out... */
      if(_iop_module_colorout && _iop_module_colorin && _iop_module_demosaic)
        break;
      iop = g_list_next(iop);
    }
  }

  /* let check which colorspace module is within */
  if (module->priority > _iop_module_colorout)
    return iop_cs_rgb;
  else if (module->priority > _iop_module_colorin)
    return iop_cs_Lab;
  else if (module->priority < _iop_module_demosaic)
    return iop_cs_RAW;

  /* fallback to rgb */
  return iop_cs_rgb;
}


static void
dt_iop_gui_reset_callback(GtkButton *button, dt_iop_module_t *module)
{
  /* reset to default params */
  memcpy(module->params, module->default_params, module->params_size);
  memcpy(module->blend_params, module->default_blendop_params, sizeof(dt_develop_blend_params_t));

  /* reset ui to its defaults */
  dt_iop_gui_reset(module);

  /* update ui to default params*/
  dt_iop_gui_update(module);

  dt_dev_add_history_item(module->dev, module, TRUE);
}



static void
_preset_popup_position(GtkMenu *menu, gint *x,gint *y,gboolean *push_in, gpointer data)
{
  gint w,h;
  GtkRequisition requisition;
  gdk_window_get_size(GTK_WIDGET(data)->window,&w,&h);
  gdk_window_get_origin (GTK_WIDGET(data)->window, x, y);
  gtk_widget_size_request (GTK_WIDGET (menu), &requisition);

  (*y)+=GTK_WIDGET(data)->allocation.height;
}

static void
popup_callback(GtkButton *button, dt_iop_module_t *module)
{
  dt_gui_presets_popup_menu_show_for_module(module);
  gtk_menu_popup(darktable.gui->presets_popup_menu, NULL, NULL, _preset_popup_position, button, 0, gtk_get_current_event_time());
  gtk_widget_show_all(GTK_WIDGET(darktable.gui->presets_popup_menu));
  gtk_menu_reposition(GTK_MENU(darktable.gui->presets_popup_menu));
}

void dt_iop_request_focus(dt_iop_module_t *module)
{
  if(darktable.gui->reset || (darktable.develop->gui_module == module)) return;

  /* lets lose the focus of previous focus module*/
  if (darktable.develop->gui_module)
  {
    if (darktable.develop->gui_module->gui_focus)
      darktable.develop->gui_module->gui_focus(darktable.develop->gui_module, FALSE);

    gtk_widget_set_state(dt_iop_gui_get_pluginui(darktable.develop->gui_module), GTK_STATE_NORMAL);

  //    gtk_widget_set_state(darktable.develop->gui_module->topwidget, GTK_STATE_NORMAL);

    /*
    GtkWidget *off = GTK_WIDGET(darktable.develop->gui_module->off);

    if (off)
      gtk_widget_set_state(off,
			   gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(off)) ?
			   GTK_STATE_ACTIVE : GTK_STATE_NORMAL);
    */

    if (darktable.develop->gui_module->operation_tags_filter())
      dt_dev_invalidate_from_gui(darktable.develop);

    dt_accel_disconnect_locals_iop(darktable.develop->gui_module);
  }

  darktable.develop->gui_module = module;

  /* set the focus on module */
  if(module)
  {
    gtk_widget_set_state(dt_iop_gui_get_pluginui(module), GTK_STATE_SELECTED);

    //gtk_widget_set_state(module->widget,    GTK_STATE_NORMAL);

    /*
    GtkWidget *off = GTK_WIDGET(darktable.develop->gui_module->off);
    if (off)
      gtk_widget_set_state(off,
			   gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(off)) ?
			   GTK_STATE_ACTIVE : GTK_STATE_NORMAL);
    */
    if (module->operation_tags_filter())
      dt_dev_invalidate_from_gui(darktable.develop);

    dt_accel_connect_locals_iop(module);

    if(module->gui_focus)
      module->gui_focus(module, TRUE);
  }
  dt_control_change_cursor(GDK_LEFT_PTR);
}


/*
 * NEW EXPANDER
 */

void dt_iop_gui_set_expanded(dt_iop_module_t *module, gboolean expanded)
{
  if(!module->expander) return;

  /* update expander arrow state */
  GtkWidget *icon;
  GtkWidget *header = gtk_bin_get_child(GTK_BIN(g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(module->expander)),0)));
  GtkWidget *pluginui = dt_iop_gui_get_widget(module);
  gint flags = CPF_DIRECTION_DOWN;

  /* get arrow icon widget */
  icon = g_list_last(gtk_container_get_children(GTK_CONTAINER(header)))->data;
  if(!expanded)
    flags=CPF_DIRECTION_LEFT;

  dtgtk_icon_set_paint(icon, dtgtk_cairo_paint_solid_arrow, flags);

  /* store expanded state of module.
   * we do that first, so update_expanded won't think it should be visible
   * and undo our changes right away. */
  module->expanded = expanded;
  char var[1024];
  snprintf(var, 1024, "plugins/darkroom/%s/expanded", module->op);
  dt_conf_set_bool(var, expanded);

  /* show / hide plugin widget */
  if (expanded)
  {
    /* show plugin ui */
    gtk_widget_show(pluginui);

    /* ensure that blending widgets are shown as they should */
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t*)module->blend_data;

    if (bd != NULL)
    {
      if(bd->modes[dt_bauhaus_combobox_get(bd->blend_modes_combo)].mode == DEVELOP_BLEND_DISABLED)
      {
        gtk_widget_hide(GTK_WIDGET(bd->opacity_slider));
        if(bd->blendif_support)
        {
          gtk_widget_hide(GTK_WIDGET(bd->blendif_box));
          gtk_widget_hide(GTK_WIDGET(bd->blendif_enable));
        }
      }
      else
      {
        gtk_widget_show(GTK_WIDGET(bd->opacity_slider));
        if(bd->blendif_support)
        {
          gtk_widget_show(GTK_WIDGET(bd->blendif_enable));
          if(dt_bauhaus_combobox_get(bd->blendif_enable) != 0)
            gtk_widget_show(GTK_WIDGET(bd->blendif_box));
        }
      }
    }

    /* set this module to receive focus / draw events*/
    dt_iop_request_focus(module);

    /* focus the current module */
    for(int k=0;k<DT_UI_CONTAINER_SIZE;k++)
      dt_ui_container_focus_widget(darktable.gui->ui, k, module->expander);

    /* redraw center, iop might have post expose */
    dt_control_queue_redraw_center();

  }
  else
  {
    gtk_widget_hide(pluginui);

    if(module->dev->gui_module == module)
    {
      dt_iop_request_focus(NULL);
      dt_control_queue_redraw_center();
    }
  }

}


void dt_iop_gui_update_expanded(dt_iop_module_t *module)
{
  if(!module->expander) return;

  gboolean expanded = module->expanded;

  /* update expander arrow state */
  GtkWidget *icon;
  GtkWidget *header = gtk_bin_get_child(GTK_BIN(g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(module->expander)),0)));
  GtkWidget *pluginui = dt_iop_gui_get_widget(module);
  gint flags = CPF_DIRECTION_DOWN;

  /* get arrow icon widget */
  icon = g_list_last(gtk_container_get_children(GTK_CONTAINER(header)))->data;
  if(!expanded)
    flags=CPF_DIRECTION_LEFT;

  dtgtk_icon_set_paint(icon, dtgtk_cairo_paint_solid_arrow, flags);

  if (expanded)
  {
    /* show plugin ui */
    gtk_widget_show(pluginui);

    /* ensure that blending widgets are show as the should */
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t*)module->blend_data;

    if (bd != NULL)
    {
      if(bd->modes[dt_bauhaus_combobox_get(bd->blend_modes_combo)].mode == DEVELOP_BLEND_DISABLED)
      {
        gtk_widget_hide(GTK_WIDGET(bd->opacity_slider));
        if(bd->blendif_support)
        {
          gtk_widget_hide(GTK_WIDGET(bd->blendif_box));
          gtk_widget_hide(GTK_WIDGET(bd->blendif_enable));
        }
      }
      else
      {
        gtk_widget_show(GTK_WIDGET(bd->opacity_slider));
        if(bd->blendif_support)
        {
          gtk_widget_show(GTK_WIDGET(bd->blendif_enable));
          if(dt_bauhaus_combobox_get(bd->blendif_enable) != 0)
            gtk_widget_show(GTK_WIDGET(bd->blendif_box));
        }
      }
    }
  }
  else
  {
    gtk_widget_hide(pluginui);
  }
}


static gboolean
_iop_plugin_body_scrolled(GtkWidget *w, GdkEvent *e, gpointer user_data)
{
  // just make sure nothing happens:
  return TRUE;
}

static gboolean
_iop_plugin_body_button_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if (e->button == 1)
  {
    dt_iop_request_focus(module);
    return TRUE;
  }
  else if (e->button == 3)
  {
    dt_gui_presets_popup_menu_show_for_module(module);
    gtk_menu_popup(darktable.gui->presets_popup_menu, NULL, NULL, NULL, NULL, e->button, e->time);
    gtk_widget_show_all(GTK_WIDGET(darktable.gui->presets_popup_menu));
    return TRUE;
  }
  return FALSE;
}

static gboolean
_iop_plugin_header_button_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  if (e->button == 1)
  {
    /* handle shiftclick on expander, hide all except this */
    if ((e->state & GDK_SHIFT_MASK))
    {
      int current_group = dt_dev_modulegroups_get(module->dev);
      GList *iop = g_list_first(module->dev->iop);
      while (iop)
      {
        dt_iop_module_t *m = (dt_iop_module_t *)iop->data;
        uint32_t additional_flags=0;

        /* add special group flag for module in active pipe */
        if(module->enabled)
          additional_flags |= IOP_SPECIAL_GROUP_ACTIVE_PIPE;

        /* add special group flag for favorite */
        if(module->showhide && dtgtk_tristatebutton_get_state (DTGTK_TRISTATEBUTTON(module->showhide))==2)
          additional_flags |= IOP_SPECIAL_GROUP_USER_DEFINED;

        /* if module is the current, always expand it */
        if (m == module)
          dt_iop_gui_set_expanded(m, TRUE);
        else if ((current_group == DT_MODULEGROUP_NONE || dt_dev_modulegroups_test(module->dev, current_group, m->groups()|additional_flags)))
          dt_iop_gui_set_expanded(m, FALSE);

        iop = g_list_next(iop);
      }
    }
    else
    {
      /* else just toggle */
      dt_iop_gui_set_expanded(module, !module->expanded);
    }

    return TRUE;
  }
  else if (e->button == 3)
  {
    dt_gui_presets_popup_menu_show_for_module(module);
    gtk_menu_popup(darktable.gui->presets_popup_menu, NULL, NULL, NULL, NULL, e->button, e->time);
    gtk_widget_show_all(GTK_WIDGET(darktable.gui->presets_popup_menu));

    return TRUE;
  }
  return FALSE;
}

GtkWidget *dt_iop_gui_get_expander(dt_iop_module_t *module)
{
  int bs = 12;
  char tooltip[512];
  GtkWidget *expander = gtk_vbox_new(FALSE, 3);
  GtkWidget *header_evb = gtk_event_box_new();
  GtkWidget *header = gtk_hbox_new(FALSE, 0);
  GtkWidget *pluginui_frame = gtk_frame_new(NULL);
  GtkWidget *pluginui = gtk_event_box_new();

  gtk_widget_set_name(pluginui,"dt-plugin-ui");

  module->header = header;
  /* connect mouse button callbacks for focus and presets */
  g_signal_connect(G_OBJECT(pluginui), "button-press-event", G_CALLBACK(_iop_plugin_body_button_press), module);
  /* avoid scrolling with wheel, it's distracting (you'll end up over a control, and scroll it's value) */
  g_signal_connect(G_OBJECT(pluginui_frame), "scroll-event", G_CALLBACK(_iop_plugin_body_scrolled), module);
  g_signal_connect(G_OBJECT(pluginui), "scroll-event", G_CALLBACK(_iop_plugin_body_scrolled), module);
  g_signal_connect(G_OBJECT(header_evb), "scroll-event", G_CALLBACK(_iop_plugin_body_scrolled), module);
  g_signal_connect(G_OBJECT(expander), "scroll-event", G_CALLBACK(_iop_plugin_body_scrolled), module);
  g_signal_connect(G_OBJECT(header), "scroll-event", G_CALLBACK(_iop_plugin_body_scrolled), module);

  /* steup the header box */
  gtk_container_add(GTK_CONTAINER(header_evb), header);
  g_signal_connect(G_OBJECT(header_evb), "button-press-event", G_CALLBACK(_iop_plugin_header_button_press), module);

  /* setup plugin content frame */
  gtk_frame_set_shadow_type(GTK_FRAME(pluginui_frame),GTK_SHADOW_IN);
  gtk_container_add(GTK_CONTAINER(pluginui_frame),pluginui);

  /* layout the main expander widget */
  gtk_box_pack_start(GTK_BOX(expander), header_evb, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(expander), pluginui_frame, TRUE, FALSE,0);

  /*
   * initialize the header widgets
   */
  int idx = 0;
  GtkWidget *hw[6]={NULL,NULL,NULL,NULL,NULL,NULL};

  /* add the expand indicator icon */
  hw[idx] = dtgtk_icon_new(dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_LEFT);
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]),bs,bs);

  /* add module label */
  char label[128];
  g_snprintf(label,128,"<span size=\"larger\">%s</span>",module->name());
  hw[idx] = gtk_label_new("");
  gtk_label_set_markup(GTK_LABEL(hw[idx++]),label);

  /* add reset button */
  hw[idx] = dtgtk_button_new(dtgtk_cairo_paint_reset, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
  module->reset_button = GTK_WIDGET(hw[idx]);
  g_object_set(G_OBJECT(hw[idx]), "tooltip-text", _("reset parameters"), (char *)NULL);
  g_signal_connect (G_OBJECT (hw[idx]), "clicked",
		    G_CALLBACK (dt_iop_gui_reset_callback), module);
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]),bs,bs);


  /* add preset button if module has implementation */
  hw[idx] = dtgtk_button_new(dtgtk_cairo_paint_presets,CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
  module->presets_button = GTK_WIDGET(hw[idx]);
  g_object_set(G_OBJECT(hw[idx]), "tooltip-text", _("presets"), (char *)NULL);
  g_signal_connect (G_OBJECT (hw[idx]), "clicked", G_CALLBACK (popup_callback), module);
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]),bs,bs);

  /* add enabled button spacer */
  hw[idx] = gtk_fixed_new();
  gtk_widget_set_no_show_all(hw[idx],TRUE);
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]),bs,bs);

  /* add enabled button */
  hw[idx] = dtgtk_togglebutton_new(dtgtk_cairo_paint_switch, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
  gtk_widget_set_no_show_all(hw[idx],TRUE);
  snprintf(tooltip, 512, module->enabled ? _("%s is switched on") : _("%s is switched off"), module->name());
  g_object_set(G_OBJECT(hw[idx]), "tooltip-text", tooltip, (char *)NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hw[idx]), module->enabled);
  g_signal_connect (G_OBJECT (hw[idx]), "toggled",
		    G_CALLBACK (dt_iop_gui_off_callback), module);
  module->off = DTGTK_TOGGLEBUTTON(hw[idx]);
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]),bs,bs);


  /* reorder header, for now, iop are always int right panel */
  for(int i=5;i>=0;i--)
    if (hw[i])
      gtk_box_pack_start(GTK_BOX(header), hw[i],i==1?TRUE:FALSE,i==1?TRUE:FALSE,2);
  gtk_misc_set_alignment(GTK_MISC(hw[1]),1.0,0.5);
  dtgtk_icon_set_paint(hw[0], dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_LEFT);

  /* add the blending ui if supported */
  GtkWidget * iopw = gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(iopw), module->widget, TRUE, TRUE, 0);
  dt_iop_gui_init_blending(iopw, module);


  /* add module widget into an alignment */
  GtkWidget *al = gtk_alignment_new(1.0, 1.0, 1.0, 1.0);
  gtk_alignment_set_padding(GTK_ALIGNMENT(al), 8, 24, 8, 8);
  gtk_container_add(GTK_CONTAINER(pluginui), al);
  gtk_container_add(GTK_CONTAINER(al), iopw);

  gtk_widget_hide_all(pluginui);

  module->expander = expander;

  /* update header */
  _iop_gui_update_header(module);


  return module->expander;
}

GtkWidget *dt_iop_gui_get_widget(dt_iop_module_t *module)
{
  return gtk_bin_get_child(GTK_BIN(gtk_bin_get_child(GTK_BIN(g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(module->expander)),1)))));
}

GtkWidget *dt_iop_gui_get_pluginui(dt_iop_module_t *module)
{
  return gtk_bin_get_child(GTK_BIN(g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(module->expander)),1)));
}

int dt_iop_breakpoint(struct dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe)
{
  if(pipe != dev->preview_pipe) sched_yield();
  if(pipe != dev->preview_pipe && pipe->changed == DT_DEV_PIPE_ZOOMED) return 1;
  if((pipe->changed != DT_DEV_PIPE_UNCHANGED && pipe->changed != DT_DEV_PIPE_ZOOMED) || dev->gui_leaving) return 1;
  return 0;
}

void dt_iop_nap(uint32_t usec)
{
  // relinquish processor
  sched_yield();

  // additionally wait the given amount of time
  struct timeval s;
  s.tv_sec = 0;
  s.tv_usec = usec;
  select(0, NULL, NULL, NULL, &s);
}

void
dt_iop_flip_and_zoom_8(
    const uint8_t *in,
    int32_t iw,
    int32_t ih,
    uint8_t *out,
    int32_t ow,
    int32_t oh,
    const int32_t orientation,
    uint32_t *width,
    uint32_t *height)
{
  // init strides:
  const uint32_t iwd = (orientation & 4) ? ih : iw;
  const uint32_t iht = (orientation & 4) ? iw : ih;
  const float scale = fmaxf(iwd/(float)ow, iht/(float)oh);
  const uint32_t wd = *width  = MIN(ow, iwd/scale);
  const uint32_t ht = *height = MIN(oh, iht/scale);
  const int bpp = 4; // bytes per pixel
  int32_t ii = 0, jj = 0;
  int32_t si = 1, sj = iw;
  if(orientation & 2)
  {
    jj = ih - jj - 1;
    sj = -sj;
  }
  if(orientation & 1)
  {
    ii = iw - ii - 1;
    si = -si;
  }
  if(orientation & 4)
  {
    int t = sj;
    sj = si;
    si = t;
  }
  const int32_t half_pixel = .5f*scale;
  const int32_t offm = half_pixel*bpp*MIN(MIN(0, si), MIN(sj, si+sj));
  const int32_t offM = half_pixel*bpp*MAX(MAX(0, si), MAX(sj, si+sj));
#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(in, out, jj, ii, sj, si, iw, ih)
#endif
  for(int j=0; j<ht; j++)
  {
    uint8_t *out2 = out + bpp*wd*j;
    const uint8_t *in2 = in + bpp*(iw*jj + ii + sj*(int32_t)(scale*j));
    float stepi = 0.0f;
    for(int i=0; i<wd; i++)
    {
      const uint8_t *in3 = in2 + ((int32_t)stepi)*si*bpp;
      // this should always be within the bounds of in[], due to the way
      // wd/ht are constructed by always just rounding down. half_pixel should never
      // add up to one pixel difference.
      // we have this check with the hope the branch predictor will get rid of it:
      if(in3 + offm >= in &&
         in3 + offM < in + bpp*iw*ih)
      {
        for(int k=0; k<3; k++) out2[k] = // in3[2-k];
          CLAMP(((int32_t)in3[bpp*half_pixel*sj      + 2-k] +
                 (int32_t)in3[bpp*half_pixel*(si+sj) + 2-k] +
                 (int32_t)in3[bpp*half_pixel*si      + 2-k] +
                 (int32_t)in3[2-k])/4, 0, 255);
      }
      out2  += bpp;
      stepi += scale;
    }
  }
}

void dt_iop_clip_and_zoom_8(const uint8_t *i, int32_t ix, int32_t iy, int32_t iw, int32_t ih, int32_t ibw, int32_t ibh,
                            uint8_t *o, int32_t ox, int32_t oy, int32_t ow, int32_t oh, int32_t obw, int32_t obh)
{
  const float scalex = iw/(float)ow;
  const float scaley = ih/(float)oh;
  int32_t ix2 = MAX(ix, 0);
  int32_t iy2 = MAX(iy, 0);
  int32_t ox2 = MAX(ox, 0);
  int32_t oy2 = MAX(oy, 0);
  int32_t oh2 = MIN(MIN(oh, (ibh - iy2)/scaley), obh - oy2);
  int32_t ow2 = MIN(MIN(ow, (ibw - ix2)/scalex), obw - ox2);
  assert((int)(ix2 + ow2*scalex) <= ibw);
  assert((int)(iy2 + oh2*scaley) <= ibh);
  assert(ox2 + ow2 <= obw);
  assert(oy2 + oh2 <= obh);
  assert(ix2 >= 0 && iy2 >= 0 && ox2 >= 0 && oy2 >= 0);
  float x = ix2, y = iy2;
  for(int s=0; s<oh2; s++)
  {
    int idx = ox2 + obw*(oy2+s);
    for(int t=0; t<ow2; t++)
    {
      for(int k=0; k<3; k++) o[4*idx + k] = //i[3*(ibw* (int)y +             (int)x             ) + k)];
          CLAMP(((int32_t)i[(4*(ibw*(int32_t) y +            (int32_t) (x + .5f*scalex)) + k)] +
                 (int32_t)i[(4*(ibw*(int32_t)(y+.5f*scaley) +(int32_t) (x + .5f*scalex)) + k)] +
                 (int32_t)i[(4*(ibw*(int32_t)(y+.5f*scaley) +(int32_t) (x             )) + k)] +
                 (int32_t)i[(4*(ibw*(int32_t) y +            (int32_t) (x             )) + k)])/4, 0, 255);
      x += scalex;
      idx++;
    }
    y += scaley;
    x = ix2;
  }
}

void
dt_iop_clip_and_zoom(float *out, const float *const in,
                     const dt_iop_roi_t *const roi_out, const dt_iop_roi_t * const roi_in, const int32_t out_stride, const int32_t in_stride)
{
  const struct dt_interpolation* itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  dt_interpolation_resample(itor, out, roi_out, out_stride*4*sizeof(float), in, roi_in, in_stride*4*sizeof(float));
}

static int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> ((((row) << 1 & 14) + ((col) & 1)) << 1) & 3;
}

/**
 * downscales and clips a mosaiced buffer (in) to the given region of interest (r_*)
 * and writes it to out in float4 format.
 * filters is the dcraw supplied int encoding of the bayer pattern, flipped with the buffer.
 * resamping is done via bilateral filtering and respecting the input mosaic pattern.
 */
void
dt_iop_clip_and_zoom_demosaic_half_size(
    float *out,
    const uint16_t *const in,
    const dt_iop_roi_t *const roi_out,
    const dt_iop_roi_t *const roi_in,
    const int32_t out_stride,
    const int32_t in_stride,
    const unsigned int filters)
{
#if 0
  printf("scale: %f\n",roi_out->scale);
  struct timeval tm1,tm2;
  gettimeofday(&tm1,NULL);
  for (int k = 0 ; k < 100 ; k++)
  {
#endif
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = 1.f/roi_out->scale;
  // how many 2x2 blocks can be sampled inside that area
  const int samples = round(px_footprint/2);

  // move p to point to an rggb block:
  int trggbx = 0, trggby = 0;
  if(FC(trggby, trggbx+1, filters) != 1) trggbx ++;
  if(FC(trggby, trggbx,   filters) != 0)
  {
    trggbx = (trggbx + 1)&1;
    trggby ++;
  }
  const int rggbx = trggbx, rggby = trggby;

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out) schedule(static)
#endif
  for(int y=0; y<roi_out->height; y++)
  {
    float *outc = out + 4*(out_stride*y);

    float fy = (y + roi_out->y)*px_footprint;
    int py = (int)fy & ~1;
    py = MIN(((roi_in->height-4) & ~1u), py) + rggby;

    int maxj = MIN(((roi_in->height-3)&~1u)+rggby, py+2*samples);

    float fx = roi_out->x*px_footprint;

    for(int x=0; x<roi_out->width; x++)
    {
      __m128 col = _mm_setzero_ps();

      fx += px_footprint;
      int px = (int)fx & ~1;
      px = MIN(((roi_in->width -4) & ~1u), px) + rggbx;

      int maxi = MIN(((roi_in->width -3)&~1u)+rggbx, px+2*samples);

      int num = 0;

      const int idx = px + in_stride*py;
      const uint16_t pc = MAX(MAX(in[idx], in[idx+1]), MAX(in[idx + in_stride], in[idx+1 + in_stride]));

      // 2x2 blocks in the middle of sampling region
      __m128i sum = _mm_set_epi32(0,0,0,0);

      for(int j=py; j<=maxj; j+=2)
        for(int i=px; i<=maxi; i+=2)
        {
          const uint16_t p1 = in[i   + in_stride*j];
          const uint16_t p2 = in[i+1 + in_stride*j];
          const uint16_t p3 = in[i   + in_stride*(j + 1)];
          const uint16_t p4 = in[i+1 + in_stride*(j + 1)];

          if (!((pc >= 60000) ^ (MAX(MAX(p1,p2),MAX(p3,p4)) >= 60000)))
          {
            sum = _mm_add_epi32(sum, _mm_set_epi32(0,p4,p3+p2,p1));
            num++;
          }
        }

      col = _mm_mul_ps(_mm_cvtepi32_ps(sum), _mm_div_ps(_mm_set_ps(0.0f,1.0f/65535.0f,0.5f/65535.0f,1.0f/65535.0f),_mm_set1_ps(num)));
      _mm_stream_ps(outc, col);
      outc += 4;
    }
  }
  _mm_sfence();
#if 0
  }
  gettimeofday(&tm2,NULL);
  float perf = (tm2.tv_sec-tm1.tv_sec)*1000.0f + (tm2.tv_usec-tm1.tv_usec)/1000.0f;
  printf("time spent: %.4f\n",perf/100.0f);
#endif
}

#if 0 // gets rid of pink artifacts, but doesn't do sub-pixel sampling, so shows some staircasing artifacts.
void
dt_iop_clip_and_zoom_demosaic_half_size_f(
    float *out,
    const float *const in,
    const dt_iop_roi_t *const roi_out,
    const dt_iop_roi_t *const roi_in,
    const int32_t out_stride,
    const int32_t in_stride,
    const unsigned int filters,
    const float clip)
{
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = 1.f/roi_out->scale;
  // how many 2x2 blocks can be sampled inside that area
  const int samples = round(px_footprint/2);

  // move p to point to an rggb block:
  int trggbx = 0, trggby = 0;
  if(FC(trggby, trggbx+1, filters) != 1) trggbx ++;
  if(FC(trggby, trggbx,   filters) != 0)
  {
    trggbx = (trggbx + 1)&1;
    trggby ++;
  }
  const int rggbx = trggbx, rggby = trggby;

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out) schedule(static)
#endif
  for(int y=0; y<roi_out->height; y++)
  {
    float *outc = out + 4*(out_stride*y);

    float fy = (y + roi_out->y)*px_footprint;
    int py = (int)fy & ~1;
    py = MIN(((roi_in->height-4) & ~1u), py) + rggby;

    int maxj = MIN(((roi_in->height-3)&~1u)+rggby, py+2*samples);

    float fx = roi_out->x*px_footprint;

    for(int x=0; x<roi_out->width; x++)
    {
      __m128 col = _mm_setzero_ps();

      fx += px_footprint;
      int px = (int)fx & ~1;
      px = MIN(((roi_in->width -4) & ~1u), px) + rggbx;

      int maxi = MIN(((roi_in->width -3)&~1u)+rggbx, px+2*samples);

      int num = 0;

      const int idx = px + in_stride*py;
      const float pc = MAX(MAX(in[idx], in[idx+1]), MAX(in[idx + in_stride], in[idx+1 + in_stride]));

      // 2x2 blocks in the middle of sampling region
      __m128 sum = _mm_setzero_ps();

      for(int j=py; j<=maxj; j+=2)
        for(int i=px; i<=maxi; i+=2)
        {
          const float p1 = in[i   + in_stride*j];
          const float p2 = in[i+1 + in_stride*j];
          const float p3 = in[i   + in_stride*(j + 1)];
          const float p4 = in[i+1 + in_stride*(j + 1)];

          if (!((pc >= clip) ^ (MAX(MAX(p1,p2),MAX(p3,p4)) >= clip)))
          {
            sum = _mm_add_ps(sum, _mm_set_ps(0,p4,p3+p2,p1));
            num++;
          }
        }

      col = _mm_mul_ps(sum, _mm_div_ps(_mm_set_ps(0.0f,1.0f,0.5f,1.0f),_mm_set1_ps(num)));
      _mm_stream_ps(outc, col);
      outc += 4;
    }
  }
  _mm_sfence();
}

#else // very fast and smooth, but doesn't handle highlights:
void
dt_iop_clip_and_zoom_demosaic_half_size_f(
    float *out,
    const float *const in,
    const dt_iop_roi_t *const roi_out,
    const dt_iop_roi_t *const roi_in,
    const int32_t out_stride,
    const int32_t in_stride,
    const unsigned int filters,
    const float clip)
{
  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = 1.f/roi_out->scale;
  // how many 2x2 blocks can be sampled inside that area
  const int samples = round(px_footprint/2);

  // move p to point to an rggb block:
  int trggbx = 0, trggby = 0;
  if(FC(trggby, trggbx+1, filters) != 1) trggbx ++;
  if(FC(trggby, trggbx,   filters) != 0)
  {
    trggbx = (trggbx + 1)&1;
    trggby ++;
  }
  const int rggbx = trggbx, rggby = trggby;

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out) schedule(static)
#endif
  for(int y=0; y<roi_out->height; y++)
  {
    float *outc = out + 4*(out_stride*y);

    float fy = (y + roi_out->y)*px_footprint;
    int py = (int)fy & ~1;
    const float dy = (fy - py)/2;
    py = MIN(((roi_in->height-6) & ~1u), py) + rggby;

    int maxj = MIN(((roi_in->height-5)&~1u)+rggby, py+2*samples);

    for(int x=0; x<roi_out->width; x++)
    {
      __m128 col = _mm_setzero_ps();

      float fx = (x + roi_out->x)*px_footprint;
      int px = (int)fx & ~1;
      const float dx = (fx - px)/2;
      px = MIN(((roi_in->width -6) & ~1u), px) + rggbx;

      int maxi = MIN(((roi_in->width -5)&~1u)+rggbx, px+2*samples);

      float p1, p2, p4;
      int i,j;
      float num = 0;

      // upper left 2x2 block of sampling region
      p1 = in[px   + in_stride*py];
      p2 = in[px+1 + in_stride*py] + in[px   + in_stride*(py + 1)];
      p4 = in[px+1 + in_stride*(py + 1)];
      col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1-dx)*(1-dy)),_mm_set_ps(0.0f, p4, p2, p1)));

      // left 2x2 block border of sampling region
      for (j = py+2 ; j <= maxj ; j+=2)
      {
          p1 = in[px   + in_stride*j];
          p2 = in[px+1 + in_stride*j] + in[px   + in_stride*(j + 1)];
          p4 = in[px+1 + in_stride*(j + 1)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(1-dx),_mm_set_ps(0.0f, p4, p2, p1)));
      }

      // upper 2x2 block border of sampling region
      for (i = px+2 ; i <= maxi ; i+=2)
      {
          p1 = in[i   + in_stride*py];
          p2 = in[i+1 + in_stride*py] + in[i   + in_stride*(py + 1)];
          p4 = in[i+1 + in_stride*(py + 1)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(1-dy),_mm_set_ps(0.0f, p4, p2, p1)));
      }

      // 2x2 blocks in the middle of sampling region
      for(int j=py+2; j<=maxj; j+=2)
        for(int i=px+2; i<=maxi; i+=2)
        {
          p1 = in[i   + in_stride*j];
          p2 = in[i+1 + in_stride*j] + in[i   + in_stride*(j + 1)];
          p4 = in[i+1 + in_stride*(j + 1)];
          col = _mm_add_ps(col, _mm_set_ps(0.0f, p4, p2, p1));
        }

      if (maxi == px + 2*samples && maxj == py + 2*samples)
      {
        // right border
        for (j = py+2 ; j <= maxj ; j+=2)
        {
          p1 = in[maxi+2   + in_stride*j];
          p2 = in[maxi+3 + in_stride*j] + in[maxi+2   + in_stride*(j + 1)];
          p4 = in[maxi+3 + in_stride*(j + 1)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx),_mm_set_ps(0.0f, p4, p2, p1)));
        }

        // upper right
        p1 = in[maxi+2   + in_stride*py];
        p2 = in[maxi+3 + in_stride*py] + in[maxi+2   + in_stride*(py + 1)];
        p4 = in[maxi+3 + in_stride*(py + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx*(1-dy)),_mm_set_ps(0.0f, p4, p2, p1)));

        // lower border
        for (i = px+2 ; i <= maxi ; i+=2)
        {
          p1 = in[i   + in_stride*(maxj+2)];
          p2 = in[i+1 + in_stride*(maxj+2)] + in[i   + in_stride*(maxj+3)];
          p4 = in[i+1 + in_stride*(maxj+3)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dy),_mm_set_ps(0.0f, p4, p2, p1)));
        }

        // lower left 2x2 block
        p1 = in[px   + in_stride*(maxj+2)];
        p2 = in[px+1 + in_stride*(maxj+2)] + in[px   + in_stride*(maxj+3)];
        p4 = in[px+1 + in_stride*(maxj+3)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1-dx)*dy),_mm_set_ps(0.0f, p4, p2, p1)));

        // lower right 2x2 block
        p1 = in[maxi+2   + in_stride*(maxj+2)];
        p2 = in[maxi+3 + in_stride*(maxj+2)] + in[maxi   + in_stride*(maxj+3)];
        p4 = in[maxi+3 + in_stride*(maxj+3)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx*dy),_mm_set_ps(0.0f, p4, p2, p1)));

        num = (samples+1)*(samples+1);
      }
      else if (maxi == px + 2*samples)
      {
        // right border
        for (j = py+2 ; j <= maxj ; j+=2)
        {
          p1 = in[maxi+2   + in_stride*j];
          p2 = in[maxi+3 + in_stride*j] + in[maxi+2   + in_stride*(j + 1)];
          p4 = in[maxi+3 + in_stride*(j + 1)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx),_mm_set_ps(0.0f, p4, p2, p1)));
        }

        // upper right
        p1 = in[maxi+2   + in_stride*py];
        p2 = in[maxi+3 + in_stride*py] + in[maxi+2   + in_stride*(py + 1)];
        p4 = in[maxi+3 + in_stride*(py + 1)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dx*(1-dy)),_mm_set_ps(0.0f, p4, p2, p1)));

        num = ((maxj-py)/2+1-dy)*(samples+1);
      }
      else if (maxj == py + 2*samples)
      {
        // lower border
        for (i = px+2 ; i <= maxi ; i+=2)
        {
          p1 = in[i   + in_stride*(maxj+2)];
          p2 = in[i+1 + in_stride*(maxj+2)] + in[i   + in_stride*(maxj+3)];
          p4 = in[i+1 + in_stride*(maxj+3)];
          col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps(dy),_mm_set_ps(0.0f, p4, p2, p1)));
        }

        // lower left 2x2 block
        p1 = in[px   + in_stride*(maxj+2)];
        p2 = in[px+1 + in_stride*(maxj+2)] + in[px   + in_stride*(maxj+3)];
        p4 = in[px+1 + in_stride*(maxj+3)];
        col = _mm_add_ps(col, _mm_mul_ps(_mm_set1_ps((1-dx)*dy),_mm_set_ps(0.0f, p4, p2, p1)));

        num = ((maxi-px)/2+1-dx)*(samples+1);
      }
      else
      {
        num = ((maxi-px)/2+1-dx)*((maxj-py)/2+1-dy);
      }

      num = 1.0f/num;
      col = _mm_mul_ps(col, _mm_set_ps(0.0f,num,0.5f*num,num));
      _mm_stream_ps(outc, col);
      outc += 4;
    }
  }
  _mm_sfence();
}
#endif

void dt_iop_RGB_to_YCbCr(const float *rgb, float *yuv)
{
  yuv[0] =  0.299*rgb[0] + 0.587*rgb[1] + 0.114*rgb[2];
  yuv[1] = -0.147*rgb[0] - 0.289*rgb[1] + 0.437*rgb[2];
  yuv[2] =  0.615*rgb[0] - 0.515*rgb[1] - 0.100*rgb[2];
}

void dt_iop_YCbCr_to_RGB(const float *yuv, float *rgb)
{
  rgb[0] = yuv[0]                + 1.140*yuv[2];
  rgb[1] = yuv[0] - 0.394*yuv[1] - 0.581*yuv[2];
  rgb[2] = yuv[0] + 2.028*yuv[1];
}

dt_iop_module_t *get_colorout_module()
{
  GList *modules = darktable.develop->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    if(!strcmp(module->op, "colorout")) return module;
    modules = g_list_next(modules);
  }
  return NULL;
}

static inline void
mat4inv(const float X[][4], float R[][4])
{
  const float det =
          X[0][3] * X[1][2] * X[2][1] * X[3][0]
        - X[0][2] * X[1][3] * X[2][1] * X[3][0]
        - X[0][3] * X[1][1] * X[2][2] * X[3][0]
        + X[0][1] * X[1][3] * X[2][2] * X[3][0]
        + X[0][2] * X[1][1] * X[2][3] * X[3][0]
        - X[0][1] * X[1][2] * X[2][3] * X[3][0]
        - X[0][3] * X[1][2] * X[2][0] * X[3][1]
        + X[0][2] * X[1][3] * X[2][0] * X[3][1]
        + X[0][3] * X[1][0] * X[2][2] * X[3][1]
        - X[0][0] * X[1][3] * X[2][2] * X[3][1]
        - X[0][2] * X[1][0] * X[2][3] * X[3][1]
        + X[0][0] * X[1][2] * X[2][3] * X[3][1]
        + X[0][3] * X[1][1] * X[2][0] * X[3][2]
        - X[0][1] * X[1][3] * X[2][0] * X[3][2]
        - X[0][3] * X[1][0] * X[2][1] * X[3][2]
        + X[0][0] * X[1][3] * X[2][1] * X[3][2]
        + X[0][1] * X[1][0] * X[2][3] * X[3][2]
        - X[0][0] * X[1][1] * X[2][3] * X[3][2]
        - X[0][2] * X[1][1] * X[2][0] * X[3][3]
        + X[0][1] * X[1][2] * X[2][0] * X[3][3]
        + X[0][2] * X[1][0] * X[2][1] * X[3][3]
        - X[0][0] * X[1][2] * X[2][1] * X[3][3]
        - X[0][1] * X[1][0] * X[2][2] * X[3][3]
        + X[0][0] * X[1][1] * X[2][2] * X[3][3];
  R[0][0] = ( X[1][2]*X[2][3]*X[3][1] - X[1][3]*X[2][2]*X[3][1] + X[1][3]*X[2][1]*X[3][2] - X[1][1]*X[2][3]*X[3][2] - X[1][2]*X[2][1]*X[3][3] + X[1][1]*X[2][2]*X[3][3] ) / det;
  R[1][0] = ( X[1][3]*X[2][2]*X[3][0] - X[1][2]*X[2][3]*X[3][0] - X[1][3]*X[2][0]*X[3][2] + X[1][0]*X[2][3]*X[3][2] + X[1][2]*X[2][0]*X[3][3] - X[1][0]*X[2][2]*X[3][3] ) / det;
  R[2][0] = ( X[1][1]*X[2][3]*X[3][0] - X[1][3]*X[2][1]*X[3][0] + X[1][3]*X[2][0]*X[3][1] - X[1][0]*X[2][3]*X[3][1] - X[1][1]*X[2][0]*X[3][3] + X[1][0]*X[2][1]*X[3][3] ) / det;
  R[3][0] = ( X[1][2]*X[2][1]*X[3][0] - X[1][1]*X[2][2]*X[3][0] - X[1][2]*X[2][0]*X[3][1] + X[1][0]*X[2][2]*X[3][1] + X[1][1]*X[2][0]*X[3][2] - X[1][0]*X[2][1]*X[3][2] ) / det;

  R[0][1] = ( X[0][3]*X[2][2]*X[3][1] - X[0][2]*X[2][3]*X[3][1] - X[0][3]*X[2][1]*X[3][2] + X[0][1]*X[2][3]*X[3][2] + X[0][2]*X[2][1]*X[3][3] - X[0][1]*X[2][2]*X[3][3] ) / det;
  R[1][1] = ( X[0][2]*X[2][3]*X[3][0] - X[0][3]*X[2][2]*X[3][0] + X[0][3]*X[2][0]*X[3][2] - X[0][0]*X[2][3]*X[3][2] - X[0][2]*X[2][0]*X[3][3] + X[0][0]*X[2][2]*X[3][3] ) / det;
  R[2][1] = ( X[0][3]*X[2][1]*X[3][0] - X[0][1]*X[2][3]*X[3][0] - X[0][3]*X[2][0]*X[3][1] + X[0][0]*X[2][3]*X[3][1] + X[0][1]*X[2][0]*X[3][3] - X[0][0]*X[2][1]*X[3][3] ) / det;
  R[3][1] = ( X[0][1]*X[2][2]*X[3][0] - X[0][2]*X[2][1]*X[3][0] + X[0][2]*X[2][0]*X[3][1] - X[0][0]*X[2][2]*X[3][1] - X[0][1]*X[2][0]*X[3][2] + X[0][0]*X[2][1]*X[3][2] ) / det;

  R[0][2] = ( X[0][2]*X[1][3]*X[3][1] - X[0][3]*X[1][2]*X[3][1] + X[0][3]*X[1][1]*X[3][2] - X[0][1]*X[1][3]*X[3][2] - X[0][2]*X[1][1]*X[3][3] + X[0][1]*X[1][2]*X[3][3] ) / det;
  R[1][2] = ( X[0][3]*X[1][2]*X[3][0] - X[0][2]*X[1][3]*X[3][0] - X[0][3]*X[1][0]*X[3][2] + X[0][0]*X[1][3]*X[3][2] + X[0][2]*X[1][0]*X[3][3] - X[0][0]*X[1][2]*X[3][3] ) / det;
  R[2][2] = ( X[0][1]*X[1][3]*X[3][0] - X[0][3]*X[1][1]*X[3][0] + X[0][3]*X[1][0]*X[3][1] - X[0][0]*X[1][3]*X[3][1] - X[0][1]*X[1][0]*X[3][3] + X[0][0]*X[1][1]*X[3][3] ) / det;
  R[3][2] = ( X[0][2]*X[1][1]*X[3][0] - X[0][1]*X[1][2]*X[3][0] - X[0][2]*X[1][0]*X[3][1] + X[0][0]*X[1][2]*X[3][1] + X[0][1]*X[1][0]*X[3][2] - X[0][0]*X[1][1]*X[3][2] ) / det;

  R[0][3] = ( X[0][3]*X[1][2]*X[2][1] - X[0][2]*X[1][3]*X[2][1] - X[0][3]*X[1][1]*X[2][2] + X[0][1]*X[1][3]*X[2][2] + X[0][2]*X[1][1]*X[2][3] - X[0][1]*X[1][2]*X[2][3] ) / det;
  R[1][3] = ( X[0][2]*X[1][3]*X[2][0] - X[0][3]*X[1][2]*X[2][0] + X[0][3]*X[1][0]*X[2][2] - X[0][0]*X[1][3]*X[2][2] - X[0][2]*X[1][0]*X[2][3] + X[0][0]*X[1][2]*X[2][3] ) / det;
  R[2][3] = ( X[0][3]*X[1][1]*X[2][0] - X[0][1]*X[1][3]*X[2][0] - X[0][3]*X[1][0]*X[2][1] + X[0][0]*X[1][3]*X[2][1] + X[0][1]*X[1][0]*X[2][3] - X[0][0]*X[1][1]*X[2][3] ) / det;
  R[3][3] = ( X[0][1]*X[1][2]*X[2][0] - X[0][2]*X[1][1]*X[2][0] + X[0][2]*X[1][0]*X[2][1] - X[0][0]*X[1][2]*X[2][1] - X[0][1]*X[1][0]*X[2][2] + X[0][0]*X[1][1]*X[2][2] ) / det;
}

static void
mat4mulv (float *dst, float mat[][4], const float *const v)
{
  for(int k=0; k<4; k++)
  {
    float x=0.0f;
    for(int i=0; i<4; i++) x += mat[k][i] * v[i];
    dst[k] = x;
  }
}

void dt_iop_estimate_cubic(const float *const x, const float *const y, float *a)
{
  // we want to fit a spline
  // [y]   [x^3 x^2 x^1 1] [a^3]
  // |y| = |x^3 x^2 x^1 1| |a^2|
  // |y|   |x^3 x^2 x^1 1| |a^1|
  // [y]   [x^3 x^2 x^1 1] [ 1 ]
  // and do that by inverting the matrix X:

  const float X[4][4] = {{x[0]*x[0]*x[0], x[0]*x[0], x[0], 1.0f},
                         {x[1]*x[1]*x[1], x[1]*x[1], x[1], 1.0f},
                         {x[2]*x[2]*x[2], x[2]*x[2], x[2], 1.0f},
                         {x[3]*x[3]*x[3], x[3]*x[3], x[3], 1.0f}};
  float X_inv[4][4];
  mat4inv(X, X_inv);
  mat4mulv(a, X_inv, y);
}

static gboolean show_module_callback(GtkAccelGroup *accel_group,
                                 GObject *acceleratable,
                                 guint keyval, GdkModifierType modifier,
                                 gpointer data)

{
  dt_iop_module_t *module = (dt_iop_module_t*)data;

  // Showing the module, if it isn't already visible
  if(!dtgtk_tristatebutton_get_state(DTGTK_TRISTATEBUTTON(module->showhide)))
  {
    dtgtk_tristatebutton_set_state(DTGTK_TRISTATEBUTTON(module->showhide), 1);
    gtk_widget_queue_draw(module->showhide);
  }

  // FIXME
  //dt_gui_iop_modulegroups_switch(module->groups());
  dt_iop_gui_set_expanded(module, TRUE);
  dt_iop_request_focus(module);
  return TRUE;
}

static gboolean enable_module_callback(GtkAccelGroup *accel_group,
                                   GObject *acceleratable,
                                   guint keyval, GdkModifierType modifier,
                                   gpointer data)

{
  dt_iop_module_t *module = (dt_iop_module_t*)data;
  gboolean active= gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(module->off));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), !active);
  return TRUE;
}



void dt_iop_connect_common_accels(dt_iop_module_t *module)
{

  GClosure* closure = NULL;
  if(module->flags() & IOP_FLAGS_DEPRECATED) return;
  // Connecting the (optional) module show accelerator
  closure = g_cclosure_new(G_CALLBACK(show_module_callback),
                           module, NULL);
  dt_accel_connect_iop(module, "show module", closure);

  // Connecting the (optional) module switch accelerator
  closure = g_cclosure_new(G_CALLBACK(enable_module_callback),
                           module, NULL);
  dt_accel_connect_iop(module, "enable module", closure);

  // Connecting the reset and preset buttons
  if(module->reset_button)
    dt_accel_connect_button_iop(module, "reset module parameters",
                                module->reset_button);
  if(module->presets_button)
    dt_accel_connect_button_iop(module, "show preset menu",
                                module->presets_button);

  if(module->fusion_slider)
    dt_accel_connect_slider_iop(module, "fusion", module->fusion_slider);

  sqlite3_stmt *stmt;
  // don't know for which image. show all we got:
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select name from presets where operation=?1 order by writeprotect desc, rowid", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
	dt_accel_connect_preset_iop(module,(char *)sqlite3_column_text(stmt, 0));
  }
  sqlite3_finalize(stmt);
}

gchar *
dt_iop_get_localized_name(const gchar * op)
{
  // Prepare mapping op -> localized name
  static GHashTable *module_names = NULL;
  if(module_names == NULL)
  {
    module_names = g_hash_table_new(g_str_hash, g_str_equal);
    GList *iop = g_list_first(darktable.iop);
    if(iop != NULL)
    {
      do
      {
        dt_iop_module_so_t * module = (dt_iop_module_so_t *)iop->data;
        g_hash_table_insert(module_names, module->op, _(module->name()));
      }
      while((iop=g_list_next(iop)) != NULL);
    }
  }

  return (gchar*)g_hash_table_lookup(module_names, op);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
