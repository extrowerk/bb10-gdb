/* Python frame unwinder interface.

   Copyright (C) 2015-2018 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "arch-utils.h"
#include "frame-unwind.h"
#include "gdb_obstack.h"
#include "gdbcmd.h"
#include "language.h"
#include "observable.h"
#include "python-internal.h"
#include "regcache.h"
#include "valprint.h"
#include "user-regs.h"
#include "py-ref.h"

#define TRACE_PY_UNWIND(level, args...) if (pyuw_debug >= level)  \
  { fprintf_unfiltered (gdb_stdlog, args); }

typedef struct
{
  PyObject_HEAD

  /* Frame we are unwinding.  */
  struct frame_info *frame_info;

  /* Its architecture, passed by the sniffer caller.  */
  struct gdbarch *gdbarch;
} pending_frame_object;

/* Saved registers array item.  */

struct saved_reg
{
  saved_reg (int n, gdbpy_ref<> &&v)
    : number (n),
      value (std::move (v))
  {
  }

  int number;
  gdbpy_ref<> value;
};

/* The data we keep for the PyUnwindInfo: pending_frame, saved registers
   and frame ID.  */

typedef struct
{
  PyObject_HEAD

  /* gdb.PendingFrame for the frame we are unwinding.  */
  PyObject *pending_frame;

  /* Its ID.  */
  struct frame_id frame_id;

  /* Saved registers array.  */
  std::vector<saved_reg> *saved_regs;
} unwind_info_object;

/* The data we keep for a frame we can unwind: frame ID and an array of
   (register_number, register_value) pairs.  */

typedef struct
{
  /* Frame ID.  */
  struct frame_id frame_id;

  /* GDB Architecture.  */
  struct gdbarch *gdbarch;

  /* Length of the `reg' array below.  */
  int reg_count;

  cached_reg_t reg[];
} cached_frame_info;

extern PyTypeObject pending_frame_object_type
    CPYCHECKER_TYPE_OBJECT_FOR_TYPEDEF ("pending_frame_object");

extern PyTypeObject unwind_info_object_type
    CPYCHECKER_TYPE_OBJECT_FOR_TYPEDEF ("unwind_info_object");

static unsigned int pyuw_debug = 0;

static struct gdbarch_data *pyuw_gdbarch_data;

/* Parses register id, which can be either a number or a name.
   Returns 1 on success, 0 otherwise.  */

static int
pyuw_parse_register_id (struct gdbarch *gdbarch, PyObject *pyo_reg_id,
                        int *reg_num)
{
  if (pyo_reg_id == NULL)
    return 0;
  if (gdbpy_is_string (pyo_reg_id))
    {
      gdb::unique_xmalloc_ptr<char> reg_name (gdbpy_obj_to_string (pyo_reg_id));

      if (reg_name == NULL)
        return 0;
      *reg_num = user_reg_map_name_to_regnum (gdbarch, reg_name.get (),
                                              strlen (reg_name.get ()));
      return *reg_num >= 0;
    }
  else if (PyInt_Check (pyo_reg_id))
    {
      long value;
      if (gdb_py_int_as_long (pyo_reg_id, &value) && (int) value == value)
        {
          *reg_num = (int) value;
          return user_reg_map_regnum_to_name (gdbarch, *reg_num) != NULL;
        }
    }
  return 0;
}

/* Convert gdb.Value instance to inferior's pointer.  Return 1 on success,
   0 on failure.  */

static int
pyuw_value_obj_to_pointer (PyObject *pyo_value, CORE_ADDR *addr)
{
  int rc = 0;
  struct value *value;

  TRY
    {
      if ((value = value_object_to_value (pyo_value)) != NULL)
        {
          *addr = unpack_pointer (value_type (value),
                                  value_contents (value));
          rc = 1;
        }
    }
  CATCH (except, RETURN_MASK_ALL)
    {
      gdbpy_convert_exception (except);
    }
  END_CATCH
  return rc;
}

/* Get attribute from an object and convert it to the inferior's
   pointer value.  Return 1 if attribute exists and its value can be
   converted.  Otherwise, if attribute does not exist or its value is
   None, return 0.  In all other cases set Python error and return
   0.  */

