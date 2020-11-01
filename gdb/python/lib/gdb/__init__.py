# Copyright (C) 2010-2018 Free Software Foundation, Inc.

# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

import traceback
import os
import sys
import _gdb

if sys.version_info[0] > 2:
    # Python 3 moved "reload"
    from imp import reload

from _gdb import *

class _GdbFile (object):
    # These two are needed in Python 3
    encoding = "UTF-8"
    errors = "strict"

    def close(self):
        # Do nothing.
        return None

    def isatty(self):
        return False

    def writelines(self, iterable):
        for line in iterable:
            self.write(line)

    def flush(self):
        flush()

class GdbOutputFile (_GdbFile):
    def write(self, s):
        write(s, stream=STDOUT)

sys.stdout = GdbOutputFile()

class GdbOutputErrorFile (_GdbFile):
    def write(self, s):
        write(s, stream=STDERR)

sys.stderr = GdbOutputErrorFile()

# Default prompt hook does nothing.
prompt_hook = None

# Ensure that sys.argv is set to something.
# We do not use PySys_SetArgvEx because it did not appear until 2.6.6.
sys.argv = ['']

# Initial pretty printers.
pretty_printers = []

# Initial type printers.
type_printers = []
# Initial xmethod matchers.
xmethods = []
# Initial frame filters.
frame_filters = {}
# Initial frame unwinders.
frame_unwinders = []

def execute_unwinders(pending_frame):
    """Internal function called from GDB to execute all unwinders.

    Runs each currently enabled unwinder until it finds the one that
    can unwind given frame.

    Arguments:
        pending_frame: gdb.PendingFrame instance.
    Returns:
        gdb.UnwindInfo instance or None.
    """
    for objfile in _gdb.objfiles():
        for unwinder in objfile.frame_unwinders:
            if unwinder.enabled:
                unwind_info = unwinder(pending_frame)
                if unwind_info is not None:
                    return unwind_info

    current_progspace = _gdb.current_progspace()
    for unwinder in current_progspace.frame_unwinders:
        if unwinder.enabled:
            unwind_info = unwinder(pending_frame)
            if unwind_info is not None:
                return unwind_info

    for unwinder in frame_unwinders:
        if unwinder.enabled:
            unwind_info = unwinder(pending_frame)
            if unwind_info is not None:
                return unwind_info

    return None

def _execute_file(filepath):
    """This function is used to replace Python 2's PyRun_SimpleFile.

    Loads and executes the given file.

    We could use the runpy module, but its documentation says:
    "Furthermore, any functions and classes defined by the executed code are
    not guaranteed to work correctly after a runpy function has returned."
    """
    globals = sys.modules['__main__'].__dict__
    set_file = False
    # Set file (if not set) so that the imported file can use it (e.g. to
    # access file-relative paths). This matches what PyRun_SimpleFile does.
    if not hasattr(globals, '__file__'):
        globals['__file__'] = filepath
        set_file = True
    try:
        with open(filepath, 'rb') as file:
            # We pass globals also as locals to match what Python does
            # in PyRun_SimpleFile.
            compiled = compile(file.read(), filepath, 'exec')
            exec(compiled, globals, globals)
    finally:
        if set_file:
            del globals['__file__']


# Convenience variable to GDB's python directory
PYTHONDIR = os.path.dirname(os.path.dirname(__file__))

# Auto-load all functions/commands.

# Packages to auto-load.

packages = [
    'function',
    'command',
    'printer'
]

# pkgutil.iter_modules is not available prior to Python 2.6.  Instead,
# manually iterate the list, collating the Python files in each module
# path.  Construct the module name, and import.

def auto_load_packages():
    for package in packages:
        location = os.path.join(os.path.dirname(__file__), package)
        if os.path.exists(location):
            py_files = filter(lambda x: x.endswith('.py')
                                        and x != '__init__.py',
                              os.listdir(location))

            for py_file in py_files:
                # Construct from foo.py, gdb.module.foo
                modname = "%s.%s.%s" % ( __name__, package, py_file[:-3] )
                try:
                    if modname in sys.modules:
                        # reload modules with duplicate names
                        reload(__import__(modname))
                    else:
                        __import__(modname)
                except:
                    sys.stderr.write (traceback.format_exc() + "\n")

auto_load_packages()

def GdbSetPythonDirectory(dir):
    """Update sys.path, reload gdb and auto-load packages."""
    global PYTHONDIR

    try:
        sys.path.remove(PYTHONDIR)
    except ValueError:
        pass
    sys.path.insert(0, dir)

    PYTHONDIR = dir

    # note that reload overwrites the gdb module without deleting existing
    # attributes
    reload(__import__(__name__))
    auto_load_packages()
