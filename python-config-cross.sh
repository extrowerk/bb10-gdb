#!/bin/bash

# Script for cross-compiling win64 and darwin and linux on linux host.
# Provided to gdb configure as an argument to --with-python
#
# It needs to respond to --exec-prefix --includes and --ldflags arguments.
# Gdb configure will pass python-configure.py (path to it) followed by one of
# the above options.
#
# also the build script or build-hooks must make sure that ${OS} and
# ${PREREQ_DIR} are set and exported

echo "$*" >> python-config-cross.log
if [ "${PREREQ_DIR}" == "" ]; then
    PREREQ_DIR=$(realpath ${PWD}/../../../GDBPython)
fi

# echo "OS=${TARGET_SYSNAME}, PREREQ_DIR=${PREREQ_DIR}" >> python-config-cross.log
python_root=${PREREQ_DIR}/${TARGET_SYSNAME}
py_include=-I${python_root}/usr/include/python3.8
py_exec_prefix="${prefix}/bin"
py_libs="-L${python_root}/usr/lib -lpython3.8"

case ${TARGET_SYSNAME} in
win64)
	py_include="-I${python_root}/usr/include ${py_include} -DWINVER=0x0500"
	py_libs="-L${python_root}/usr/lib -lpython3.8.dll"
	;;
darwin)
	;;
linux)
	# For testing, it is convenient to be able to run gdb from build directory
#	ln -fs ${gdb_prereq_dir}/python27 ..
	;;
*)
	echo "${TARGET_SYSNAME} is not configured for python" >> python-config-cross.log
	exit 1
	;;
esac


if [ $2 == --includes ] ; then
  echo "${py_include}"
  echo "${py_include}" >> python-config-cross.log
elif [ $2 == --ldflags ] ; then
  echo "${py_libs}"
  echo "${py_libs}" >> python-config-cross.log
elif [ $2 == --exec-prefix ] ; then
  echo "${py_exec_prefix}"
  echo "${py_exec_prefix}" >> python-config-cross.log
else
echo "Unknown parameter $2" >> python-config-cross.log
  exit 1
fi
echo "" >> python-config-cross.log