static int
pyuw_object_attribute_to_pointer (PyObject *pyo, const char *attr_name,
                                  CORE_ADDR *addr)
{
  int rc = 0;

  if (PyObject_HasAttrString (pyo, attr_name))
    {
      gdbpy_ref<> pyo_value (PyObject_GetAttrString (pyo, attr_name));

      if (pyo_value != NULL && pyo_value != Py_None)
        {
          rc = pyuw_value_obj_to_pointer (pyo_value.get (), addr);
          if (!rc)
            PyErr_Format (
                PyExc_ValueError,
                _("The value of the '%s' attribute is not a pointer."),
                attr_name);
        }
    }
  return rc;
}

/* Called by the Python interpreter to obtain string representation
   of the UnwindInfo object.  */

static PyObject *
unwind_infopy_str (PyObject *self)
{
  unwind_info_object *unwind_info = (unwind_info_object *) self;
  string_file stb;

  stb.puts ("Frame ID: ");
  fprint_frame_id (&stb, unwind_info->frame_id);
  {
    const char *sep = "";
    struct value_print_options opts;

    get_user_print_options (&opts);
    stb.printf ("\nSaved registers: (");
    for (const saved_reg &reg : *unwind_info->saved_regs)
      {
        struct value *value = value_object_to_value (reg.value.get ());

        stb.printf ("%s(%d, ", sep, reg.number);
        if (value != NULL)
          {
            TRY
              {
                value_print (value, &stb, &opts);
                stb.puts (")");
              }
            CATCH (except, RETURN_MASK_ALL)
              {
                GDB_PY_HANDLE_EXCEPTION (except);
              }
            END_CATCH
          }
        else
          stb.puts ("<BAD>)");
        sep = ", ";
      }
    stb.puts (")");
  }

  return PyString_FromString (stb.c_str ());
}

/* Create UnwindInfo instance for given PendingFrame and frame ID.
   Sets Python error and returns NULL on error.  */

static PyObject *
pyuw_create_unwind_info (PyObject *pyo_pending_frame,
                         struct frame_id frame_id)
{
  unwind_info_object *unwind_info
      = PyObject_New (unwind_info_object, &unwind_info_object_type);

  if (((pending_frame_object *) pyo_pending_frame)->frame_info == NULL)
    {
      PyErr_SetString (PyExc_ValueError,
                       "Attempting to use stale PendingFrame");
      return NULL;
    }
  unwind_info->frame_id = frame_id;
  Py_INCREF (pyo_pending_frame);
  unwind_info->pending_frame = pyo_pending_frame;
  unwind_info->saved_regs = new std::vector<saved_reg>;
  return (PyObject *) unwind_info;
}

/* The implementation of
   gdb.UnwindInfo.add_saved_register (REG, VALUE) -> None.  */

static PyObject *
unwind_infopy_add_saved_register (PyObject *self, PyObject *args)
{
  unwind_info_object *unwind_info = (unwind_info_object *) self;
  pending_frame_object *pending_frame
      = (pending_frame_object *) (unwind_info->pending_frame);
  PyObject *pyo_reg_id;
  PyObject *pyo_reg_value;
  int regnum;

  if (pending_frame->frame_info == NULL)
    {
      PyErr_SetString (PyExc_ValueError,
                       "UnwindInfo instance refers to a stale PendingFrame");
      return NULL;
    }
  if (!PyArg_UnpackTuple (args, "previous_frame_register", 2, 2,
                          &pyo_reg_id, &pyo_reg_value))
    return NULL;
  if (!pyuw_parse_register_id (pending_frame->gdbarch, pyo_reg_id, &regnum))
    {
      PyErr_SetString (PyExc_ValueError, "Bad register");
      return NULL;
    }
  {
    struct value *value;
    size_t data_size;

    if (pyo_reg_value == NULL
      || (value = value_object_to_value (pyo_reg_value)) == NULL)
      {
        PyErr_SetString (PyExc_ValueError, "Bad register value");
        return NULL;
      }
    data_size = register_size (pending_frame->gdbarch, regnum);
    if (data_size != TYPE_LENGTH (value_type (value)))
      {
        PyErr_Format (
            PyExc_ValueError,
            "The value of the register returned by the Python "
            "sniffer has unexpected size: %u instead of %u.",
            (unsigned) TYPE_LENGTH (value_type (value)),
            (unsigned) data_size);
        return NULL;
      }
  }
  {
    gdbpy_ref<> new_value = gdbpy_ref<>::new_reference (pyo_reg_value);
    bool found = false;
    for (saved_reg &reg : *unwind_info->saved_regs)
      {
        if (regnum == reg.number)
          {
	    found = true;
	    reg.value = std::move (new_value);
            break;
          }
      }
    if (!found)
      unwind_info->saved_regs->emplace_back (regnum, std::move (new_value));
  }
  Py_RETURN_NONE;
}

