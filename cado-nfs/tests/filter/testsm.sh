#!/usr/bin/env bash

# The main functions to compute SM are tested in utils/test_sm_utils.
# This test is here to check that the multi-threaded and mono-threaded versions
# give the same results and that the -nsm option works correctly.

SM="$1"
SOURCE_TEST_DIR="`dirname "$0"`"
WORKDIR=`mktemp -d ${TMPDIR-/tmp}/cado-nfs.XXXXXXXX`
# Make temp direcotry world-readable for easier debugging
chmod a+rx "${WORKDIR}"

poly="${SOURCE_TEST_DIR}/testsm.p59.poly"
purged="${SOURCE_TEST_DIR}/testsm.p59.purged.gz"
id="${SOURCE_TEST_DIR}/testsm.p59.index.gz"
go="2926718140519"


args="-poly ${poly} -purged ${purged} -index ${id} -ell ${go} -nsm 0,4"
args2="-poly ${poly} -purged ${purged} -index ${id} -ell ${go} -nsm 0,2"

#with -nsm 0,4 (ie nsm = deg F-1)
if ! "${SM}" ${args} -out "${WORKDIR}/sm.4.1" ; then
  echo "$0: sm binary failed without -nsm. Files remain in ${WORKDIR}"
  exit 1
fi

#with -nsm 0,2 and -t 1
if ! "${SM}" ${args2} -out "${WORKDIR}/sm.2.1" ; then
  echo "$0: sm binary failed with -nsm1 2. Files remain in ${WORKDIR}"
  exit 1
fi


tail -n +2 "${WORKDIR}/sm.4.1" | cut -d ' ' -f1-2 > "${WORKDIR}/sm.4.1.short"
tail -n +2 "${WORKDIR}/sm.2.1" > "${WORKDIR}/sm.2.1.short"

if ! diff -b "${WORKDIR}/sm.4.1.short" "${WORKDIR}/sm.2.1.short" > /dev/null ; then
  echo "$0: First two SMs computed without -nsm do not match SMs computed with -nsm 0,2). Files remain in ${WORKDIR}"
  exit 1
fi

rm -f "${WORKDIR}/sm.4.1"
rm -f "${WORKDIR}/sm.2.1"
rm -f "${WORKDIR}/sm.4.1.short" "${WORKDIR}/sm.2.1.short"
rmdir "${WORKDIR}"