/* UnwindInfo cleanup.  */

static void
unwind_infopy_dealloc (PyObject *self)
{
  unwind_info_object *unwind_info = (unwind_info_object *) self;
  int i;
  saved_reg *reg;

  Py_XDECREF (unwind_info->pending_frame);
  delete unwind_info->saved_regs;
  Py_TYPE (self)->tp_free (self);
}

/* Called by the Python interpreter to obtain string representation
   of the PendingFrame object.  */

static PyObject *
pending_framepy_str (PyObject *self)
{
  struct frame_info *frame = ((pending_frame_object *) self)->frame_info;
  const char *sp_str = NULL;
  const char *pc_str = NULL;

  if (frame == NULL)
    return PyString_FromString ("Stale PendingFrame instance");
  TRY
    {
      sp_str = core_addr_to_string_nz (get_frame_sp (frame));
      pc_str = core_addr_to_string_nz (get_frame_pc (frame));
    }
  CATCH (except, RETURN_MASK_ALL)
    {
      GDB_PY_HANDLE_EXCEPTION (except);
    }
  END_CATCH

  return PyString_FromFormat ("SP=%s,PC=%s", sp_str, pc_str);
}

/* Implementation of gdb.PendingFrame.read_register (self, reg) -> gdb.Value.
   Returns the value of register REG as gdb.Value instance.  */

static PyObject *
pending_framepy_read_register (PyObject *self, PyObject *args)
{
  pending_frame_object *pending_frame = (pending_frame_object *) self;
  struct value *val = NULL;
  int regnum;
  PyObject *pyo_reg_id;

  if (pending_frame->frame_info == NULL)
    {
      PyErr_SetString (PyExc_ValueError,
                       "Attempting to read register from stale PendingFrame");
      return NULL;
    }
  if (!PyArg_UnpackTuple (args, "read_register", 1, 1, &pyo_reg_id))
    return NULL;
  if (!pyuw_parse_register_id (pending_frame->gdbarch, pyo_reg_id, &regnum))
    {
      PyErr_SetString (PyExc_ValueError, "Bad register");
      return NULL;
    }

  TRY
    {
      /* Fetch the value associated with a register, whether it's
	 a real register or a so called "user" register, like "pc",
	 which maps to a real register.  In the past,
	 get_frame_register_value() was used here, which did not
	 handle the user register case.  */
      val = value_of_register (regnum, pending_frame->frame_info);
      if (val == NULL)
        PyErr_Format (PyExc_ValueError,
                      "Cannot read register %d from frame.",
                      regnum);
    }
  CATCH (except, RETURN_MASK_ALL)
    {
      GDB_PY_HANDLE_EXCEPTION (except);
    }
  END_CATCH

  return val == NULL ? NULL : value_to_value_object (val);
}

/* Implementation of
   PendingFrame.create_unwind_info (self, frameId) -> UnwindInfo.  */

static PyObject *
pending_framepy_create_unwind_info (PyObject *self, PyObject *args)
{
  PyObject *pyo_frame_id;
  CORE_ADDR sp;
  CORE_ADDR pc;
  CORE_ADDR special;

  if (!PyArg_ParseTuple (args, "O:create_unwind_info", &pyo_frame_id))
      return NULL;
  if (!pyuw_object_attribute_to_pointer (pyo_frame_id, "sp", &sp))
    {
      PyErr_SetString (PyExc_ValueError,
                       _("frame_id should have 'sp' attribute."));
      return NULL;
    }

  /* The logic of building frame_id depending on the attributes of
     the frame_id object:
     Has     Has    Has           Function to call
     'sp'?   'pc'?  'special'?
     ------|------|--------------|-------------------------
     Y       N      *             frame_id_build_wild (sp)
     Y       Y      N             frame_id_build (sp, pc)
     Y       Y      Y             frame_id_build_special (sp, pc, special)
  */
  if (!pyuw_object_attribute_to_pointer (pyo_frame_id, "pc", &pc))
    return pyuw_create_unwind_info (self, frame_id_build_wild (sp));
  if (!pyuw_object_attribute_to_pointer (pyo_frame_id, "special", &special))
    return pyuw_create_unwind_info (self, frame_id_build (sp, pc));
  else
    return pyuw_create_unwind_info (self,
                                    frame_id_build_special (sp, pc, special));
}

/* frame_unwind.this_id method.  */

static void
pyuw_this_id (struct frame_info *this_frame, void **cache_ptr,
              struct frame_id *this_id)
{
  *this_id = ((cached_frame_info *) *cache_ptr)->frame_id;
  if (pyuw_debug >= 1)
    {
      fprintf_unfiltered (gdb_stdlog, "%s: frame_id: ", __FUNCTION__);
      fprint_frame_id (gdb_stdlog, *this_id);
      fprintf_unfiltered (gdb_stdlog, "\n");
    }
}

/* frame_unwind.prev_register.  */

static struct value *
pyuw_prev_register (struct frame_info *this_frame, void **cache_ptr,
                    int regnum)
{
  cached_frame_info *cached_frame = (cached_frame_info *) *cache_ptr;
  cached_reg_t *reg_info = cached_frame->reg;
  cached_reg_t *reg_info_end = reg_info + cached_frame->reg_count;

  TRACE_PY_UNWIND (1, "%s (frame=%p,...,reg=%d)\n", __FUNCTION__, this_frame,
                   regnum);
  for (; reg_info < reg_info_end; ++reg_info)
    {
      if (regnum == reg_info->num)
        return frame_unwind_got_bytes (this_frame, regnum, reg_info->data);
    }

  return frame_unwind_got_optimized (this_frame, regnum);
}

/* Frame sniffer dispatch.  */

static int
pyuw_sniffer (const struct frame_unwind *self, struct frame_info *this_frame,
              void **cache_ptr)
{
  struct gdbarch *gdbarch = (struct gdbarch *) (self->unwind_data);
  cached_frame_info *cached_frame;

  gdbpy_enter enter_py (gdbarch, current_language);

  TRACE_PY_UNWIND (3, "%s (SP=%s, PC=%s)\n", __FUNCTION__,
                   paddress (gdbarch, get_frame_sp (this_frame)),
                   paddress (gdbarch, get_frame_pc (this_frame)));

  /* Create PendingFrame instance to pass to sniffers.  */
  pending_frame_object *pfo = PyObject_New (pending_frame_object,
					    &pending_frame_object_type);
  gdbpy_ref<> pyo_pending_frame ((PyObject *) pfo);
  if (pyo_pending_frame == NULL)
    {
      gdbpy_print_stack ();
      return 0;
    }
  pfo->gdbarch = gdbarch;
  scoped_restore invalidate_frame = make_scoped_restore (&pfo->frame_info,
							 this_frame);

  /* Run unwinders.  */
  if (gdb_python_module == NULL
      || ! PyObject_HasAttrString (gdb_python_module, "execute_unwinders"))
    {
      PyErr_SetString (PyExc_NameError,
                       "Installation error: gdb.execute_unwinders function "
                       "is missing");
      gdbpy_print_stack ();
      return 0;
    }
  gdbpy_ref<> pyo_execute (PyObject_GetAttrString (gdb_python_module,
						   "execute_unwinders"));
  if (pyo_execute == NULL)
    {
      gdbpy_print_stack ();
      return 0;
    }

  gdbpy_ref<> pyo_unwind_info
    (PyObject_CallFunctionObjArgs (pyo_execute.get (),
				   pyo_pending_frame.get (), NULL));
  if (pyo_unwind_info == NULL)
    {
      /* If the unwinder is cancelled due to a Ctrl-C, then propagate
	 the Ctrl-C as a GDB exception instead of swallowing it.  */
      if (PyErr_ExceptionMatches (PyExc_KeyboardInterrupt))
	{
	  PyErr_Clear ();
	  quit ();
	}
      gdbpy_print_stack ();
      return 0;
    }
  if (pyo_unwind_info == Py_None)
    return 0;

  /* Received UnwindInfo, cache data.  */
  if (PyObject_IsInstance (pyo_unwind_info.get (),
                           (PyObject *) &unwind_info_object_type) <= 0)
    error (_("A Unwinder should return gdb.UnwindInfo instance."));

  {
    unwind_info_object *unwind_info =
      (unwind_info_object *) pyo_unwind_info.get ();
    int reg_count = unwind_info->saved_regs->size ();

    cached_frame
      = ((cached_frame_info *)
	 xmalloc (sizeof (*cached_frame)
		  + reg_count * sizeof (cached_frame->reg[0])));
    cached_frame->gdbarch = gdbarch;
    cached_frame->frame_id = unwind_info->frame_id;
    cached_frame->reg_count = reg_count;

    /* Populate registers array.  */
    for (int i = 0; i < unwind_info->saved_regs->size (); ++i)
      {
	saved_reg *reg = &(*unwind_info->saved_regs)[i];

        struct value *value = value_object_to_value (reg->value.get ());
        size_t data_size = register_size (gdbarch, reg->number);

	cached_frame->reg[i].num = reg->number;

        /* `value' validation was done before, just assert.  */
        gdb_assert (value != NULL);
        gdb_assert (data_size == TYPE_LENGTH (value_type (value)));

	cached_frame->reg[i].data = (gdb_byte *) xmalloc (data_size);
        memcpy (cached_frame->reg[i].data, value_contents (value), data_size);
      }
  }

  *cache_ptr = cached_frame;
  return 1;
}

/* Frame cache release shim.  */

static void
pyuw_dealloc_cache (struct frame_info *this_frame, void *cache)
{
  TRACE_PY_UNWIND (3, "%s: enter", __FUNCTION__);
  cached_frame_info *cached_frame = (cached_frame_info *) cache;

  for (int i = 0; i < cached_frame->reg_count; i++)
    xfree (cached_frame->reg[i].data);

  xfree (cache);
}

struct pyuw_gdbarch_data_type
{
  /* Has the unwinder shim been prepended? */
  int unwinder_registered;
};

static void *
pyuw_gdbarch_data_init (struct gdbarch *gdbarch)
{
  return GDBARCH_OBSTACK_ZALLOC (gdbarch, struct pyuw_gdbarch_data_type);
}

/* New inferior architecture callback: register the Python unwinders
   intermediary.  */

static void
pyuw_on_new_gdbarch (struct gdbarch *newarch)
{
  struct pyuw_gdbarch_data_type *data
    = (struct pyuw_gdbarch_data_type *) gdbarch_data (newarch,
						      pyuw_gdbarch_data);

  if (!data->unwinder_registered)
    {
      struct frame_unwind *unwinder
          = GDBARCH_OBSTACK_ZALLOC (newarch, struct frame_unwind);

      unwinder->type = NORMAL_FRAME;
      unwinder->stop_reason = default_frame_unwind_stop_reason;
      unwinder->this_id = pyuw_this_id;
      unwinder->prev_register = pyuw_prev_register;
      unwinder->unwind_data = (const struct frame_data *) newarch;
      unwinder->sniffer = pyuw_sniffer;
      unwinder->dealloc_cache = pyuw_dealloc_cache;
      frame_unwind_prepend_unwinder (newarch, unwinder);
      data->unwinder_registered = 1;
    }
}

/* Initialize unwind machinery.  */

int
gdbpy_initialize_unwind (void)
{
  int rc;
  add_setshow_zuinteger_cmd
      ("py-unwind", class_maintenance, &pyuw_debug,
        _("Set Python unwinder debugging."),
        _("Show Python unwinder debugging."),
        _("When non-zero, Python unwinder debugging is enabled."),
        NULL,
        NULL,
        &setdebuglist, &showdebuglist);
  pyuw_gdbarch_data
      = gdbarch_data_register_post_init (pyuw_gdbarch_data_init);
  gdb::observers::architecture_changed.attach (pyuw_on_new_gdbarch);

  if (PyType_Ready (&pending_frame_object_type) < 0)
    return -1;
  rc = gdb_pymodule_addobject (gdb_module, "PendingFrame",
      (PyObject *) &pending_frame_object_type);
  if (rc)
    return rc;

  if (PyType_Ready (&unwind_info_object_type) < 0)
    return -1;
  return gdb_pymodule_addobject (gdb_module, "UnwindInfo",
      (PyObject *) &unwind_info_object_type);
}

static PyMethodDef pending_frame_object_methods[] =
{
  { "read_register", pending_framepy_read_register, METH_VARARGS,
    "read_register (REG) -> gdb.Value\n"
    "Return the value of the REG in the frame." },
  { "create_unwind_info",
    pending_framepy_create_unwind_info, METH_VARARGS,
    "create_unwind_info (FRAME_ID) -> gdb.UnwindInfo\n"
    "Construct UnwindInfo for this PendingFrame, using FRAME_ID\n"
    "to identify it." },
  {NULL}  /* Sentinel */
};

PyTypeObject pending_frame_object_type =
{
  PyVarObject_HEAD_INIT (NULL, 0)
  "gdb.PendingFrame",             /* tp_name */
  sizeof (pending_frame_object),  /* tp_basicsize */
  0,                              /* tp_itemsize */
  0,                              /* tp_dealloc */
  0,                              /* tp_print */
  0,                              /* tp_getattr */
  0,                              /* tp_setattr */
  0,                              /* tp_compare */
  0,                              /* tp_repr */
  0,                              /* tp_as_number */
  0,                              /* tp_as_sequence */
  0,                              /* tp_as_mapping */
  0,                              /* tp_hash  */
  0,                              /* tp_call */
  pending_framepy_str,            /* tp_str */
  0,                              /* tp_getattro */
  0,                              /* tp_setattro */
  0,                              /* tp_as_buffer */
  Py_TPFLAGS_DEFAULT,             /* tp_flags */
  "GDB PendingFrame object",      /* tp_doc */
  0,                              /* tp_traverse */
  0,                              /* tp_clear */
  0,                              /* tp_richcompare */
  0,                              /* tp_weaklistoffset */
  0,                              /* tp_iter */
  0,                              /* tp_iternext */
  pending_frame_object_methods,   /* tp_methods */
  0,                              /* tp_members */
  0,                              /* tp_getset */
  0,                              /* tp_base */
  0,                              /* tp_dict */
  0,                              /* tp_descr_get */
  0,                              /* tp_descr_set */
  0,                              /* tp_dictoffset */
  0,                              /* tp_init */
  0,                              /* tp_alloc */
};

static PyMethodDef unwind_info_object_methods[] =
{
  { "add_saved_register",
    unwind_infopy_add_saved_register, METH_VARARGS,
    "add_saved_register (REG, VALUE) -> None\n"
    "Set the value of the REG in the previous frame to VALUE." },
  { NULL }  /* Sentinel */
};

PyTypeObject unwind_info_object_type =
{
  PyVarObject_HEAD_INIT (NULL, 0)
  "gdb.UnwindInfo",               /* tp_name */
  sizeof (unwind_info_object),    /* tp_basicsize */
  0,                              /* tp_itemsize */
  unwind_infopy_dealloc,          /* tp_dealloc */
  0,                              /* tp_print */
  0,                              /* tp_getattr */
  0,                              /* tp_setattr */
  0,                              /* tp_compare */
  0,                              /* tp_repr */
  0,                              /* tp_as_number */
  0,                              /* tp_as_sequence */
  0,                              /* tp_as_mapping */
  0,                              /* tp_hash  */
  0,                              /* tp_call */
  unwind_infopy_str,              /* tp_str */
  0,                              /* tp_getattro */
  0,                              /* tp_setattro */
  0,                              /* tp_as_buffer */
  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /* tp_flags */
  "GDB UnwindInfo object",        /* tp_doc */
  0,                              /* tp_traverse */
  0,                              /* tp_clear */
  0,                              /* tp_richcompare */
  0,                              /* tp_weaklistoffset */
  0,                              /* tp_iter */
  0,                              /* tp_iternext */
  unwind_info_object_methods,     /* tp_methods */
  0,                              /* tp_members */
  0,                              /* tp_getset */
  0,                              /* tp_base */
  0,                              /* tp_dict */
  0,                              /* tp_descr_get */
  0,                              /* tp_descr_set */
  0,                              /* tp_dictoffset */
  0,                              /* tp_init */
  0,                              /* tp_alloc */
};
